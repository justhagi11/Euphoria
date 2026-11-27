#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <array>

namespace euphoria::shim {
namespace {

std::uintptr_t stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("gdk: unimplemented shim called");
    return 0;
}

constexpr std::array<Entry, 2> kEntries{{
    {"XGameGetXboxTitleId", 0, stub},
    {"XAppGetCurrentLocale", 0, stub},
}};

const Library kLibrary{"gdk", kEntries};

}

const Library& gdk_library() noexcept
{
    return kLibrary;
}

}
