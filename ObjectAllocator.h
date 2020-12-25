#pragma once

#include <Windows.h>
#include <list>
#include <cstdint>
#include <utility>
#include <functional>
#include <type_traits>
#include <crtdbg.h>

#include "RawSlabCache.h"

namespace Hydra
{
    /*!
     * \struct Depot ObjectAllocator.h
     *
     * This structure is designed to ease managing several depots in an
     * ObjectAllocator so that the magazine size can be dynamically expanded
     * at runtime. Each Depot represents a depot layer as described in
     * <em>Magazines and Vmem: Extending the Slab Allocator to Many CPUs and
     * Arbitrary Resources</em> [[BonwickAdams01](https://www.usenix.org/
     *conference/2001-usenix-annual-technical-conference/
     *magazines-and-vmem-extending-slab-allocator-many)]. In this regard, Depot
     * is an implementation detail of ObjectAllocator.
     */
    struct Depot
    {
        RawSlabCache     *magazines;
        void             **full;
        void             **empty;
        unsigned         mag_size;
        unsigned         ref_count;
        unsigned         slab_pages;
        CRITICAL_SECTION lock;
        typename std::list<Depot*>::iterator
                         list_ref;

        /*!
         * \brief Construct a new depot with it's own magzine allocator.
         *
         * The last entry of a magazine is its full/empty list linkage, so the
         * object size for the slab cache is set to size + 1. As mentioned in
         * the parameter description below, for magazines to be used as
         * intended, the magazine size should be one less than a multiple of a
         * cache line. When selecting slab size, keep in mind that acquiring
         * a new slab results in iterating over all buffers in the slab to
         * construct a free list. This iteration can be held constant by
         * choosing values that satisfy (slab_pages * page_size)
         * / (mag_size + 1) = C, where C is the number of magazines in each
         * slab.
         *
         * \param[in] size Number of entries in each magazine. To mitigate
         *                 false sharing, this should be one less than some
         *                 multiple of the data cache line size to fit the
         *                 depot list linkage in the last entry.
         * \param[in] align Alignment of each magazine array. To mitigate
         *                  false sharing of caches, this should be set
         *                  to the data cache line size (preferably L1?).
         * \param[in] alloc_const A constant used to determine the number of
         *                        pages per slab based on the magazine size.
         *                        If this value is C > 0, then we calculate
         *                        C * (mag_size + 1) * sizeof(void*) rounded up
         *                        to the nearest page and then divided by the
         *                        page size. C == 0 is invalid.
         */
        Depot(
            unsigned size,
            unsigned align,
            unsigned alloc_const);

        /*!
         * \brief Destroy the depot, releasing the magazine allocator.
         *
         * The depot stucture is intentionally unaware of the types to which
         * it stores pointers in its magazines. Destructing/releasing
         * those objects on the depot's full list using the correct allocator
         * is the responsibility of the magazine-based object allocator's
         * implementation.
         */
        ~Depot();

        /*!
         * \brief Allocate a new empty magazine from the depot's slab
         *        allocator.
         *
         * Since the slab allocator is serialized holding depot->lock is
         * optional.
         *
         * \return A pointer to a new empty magazine if the allocation can
         *         be made. Null otherwise.
         */
        void *newEmpty();

        /*!
         * \brief Get an empty magazine from the empty list.
         *
         * Caller must have acquired depot->lock before calling this.
         *
         * \return A pointer to an empty magazine if one is available.
         *         Null if the empty list is empty.
         */
        void *getEmpty();

        /*!
         * \brief Place an empty magazine on the empty list.
         *
         * Caller must have acquired depot->lock before calling this.
         *
         * \param[in] mag A pointer to an empty magazine that was originally
         *                retrieved from this depot.
         */
        void putEmpty(void *mag);

        /*!
         * \brief Get a full magazine from the full list.
         *
         * Caller must have acquired depot->lock before calling this.
         *
         * \return A pointer to an full magazine if one is available.
         *         Null if the full list is empty.
         */
        void *getFull();

