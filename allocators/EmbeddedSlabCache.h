#pragma once

#include <Windows.h>

namespace Hydra
{
    /*!
     * \class EmbeddedSlabCache EmbeddedSlabCache.h
     *
     * The EmbeddedSlabCache implements a single object cache for a
     * given buffer size, inspired by the design of the slab allocator
     * detailed in <em>The Slab Allocator: An Object-Caching Kernel Memory
     * Allocator</em> [[Bonwick94](https://www.usenix.org/conference/
     *usenix-summer-1994-technical-conference/
     *slab-allocator-object-caching-kernel)]. Embedded in this context
     * refers both to the fact that metadata structures are embedded
     * in the slab memory and that the class is embedded as a member in
     * the cache implementation for larger objects, as mentioned below.
     *
     * The EmbeddedSlabCache follows the "Slab Layout for Small Objects"
     * given in section 3.2.2 of the paper. Specifically, objects must
     * be less than an eighth of a page size, and all recordkeeping structures
     * are stored in the the slab itself. This class is designed for three
     * purposes:
     *   1. An self-contained object cache for small sizes that benefits from
     *      allocations touching data mostly on the same page when transitions
     *      between slabs are not required.
     *   2. An object cache for metadata structures that must be stored
     *      separately for larger objects as described in section 3.2.3
     *      of the paper. See RawSlabCache.
     *   3. For use in the "magazine" based CPU cache allocator described
     *      in the later paper by Bonwick and Adams. In accordance with
     *      this point, the allocator does not apply constructors, instead
     *      managing raw buffers.
     */
    class EmbeddedSlabCache
    {
    public:
        /*!
         * \brief Create a new object cache for a single buffer size.
         *
         * The constructor allows for pre-allocation of slabs. If any of
         * of the pre-allocations fail, then the number of allocated slabs
         * will be less than pre_alloc. Pre-allocations will fail silently.
         *
         * \param[in] obj_size  The size in bytes of each object in this cache.
         * \param[in] obj_align The alignmnet of each object in this cache.
         *                      Must be a power of two no larger than the
         *                      L1 cache size.
         * \param[in] pre_alloc The number of slabs to pre-allocate.
         */
        EmbeddedSlabCache(
            unsigned obj_size,
            unsigned obj_align,
            unsigned pre_alloc);

        /*!
         * \brief Releases all slabs to the virtual page allocator.
         *
         * The destructor is not thread safe. Make sure all allocations
         * from the cache have been released, or will no longer be used,
         * before destruction.
         */
        ~EmbeddedSlabCache();

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
         * \param[in] A pointer to the start of a buffer that was
         *            returned from EmbeddedSlabCache::alloc().
         */
        void dealloc(void *obj) noexcept;

        /*!
         * \static
         * \brief Calculates the number of objects of obj_size with alignment
         *        obj_align that can be cached in a slab of one page.
         *
         * \param[in] obj_size  The size of each object.
         * \param[in] obj_align The requested alignment of each object.
         *                      Must be a power of two.
         *
         * \return The number of aligned objects that one slab will
         *         be able to store.
         */
        static unsigned objectsPerSlab(
            unsigned obj_size,
            unsigned obj_align
        ) noexcept;

        /*!
         * \static
         * \brief Returns obj_size rounded up to the nearest multiple of
         *        obj_align, or the minimum alignment (size of a
         *        pointer), whichever is less.
         *
         * \param[in] obj_size  The size of an object.
         * \param[in] obj_align The requested alignment of each object.
         *                      Must be a power of two.
         *
         * \return The minumum buffer size that will be aligned for the
         *         requested object size.
         */
        static unsigned alignedSize(
            unsigned obj_size,
            unsigned obj_align
        ) noexcept;

    private:
        /*!
         * \struct SlabInfo EmbeddedSlabCache.h
         *
         * A slab record is associated with each slab and is stored at the
         * end of the slab's memory. It contains the slab's linkage on the
         * doubly-linked circular list of slabs in the cache as well as
         * all information needed to make allocations from the slab.
         */
        struct SlabInfo
        {
            unsigned dwRefCount; ///< Number of allocations on slab
            SlabInfo *pNext; ///< Linkage to the next slab
            SlabInfo *pPrev; ///< Linkage to the previous slab
            void     **pHead; ///< Top of the slab's free list
        };

        /*!
         * \brief Requests a new slab from the virtual page allocator.
         *
         * <b>Must be called under lock, except during construction.<\b>
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
    };
}
