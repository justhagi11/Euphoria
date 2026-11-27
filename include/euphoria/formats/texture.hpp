#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace euphoria::formats {

struct Swizzle {
    std::uint32_t value{0};

    static const Swizzle LINEAR;
    static const Swizzle SW_256B_S;
    static const Swizzle SW_256B_D;
    static const Swizzle SW_4KB_S;
    static const Swizzle SW_4KB_D;
    static const Swizzle SW_64KB_S;
    static const Swizzle SW_64KB_D;
    static const Swizzle SW_64KB_S_T;
    static const Swizzle SW_64KB_D_T;
    static const Swizzle SW_4KB_S_X;
    static const Swizzle SW_4KB_D_X;
    static const Swizzle SW_4KB_R_X;
    static const Swizzle SW_64KB_Z_X;
    static const Swizzle SW_64KB_S_X;
    static const Swizzle SW_64KB_D_X;
    static const Swizzle SW_64KB_R_X;

    constexpr std::optional<std::uint32_t> block_log2() const noexcept
    {
        switch (value) {
        case 0:
            return 0u;
        case 1:
        case 2:
            return 8u;
        case 5:
        case 6:
        case 21:
        case 22:
        case 23:
            return 12u;
        case 9:
        case 10:
        case 17:
        case 18:
        case 24:
        case 25:
        case 26:
        case 27:
            return 16u;
        default:
            return std::nullopt;
        }
    }

    constexpr bool standard() const noexcept
    {
        return value == 1 || value == 5 || value == 9 || value == 17 || value == 21 || value == 25;
    }

    constexpr bool display() const noexcept
    {
        return value == 2 || value == 6 || value == 10 || value == 18 || value == 22 || value == 26;
    }

    constexpr bool z_order() const noexcept { return value == 24; }
    constexpr bool is_xor() const noexcept { return value >= 16; }

    friend constexpr bool operator==(Swizzle, Swizzle) noexcept = default;
};

inline constexpr Swizzle Swizzle::LINEAR{0};
inline constexpr Swizzle Swizzle::SW_256B_S{1};
inline constexpr Swizzle Swizzle::SW_256B_D{2};
inline constexpr Swizzle Swizzle::SW_4KB_S{5};
inline constexpr Swizzle Swizzle::SW_4KB_D{6};
inline constexpr Swizzle Swizzle::SW_64KB_S{9};
inline constexpr Swizzle Swizzle::SW_64KB_D{10};
inline constexpr Swizzle Swizzle::SW_64KB_S_T{17};
inline constexpr Swizzle Swizzle::SW_64KB_D_T{18};
inline constexpr Swizzle Swizzle::SW_4KB_S_X{21};
inline constexpr Swizzle Swizzle::SW_4KB_D_X{22};
inline constexpr Swizzle Swizzle::SW_4KB_R_X{23};
inline constexpr Swizzle Swizzle::SW_64KB_Z_X{24};
inline constexpr Swizzle Swizzle::SW_64KB_S_X{25};
inline constexpr Swizzle Swizzle::SW_64KB_D_X{26};
inline constexpr Swizzle Swizzle::SW_64KB_R_X{27};

enum class Dimension : std::uint8_t {
    Tex1d,
    Tex2d,
    Tex3d,
};

enum class Error : std::uint8_t {
    Invalid,
    Unsupported,
    TooSmall,
};

const char* error_name(Error error) noexcept;

struct Desc {
    Dimension dimension{Dimension::Tex2d};
    std::uint32_t width{1};
    std::uint32_t height{1};
    std::uint32_t depth{1};
    std::uint32_t mips{1};
    std::uint32_t element_bytes{4};
    std::uint32_t block{1};
    std::uint32_t samples{1};
    Swizzle swizzle{};

    friend bool operator==(const Desc&, const Desc&) noexcept = default;
};

struct Tail {
    std::uint32_t offset{0};
    std::uint32_t x{0};
    std::uint32_t y{0};

    friend bool operator==(const Tail&, const Tail&) noexcept = default;
};

struct Mip {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t depth{0};
    std::uint32_t pitch{0};
    std::uint32_t rows{0};
    std::uint32_t depth_pitch{0};
    std::uint64_t offset{0};
    std::optional<Tail> tail{};

    friend bool operator==(const Mip&, const Mip&) noexcept = default;
};

class Layout {
public:
    static std::expected<Layout, Error> create(const Desc& desc);

    bool addressable() const noexcept;
    std::expected<std::uint64_t, Error> offset(std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z) const;
    std::uint32_t subresource_depth(std::uint32_t mip) const noexcept;

    std::expected<void, Error> detile(std::span<const std::uint8_t> tiled,
                                      std::uint32_t mip,
                                      std::uint32_t slice,
                                      std::span<std::uint8_t> linear,
                                      std::size_t row_pitch,
                                      std::size_t depth_pitch) const;

    std::expected<void, Error> tile(std::span<std::uint8_t> tiled,
                                    std::uint32_t mip,
                                    std::uint32_t slice,
                                    std::span<const std::uint8_t> linear,
                                    std::size_t row_pitch,
                                    std::size_t depth_pitch) const;

    const std::array<std::vector<std::uint32_t>, 3>& pattern() const noexcept { return pattern_; }

    Desc desc{};
    std::array<std::uint32_t, 3> block{};
    bool thick{false};
    std::uint32_t slices{0};
    std::uint64_t slice_size{0};
    std::uint64_t size{0};
    std::uint64_t alignment{0};
    std::uint32_t first_tail_mip{0};
    std::vector<Mip> mips;

private:
    static Layout linear(const Desc& d);
    static Layout tiled(const Desc& d, std::uint32_t block_log2);

    std::pair<std::uint32_t, std::uint32_t> split(std::uint32_t z) const noexcept;
    std::uint64_t row(const Mip& m, std::uint32_t y, std::uint32_t z) const noexcept;
    std::uint64_t column(const Mip& m, std::uint32_t x) const noexcept;

    template <bool Detile>
    std::expected<void, Error> walk(const std::uint8_t* src,
                                    std::size_t src_size,
                                    std::uint8_t* dst,
                                    std::size_t dst_size,
                                    std::uint32_t mip,
                                    std::uint32_t slice,
                                    std::size_t row_pitch,
                                    std::size_t depth_pitch) const;

    std::array<std::vector<std::uint32_t>, 3> pattern_{};
};

struct Format {
    std::uint32_t bytes{0};
    std::uint32_t block{1};
    bool stencil{false};
};

std::optional<Format> format_info(std::uint32_t dxgi) noexcept;

struct TextureDesc {
    Dimension dimension{Dimension::Tex2d};
    std::uint32_t width{1};
    std::uint32_t height{1};
    std::uint32_t depth_or_array{1};
    std::uint32_t mips{1};
    std::uint32_t format{0};
    std::uint32_t samples{1};
    Swizzle swizzle{};
};

struct Plane {
    std::uint64_t offset{0};
    bool stencil{false};
    Layout layout;
};

struct Texture {
    static std::expected<Texture, Error> create(const TextureDesc& desc);

    std::vector<Plane> planes;
    std::uint64_t size{0};
    std::uint64_t alignment{0};
};

bool texture_selftest();

}
