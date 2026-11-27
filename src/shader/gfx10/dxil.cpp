#include <euphoria/shader/gfx10/dxil.hpp>

#include <euphoria/shader/gfx10/dxbc.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace euphoria::shader::gfx10 {
namespace {

constexpr std::array<std::uint8_t, 4> kBitcodeMagic{0x42, 0x43, 0xC0, 0xDE};
constexpr std::uint32_t kStandardBitcodeOffset = 16;
constexpr std::size_t kProgramHeader = 24;

constexpr std::array<const char*, 16> kKindNames{
    "pixel",        "vertex",     "geometry", "hull",     "domain", "compute",
    "library",      "raygeneration", "intersection", "anyhit", "closesthit", "miss",
    "callable",     "mesh",       "amplification", "node",
};

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

}

const char* shader_kind_name(ShaderKind kind) noexcept
{
    const auto raw = static_cast<std::uint16_t>(kind);
    return raw < kKindNames.size() ? kKindNames[raw] : "unknown";
}

std::expected<DxilProgram, ShaderError> DxilProgram::parse(std::span<const std::uint8_t> part)
{
    const auto version = u32_at(part, 0);
    if (!version) {
        return std::unexpected(version.error());
    }
    const auto dwords = u32_at(part, 4);
    if (!dwords) {
        return std::unexpected(dwords.error());
    }
    const std::uint64_t size = static_cast<std::uint64_t>(*dwords) * 4;
    if (size > part.size() || size < kProgramHeader) {
        return std::unexpected(ShaderError::Malformed("program size disagrees with part size"));
    }
    part = part.first(static_cast<std::size_t>(size));
    if (std::memcmp(part.data() + 8, "DXIL", 4) != 0) {
        return std::unexpected(ShaderError::Malformed("missing DXIL magic"));
    }
    const std::uint32_t dxil_version = *u32_at(part, 12);
    const std::uint32_t offset = *u32_at(part, 16);
    const std::uint32_t length = *u32_at(part, 20);
    const std::uint64_t start = 8 + static_cast<std::uint64_t>(offset);
    if (start > part.size() || part.size() - start < length) {
        return std::unexpected(ShaderError::Malformed("bitcode out of bounds"));
    }
    const auto bitcode = part.subspan(static_cast<std::size_t>(start), length);
    if (bitcode.size() < kBitcodeMagic.size() || !std::equal(kBitcodeMagic.begin(), kBitcodeMagic.end(), bitcode.begin())) {
        return std::unexpected(ShaderError::Malformed("bitcode lacks LLVM magic"));
    }
    if (start < kProgramHeader) {
        return std::unexpected(ShaderError::Malformed("bitcode offset too small"));
    }
    DxilProgram program;
    program.kind = static_cast<ShaderKind>(static_cast<std::uint16_t>(*version >> 16));
    program.shader_model = {static_cast<std::uint8_t>((*version >> 4) & 0xF), static_cast<std::uint8_t>(*version & 0xF)};
    program.dxil_version = dxil_version;
    program.bitcode = bitcode;
    program.prefix = part.subspan(kProgramHeader, static_cast<std::size_t>(start) - kProgramHeader);
    program.trailer = part.subspan(static_cast<std::size_t>(start) + length);
    return program;
}

bool DxilProgram::is_standard_layout() const noexcept
{
    return prefix.empty() && trailer.empty();
}

XboxExtras DxilProgram::xbox_extras() const noexcept
{
    constexpr std::array<std::uint8_t, 4> kXdat{'X', 'D', 'A', 'T'};
    constexpr std::array<std::uint8_t, 4> kXbop{'x', 'b', 'o', 'p'};
    XboxExtras extras;
    extras.xdat = prefix.size() >= kXdat.size() && std::equal(kXdat.begin(), kXdat.end(), prefix.begin());
    extras.xbop = !std::ranges::search(trailer, kXbop).empty();
    return extras;
}

std::vector<std::uint8_t> build_standard(std::uint16_t kind, std::pair<std::uint8_t, std::uint8_t> shader_model,
                                         std::uint32_t dxil_version, std::span<const std::uint8_t> bitcode)
{
    const std::uint32_t version = (static_cast<std::uint32_t>(kind) << 16) | (static_cast<std::uint32_t>(shader_model.first) << 4)
                                | shader_model.second;
    const std::size_t padded = (bitcode.size() + 3) & ~std::size_t{3};
    std::vector<std::uint8_t> out;
    put_u32(out, version);
    put_u32(out, static_cast<std::uint32_t>((kProgramHeader + padded) / 4));
    out.insert(out.end(), {'D', 'X', 'I', 'L'});
    put_u32(out, dxil_version);
    put_u32(out, kStandardBitcodeOffset);
    put_u32(out, static_cast<std::uint32_t>(bitcode.size()));
    out.insert(out.end(), bitcode.begin(), bitcode.end());
    out.resize(kProgramHeader + padded, 0);
    return out;
}

}
