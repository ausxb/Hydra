#include <iostream>
#include <random>
#include <algorithm>
#include "../util.h"
#include "ObjectAllocator.h"

struct Data {
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t d;

    /* For forwarding rvalues via ObjectAllocator::alloc below
    Data(uint64_t &&_a, uint64_t &&_b, uint64_t &&_c, uint64_t &&_d)
        : a{_a}, b{_b}, c{_c}, d{_d}
    {

    }*/

    /* So we can tell when an object has been destructed */
    ~Data()
    {
        a = 8;
        b = 7;
        c = 6;
        d = 5;
    }
};

void test_Embedded_freelist_linking()
{
    Hydra::EmbeddedSlabCache slab_cache{ sizeof(Data), 8, 0 };
    std::vector<void*> addrs;
    std::random_device rdev;
    unsigned count = Hydra::EmbeddedSlabCache::objectsPerSlab(sizeof(Data), 8);

    /* Take all objects from the allocator */
    for (unsigned i = 0; i < count; i++)
    {
        addrs.push_back(slab_cache.alloc());
    }

    /* Shuffle and deallocate in random order */
    std::shuffle(addrs.begin(), addrs.end(), std::mt19937{rdev()});

    for (void *p : addrs)
    {
        slab_cache.dealloc(p);
    }

    /* Now allocate all objects again and verify that their addresses were
       from the original allocation, and are on the free list in FIFO order */
    for (unsigned i = count; i > 0; i--)
    {
        bool b = addrs[i - 1] == slab_cache.alloc();
        if (!b)
        {
            std::cout << "test_Embedded_linking_correctness: "
                << "Invalid address retrieved after deallocations"
                << std::endl;

            return;
        }
    }

    std::cout << "Passed test_Embedded_linking_correctness" << std::endl;
}

void test_Raw_freelist_linking()
{
    Hydra::RawSlabCache slab_cache{ sizeof(Data), 8, 3, 0 };
    std::vector<void*> addrs;
    std::random_device rdev;
    unsigned count = 3 * Hydra::static_info.sys.dwPageSize / sizeof(Data);

    /* Take all objects from the allocator */
    for (unsigned i = 0; i < count; i++)
    {
        addrs.push_back(slab_cache.alloc());
    }

    /* Shuffle and deallocate in random order */
    std::shuffle(addrs.begin(), addrs.end(), std::mt19937{rdev()});

    for (void *p : addrs)
    {
        slab_cache.dealloc(p);
    }

    /* Now allocate all objects again and verify that their addresses were
       from the original allocation, and are on the free list in FIFO order */
    for (unsigned i = count; i > 0; i--)
    {
        bool b = addrs[i - 1] == slab_cache.alloc();
        if (!b)
        {
            std::cout << "test_Raw_linking_correctness: "
                << "Invalid address retrieved after deallocations"
                << std::endl;

            return;
        }
    }

    std::cout << "Passed test_Raw_linking_correctness" << std::endl;
}

void test_Embedded_slab_empty_to_partial()
{
    Hydra::EmbeddedSlabCache slab_cache{ sizeof(Data), 8, 3 };
    void *addr;
    unsigned count = Hydra::EmbeddedSlabCache::objectsPerSlab(sizeof(Data), 8);

    /* Make the first allocation */
    addr = slab_cache.alloc();

    /* Empty the first two slabs */
    for (unsigned i = 1; i < count * 2; i++)
    {
        slab_cache.alloc();
    }

    /* Deallocate the first address */
    slab_cache.dealloc(addr);

    /* Now allocate again and verify that the first slab was moved after
       the current free slab when it went from empty to partially filled */
    if (slab_cache.alloc() != addr)
    {
        std::cout << "test_Embedded_slab_empty_to_partial: "
            << "First slab not moved in front of full slab when going "
            << "from empty to partially filled"
            << std::endl;
    }
    else
    {
        std::cout << "Passed test_Embedded_slab_empty_to_partial" << std::endl;
    }
}

void test_Raw_slab_empty_to_partial()
{
    /* Pre-allocate three slabs of two pages each */
    Hydra::RawSlabCache slab_cache{ sizeof(Data), 8, 2, 3 };
    void *addr;
    unsigned count = Hydra::RawSlabCache::objectsPerSlab(sizeof(Data), 8, 2);

    /* Make the first allocation */
    addr = slab_cache.alloc();

    /* Empty the first two slabs */
    for (unsigned i = 1; i < count * 2; i++)
    {
        slab_cache.alloc();
    }

    /* Deallocate the first address */
    slab_cache.dealloc(addr);

    /* Now allocate again and verify that the first slab was moved after
       the current free slab when it went from empty to partially filled */
    if (slab_cache.alloc() != addr)
    {
        std::cout << "test_Raw_slab_empty_to_partial: "
            << "First slab not moved in front of full slab when going "
            << "from empty to partially filled"
            << std::endl;
    }
    else
    {
        std::cout << "Passed test_Raw_slab_empty_to_partial" << std::endl;
    }
}

