#include "../util.h"
#include "RawSlabCache.h"
#include <crtdbg.h>

//========================================================================
// RawSlabCache
//========================================================================
Hydra::RawSlabCache::RawSlabCache(
    unsigned obj_size,
    unsigned obj_align,
    unsigned slab_pages,
    unsigned pre_alloc
) : dwSlabSize { slab_pages * Hydra::static_info.sys.dwPageSize },
    dwObjSize { EmbeddedSlabCache::alignedSize(obj_size, obj_align) },
    dwObjPerSlab { dwSlabSize / dwObjSize },
    mSlabList { &mSlabList, &mSlabList, nullptr, nullptr, dwObjPerSlab },
    mSlabInfoCache {
        sizeof(SlabInfo), 8,
        1 + pre_alloc * dwObjPerSlab
            / EmbeddedSlabCache::objectsPerSlab(sizeof(SlabInfo), 8)
    }
{
    _ASSERT_EXPR(obj_size >= sizeof(void*),
        L"RawSlabCache::RawSlabCache(): obj_size must be at least the "
        L"size of a pointer");

    _ASSERT_EXPR(slab_pages != 0,
        L"RawSlabCache::RawSlabCache(): slab_pages must be non-zero");

    _ASSERT_EXPR(dwObjPerSlab != 0,
        L"RawSlabCache::RawSlabCache(): slab_pages isn't enough to store "
        L"a single object");

    _ASSERT_EXPR((obj_align & (obj_align - 1)) == 0,
        L"RawSlabCache::RawSlabCache(): obj_align is not a power of two");

    /* This is a somewhat arbitrary restriction, even more so in this class */
    _ASSERT_EXPR(obj_align <= Hydra::static_info.cpu.l1.LineSize,
        L"RawSlabCache::RawSlabCache(): obj_align must be less than or "
        L"equal to the cache line size to avoid excess waste");

    /* If no pre-allocations were made, then the first call
       to alloc() below will attempt to allocate on the circular
       list origin, which is invalid. To avoid this, the list
       origin's reference count must be initialized to full
       as done above */
   for ( ; pre_alloc > 0; pre_alloc--)
       acquireSlab();
    pFree = mSlabList.pNext;

    InitializeCriticalSection(&lock);
}

Hydra::RawSlabCache::~RawSlabCache()
{
    SlabInfo *ptr = mSlabList.pNext;
    SlabInfo *next;

    /* Don't unlink anything, just release everything.
       Destructors will take care of the rest. */
    while (ptr != &mSlabList)
    {
        next = ptr->pNext;
        VirtualFree(ptr->pvStart, 0, MEM_RELEASE);
        ptr = next;
    }

    DeleteCriticalSection(&lock);
}

void *Hydra::RawSlabCache::alloc() noexcept
{
    void *obj;

    EnterCriticalSection(&lock);
    /* Check that the current slab isn't empty */
    if (pFree->dwRefCount == dwObjPerSlab)
        pFree = pFree->pNext;

    /* Running into the circular list's origin
       means we need a new slab */
    if (pFree == &mSlabList)
    {
        /*
            We don't want to release the lock, and then lock inside
            the critical section of acquireSlab(), since multiple
            threads may all see that a new slab is needed and
            attempt allocation. To allocate one slab at a time,
            all other threads would need to wait anyway.
        */
        obj = acquireSlab();
        if (!obj)
        {
            LeaveCriticalSection(&lock);
            return obj;
        }

        /* A new slab is always placed at the back of the list, so we
           don't ruin ordering by pointing pFree to the new slab */
        pFree = static_cast<SlabInfo*>(obj);
    }

    obj = pFree->pHead;
    pFree->pHead = static_cast<void**>(*pFree->pHead);
    pFree->dwRefCount++;

    LeaveCriticalSection(&lock);

    return obj;
}

