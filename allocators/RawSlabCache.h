#pragma once

#include <Windows.h>
#include <unordered_map>
#include "EmbeddedSlabCache.h"

namespace Hydra
{
    /*!
     * \class RawSlabCache EmbeddedSlabCache.h
     *
     * The RawSlabCache implements a single object cache for a
     * given buffer size, inspired by the design of the slab allocator
     * detailed in <em>The Slab Allocator: An Object-Caching Kernel Memory
     * Allocator</em> [[Bonwick94](https://www.usenix.org/conference/\
     *usenix-summer-1994-technical-conference/\
     *slab-allocator-object-caching-kernel)].
     *
     * Unlike EmbeddedSlabCache, a RawSlabCache isn't self sufficient.
     * Each instance contains both an EmbeddedSlabCache as well as a
     * std::unoredered_map that uses the STL's default allocator and hence
     * the runtime's underlying heap. Consequently, the RawSlabCache has a
     * larger memory footprint, worse cache characteristics, and larger
     * allocation/deallocation times because each transaction requires an
     * insertion or lookup into a hash table mapping buffers to their slabs.
     * In return, slabs may be arbitrary integer multiples of the page size
     * and the entire page is available to allocate.
     *
     * The RawSlabCache follows the "Slab Layout for Large Objects" given
     * in section 3.2.3 of the paper with modifications. Specifically, no
     * separate bufctl structure is used, and freelist linkage for buffers
     * are contained in the buffer itself, as in the EmbeddedSlabCache.
     * However, recordkeeping structures are stored in separate designated
     * buffers. Necessarily, there is a hash table entry for each cached
     * buffer, and so the hash table scales linearly with the number of
     * objects in all slabs. This class is designed for two purposes:
     *   1. For maintaining a pool of larger buffers, such as those needed
     *      during bulk asynchronous IO. For balanced allocations and
     *      deallocations, and a reasonable number of objects per slab,
     *      the transactions should be minimally invasive, as is the
     *      intent of the slab allocator's design.
     *   2. For use in the "magazine" based CPU cache allocator described
     *      in the later paper by Bonwick and Adams. In accordance with
     *      this point, the allocator does not apply constructors, instead
     *      managing raw buffers.
     */
    class RawSlabCache
    {
    public:
        /*!
         * \brief Create a new object cache for a single buffer size.
         *
         * The constructor allows for pre-allocation of slabs. If any of
         * of the pre-allocations fail, then the number of allocated slabs
         * will be less than pre_alloc. Pre-allocations will fail silently.
         *
         * \param[in] obj_size The size in bytes of each object in this cache.
         * \param[in] obj_align The alignmnet of each object in this cache.
         *                      Must be a power of two no larger than the
         *                      L1 cache size.
         * \param[in] slab_pages The number of pages in each slab.
         * \param[in] pre_alloc The number of slabs to pre-allocate.
         */
        RawSlabCache(
            unsigned obj_size,
            unsigned obj_align,
            unsigned slab_pages,
            unsigned pre_alloc);

        /*!
         * \brief Releases all slabs to the virtual page allocator.
         *
         * The destructor is not thread safe. Make sure all allocations
         * from the cache have been released, or will no longer be used,
         * before destruction.
         */
        ~RawSlabCache();

        /*!
         * \brief Get a new buffer from the cache.
         *
         * \return The start address to a region of the cache's buffer
         *         size on success. NULL otherwise.
         */
        void *alloc() noexcept;

        /*!
         * \brief Return a buffer to the cache.
         *
         * \param[in] obj A pointer to the start of a buffer that was
         *                returned from EmbeddedSlabCache::alloc().
         */
        void dealloc(void *obj) noexcept;

        /*!
         * \static
         * \brief Calculates the number of objects of obj_size with alignment
         *        obj_align that can be cached into a slab of the given size.
         *
         * \param[in] obj_size  The size of each object.
         * \param[in] obj_align The requested alignment of each object.
         *                      Must be a power of two.
        * \param[in] slab_pages The number of pages in each slab.
         *
         * \return The number of aligned objects that one slab will
         *         be able to store.
         */
        static unsigned objectsPerSlab(
            unsigned obj_size,
            unsigned obj_align,
            unsigned slab_pages
        ) noexcept;

    private:
        /*!
         * \struct SlabInfo EmbeddedSlabCache.h
         *
         * A slab record is associated with each slab and is stored separately
         * in RawSlabCache::mSlabInfoCache. It contains the slab's linkage
         * on the doubly-linked circular list of slabs in the cache as well
         * as all information needed to make allocations from the slab.
         */
        struct SlabInfo
        {
            SlabInfo *pNext; ///< Linkage to the next slab
            SlabInfo *pPrev; ///< Linkage to the previous slab
            void     **pHead; ///< Top of the slab's free list
            void     *pvStart; ///< Pointer to the start of the slab's memory
            unsigned dwRefCount; ///< Number of allocations on slab
        };

        /*!
         * \brief Requests a new slab from the virtual page allocator.
         *
         * <b>Must be called under lock, except during construction.</b>
         *
         * \return A pointer to the new SlabInfo if the slab
         *         was successfully created. NULL otherwise.
         */
        SlabInfo *acquireSlab() noexcept;

        /*!
         * \brief Stamps buffers of dwObjSize into the new slab
         *        and initializes the slab's freelist.
         *
         * The original Bonwick '94 paper places the freelist linkage
         * "at the end of the buffer... to facilitate debugging."
         * However, this requires additional computation for each
         * object allocation or deallocation, and the debugging features
         * aren't part of this implementation. For simplicity, the freelist
         * linkage will be placed at the beginning of the buffer.
         *
         * \param[in] slab       A pointer to the new SlabInfo.
         * \param[in] slab_start A pointer to the first byte of the new slab.
         */
        void embedBuffers(
            SlabInfo *slab,
            unsigned char *slab_start
        ) noexcept;

        /*!
         * \brief Returns all full slabs to the virtual page allocator.
         *
         * The original Bonwick '94 paper mentions reserving a "15-second
         * working set of recently-used slabs to prevent thrashing." However,
         * (a) this implementation isn't nearly as sophisticated, and (b) if
         * wanted, the client application can determine when unused
         * resources can be released in bulk.
         */
        void releaseFullSlabs() noexcept;

        unsigned dwSlabSize;
        unsigned dwObjSize;
        unsigned dwObjPerSlab;
        SlabInfo mSlabList;
        SlabInfo *pFree;
        CRITICAL_SECTION lock;

        EmbeddedSlabCache mSlabInfoCache;
        std::unordered_map<void*, SlabInfo*> mSlabMap;
    };
}