void test_Object_Embedded_swap_correctness()
{
    constexpr uint32_t mag_alloc_const = 128;
    constexpr uint32_t mag_entries = 7;
    int64_t freq;
    Data *addrs[4][mag_entries];

    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&freq));

    Hydra::ObjectAllocator<Data, Hydra::EmbeddedSlabCache, 64> obj_cache{
        1, 1, 1, mag_alloc_const,
        freq,
        [] (void* ptr) {
            new(ptr) Data{ 4, 3, 2, 1 };
        } // ptr is put on the cache following construction here
    };

    /* Empty four magazines, the initial two magazines should be put
       on the depot's empty list */
    for (unsigned i = 0; i < 4; i++)
    {
        for (unsigned j = 0; j < mag_entries; j++)
        {
            addrs[i][j] = obj_cache.alloc(0, 4u, 3u, 2u, 1u);
        }
    }

    /* Then put objects back in their original order, i.e. reverse of
       allocation order. Everything should now be in the original
       allocation order in the cache and the depot's full list. */
    for (unsigned i = 4; i > 0; i--)
    {
        for (unsigned j = mag_entries; j > 0; j--)
        {
            obj_cache.dealloc(0, addrs[i - 1][j - 1]);
        }
    }

    /* Now verify the allocation order, and check that object's were cached,
       i.e. not destructed, so the struct values should be the same */
    for (unsigned i = 0; i < 4; i++)
    {
        for (unsigned j = 0; j < mag_entries; j++)
        {
            // Don't pass alloc params on second allocation
            Data *obj = obj_cache.alloc(0);
            bool b1 = obj->a == 4;
            bool b2 = obj->b == 3;
            bool b3 = obj->c == 2;
            bool b4 = obj->d == 1;

            if (obj != addrs[i][j])
            {
                std::cout << "test_Object_Embedded_swap_correctness: "
                    << "Out of order allocation after deallocations, "
                    << "group " << i << ", item " << j
                    << std::endl;

                return;
            }
            else if ( !(b1 && b2 && b3 && b4) )
            {
                std::cout << "test_Object_Embedded_swap_correctness: "
                    << "Object modified between allocations, "
                    << "group " << i << ", item " << j
                    << std::endl;

                return;
            }
        }
    }

    std::cout << "Passed test_Object_Embedded_swap_correctness" << std::endl;
}

void test_Object_Raw_swap_correctness()
{
    constexpr uint32_t slab_pages = 4;
    constexpr uint32_t mag_alloc_const = 128;
    constexpr uint32_t mag_entries = 7;
    int64_t freq;
    Data *addrs[4][mag_entries];

    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&freq));

    Hydra::ObjectAllocator<Data, Hydra::RawSlabCache, 64> obj_cache{
        1, 1, 1, mag_alloc_const,
        freq, slab_pages,
        [] (void* ptr) {
            new(ptr) Data{ 4, 3, 2, 1 };
        } // ptr is put on the cache following construction here
    };

    /* Empty four magazines, the initial two magazines should be put
       on the depot's empty list */
    for (unsigned i = 0; i < 4; i++)
    {
        for (unsigned j = 0; j < mag_entries; j++)
        {
            addrs[i][j] = obj_cache.alloc(0, 4u, 3u, 2u, 1u);
        }
    }

    /* Then put objects back in their original order, i.e. reverse of
       allocation order. Everything should now be in the original
       allocation order in the cache and the depot's full list. */
    for (unsigned i = 4; i > 0; i--)
    {
        for (unsigned j = mag_entries; j > 0; j--)
        {
            obj_cache.dealloc(0, addrs[i - 1][j - 1]);
        }
    }

    /* Now verify the allocation order, and check that object's were cached,
       i.e. not destructed, so the struct values should be the same */
    for (unsigned i = 0; i < 4; i++)
    {
        for (unsigned j = 0; j < mag_entries; j++)
        {
            // Don't pass alloc params on second allocation
            Data *obj = obj_cache.alloc(0);
            bool b1 = obj->a == 4;
            bool b2 = obj->b == 3;
            bool b3 = obj->c == 2;
            bool b4 = obj->d == 1;

            if (obj != addrs[i][j])
            {
                std::cout << "test_Object_Raw_swap_correctness: "
                    << "Out of order allocation after deallocations, "
                    << "group " << i << ", item " << j
                    << std::endl;

                return;
            }
            else if ( !(b1 && b2 && b3 && b4) )
            {
                std::cout << "test_Object_Raw_swap_correctness: "
                    << "Object modified between allocations, "
                    << "group " << i << ", item " << j
                    << std::endl;

                return;
            }
        }
    }

    std::cout << "Passed test_Object_Raw_swap_correctness" << std::endl;
}

int main()
{
    test_Embedded_freelist_linking();
    test_Raw_freelist_linking();
    test_Embedded_slab_empty_to_partial();
    test_Raw_slab_empty_to_partial();
    test_Object_Embedded_swap_correctness();
    test_Object_Raw_swap_correctness();
    return 0;
}
