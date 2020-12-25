#include "util.h"
#include "EmbeddedSlabCache.h"
#include <crtdbg.h>

//========================================================================
// EmbeddedSlabCache
//========================================================================
Hydra::EmbeddedSlabCache::EmbeddedSlabCache(
    unsigned obj_size,
    unsigned obj_align,
    unsigned pre_alloc
) : dwSlabSize{ Hydra::static_info.sys.dwPageSize },
    dwObjSize{ alignedSize(obj_size, obj_align) },
    dwObjPerSlab{ objectsPerSlab(obj_size, obj_align) },
    mSlabList{ dwObjPerSlab, &mSlabList, &mSlabList, nullptr }
{
    _ASSERT_EXPR(obj_size >= sizeof(void*),
        L"EmbeddedSlabCache::EmbeddedSlabCache(): obj_size must be at "
        L"least the size of a pointer");

    /* This restriction comes from the Bonwick '94 paper */
    _ASSERT_EXPR(obj_size <= (dwSlabSize / 8),
        L"EmbeddedSlabCache::EmbeddedSlabCache(): obj_size must be less "
        L"than an eighth of page size to use this allocator");

    _ASSERT_EXPR((obj_align & (obj_align - 1)) == 0,
        L"EmbeddedSlabCache::EmbeddedSlabCache(): obj_align is not "
        L"a power of two");

    /* This is a somewhat arbitrary restriction */
    _ASSERT_EXPR(obj_align <= Hydra::static_info.cpu.l1.LineSize,
        L"EmbeddedSlabCache::EmbeddedSlabCache(): obj_align must be less "
        L"than or equal to the cache line size to avoid excess waste");

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

Hydra::EmbeddedSlabCache::~EmbeddedSlabCache()
{
    SlabInfo *ptr = mSlabList.pNext;
    SlabInfo *next;
    std::size_t addr;

    /* Don't unlink anything, just release everything */
    while (ptr != &mSlabList)
    {
        next = ptr->pNext;
        addr = reinterpret_cast<std::size_t>(ptr);
        VirtualFree(reinterpret_cast<void*>(addr &
                ~static_cast<std::size_t>(dwSlabSize - 1)),
            0, MEM_RELEASE
        );
        ptr = next;
    }

    DeleteCriticalSection(&lock);
}

void *Hydra::EmbeddedSlabCache::alloc() noexcept
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

void Hydra::EmbeddedSlabCache::dealloc(void *obj) noexcept
{
    /* Since the slab size is the page size, we get the start
       address of the slab by masking off enough lower bits.
       The slab struct is stored in the last sizeof(SlabInfo)
       bytes of the slab. */
    std::size_t addr = reinterpret_cast<std::size_t>(obj);
    addr &= ~static_cast<std::size_t>(dwSlabSize - 1);
    addr += dwSlabSize - sizeof(SlabInfo);

    SlabInfo *slab = reinterpret_cast<SlabInfo*>(addr);
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

unsigned Hydra::EmbeddedSlabCache::objectsPerSlab(
    unsigned obj_size,
    unsigned obj_align
) noexcept
{
    return (Hydra::static_info.sys.dwPageSize - sizeof(SlabInfo))
            / alignedSize(obj_size, obj_align);
}

unsigned Hydra::EmbeddedSlabCache::alignedSize(
    unsigned obj_size,
    unsigned obj_align
) noexcept
{
    if (obj_align < sizeof(void*))
        obj_align = sizeof(void*);
    return align_up2(obj_size, obj_align);
}

Hydra::EmbeddedSlabCache::SlabInfo*
Hydra::EmbeddedSlabCache::acquireSlab() noexcept
{
    SlabInfo *slab = nullptr;

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

        /* Place at back of list. mSlabList is always the origin of
           the circular list so that branching isn't required here */
        slab->pNext = &mSlabList;
        slab->pPrev = mSlabList.pPrev;
        mSlabList.pPrev->pNext = slab;
        mSlabList.pPrev = slab;
    }

    return slab;
}

void Hydra::EmbeddedSlabCache::embedBuffers(
    SlabInfo *slab,
    unsigned char *slab_start
) noexcept
{
    // Add cache coloring offset to slab_start here
    void **prev = reinterpret_cast<void**>(slab_start);
    slab->pHead = reinterpret_cast<void**>(slab_start);

    for (unsigned i = 1; i < dwObjPerSlab; i++)
    {
        slab_start += dwObjSize;
        *prev = slab_start;
        prev = reinterpret_cast<void**>(slab_start);
    }
    *prev = nullptr;
}

void Hydra::EmbeddedSlabCache::releaseFullSlabs() noexcept
{
    SlabInfo *ptr;
    SlabInfo *prev;
    std::size_t addr;

    EnterCriticalSection(&lock);

    /* Full slabs are contiguous and at the back of the list */
    ptr = mSlabList.pPrev;

    while (ptr->dwRefCount == 0)
    {
        prev = ptr->pPrev;
        addr = reinterpret_cast<std::size_t>(ptr);
        VirtualFree(reinterpret_cast<void*>(addr &
                ~static_cast<std::size_t>(dwSlabSize - 1)),
            0, MEM_RELEASE
        );
        ptr = prev;
    }

    /* Unlink everything at once */
    ptr->pNext = &mSlabList;
    mSlabList.pPrev = ptr;

    LeaveCriticalSection(&lock);
}
