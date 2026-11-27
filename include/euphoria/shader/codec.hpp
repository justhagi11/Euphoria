#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace euphoria::shader {

inline constexpr std::uint32_t kCodecMagic = 0x31435345;
inline constexpr std::uint16_t kCodecVersion = 1;
inline constexpr std::uint8_t kMethodLz = 1;

std::uint64_t hash64(std::span<const std::uint8_t> data) noexcept;

std::vector<std::uint8_t> encode(std::span<const std::uint8_t> data);
std::optional<std::vector<std::uint8_t>> decode(std::span<const std::uint8_t> stored);

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> data);
std::optional<std::vector<std::uint8_t>> decompress(std::span<const std::uint8_t> stored,
                                                    std::uint32_t raw_size);

}
