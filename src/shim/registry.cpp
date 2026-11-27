#include <euphoria/shim/registry.hpp>

#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <array>
#include <string>

namespace euphoria::shim {
namespace {

const std::array<Library, 11> kLibraries{{
    xg_library(),
    acphal_library(),
    xapi_library(),
    gdk_library(),
    kernel_library(),
    dx_library(),
    audio_library(),
    input_library(),
    net_library(),
    live_library(),
    system_library(),
}};

}

std::span<const Library> libraries() noexcept
{
    return kLibraries;
}

const Library* find(std::string_view name) noexcept
{
    for (const auto& library : kLibraries) {
        if (library.name == name) {
            return &library;
        }
    }
    return nullptr;
}

const Entry* resolve(std::string_view library, std::string_view name, std::uint32_t ordinal) noexcept
{
    const Library* found = find(library);
    if (found == nullptr) {
        return nullptr;
    }

    for (const auto& entry : found->entries) {
        if (!name.empty() && entry.name == name) {
            return &entry;
        }
        if (name.empty() && ordinal != 0 && entry.ordinal == ordinal) {
            return &entry;
        }
    }
    return nullptr;
}

void log_libraries()
{
    for (const auto& library : kLibraries) {
        std::string line{"shim "};
        line += library.name;
        line += ": ";
        line += std::to_string(library.entries.size());
        line += " entries";
        log::info(line);
    }
}

}
