#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <array>

namespace euphoria::shim {
namespace {

std::uintptr_t stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("kernel: unimplemented shim called");
    return 0;
}

constexpr std::array<Entry, 3> kEntries{{
    {"CreateFileW", 0, stub},
    {"CloseHandle", 0, stub},
    {"Sleep", 0, stub},
}};

const Library kLibrary{"kernel", kEntries};

}

const Library& kernel_library() noexcept
{
    return kLibrary;
}

}
