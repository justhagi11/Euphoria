#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace euphoria::shader {

enum class Encoding : std::uint8_t {
    Sop1,
    Sop2,
    Sopc,
    Sopk,
    Sopp,
    Vop1,
    Vop2,
    Vopc,
    Vop3,
    Vop3p,
    Vintrp,
    Exp,
    Ds,
    Smem,
    Mubuf,
    Mtbuf,
    Mimg,
    Flat,
    Unknown,
};

inline constexpr std::size_t kEncodingCount = static_cast<std::size_t>(Encoding::Unknown) + 1;

struct ScanResult {
    std::uint64_t instructions{};
    std::uint64_t words{};
    std::uint64_t blocks{};
    std::uint64_t branches{};
    std::uint64_t returns{};
    std::array<std::uint64_t, kEncodingCount> formats{};
    std::vector<std::pair<std::uint32_t, std::uint64_t>> opcodes;
};

ScanResult scan_rdna2(std::span<const std::uint32_t> words);
const char* encoding_name(Encoding encoding) noexcept;

}
