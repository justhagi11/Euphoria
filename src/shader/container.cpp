#include <euphoria/shader/container.hpp>

#include <algorithm>

namespace euphoria::shader {
namespace {

constexpr std::uint32_t kDxbcMagic = 0x43425844;
constexpr std::size_t kHeaderSize = 32;

std::uint32_t get_u32(std::span<const std::uint8_t> data, std::size_t offset)
{
    return static_cast<std::uint32_t>(data[offset])
         | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
         | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
         | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

}

std::string fourcc_name(std::uint32_t fourcc)
{
    std::string name(4, '\0');
    for (std::size_t index = 0; index < 4; ++index) {
        const char value = static_cast<char>((fourcc >> (index * 8)) & 0xFF);
        name[index] = value >= 32 && value < 127 ? value : '?';
    }
    return name;
}

const Chunk* Container::find(std::string_view name) const noexcept
{
    for (const auto& chunk : chunks) {
        if (chunk.name == name) {
            return &chunk;
        }
    }
    return nullptr;
}

std::optional<Container> parse_container(std::span<const std::uint8_t> data)
{
    if (data.size() < kHeaderSize || get_u32(data, 0) != kDxbcMagic) {
        return std::nullopt;
    }

    const std::uint32_t declared_size = get_u32(data, 24);
    const std::uint32_t chunk_count = get_u32(data, 28);
    if (declared_size > data.size() || chunk_count == 0 || chunk_count > 256) {
        return std::nullopt;
    }
    if (kHeaderSize + static_cast<std::size_t>(chunk_count) * 4 > data.size()) {
        return std::nullopt;
    }

    Container container;
    container.source = data;
    container.chunks.reserve(chunk_count);

    for (std::uint32_t index = 0; index < chunk_count; ++index) {
        const std::uint32_t offset = get_u32(data, kHeaderSize + index * 4);
        if (offset + 8 > declared_size) {
            return std::nullopt;
        }
        const std::uint32_t fourcc = get_u32(data, offset);
        const std::uint32_t size = get_u32(data, offset + 4);
        if (offset + 8 + size > declared_size) {
            return std::nullopt;
        }

        Chunk chunk;
        chunk.fourcc = fourcc;
        chunk.name = fourcc_name(fourcc);
        chunk.data = data.subspan(offset + 8, size);
        container.chunks.push_back(std::move(chunk));
    }

    return container;
}

}
