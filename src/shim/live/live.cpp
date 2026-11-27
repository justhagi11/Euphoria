#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <array>

namespace euphoria::shim {
namespace {

std::uintptr_t stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("live: unimplemented shim called");
    return 0;
}

constexpr std::array<Entry, 2> kEntries{{
    {"XUserAddAsync", 0, stub},
    {"XGameSaveInitialize", 0, stub},
}};

const Library kLibrary{"live", kEntries};

}

const Library& live_library() noexcept
{
    return kLibrary;
}

}
