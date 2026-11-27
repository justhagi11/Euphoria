#pragma once

#include <euphoria/shader/gfx10/error.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10 {

enum class ShaderKind : std::uint16_t {
    Pixel = 0,
    Vertex = 1,
    Geometry = 2,
    Hull = 3,
    Domain = 4,
    Compute = 5,
    Library = 6,
    RayGeneration = 7,
    Intersection = 8,
    AnyHit = 9,
    ClosestHit = 10,
    Miss = 11,
    Callable = 12,
    Mesh = 13,
    Amplification = 14,
    Node = 15,
};

constexpr bool is_unknown(ShaderKind kind) noexcept
{
    return static_cast<std::uint16_t>(kind) > static_cast<std::uint16_t>(ShaderKind::Node);
}

const char* shader_kind_name(ShaderKind kind) noexcept;

struct XboxExtras {
    bool xdat{false};
    bool xbop{false};

    friend constexpr bool operator==(const XboxExtras&, const XboxExtras&) noexcept = default;
};

struct DxilProgram {
    ShaderKind kind{ShaderKind::Pixel};
    std::pair<std::uint8_t, std::uint8_t> shader_model{};
    std::uint32_t dxil_version{};
    std::span<const std::uint8_t> bitcode;
    std::span<const std::uint8_t> prefix;
    std::span<const std::uint8_t> trailer;

    static std::expected<DxilProgram, ShaderError> parse(std::span<const std::uint8_t> part);

    bool is_standard_layout() const noexcept;
    XboxExtras xbox_extras() const noexcept;
};

std::vector<std::uint8_t> build_standard(std::uint16_t kind, std::pair<std::uint8_t, std::uint8_t> shader_model,
                                         std::uint32_t dxil_version, std::span<const std::uint8_t> bitcode);

}