        /*!
         * \brief Place a full magazine on the full list.
         *
         * Caller must have acquired depot->lock before calling this.
         *
         * \param[in] mag A pointer to a full magazine that was originally
         *                retrieved from this depot.
         */
        void putFull(void *mag);
    };

    /*!
     * \class ObjectAllocator ObjectAllocator.h
     *
     * ObjectAllocator implements a per-thread cached object allocator,
     * inspired by the improvements to the slab allocator detailed in
     * <em>Magazines and Vmem: Extending the Slab Allocator to Many CPUs and
     * Arbitrary Resources</em> [[BonwickAdams01](https://www.usenix.org/
     *conference/2001-usenix-annual-technical-conference/
     *magazines-and-vmem-extending-slab-allocator-many)]. In this paper, the
     * slab allocator is enhanced with a per-CPU cache of constructed objects
     * with the aim of introducing scalability that is linear in the number of
     * processors on a multicore machine.
     *
     * Although the design mentioned above is based on the number of cores in
     * a machine, this implementation accounts for a given number of threads
     * specified at construction, rather than being truly per-CPU. This does
     * not increase scalability, and does not do much more than remove the
     * need for locking at the magazine (thread cache) layer. Each thread
     * accesses the allocator with a unique index so that its transactions are
     * isolated, with regard to CPU cache lines, from all other threads.
     *
     * The Bonwick, Adams '01 paper describes a method for dynamically
     * tuning the magazine size based on lock contention when accessing the
     * depot. This implementation takes a <em>laissez-faire</em> approach to
     * magazine expansion, allowing multiple depots to be in use simultaneously
     * and changing each thread's cache independently of the others instead of
     * halting all threads to update their caches at the same time. A thread
     * will begin using the latest (largest size) depot only when it
     * experiences contention that meets the threshold passed in the
     * allocator's constructor. If it is the last thread using the depot, all
     * objects cached in the depot will be destructed and released to the
     * slab allocator prior to deleting the depot. If the thread is using the
     * most up to date (largest size) depot, then it will attempt creating a
     * new depot unless the magazine expansion limit has been reached.
     *
     * \tparam T
     * \tparam S
     * \tparam N
     */
    template<typename T, typename S, unsigned N>
    class ObjectAllocator
    {
    public:
        static_assert(std::is_same<S, EmbeddedSlabCache>::value
                      || std::is_same<S, RawSlabCache>::value,
                      "Template parameter S must be one of "
                      "EmbeddedSlabCache or RawSlabCache");

        static_assert((N & (N - 1)) == 0, "Template parameter N, the cache "
            "line size, is not a power of two");

        ObjectAllocator(
            unsigned thread_caches,
            unsigned mag_init,
            unsigned mag_max,
            unsigned mag_alloc_const,
            int64_t contention_thresh,
            const std::function<void(void*)> &obj_init
        );

        /*!
         * \brief Constructor with the additional slab_pages for RawSlabCache.
         *
         * All other parameters are the same as in the constructor without
         * slab_pages. This overload is used for the specilization when
         * RawSlabCache is used, and attempting to use this constructor will
         * fail when using EmbeddedSlabCache.
         */
        ObjectAllocator(
            unsigned thread_caches,
            unsigned mag_init,
            unsigned mag_max,
            unsigned mag_alloc_const,
            int64_t contention_thresh,
            unsigned slab_pages,
            const std::function<void(void*)> &obj_init
        );

        ~ObjectAllocator();

        /*!
         * \brief Allocate an object from the thread's cache, specified by id,
         *        constructing a new object using the optional forwarded
         *        parameters if one isn't readily available.
         *
         * \attention If multiple threads that are otherwise not synchronized
         *            allocate using the same 'id', then this function results
         *            in a race condition.
         */
        template<typename... Args>
        T *alloc(unsigned id, Args&&... a) noexcept;

        /*!
         * \brief Return an object to the thread's cache, specified by id,
         *        destructing the object if an empty slot isn't readily
         *        available.
         *
         * \attention If multiple threads that are otherwise not synchronized
         *            deallocate using the same 'id', then this function
         *            results in a race condition.
         */
        void dealloc(unsigned id, T *obj) noexcept;

