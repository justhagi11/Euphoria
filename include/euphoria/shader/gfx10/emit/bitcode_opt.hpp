#pragma once

#include <euphoria/shader/gfx10/emit/bitcode.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace euphoria::shader::gfx10::emit {

namespace opt {

inline constexpr std::uint32_t DEAD = 1;
inline constexpr std::uint32_t FOLD = 2;
inline constexpr std::uint32_t REUSE = 4;
inline constexpr std::uint32_t FACTS = 8;
inline constexpr std::uint32_t LOADS = 16;
inline constexpr std::uint32_t FIRST = 32;
inline constexpr std::uint32_t WIDE = 64;
inline constexpr std::uint32_t MODULE = DEAD | FOLD | REUSE | FACTS | LOADS;
inline constexpr std::uint32_t ALL = MODULE | FIRST | WIDE;
inline constexpr std::uint32_t DEFAULT = ALL & ~WIDE;

}

struct Usage {
    bool wave_ops{false};
    bool int64{false};
    bool typed_uav_loads{false};
    bool heap_views{false};
    bool heap_samplers{false};
    bool atomic64{false};

    friend bool operator==(const Usage&, const Usage&) = default;
};

enum class Effect : std::uint8_t {
    Pure,
    Reads,
    Lanes,
    Writes,
};

Effect effect(const Func& f);

std::uint32_t parse_shader_opt(std::optional<std::string_view> value);
std::uint32_t shader_opt();
void set_shader_opt(std::uint32_t passes);

}
