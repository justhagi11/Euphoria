#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace euphoria::shim {

using Thunk = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

struct Entry {
    std::string_view name;
    std::uint32_t ordinal;
    Thunk thunk;
};

struct Library {
    std::string_view name;
    std::span<const Entry> entries;
};

std::span<const Library> libraries() noexcept;
const Library* find(std::string_view name) noexcept;
const Entry* resolve(std::string_view library, std::string_view name, std::uint32_t ordinal) noexcept;
void log_libraries();

}