    private:
        static constexpr unsigned N_align =
            align_up2(4 * sizeof(void*) + 3 * sizeof(unsigned), N);

        /*!
         * \struct MagazineCache ObjectAllocator.h
         *
         * The MagazineCache is the per-CPU cache described in the original
         * Bonwick, Adams '01 paper, although the ObjectAllocator can be
         * constructed with as many caches as desired. The structure contains
         * the two magazines, a pointer to the depot used to allocate both
         * magazines, and a timestamp to track depot contention. Padding is
         * added to align the structure to the cache line size specified
         * in the template parameter to ObjectAllocator.
         */
        struct MagazineCache
        {
            T          **loaded;
            T          **prev;
            Depot      *depot;
            int64_t    last_qpc;
            unsigned   loaded_cnt;
            unsigned   prev_cnt;
            unsigned   mag_size;
            uint8_t    _pad[ N_align - 4 * sizeof(void*)
                                     - 3 * sizeof(unsigned) ];
        };

        static_assert(sizeof(MagazineCache) == N, "The size of "
            "ObjectAllocator::MagazineCache is not the same as "
            "the specified size of a cache line");

        /*!
         * \brief Fetch new magazines from depot, changing depots if the
         *        contention threshold is reached.
         *
         * This function is called when a magazine cache needs to enter the
         * depot layer. On alloc, this function is responsible for trying to
         * acquire a new full magazine. On dealloc, this function is
         * responsible for trying to acquire an empty magazine. If it succeeds
         * the MagazineCache will be updated, so that the calling function
         * can restart the transaction without additional work.
         *
         * The function implements switching depots based on lock contention
         *
         * \param[in] mc        A pointer to the MagazineCache making the
         *                      request to drop into the depot layer.
         * \param[in] alloc_req True if the invocation is being made as
         *                       part of an object allocation request. False
         *                       if the invocation is part of an object
         *                       deallocation request.
         *
         * \return True if a magazine was available. False if no magazine
         *         was available.
         */
        bool depot_fetch(MagazineCache *mc, bool alloc_req) noexcept;

        /*!
         * \brief Attempts to service an alloc/dealloc request
         *        on a depot that is being switched to. Updates magazine
         *        cache if successful, and otherwise has no effect.
         *
         * After switching to a new depot, there's no guarantee that the
         * the new depot can service the alloc/dealloc request or even
         * leave the magazine cache in a valid state. The latter problem
         * arises when the new depot can't provide two empty magazines.
         * If we updated the magazine cache's entries first, and
         * subsequently observed such a failure with the new depot, then
         * the cache would end up in an irrecoverable state. Specifically,
         * the two magazine pointers would point into an older (possibly
         * deleted) depot, and the magazine sizes would be incorrect.
         *
         * To be able to recover from that double failure, we attempt
         * setting up the magazine cache with the new depot separately.
         * There are three outcomes:
         *   1. If we can set up the new depot <b>and</b> service the request,
         *      we set serviced to true and return true. This implementation
         *      deviates from the original Bonwick, Adams '01 paper in that
         *      both pointers (instead of only prev) are set to empty
         *      magazines for dealloc. This is needed when switching depots
         *      to ensure that the same depot is in use for both pointers.
         *   2. If we can set up the new depot <b>but</b> we cannot service the
         *      request, we set serviced to false and return true. This occurs
         *      only when we cannot presently get a full magazine for alloc,
         *      but the new depot is otherwise usable.
         *   3. If we are unable to set up the new depot, then we <b>do not
         *      change</b> serviced and return false. This is the recovery
         *      mechanism from a double failure. The attempt at switching
         *      depots, while expensive, ends up having no effect.
         *
         * If this function returns true, both magazines are returned to the
         * correct list in the old depot, both pointers are set to magazines
         * in the new depot, and the magazine sizes are updated appropriately.
         * No other members of mc are touched.
         *
         * \param[in] mc        A pointer to the magazine cache being updated.
         * \param[in] alloc_req True if the invocation is being made as
         *                      part of an object allocation request.
         *                      False if the invocation is part of an object
         *                      deallocation request.
         * \param[in] new_depot A pointer to the depot being swtiched to.
         * \param[in] serviced A reference to the serviced variable in
         *                     ObjectAllocator::depot_fetch.
         *
         * \return True if the new depot can be used. False if not.
         */
        bool try_new_depot(
        	MagazineCache *mc,
        	bool alloc_req,
        	Depot *new_depot,
        	bool &serviced
        ) noexcept;