void Hydra::RawSlabCache::dealloc(void *obj) noexcept
{
    SlabInfo *slab = mSlabMap[obj];
    void **buf = static_cast<void**>(obj);
    bool wasEmpty = false;

    EnterCriticalSection(&lock);
    if (slab->dwRefCount == dwObjPerSlab)
        wasEmpty = true;

    /* Buffers are placed LIFO, so no explicit tail needed */
    *buf = slab->pHead;
    slab->pHead = buf;
    slab->dwRefCount--;

    /* If the slab is now full, move it to the back of the queue
       to maintain order, unless it is already at the back */
    if (slab->dwRefCount == 0 && slab->pNext != &mSlabList)
    {
        slab->pNext = &mSlabList;
        slab->pPrev = mSlabList.pPrev;
        mSlabList.pPrev->pNext = slab;
        mSlabList.pPrev = slab;
    }

    /* If the slab was empty, it's now a partial slab and should
       be moved after pFree to maintain order, unless pFree
       still points to the slab */
    else if (wasEmpty && pFree != slab)
    {
        slab->pNext = pFree->pNext;
        slab->pPrev = pFree;
        pFree->pNext->pPrev = slab;
        pFree->pNext = slab;
    }

    LeaveCriticalSection(&lock);
}

unsigned Hydra::RawSlabCache::objectsPerSlab(
    unsigned obj_size,
    unsigned obj_align,
    unsigned slab_pages
) noexcept
{
    return (Hydra::static_info.sys.dwPageSize * slab_pages)
            / EmbeddedSlabCache::alignedSize(obj_size, obj_align);
}

Hydra::RawSlabCache::SlabInfo*
Hydra::RawSlabCache::acquireSlab() noexcept
{
    SlabInfo *slab = static_cast<SlabInfo*>(mSlabInfoCache.alloc());

    unsigned char *mem = static_cast<unsigned char*>(
        VirtualAlloc(
            NULL, dwSlabSize,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE
        )
    );

    if(mem)
    {
        slab = reinterpret_cast<SlabInfo*>(
            mem + dwSlabSize - sizeof(SlabInfo)
        );
        // Adjust cache coloring before this
        embedBuffers(slab, mem);

        slab->dwRefCount = 0;
        slab->pvStart = mem;

        /* Place at back of list. mSlabList is always the origin of
           the circular list so that branching isn't required here */
        slab->pNext = &mSlabList;
        slab->pPrev = mSlabList.pPrev;
        mSlabList.pPrev->pNext = slab;
        mSlabList.pPrev = slab;
    }

    return slab;
}

void Hydra::RawSlabCache::embedBuffers(
    SlabInfo *slab,
    unsigned char *slab_start
) noexcept
{
    // Add cache coloring offset to slab_start here
    void **prev = reinterpret_cast<void**>(slab_start);
    slab->pHead = reinterpret_cast<void**>(slab_start);
    mSlabMap[slab_start] = slab;

    for (unsigned i = 1; i < dwObjPerSlab; i++)
    {
        slab_start += dwObjSize;
        *prev = slab_start;
        prev = reinterpret_cast<void**>(slab_start);
        mSlabMap[slab_start] = slab;
    }
    *prev = nullptr;
}

void Hydra::RawSlabCache::releaseFullSlabs() noexcept
{
    SlabInfo *ptr;
    SlabInfo *prev;

    EnterCriticalSection(&lock);

    /* Full slabs are contiguous and at the back of the list */
    ptr = mSlabList.pPrev;

    while (ptr->dwRefCount == 0)
    {
        /* Delete reverse mappings for the slab as well */
        for (void **buf = ptr->pHead; buf != nullptr;
             buf = static_cast<void**>(*buf))
                mSlabMap.erase(buf);

        prev = ptr->pPrev;
        VirtualFree(ptr->pvStart, 0, MEM_RELEASE);
        mSlabInfoCache.dealloc(ptr);
        ptr = prev;
    }

    /* Unlink everything at once */
    ptr->pNext = &mSlabList;
    mSlabList.pPrev = ptr;

    LeaveCriticalSection(&lock);
}
