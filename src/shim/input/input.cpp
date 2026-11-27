#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <array>

namespace euphoria::shim {
namespace {

std::uintptr_t stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("input: unimplemented shim called");
    return 0;
}

constexpr std::array<Entry, 2> kEntries{{
    {"XInputGetState", 0, stub},
    {"GameInputCreate", 0, stub},
}};

const Library kLibrary{"input", kEntries};

}

const Library& input_library() noexcept
{
    return kLibrary;
}

}