        /*!
         * \brief Prepares a depot for destruction by destroying the objects
         *        cached in the full list and returning the memory to the
         *        allocator's backing store.
         *
         * \attention The emptied magazines are not put on the empty list, as
         *            this call is intended to be called right before the
         *            destruction of a depot. Otherwise, calling this results
         *            in leaking magazine allocations. Make sure all full
         *            magazines have been returned to the depot.
         *
         * \param[in] depot The Depot to be cleared out.
         */
        void discard_full(Depot *depot);

        void init_magazine_cache(
            MagazineCache *mc,
            Depot *init,
            const std::function<void(void*)> &obj_init
        );

        void deinit_magazine_cache(MagazineCache *mc) noexcept;

        MagazineCache *magazine_cache;

        unsigned num_caches;
        unsigned top_magsize;
        unsigned max_magsize;
        unsigned mag_const;
        int64_t threshold_qpc;

        CRITICAL_SECTION list_lock;
        std::list<Depot*> depot_list;

        S backing_store;
    };

//========================================================================
// ObjectAllocator Implementation
//========================================================================
    template<typename T, typename S, unsigned N>
    ObjectAllocator<T, S, N>::ObjectAllocator(
        unsigned thread_caches,
        unsigned mag_init,
        unsigned mag_max,
        unsigned mag_alloc_const,
        int64_t contention_thresh,
        const std::function<void(void*)> &obj_init
    ) : num_caches{ thread_caches },
        top_magsize{ mag_init * N / sizeof(void*) - 1 },
        max_magsize{ mag_max * N / sizeof(void*) },
        mag_const{ mag_alloc_const },
        threshold_qpc{ contention_thresh },
        backing_store{ sizeof(T), alignof(T), 1 }
    {
        _ASSERT_EXPR(mag_max >= mag_init,
            L"ObjectAllocator::ObjectAllocator(): The intial magazine "
            L"size is greater than the maximum magazine size");

        _ASSERT_EXPR(mag_alloc_const != 0,
            L"ObjectAllocator::ObjectAllocator(): mag_alloc_const "
            L"must be non-zero");

        _ASSERT_EXPR(contention_thresh > 0,
            L"ObjectAllocator::ObjectAllocator(): The depot contention "
            L"threshold must be greater than zero");

        magazine_cache = static_cast<MagazineCache*>(
            _aligned_malloc(sizeof(MagazineCache) * thread_caches, N)
        );

        if (!magazine_cache)
        {
            throw std::runtime_error{
                "ObjectAllocator::ObjectAllocator(): Unable to allocate "
                "the requested number of magzine caches"
            };
        }

        /* Set up first depot and init empty magazines */
        Depot *init = new Depot{ top_magsize, N, mag_const };

        for (unsigned i = 0; i < thread_caches; i++)
        {
            init_magazine_cache(magazine_cache + i, init, obj_init);
        }

        depot_list.push_front(init);

        InitializeCriticalSection(&list_lock);
    }

