#pragma once

#include <euphoria/shader/gfx10/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10 {

struct FourCC {
    std::array<std::uint8_t, 4> bytes{};

    static constexpr FourCC from(const char (&text)[5]) noexcept
    {
        return FourCC{{static_cast<std::uint8_t>(text[0]), static_cast<std::uint8_t>(text[1]),
                       static_cast<std::uint8_t>(text[2]), static_cast<std::uint8_t>(text[3])}};
    }

    std::string to_string() const;

    friend constexpr bool operator==(const FourCC&, const FourCC&) noexcept = default;

    static const FourCC DXIL;
    static const FourCC STAT;
    static const FourCC RTS0;
    static const FourCC PSV0;
    static const FourCC ISG1;
    static const FourCC OSG1;
    static const FourCC PSG1;
    static const FourCC SFI0;
    static const FourCC HASH;
    static const FourCC ILDB;
    static const FourCC ILDN;
};

inline constexpr FourCC FourCC::DXIL = FourCC::from("DXIL");
inline constexpr FourCC FourCC::STAT = FourCC::from("STAT");
inline constexpr FourCC FourCC::RTS0 = FourCC::from("RTS0");
inline constexpr FourCC FourCC::PSV0 = FourCC::from("PSV0");
inline constexpr FourCC FourCC::ISG1 = FourCC::from("ISG1");
inline constexpr FourCC FourCC::OSG1 = FourCC::from("OSG1");
inline constexpr FourCC FourCC::PSG1 = FourCC::from("PSG1");
inline constexpr FourCC FourCC::SFI0 = FourCC::from("SFI0");
inline constexpr FourCC FourCC::HASH = FourCC::from("HASH");
inline constexpr FourCC FourCC::ILDB = FourCC::from("ILDB");
inline constexpr FourCC FourCC::ILDN = FourCC::from("ILDN");

struct Part {
    FourCC fourcc;
    std::size_t offset{};
    std::span<const std::uint8_t> data;
};

struct Container {
    std::array<std::uint8_t, 16> digest{};
    std::pair<std::uint16_t, std::uint16_t> version{};
    std::vector<Part> parts;

    static std::expected<Container, ShaderError> parse(std::span<const std::uint8_t> data);

    const Part* part(FourCC fourcc) const noexcept;
};

std::expected<std::uint16_t, ShaderError> u16_at(std::span<const std::uint8_t> data, std::size_t at) noexcept;
std::expected<std::uint32_t, ShaderError> u32_at(std::span<const std::uint8_t> data, std::size_t at) noexcept;
std::expected<std::uint64_t, ShaderError> u64_at(std::span<const std::uint8_t> data, std::size_t at) noexcept;

using BuildPart = std::pair<FourCC, std::span<const std::uint8_t>>;

std::vector<std::uint8_t> build(std::span<const BuildPart> parts);
std::vector<std::uint8_t> build(std::initializer_list<BuildPart> parts);

}
