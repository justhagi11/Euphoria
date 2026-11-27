#pragma once

#include <euphoria/shader/gfx10/emit/bitcode.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace euphoria::shader::gfx10::emit {

namespace census {

inline constexpr std::array<std::string_view, 26> KINDS{
    "total",
    "arith",
    "math_call",
    "bitcast",
    "cast",
    "select",
    "guard_select",
    "phi",
    "extract",
    "buffer_load",
    "cbuffer_load",
    "texture_read",
    "lds_load",
    "buffer_store",
    "lds_store",
    "atomic",
    "handle",
    "annotate",
    "dimensions",
    "barrier",
    "wave_op",
    "io_other",
    "gep",
    "clamp",
    "loop_counter",
    "blocks",
};

}

struct Census {
    std::array<std::uint32_t, census::KINDS.size()> counts{};

    friend bool operator==(const Census&, const Census&) = default;
};

namespace census {

bool enabled();
void set_enabled(bool on);
std::optional<Census> take(std::span<const std::uint8_t> dxil);
void note(const Module& m, std::span<const std::uint8_t> bitcode);
std::size_t kind(std::string_view name);

}

}