    /*
     * Specialization for RawSlabCache, takes the slab size (in pages) as an
     * argument to the constructor.
     */
    template<typename T, typename S, unsigned N>
    ObjectAllocator<T, S, N>::ObjectAllocator(
        unsigned thread_caches,
        unsigned mag_init,
        unsigned mag_max,
        unsigned mag_alloc_const,
        int64_t contention_thresh,
        unsigned slab_pages,
        const std::function<void(void*)> &obj_init
    ) : num_caches{ thread_caches },
        top_magsize{ mag_init * N / sizeof(void*) - 1 },
        max_magsize{ mag_max * N / sizeof(void*) },
        mag_const{ mag_alloc_const },
        threshold_qpc{ contention_thresh },
        backing_store{ sizeof(T), alignof(T), slab_pages, 1 }
    {
        static_assert(std::is_same<S, RawSlabCache>::value,
                      "Template parameter S must be RawSlabCache "
                      "to use the constructor with slab_pages");

        _ASSERT_EXPR(mag_max >= mag_init,
            L"ObjectAllocator::ObjectAllocator(): The intial magazine "
            L"size is greater than the maximum magazine size");

        _ASSERT_EXPR(mag_alloc_const != 0,
            L"ObjectAllocator::ObjectAllocator(): mag_alloc_const "
            L"must be non-zero");

        _ASSERT_EXPR(contention_thresh > 0,
            L"ObjectAllocator::ObjectAllocator(): The depot contention "
            L"threshold must be greater than zero");

        magazine_cache = static_cast<MagazineCache*>(
            _aligned_malloc(sizeof(MagazineCache) * thread_caches, N)
        );

        if (!magazine_cache)
        {
            throw std::runtime_error{
                "Unable to allocate the requested number of magzine caches"
            };
        }

        /* Set up first depot and init empty magazines */
        Depot *init = new Depot{ top_magsize, N, mag_const };

        for (unsigned i = 0; i < thread_caches; i++)
        {
            init_magazine_cache(magazine_cache + i, init, obj_init);
        }

        depot_list.push_front(init);

        InitializeCriticalSection(&list_lock);
    }

    template<typename T, typename S, unsigned N>
    ObjectAllocator<T, S, N>::~ObjectAllocator()
    {
        /* Release all constructed objects before doing the same for
           each depot. */
        for (unsigned i = 0; i < num_caches; i++)
        {
            deinit_magazine_cache(magazine_cache + i);
        }

        while (!depot_list.empty())
        {
            discard_full(depot_list.front());
            delete depot_list.front();
            depot_list.pop_front();
        }

        DeleteCriticalSection(&list_lock);

        _aligned_free(magazine_cache);
    }

    template<typename T, typename S, unsigned N>
    template<typename... Args>
    T *ObjectAllocator<T, S, N>::alloc(
        unsigned id,
        Args&&... a
    ) noexcept
    {
        MagazineCache *mc = magazine_cache + id;

        while (true)
        {
            if (mc->loaded_cnt > 0)
            {
                return mc->loaded[--mc->loaded_cnt];
            }
            else if (mc->prev_cnt == mc->mag_size)
            {
                T **t = mc->loaded;
                mc->loaded = mc->prev;
                mc->prev = t;

                unsigned tmp = mc->loaded_cnt;
                mc->loaded_cnt = mc->prev_cnt;
                mc->prev_cnt = tmp;
            }
            else
            {
                /* If no full magazines are available on alloc, allocate
                   directly from the slab, in-place construct the object, and
                   wait for deallocations to populate the empty magazine list,
                   as is described in the paper. */
                if (!depot_fetch(mc, true))
                {
                    T *obj = nullptr;
                    void *mem = backing_store.alloc();
                    if (mem)
                        obj = new(mem) T{std::forward<Args>(a)...};
                    return obj;
                }

                /* else try again from the top */
            }
        }
    }

    template<typename T, typename S, unsigned N>
    void ObjectAllocator<T, S, N>::dealloc(
        unsigned id,
        T *obj
    ) noexcept
    {
        MagazineCache *mc = magazine_cache + id;

        while (true)
        {
            if (mc->loaded_cnt < mc->mag_size)
            {
                mc->loaded[mc->loaded_cnt++] = obj;
                return;
            }
            else if (mc->prev_cnt == 0)
            {
                T **t = mc->loaded;
                mc->loaded = mc->prev;
                mc->prev = t;

                unsigned tmp = mc->loaded_cnt;
                mc->loaded_cnt = mc->prev_cnt;
                mc->prev_cnt = tmp;
            }
            else
            {
                if (!depot_fetch(mc, false))
                {
                    obj->~T();
                    backing_store.dealloc(obj);
                    return;
                }

                /* else try again from the top */
            }
        }
    }

