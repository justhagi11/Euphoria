#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <array>

namespace euphoria::shim {
namespace {

std::uintptr_t stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("net: unimplemented shim called");
    return 0;
}

constexpr std::array<Entry, 2> kEntries{{
    {"WSAStartup", 0, stub},
    {"socket", 0, stub},
}};

const Library kLibrary{"net", kEntries};

}

const Library& net_library() noexcept
{
    return kLibrary;
}

}
