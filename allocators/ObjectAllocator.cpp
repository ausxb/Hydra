#include "../util.h"
#include "ObjectAllocator.h"

//========================================================================
// Depot
//========================================================================
Hydra::Depot::Depot(
    unsigned size,
    unsigned align,
    unsigned alloc_const
) : full{ nullptr }, empty{ nullptr },
    mag_size{ size }, ref_count{ 0 },
    slab_pages{ align_up2(alloc_const * sizeof(void*) * (size + 1),
                          Hydra::static_info.sys.dwPageSize)
                >> Hydra::static_info.page_shift }
{
    _ASSERT_EXPR(alloc_const > 0,
        L"Depot::Depot(): alloc_const must be greater than zero");

    magazines = new RawSlabCache{
        sizeof(void*) * (mag_size + 1),
        align,
        slab_pages,
        1
    };

    if (!magazines)
    {
        throw std::runtime_error{
            "Depot::Depot(): Unable to allocate a new magazine allocator"
        };
    }

    InitializeCriticalSection(&lock);
}

Hydra::Depot::~Depot()
{
    DeleteCriticalSection(&lock);
    delete magazines;
}