    template<typename T, typename S, unsigned N>
    bool ObjectAllocator<T, S, N>::depot_fetch(
        MagazineCache *mc,
        bool alloc_req
    ) noexcept
    {
        int64_t current_qpc = -1;
        bool serviced = false;
        bool switch_ok = false;

        if (!TryEnterCriticalSection(&mc->depot->lock))
        {
            /* Add one because mag_size is always a one less than a multiple
               of a cache line to accommodate depot linkage */
            if (mc->mag_size + 1 < max_magsize)
            {
                QueryPerformanceCounter(
                    reinterpret_cast<LARGE_INTEGER*>(&current_qpc)
                );
            }

            EnterCriticalSection(&mc->depot->lock);
        }

        /* Lock contention is beyond the specified threshold,
           switch depots */
        if (current_qpc != -1
            && current_qpc - mc->last_qpc <= threshold_qpc)
        {
            /* We preserve the refcount until switching to the new depot
               is successful. Otherwise, we could fail with some other
               thread having deleted the depot in the meantime. */
            unsigned ref_count = mc->depot->ref_count;

            LeaveCriticalSection(&mc->depot->lock);

            Depot *top_depot = nullptr;
            bool depot_ok = true;
            unsigned depot_pages;

            if (ref_count == 1)
            {
                /* Although it would be nice to be outside the lock while
                   allocating a new depot, if someone is destroying another
                   depot and sees that top_magsize as set here is already
                   larger than their mag_size, then they will attempt to grab
                   the new depot which is yet to be constructed. They will
                   instead get this one which is about to be destroyed, so
                   the addition of the new depot has to be atomic anyway. */
                EnterCriticalSection(&list_lock);
                if (mc->depot->mag_size == top_magsize)
                {
                    top_magsize += N;

                    try
                    {
                        top_depot = new Depot{ top_magsize, N, mag_const };
                    }
                    catch(const std::exception &e)
                    {
                        /* Clients should not use exception handling for
                           allocations. Failure is indicated by returning
                           NULL. top_depot will still be nullptr, so we will
                           continue down the failure path below */
                    }

                    /* If we couldn't get a new depot, roll back
                       and continue using the current one */
                    if (top_depot)
                    {
                        depot_list.push_front(top_depot);
                        top_depot->list_ref = depot_list.begin();
                        top_depot->ref_count = 1;
                        depot_list.erase(mc->depot->list_ref);
                    }
                    else
                    {
                        top_magsize -= N;
                        depot_ok = false;
                    }
                }
                LeaveCriticalSection(&list_lock);
            }

            /*  */
            if (depot_ok)
            {
                /* Get the largest sized depot, unless we set it up a
                   moment ago. */
                if (!top_depot)
                {
                    EnterCriticalSection(&list_lock);
                    top_depot = depot_list.front();
                    LeaveCriticalSection(&list_lock);
                }

                /* Attempt using the new depot to service the request. mc's
                   magazine pointers still point into the old depot, and we
                   want to keep them there until we know that, at the very
                   least, we can get two empty magazines from the new depot.
                   If this call succeeds, then the magazine pointers and
                   free counts have already been switched. */
                switch_ok = try_new_depot(
                    mc, alloc_req, top_depot, serviced
                );

                if (switch_ok)
                {
                    if (ref_count == 1)
                    {
                        /* Since we will be destroying all magazine arrays,
                           deleting the magazine allocator suffices as long as
                           all objects on the full list have been returned to
                           their original slab allocator. */
                        discard_full(mc->depot);

                        delete mc->depot;
                    }
                    else
                    {
                        EnterCriticalSection(&mc->depot->lock);
                        mc->depot->ref_count--;
                        LeaveCriticalSection(&mc->depot->lock);
                    }

                    mc->depot = top_depot;
                    mc->mag_size = top_depot->mag_size;
                }
            }

            mc->last_qpc = current_qpc;

            EnterCriticalSection(&mc->depot->lock);
        }

        /* If both booleans are false, then the lock was not contended or
           switching depots was unsuccessful */
        if ( !(switch_ok || serviced) )
        {
            /* The requester will allocate or deallocate directly on the slab
               allocator as needed based on the returned value of serviced. */
            if (alloc_req)
            {
                void *new_mag = mc->depot->getFull();

                if (new_mag)
                {
                    mc->depot->putEmpty(mc->prev);

                    mc->prev = mc->loaded;
                    mc->prev_cnt = mc->loaded_cnt;
                    mc->loaded = static_cast<T**>(new_mag);
                    mc->loaded_cnt = mc->mag_size;

                    serviced = true;
                }
            }
            else
            {
                void *new_mag = mc->depot->getEmpty();

                if (!new_mag) new_mag = mc->depot->newEmpty();

                if (new_mag)
                {
                    mc->depot->putFull(mc->prev);

                    mc->prev = mc->loaded;
                    mc->prev_cnt = mc->loaded_cnt;
                    mc->loaded = static_cast<T**>(new_mag);
                    mc->loaded_cnt = 0;

                    serviced = true;
                }
            }
        }

        LeaveCriticalSection(&mc->depot->lock);

        return serviced;
    }

