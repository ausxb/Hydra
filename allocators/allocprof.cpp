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

struct ObjAllocCtx
{
    uint32_t id;
    Hydra::ObjectAllocator<Data, Hydra::EmbeddedSlabCache, 64> *obj_cache;
    unsigned thread_caches;
    unsigned mag_init;
    unsigned mag_max;
    unsigned mag_alloc_const;
    int64_t contention_thresh;
    unsigned slab_pages;
    std::vector<double> *alloc_time;
    std::vector<double> *dealloc_time;
};

DWORD profile_magazine_only(void *params)
{
    ObjAllocCtx *p = static_cast<ObjAllocCtx*>(params);
    int64_t freq, start, end;
    Data *mem;
    std::vector<Data*> addrs;
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&freq));

    for (uint32_t i = 0; i < 2 * (p->mag_init * 8 - 1); i++)
    {
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&start));
        mem = p->obj_cache->alloc(p->id);
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&end));
        addrs.push_back(mem);

        double latency = static_cast<double>(end - start) / freq;
        p->alloc_time->push_back(latency);
    }

    for (Data *mem : addrs)
    {
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&start));
        p->obj_cache->dealloc(p->id, mem);
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&end));

        double latency = static_cast<double>(end - start) / freq;
        p->dealloc_time->push_back(latency);
    }

    return 0;
}

void profile_Object_Allocator(LPTHREAD_START_ROUTINE action)
{
    constexpr uint32_t threads = 4;
    constexpr uint32_t size = 2;
    constexpr uint32_t max_size = 6;
    constexpr uint32_t mag_alloc_const = 128;
    constexpr uint32_t mag_entries = 7;
    int64_t freq;

    HANDLE handles[threads];
    ObjAllocCtx params[threads];
    std::vector<double> alloc_latencies[threads];
    std::vector<double> dealloc_latencies[threads];

    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&freq));

    Hydra::ObjectAllocator<Data, Hydra::EmbeddedSlabCache, 64> obj_cache {
        threads, size, max_size, mag_alloc_const,
        freq * 2,
        [] (void* ptr) {
            new(ptr) Data{ 4, 3, 2, 1 };
        } // ptr is put on the cache following construction here
    };

    for (uint32_t i = 0; i < threads; i++)
    {
        params[i] = ObjAllocCtx {
            i, &obj_cache, threads, size,
            max_size, mag_alloc_const,
            freq * 2, 0,
            alloc_latencies + i,
            dealloc_latencies + i
        };

        handles[i] = CreateThread(
            NULL, 0, action, params + i, 0, NULL
        );
    }

    WaitForMultipleObjects(threads, handles, TRUE, INFINITE);

    for (uint32_t i = 0; i < threads; i++)
    {
        CloseHandle(handles[i]);
    }

    std::cout << "QPC frequency: " << freq << std::endl;

    std::cout << "Allocation latencies" << std::endl;
    for (uint32_t i = 0; i < threads; i++)
    {
        double avg = 0;
        uint64_t c = 0;

        std::cout << "Thread " << i << ": ";
        for (double d : alloc_latencies[i])
        {
            std::cout << d << ' ';
            avg += d;
            c++;
        }

        avg /= c;
        std::cout << std::endl << "Thread " << i << " average: "
            << avg << std::endl;
    }

    std::cout << "Deallocation latencies" << std::endl;
    for (uint32_t i = 0; i < threads; i++)
    {
        double avg = 0;
        uint64_t c = 0;

        std::cout << "Thread " << i << ": ";
        for (double d : dealloc_latencies[i])
        {
            std::cout << d << ' ';
            avg += d;
            c++;
        }

        avg /= c;
        std::cout << std::endl << "Thread " << i << " average: "
            << avg << std::endl;
    }
}

int main()
{
    std::cout << "profile_magazine_only" << std::endl;
    profile_Object_Allocator(profile_magazine_only);

    return 0;
}
