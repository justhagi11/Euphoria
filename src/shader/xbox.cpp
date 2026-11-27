#include <euphoria/shader/xbox.hpp>

#include <euphoria/shader/container.hpp>

#include <openscarlett/io/zlib_rfc1950.h>

#include <cstring>

namespace euphoria::shader {
namespace {

constexpr std::uint64_t kDeadBeef = 0xDEADBEEFDEADBEEFull;
constexpr std::size_t kSlotOffsets[8]{0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x48, 0x60};
constexpr const char* kStageNames[8]{"ls", "hs", "es", "gs", "vs", "ps", "ms", "cs"};

std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t offset)
{
    return static_cast<std::uint32_t>(data[offset])
         | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
         | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
         | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

std::uint64_t read_u64(std::span<const std::uint8_t> data, std::size_t offset)
{
    return static_cast<std::uint64_t>(read_u32(data, offset))
         | (static_cast<std::uint64_t>(read_u32(data, offset + 4)) << 32);
}

std::optional<std::span<const std::uint8_t>> dxil_part(std::span<const std::uint8_t> container)
{
    const std::optional<Container> parsed = parse_container(container);
    if (!parsed) {
        return std::nullopt;
    }
    const Chunk* dxil = parsed->find("DXIL");
    if (dxil == nullptr) {
        return std::nullopt;
    }
    return dxil->data;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> find_native(std::span<const std::uint8_t> part)
{
    if (part.size() < 0x30 || std::memcmp(part.data() + 0x18, "XDAT", 4) != 0) {
        return std::nullopt;
    }
    const std::uint32_t count = read_u32(part, 0x1C);
    if (count > 64) {
        return std::nullopt;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t base = 0x24 + static_cast<std::size_t>(index) * 12;
        if (base + 12 > part.size()) {
            return std::nullopt;
        }
        const std::uint32_t kind = read_u32(part, base);
        const std::uint32_t offset = read_u32(part, base + 4);
        const std::uint32_t size = read_u32(part, base + 8);
        if (kind == 0x0D && offset + 4 <= part.size()
            && std::memcmp(part.data() + offset, "XBCH", 4) == 0) {
            return std::pair{offset, size};
        }
    }
    return std::nullopt;
}

}

std::optional<XboxShader> extract_xbox_shader(std::span<const std::uint8_t> container)
{
    const std::optional<std::span<const std::uint8_t>> part = dxil_part(container);
    if (!part) {
        return std::nullopt;
    }
    const std::optional<std::pair<std::uint32_t, std::uint32_t>> native = find_native(*part);
    if (!native) {
        return std::nullopt;
    }

    const std::uint32_t offset = native->first;
    if (offset + 24 > part->size()) {
        return std::nullopt;
    }
    const std::uint64_t packed = read_u64(*part, offset + 8);
    const std::uint64_t unpacked = read_u64(*part, offset + 16);
    if (packed < 6 || unpacked < 0xD0 || unpacked > 32ull * 1024ull * 1024ull
        || offset + 24 + packed > part->size()) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> fssm;
    if (!openscarlett::zlib_rfc1950::Decompress(part->data() + offset + 24,
                                               static_cast<std::size_t>(packed),
                                               static_cast<std::size_t>(unpacked), fssm)) {
        return std::nullopt;
    }
    if (fssm.size() < 0xD0 || std::memcmp(fssm.data(), "FSSM", 4) != 0) {
        return std::nullopt;
    }

    XboxShader shader;
    bool found = false;
    const std::span<const std::uint8_t> payload{fssm.data() + 16, fssm.size() - 16};
    for (std::size_t index = 0; index < 8; ++index) {
        if (kSlotOffsets[index] + 8 > payload.size()) {
            continue;
        }
        const std::uint64_t record_offset = read_u64(payload, kSlotOffsets[index]);
        if (record_offset == kDeadBeef || record_offset == 0
            || record_offset + 0x58 > payload.size()) {
            continue;
        }

        const std::uint64_t code_offset = read_u64(payload, static_cast<std::size_t>(record_offset));
        const std::uint64_t code_size = read_u64(payload, static_cast<std::size_t>(record_offset) + 8);
        if (code_size == 0 || (code_size & 3) != 0
            || code_offset + code_size > payload.size()) {
            continue;
        }

        const std::uint32_t flags = read_u32(payload, static_cast<std::size_t>(record_offset) + 0x14);
        const std::uint32_t resources =
            read_u32(payload, static_cast<std::size_t>(record_offset) + 0x50);

        shader.stage = static_cast<std::uint8_t>(index);
        shader.wave = ((flags >> 5) & 1) != 0 ? 32 : 64;
        shader.vgprs = resources & 0x3FF;
        shader.sgprs = (resources >> 20) & 0xFF;
        shader.code.resize(static_cast<std::size_t>(code_size) / 4);
        std::memcpy(shader.code.data(), payload.data() + code_offset,
                    static_cast<std::size_t>(code_size));
        found = true;
        break;
    }

    return found ? std::optional<XboxShader>(std::move(shader)) : std::nullopt;
}

const char* stage_name(std::uint8_t stage) noexcept
{
    return stage < 8 ? kStageNames[stage] : "?";
}

}