    template<typename T, typename S, unsigned N>
    bool ObjectAllocator<T, S, N>::try_new_depot(
    	MagazineCache *mc,
    	bool alloc_req,
    	Depot *new_depot,
    	bool &serviced
    ) noexcept
    {
    	void *new_loaded = nullptr;
    	void *new_prev = nullptr;

    	/* Step 1: Attempt getting an empty magazine from the
    	   new depot */
    	EnterCriticalSection(&new_depot->lock);
    	new_prev = new_depot->getEmpty();

    	if (!new_prev) new_prev = new_depot->newEmpty();

    	/* Since the empty magazine is needed both on allocation
    	   and deallocation, fail if not acquired */
    	if (!new_prev)
    	{
    		LeaveCriticalSection(&new_depot->lock);
    		return false;
    	}

    	/* Step 2: If servicing an allocation request, try
    	   getting a full magazine first from the new depot */
    	if (alloc_req)
    	{
    		new_loaded = new_depot->getFull();

    		if (new_loaded)
    		{
                LeaveCriticalSection(&new_depot->lock);

    			mc->depot->putEmpty(mc->loaded);
    			mc->depot->putEmpty(mc->prev);

    			/* Accessing new_depot->mag_size is not a race
    			   condition as it is set once on construction */
    			mc->loaded = static_cast<T**>(new_loaded);
    			mc->loaded_cnt = new_depot->mag_size;
    			mc->prev = static_cast<T**>(new_prev);
    			mc->prev_cnt = 0;

    			serviced = true;
    			return true;
    		}
    	}

    	/* Step 3: If servicing a deallocation requeust, or getting a full
           magazine on allocation failed, try getting another empty magazine
    	   from the new depot */
    	new_loaded = new_depot->getEmpty();
    	if (!new_loaded) new_loaded = new_depot->newEmpty();

    	/* Entering this conditional means our magazine cache can't be set
    	   up with the new depot. Release the empty magazine from up top
    	   and indicate failure. */
    	if (!new_loaded)
    	{
    		new_depot->putEmpty(new_prev);
    		LeaveCriticalSection(&new_depot->lock);
    		return false;
    	}

    	LeaveCriticalSection(&new_depot->lock);

    	/* At this point, both pointers are set to fresh empty magazines in
           the new depot. On deallocation, the request has successfully been
           serviced through the new depot. */
    	if (alloc_req)
    	{
    		mc->depot->putEmpty(mc->loaded);
    		mc->depot->putEmpty(mc->prev);
    	}
    	else
    	{
    		mc->depot->putFull(mc->loaded);
    		mc->depot->putFull(mc->prev);
    		serviced = true;
    	}

    	mc->loaded = static_cast<T**>(new_loaded);
    	mc->loaded_cnt = 0;
    	mc->prev = static_cast<T**>(new_prev);
    	mc->prev_cnt = 0;

    	return true;
    }

