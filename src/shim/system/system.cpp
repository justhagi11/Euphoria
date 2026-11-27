#include <euphoria/log/log.hpp>
#include <euphoria/shim/dstorage.hpp>
#include <euphoria/shim/libraries.hpp>

#include <array>
#include <cstdint>

namespace euphoria::shim {
namespace {

std::uintptr_t stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("system: unimplemented shim called");
    return 0;
}

const std::array<Entry, 3> kEntries{{
    {"DStorageGetFactory", 0, reinterpret_cast<Thunk>(&dstorage::get_factory)},
    {"XSystemGetConsoleId", 0, reinterpret_cast<Thunk>(&stub)},
    {"XSystemGetDeviceType", 0, reinterpret_cast<Thunk>(&stub)},
}};

const Library kLibrary{"system", kEntries};

}

const Library& system_library() noexcept
{
    return kLibrary;
}

}
