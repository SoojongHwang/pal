/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#include "memoryCacheLayer.h"
#include "palHashMapImpl.h"
#include "palIntrusiveListImpl.h"
#include "palAssert.h"
#include "core/platform.h"

namespace Util
{

// =====================================================================================================================
MemoryCacheLayer::MemoryCacheLayer(
    const AllocCallbacks& callbacks,
    size_t                maxMemorySize,
    size_t                maxObjectCount,
    bool                  evictOnFull,
    bool                  evictDuplicates)
    :
    CacheLayerBase    { callbacks },
    m_maxSize         { maxMemorySize },
    m_maxCount        { maxObjectCount },
    m_evictOnFull     { evictOnFull },
    m_evictDuplicates { evictDuplicates },
    m_lock            {},
    m_curSize         { 0 },
    m_curCount        { 0 },
    m_recentEntryList {},
    m_entryLookup     { 2048, Allocator() }
{
}

// =====================================================================================================================
MemoryCacheLayer::~MemoryCacheLayer()
{
    while (m_recentEntryList.IsEmpty() == false)
    {
        Entry* pEntry = m_recentEntryList.Front();
        m_entryLookup.Erase(*pEntry->HashId());
        m_recentEntryList.Erase(pEntry->ListNode());
        pEntry->Destroy();
    }
}

// =====================================================================================================================
// Initialize the cache layer
Result MemoryCacheLayer::Init()
{
    Result result = CacheLayerBase::Init();

    if (result == Result::Success)
    {
        result = m_entryLookup.Init();
    }

    return result;
}

// =====================================================================================================================
// Check if a requested id is present
Result MemoryCacheLayer::QueryInternal(
    const Hash128*  pHashId,
    QueryResult*    pQuery)
{
    Result result = Result::Success;

    Entry** ppFound = nullptr;

    RWLockAuto<RWLock::ReadWrite> lock { &m_lock };

    ppFound = m_entryLookup.FindKey(*pHashId);

    if (ppFound == nullptr)
    {
        result = Result::NotFound;
    }
    else if (*ppFound != nullptr)
    {
        Entry::Node* pNode = (*ppFound)->ListNode();
        m_recentEntryList.Erase(pNode);
        m_recentEntryList.PushBack(pNode);

        pQuery->hashId             = *pHashId;
        pQuery->pLayer             = this;
        pQuery->dataSize           = (*ppFound)->DataSize();
        pQuery->context.pEntryInfo = (*ppFound)->Data();
        if (pQuery->dataSize == 0)
        {
            result = Result::NotReady;
        }
    }
    else
    {
        result = Result::ErrorUnknown;
    }

    return result;
}

// =====================================================================================================================
// Add data passed in to the cache
Result MemoryCacheLayer::StoreInternal(
    const Hash128*  pHashId,
    const void*     pData,
    size_t          dataSize)
{
    Result result = Result::Success;

    if ((pHashId == nullptr) ||
        (pData == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }

    if (dataSize == 0)
    {
        result = Result::ErrorInvalidValue;
    }

    bool setData = false;
    if (result == Result::Success)
    {
        Entry** ppFound = nullptr;

        RWLockAuto<RWLock::ReadWrite> lock { &m_lock };

        ppFound = m_entryLookup.FindKey(*pHashId);

        if (ppFound != nullptr)
        {
            if (*ppFound != nullptr)
            {
                if ((*ppFound)->Data() == nullptr)
                {
                    result = SetDataToEntry(*ppFound, pData, dataSize);
                    if (result == Result::Success)
                    {
                        setData = true;
                        m_conditionVariable.WakeAll();
                    }
                }
                else if (m_evictDuplicates)
                {
                    result = EvictEntryFromCache(*ppFound);
                }
                else
                {
                    result = Result::AlreadyExists;
                }
            }
            else
            {
                result = Result::ErrorUnknown;
            }
        }
    }

    if ((result == Result::Success) && (setData == false))
    {
        RWLockAuto<RWLock::ReadWrite> lock { &m_lock };

        result = EnsureAvailableSpace(dataSize, 1);
    }

    if ((result == Result::Success) && (setData == false))
    {
        Entry* pEntry = Entry::Create(Allocator(), pHashId, pData, dataSize);

        if (pEntry != nullptr)
        {
            RWLockAuto<RWLock::ReadWrite> lock { &m_lock };

            result = AddEntryToCache(pEntry);

            if (result != Result::Success)
            {
                pEntry->Destroy();
                pEntry = nullptr;
            }
        }
        else
        {
            result = Result::ErrorOutOfMemory;
        }
    }

    return result;
}

// =====================================================================================================================
// Copy data from cache to the provided buffer
Result MemoryCacheLayer::LoadInternal(
    const QueryResult* pQuery,
    void*              pBuffer)
{
    Result result = Result::Success;

    if ((pQuery == nullptr) ||
        (pBuffer == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        Entry** ppFound = nullptr;

        RWLockAuto<RWLock::ReadOnly> lock { &m_lock };

        ppFound = m_entryLookup.FindKey(pQuery->hashId);
        if (ppFound != nullptr)
        {
            if ((*ppFound)->Data())
            {
                memcpy(pBuffer, (*ppFound)->Data(), (*ppFound)->DataSize());
            }
            else
            {
                result = Result::NotReady;
            }
        }
        else
        {
            // The specified entry is evicted, not available any more.
            result = Result::ErrorInvalidPointer;
        }
    }

    return result;
}

// =====================================================================================================================
Result MemoryCacheLayer::AcquireCacheRef(
    const QueryResult* pQuery)
{
    Result result = Result::Success;

    if (pQuery == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        Entry** ppFound = nullptr;

        RWLockAuto<RWLock::ReadOnly> lock { &m_lock };

        ppFound = m_entryLookup.FindKey(pQuery->hashId);
        if (ppFound != nullptr)
        {
            (*ppFound)->IncreaseRef();
        }
        else
        {
            result = Result::NotFound;
        }
    }

    return result;
}

// =====================================================================================================================
Result MemoryCacheLayer::ReleaseCacheRef(
    const QueryResult* pQuery)
{
    Result result = Result::Success;

    if (pQuery == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        Entry** ppFound = nullptr;

        RWLockAuto<RWLock::ReadOnly> lock { &m_lock };

        ppFound = m_entryLookup.FindKey(pQuery->hashId);
        if (ppFound != nullptr)
        {
            (*ppFound)->DecreaseRef();
            if ((*ppFound)->IsBad())
            {
                Evict(&pQuery->hashId);
            }
        }
        else
        {
            PAL_ASSERT_ALWAYS();
            // This should never happen, ReleaseCacheRef is after AcquireCacheRef.
            result = Result::NotFound;
        }
    }

    return result;
}

// =====================================================================================================================
Result MemoryCacheLayer::GetCacheData(
    const QueryResult* pQuery,
    const void** ppData)
{
    Result result = Result::Success;

    if ((pQuery == nullptr) || (ppData == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        Entry** ppFound = nullptr;

        RWLockAuto<RWLock::ReadOnly> lock { &m_lock };

        ppFound = m_entryLookup.FindKey(pQuery->hashId);
        if (ppFound != nullptr)
        {
            if ((*ppFound)->Data())
            {
                *ppData = (*ppFound)->Data();
            }
            else
            {
                result = Result::NotReady;
            }
        }
        else
        {
            result = Result::NotFound;
        }
    }

    return result;

}

static constexpr uint32_t CacheTimeout = 500;
// =====================================================================================================================
// Wait for the specified entry ready
Result MemoryCacheLayer::WaitForEntry(
    const Hash128* pHashId)
{
    Result result = Result::Success;

    if (pHashId == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        Entry** ppFound = nullptr;

        m_conditionMutex.Lock();
        for (;;)
        {
            {
                RWLockAuto<RWLock::ReadOnly> lock{ &m_lock };
                ppFound = m_entryLookup.FindKey(*pHashId);
                if (ppFound == nullptr)
                {
                    result = Result::NotFound;
                    break;
                }
                if ((*ppFound)->IsBad())
                {
                    result = Result::ErrorInvalidValue;
                    break;
                }
                if ((ppFound != nullptr) && ((*ppFound)->Data() != nullptr))
                {
                    result = Result::Success;
                    break;
                }
            }
            m_conditionVariable.Wait(&m_conditionMutex, CacheTimeout);
        }
        m_conditionMutex.Unlock();
    }

    return result;
}

// =====================================================================================================================
// Evict the specified entry
Result MemoryCacheLayer::Evict(
    const Hash128* pHashId)
{
    Result result = Result::Success;

    if (pHashId == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        Entry** ppFound = nullptr;

        RWLockAuto<RWLock::ReadOnly> lock { &m_lock };
        ppFound = m_entryLookup.FindKey(*pHashId);
        if (ppFound != nullptr)
        {
            result = EvictEntryFromCache(*ppFound);
        }
        else
        {
            result = Result::NotFound;
        }
    }

    return result;
}

// =====================================================================================================================
// Mark entry bad
Result MemoryCacheLayer::MarkEntryBad(
    const Hash128* pHashId)
{
    Result result = Result::Success;

    if (pHashId == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        Entry** ppFound = nullptr;

        RWLockAuto<RWLock::ReadOnly> lock { &m_lock };
        ppFound = m_entryLookup.FindKey(*pHashId);
        if (ppFound != nullptr)
        {
            (*ppFound)->SetIsBad(true);
            m_conditionVariable.WakeAll();
        }
        else
        {
            result = Result::NotFound;
        }
    }

    return result;

}

// =====================================================================================================================
// Evict entries until a specified count is reached
Result MemoryCacheLayer::EvictEntryByCount(
    size_t numToEvict)
{
    Result result = Result::Success;

    size_t numEvicted = 0;

    while ((result == Result::Success) &&
           (numEvicted < numToEvict))
    {
        Entry* const pEntry = m_recentEntryList.Front();

        if (pEntry != nullptr)
        {
            result = EvictEntryFromCache(pEntry);

            if (result == Result::Success)
            {
                ++numEvicted;
            }
        }
        else
        {
            result = Result::ErrorShaderCacheFull;
        }
    }

    return result;
}

// =====================================================================================================================
// Evict entries until a specified size is reached
Result MemoryCacheLayer::EvictEntryBySize(
    size_t minSizeToEvict)
{
    Result result = Result::Success;

    size_t evictedSize = 0;

    while ((result == Result::Success) &&
           (evictedSize < minSizeToEvict))
    {
        Entry* const pEntry = m_recentEntryList.Front();

        if (pEntry != nullptr)
        {
            const size_t dataSize = pEntry->DataSize();

            result = EvictEntryFromCache(pEntry);

            if (result == Result::Success)
            {
                evictedSize += dataSize;
            }
        }
        else
        {
            result = Result::ErrorShaderCacheFull;
        }
    }

    return result;
}

// =====================================================================================================================
// Remove an entry from the cache table, list, and metrics.
Result MemoryCacheLayer::EvictEntryFromCache(
    Entry* pEntry)
{
    PAL_ASSERT(pEntry != nullptr);

    Result result = Result::ErrorUnknown;

    if (pEntry->CanEvict())
    {
        if (m_entryLookup.Erase(*pEntry->HashId()))
        {
            result = Result::Success;

            m_recentEntryList.Erase(pEntry->ListNode());
            m_curSize -= pEntry->DataSize();
            m_curCount -= 1;
            pEntry->Destroy();
        }
    }

    return result;
}

// =====================================================================================================================
// Insert the entry into our cache lookup table and LRU list
Result MemoryCacheLayer::AddEntryToCache(
    Entry* pEntry)
{
    PAL_ASSERT(pEntry != nullptr);

    Result result = m_entryLookup.Insert(*pEntry->HashId(), pEntry);

    if (result == Result::Success)
    {
        m_recentEntryList.PushBack(pEntry->ListNode());
        m_curSize += pEntry->DataSize();
        m_curCount++;
    }

    return result;
}

// =====================================================================================================================
// Set data to Entry
Result MemoryCacheLayer::SetDataToEntry(
    Entry*      pEntry,
    const void* pData,
    size_t      dataSize)
{
    PAL_ASSERT(pEntry != nullptr);
    Result result = Result::Success;

    if ((pData != nullptr) && (dataSize > 0))
    {
        result = pEntry->SetData(pData, dataSize);

        if (result == Result::Success)
        {
            m_curSize += pEntry->DataSize();
        }
    }

    return result;
}

// =====================================================================================================================
// Ensure size requested is available within the cache, may evict data
Result MemoryCacheLayer::EnsureAvailableSpace(
    size_t entrySize,
    size_t entryCount)
{
    PAL_ASSERT(entrySize <= m_maxSize);
    PAL_ASSERT(entryCount <= m_maxCount);

    Result result = Result::Success;

    const size_t availableCount = m_maxCount - m_curCount;

    if (entryCount > availableCount)
    {
        result = Result::ErrorShaderCacheFull;

        if (m_evictOnFull)
        {
            result = EvictEntryByCount(entryCount - availableCount);
        }
    }

    const size_t availableSize = m_maxSize - m_curSize;

    if ((result == Result::Success) &&
        (entrySize > availableSize))
    {
        result = Result::ErrorShaderCacheFull;

        if (m_evictOnFull)
        {
            result = EvictEntryBySize(entrySize - availableSize);
        }
    }

    return result;
}

// =====================================================================================================================
// Promote data from another layer to ourselves
Result MemoryCacheLayer::PromoteData(
    ICacheLayer* pNextLayer,
    QueryResult* pQuery)
{
    PAL_ASSERT(pNextLayer != nullptr);
    PAL_ASSERT(pQuery != nullptr);

    Result result = Result::Success;

    if ((pNextLayer == nullptr) ||
        (pQuery == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }

    if (pQuery->dataSize == 0)
    {
        result = Result::ErrorInvalidValue;
    }

    Entry** ppFound = nullptr;

    {
        RWLockAuto<RWLock::ReadOnly> lock { &m_lock };

        ppFound = m_entryLookup.FindKey(pQuery->hashId);
    }

    if (ppFound != nullptr)
    {
        result = Result::AlreadyExists;
    }

    if (result == Result::Success)
    {
        RWLockAuto<RWLock::ReadWrite> lock { &m_lock };

        result = EnsureAvailableSpace(pQuery->dataSize, 1);
    }

    if (result == Result::Success)
    {
        Entry* pEntry = Entry::Create(Allocator(), &pQuery->hashId, nullptr, pQuery->dataSize);

        if (pEntry != nullptr)
        {
            result = pNextLayer->Load(pQuery, pEntry->Data());

            if (result == Result::Success)
            {
                RWLockAuto<RWLock::ReadWrite> lock { &m_lock };

                result = AddEntryToCache(pEntry);
                m_entryLookup.Insert(pQuery->hashId, pEntry);
            }

            if (result == Result::Success)
            {
                // Update the query to reflect our entry
                pQuery->pLayer             = this;
                pQuery->context.pEntryInfo = pEntry->Data();
            }
            else
            {
                pEntry->Destroy();
                pEntry = nullptr;
            }
        }
        else
        {
            result = Result::ErrorOutOfMemory;
        }
    }

    return result;
}

// =====================================================================================================================
// Reserve an empty Entry
Result MemoryCacheLayer::Reserve(
    const Hash128* pHashId)
{
    PAL_ASSERT(pHashId != nullptr);
    Result result = Result::Success;

    if (pHashId == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }

    if (result == Result::Success)
    {
        Entry** ppFound = nullptr;

        RWLockAuto<RWLock::ReadWrite> lock { &m_lock };

        ppFound = m_entryLookup.FindKey(*pHashId);
        if (ppFound != nullptr)
        {
            if (*ppFound != nullptr)
            {
                result = Result::AlreadyExists;
            }
            else
            {
                result = Result::ErrorUnknown;
            }
        }
    }

    if (result == Result::Success)
    {
        Entry* pEntry = Entry::Create(Allocator(), pHashId, nullptr, 0);
        if (pEntry != nullptr)
        {
            RWLockAuto<RWLock::ReadWrite> lock { &m_lock };
            result = AddEntryToCache(pEntry);
            if (result != Result::Success)
            {
                pEntry->Destroy();
                pEntry = nullptr;
            }
        }
        else
        {
          result = Result::ErrorOutOfMemory;
        }
    }

    return result;
}

// =====================================================================================================================
// Get the memory size for a in-memory cache layer
size_t GetMemoryCacheLayerSize(
    const MemoryCacheCreateInfo* pCreateInfo)
{
    return sizeof(MemoryCacheLayer);
}

// =====================================================================================================================
// Create an in-memory key-value caching layer
Result CreateMemoryCacheLayer(
    const MemoryCacheCreateInfo* pCreateInfo,
    void*                        pPlacementAddr,
    ICacheLayer**                ppCacheLayer)
{
    PAL_ASSERT(pCreateInfo != nullptr);
    PAL_ASSERT(pPlacementAddr != nullptr);
    PAL_ASSERT(ppCacheLayer != nullptr);

    Result            result = Result::Success;
    MemoryCacheLayer* pLayer = nullptr;

    if ((pCreateInfo == nullptr) ||
        (pPlacementAddr == nullptr) ||
        (ppCacheLayer == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        AllocCallbacks  callbacks = {};

        if (pCreateInfo->baseInfo.pCallbacks == nullptr)
        {
            Pal::GetDefaultAllocCb(&callbacks);
        }

        pLayer = PAL_PLACEMENT_NEW(pPlacementAddr) MemoryCacheLayer(
            (pCreateInfo->baseInfo.pCallbacks == nullptr) ? callbacks : *pCreateInfo->baseInfo.pCallbacks,
            pCreateInfo->maxMemorySize,
            pCreateInfo->maxObjectCount,
            pCreateInfo->evictOnFull,
            pCreateInfo->evictDuplicates);

        result = pLayer->Init();

        if (result == Result::Success)
        {
            *ppCacheLayer = pLayer;
        }
        else
        {
            pLayer->Destroy();
        }
    }

    return result;
}

// =====================================================================================================================
Result GetMemoryCacheLayerCurSize(
    ICacheLayer*    pCacheLayer,
    size_t*         pCurCount,    // [out] nubmer of Entries in memoryCache.
    size_t*         pCurSize)     // [out] total cahce data size
{
    auto pMemoryCache = static_cast<MemoryCacheLayer*>(pCacheLayer);

    return pMemoryCache->GetMemoryCacheSize(pCurCount, pCurSize);
}

// =====================================================================================================================
Result MemoryCacheLayer::GetMemoryCacheHashIds(
    size_t          curCount,
    Hash128*        pHashIds)
{
    Result result = Result::Success;

    RWLockAuto<RWLock::ReadOnly> lock { &m_lock };
    // Iterate through all Entries and copy their hash ID to pHashIds array.
    if (curCount == m_curCount)
    {
        uint32 i = 0;

        for (auto iter = m_recentEntryList.Begin(); iter.IsValid(); iter.Next())
        {
            Entry* pEntry = iter.Get();

            pHashIds[i++] = *pEntry->HashId();
        }
    }
    else
    {
        result = Result::ErrorInvalidMemorySize;
    }

    return result;
}

// =====================================================================================================================
Result GetMemoryCacheLayerHashIds(
    ICacheLayer*    pCacheLayer,
    size_t          curCount,
    Hash128*        pHashIds)
{
    auto pMemoryCache = static_cast<MemoryCacheLayer*>(pCacheLayer);

    return pMemoryCache->GetMemoryCacheHashIds(curCount, pHashIds);
}

// =====================================================================================================================
MemoryCacheLayer::Entry* MemoryCacheLayer::Entry::Create(
    ForwardAllocator* pAllocator,
    const Hash128*    pHashId,
    const void*       pInitialData,
    size_t            dataSize)
{
    PAL_ASSERT(pAllocator != nullptr);
    PAL_ASSERT(pHashId != nullptr);

    Entry* pEntry = nullptr;
    void*  pMem   = PAL_MALLOC(sizeof(Entry), pAllocator, AllocInternal);

    if (pMem != nullptr)
    {
        pEntry = PAL_PLACEMENT_NEW(pMem) Entry(pAllocator);

        void* pData = nullptr;

        if (dataSize > 0)
        {
            pData = PAL_MALLOC(dataSize, pAllocator, AllocInternal);
            if (pData != nullptr)
            {
                if (pInitialData != nullptr)
                {
                    memcpy(pData, pInitialData, dataSize);
                }
            }
            else
            {
                PAL_SAFE_FREE(pEntry, pAllocator);
            }
        }

        if (pEntry != nullptr)
        {
            pEntry->m_hashId   = *pHashId;
            pEntry->m_pData    = pData;
            pEntry->m_dataSize = dataSize;
            pEntry->m_zeroCopyCount = 0;
        }
    }

    return pEntry;
}

// =====================================================================================================================
Result MemoryCacheLayer::Entry::SetData(
    const void* pData,
    size_t      dataSize)
{
    PAL_ASSERT(m_pData == nullptr);
    Result result = Result::Success;

    if (pData)
    {
        m_pData = PAL_MALLOC(dataSize, m_pAllocator, AllocInternal);
        if (m_pData != nullptr)
        {
            memcpy(m_pData, pData, dataSize);
            m_dataSize = dataSize;
        }
        else
        {
            result = Result::ErrorOutOfMemory;
        }
    }
    return result;
}

// =====================================================================================================================
void MemoryCacheLayer::Entry::Destroy()
{
    ForwardAllocator* pAllocator = m_pAllocator;
    this->~Entry();
    if (m_pData != nullptr)
    {
        PAL_FREE(m_pData, pAllocator);
    }
    PAL_FREE(this, pAllocator);
}

} //namespace Util