    template<typename T, typename S, unsigned N>
    void ObjectAllocator<T, S, N>::discard_full(Depot *depot)
    {
        T **discard = static_cast<T**>(depot->getFull());
        while (discard)
        {
            for (T **top = discard + depot->mag_size;
                 discard < top; discard++)
            {
                (*discard)->~T();
                backing_store.dealloc(*discard);
            }

            discard = static_cast<T**>(depot->getFull());
        }
    }

    template<typename T, typename S, unsigned N>
    void ObjectAllocator<T, S, N>::init_magazine_cache(
        MagazineCache *mc,
        Depot *init,
        const std::function<void(void*)> &obj_init
    )
    {
        mc->loaded = static_cast<T**>(init->newEmpty());
        mc->prev = static_cast<T**>(init->newEmpty());

        if (mc->loaded == nullptr || mc->prev == nullptr)
        {
            delete init;
            throw std::runtime_error{
                "ObjectAllocator::init_magazine_cache(): Unable to initialize "
                "magazine caches"
            };
        }

        /* If an allocation from the backing store fails, we may end up
           with a single partially filled magazine. However, the magazine
           cache's state is valid since
             (1) if allocation fails when initializing loaded, prev will
                 be empty, and
             (2) if allocation fails when initializing prev, loaded will
                 be full. */
        for (mc->loaded_cnt = 0;
             mc->loaded_cnt < top_magsize;
             mc->loaded_cnt++)
        {
            void *mem = backing_store.alloc();
            if (mem == nullptr)
                break;

            obj_init(mem);
            mc->loaded[mc->loaded_cnt] = static_cast<T*>(mem);
        }

        mc->prev_cnt = 0;
        if (mc->loaded_cnt == top_magsize)
        {
            for ( ; mc->prev_cnt < top_magsize; mc->prev_cnt++)
            {
                void *mem = backing_store.alloc();
                if (mem == nullptr)
                    break;

                obj_init(mem);
                mc->prev[mc->prev_cnt] = static_cast<T*>(mem);
            }
        }

        mc->depot = init;
        mc->mag_size = top_magsize;
        QueryPerformanceCounter(
            reinterpret_cast<LARGE_INTEGER*>(&mc->last_qpc)
        );
    }

    template<typename T, typename S, unsigned N>
    void ObjectAllocator<T, S, N>::deinit_magazine_cache(
        MagazineCache *mc
    ) noexcept
    {
        while (mc->loaded_cnt > 0)
        {
            backing_store.dealloc(mc->loaded[--mc->loaded_cnt]);
        }

        while (mc->prev_cnt > 0)
        {
            backing_store.dealloc(mc->prev[--mc->prev_cnt]);
        }
    }

//========================================================================
// Depot Implementation
//========================================================================
    inline void *Depot::newEmpty()
    {
        return magazines->alloc();
    }

    inline void *Depot::getEmpty()
    {
        void *ptr = nullptr;
        if (empty != nullptr)
        {
            ptr = empty - mag_size;
            empty = static_cast<void**>(*empty);
        }
        return ptr;
    }

    inline void Depot::putEmpty(void *mag)
    {
        void **ptr = static_cast<void**>(mag);
        ptr[mag_size] = empty;
        empty = ptr + mag_size;
    }

    inline void *Depot::getFull()
    {
        void *ptr = nullptr;
        if (full != nullptr)
        {
            ptr = full - mag_size;
            full = static_cast<void**>(*full);
        }
        return ptr;
    }

    inline void Depot::putFull(void *mag)
    {
        void **ptr = static_cast<void**>(mag);
        ptr[mag_size] = full;
        full = ptr + mag_size;
    }
}
