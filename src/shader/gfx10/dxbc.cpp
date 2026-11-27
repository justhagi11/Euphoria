#include <euphoria/shader/gfx10/dxbc.hpp>

#include <algorithm>
#include <cstring>

namespace euphoria::shader::gfx10 {
namespace {

constexpr std::size_t kHeaderSize = 0x20;
constexpr std::size_t kMaxParts = 256;

template <typename T>
std::expected<T, ShaderError> read_le(std::span<const std::uint8_t> data, std::size_t at) noexcept
{
    if (at > data.size() || data.size() - at < sizeof(T)) {
        return std::unexpected(ShaderError::Malformed("truncated"));
    }
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(static_cast<T>(data[at + i]) << (8 * i));
    }
    return value;
}

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

}

std::string FourCC::to_string() const
{
    std::string text;
    for (const std::uint8_t byte : bytes) {
        text.push_back(byte > 0x20 && byte < 0x7F ? static_cast<char>(byte) : '.');
    }
    return text;
}

std::expected<std::uint16_t, ShaderError> u16_at(std::span<const std::uint8_t> data, std::size_t at) noexcept
{
    return read_le<std::uint16_t>(data, at);
}

std::expected<std::uint32_t, ShaderError> u32_at(std::span<const std::uint8_t> data, std::size_t at) noexcept
{
    return read_le<std::uint32_t>(data, at);
}

std::expected<std::uint64_t, ShaderError> u64_at(std::span<const std::uint8_t> data, std::size_t at) noexcept
{
    return read_le<std::uint64_t>(data, at);
}

std::expected<Container, ShaderError> Container::parse(std::span<const std::uint8_t> data)
{
    if (data.size() < kHeaderSize || std::memcmp(data.data(), "DXBC", 4) != 0) {
        return std::unexpected(ShaderError::NotDxbc());
    }
    Container container;
    std::copy_n(data.begin() + 4, 16, container.digest.begin());
    container.version = {*u16_at(data, 0x14), *u16_at(data, 0x16)};
    const std::size_t total = *u32_at(data, 0x18);
    if (total > data.size() || total < kHeaderSize) {
        return std::unexpected(ShaderError::Malformed("container size disagrees with data"));
    }
    data = data.first(total);
    const std::size_t count = *u32_at(data, 0x1C);
    if (count > kMaxParts) {
        return std::unexpected(ShaderError::Malformed("implausible part count"));
    }
    container.parts.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = u32_at(data, kHeaderSize + 4 * i);
        if (!offset) {
            return std::unexpected(offset.error());
        }
        const std::size_t at = *offset;
        if (at > data.size() || data.size() - at < 4) {
            return std::unexpected(ShaderError::Malformed("part header out of bounds"));
        }
        Part part;
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at), 4, part.fourcc.bytes.begin());
        part.offset = at;
        const auto size = u32_at(data, at + 4);
        if (!size) {
            return std::unexpected(size.error());
        }
        if (data.size() - (at + 8) < *size) {
            return std::unexpected(ShaderError::Malformed("part data out of bounds"));
        }
        part.data = data.subspan(at + 8, *size);
        container.parts.push_back(part);
    }
    return container;
}

const Part* Container::part(FourCC fourcc) const noexcept
{
    const auto found = std::ranges::find(parts, fourcc, &Part::fourcc);
    return found == parts.end() ? nullptr : &*found;
}

std::vector<std::uint8_t> build(std::span<const BuildPart> parts)
{
    const std::size_t table = kHeaderSize + 4 * parts.size();
    std::size_t total = table;
    for (const auto& [fourcc, data] : parts) {
        total += 8 + data.size();
    }
    std::vector<std::uint8_t> out;
    out.reserve(total);
    out.insert(out.end(), {'D', 'X', 'B', 'C'});
    out.resize(out.size() + 16, 0);
    put_u16(out, 1);
    put_u16(out, 0);
    put_u32(out, static_cast<std::uint32_t>(total));
    put_u32(out, static_cast<std::uint32_t>(parts.size()));
    std::size_t offset = table;
    for (const auto& [fourcc, data] : parts) {
        put_u32(out, static_cast<std::uint32_t>(offset));
        offset += 8 + data.size();
    }
    for (const auto& [fourcc, data] : parts) {
        out.insert(out.end(), fourcc.bytes.begin(), fourcc.bytes.end());
        put_u32(out, static_cast<std::uint32_t>(data.size()));
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

std::vector<std::uint8_t> build(std::initializer_list<BuildPart> parts)
{
    return build(std::span<const BuildPart>{parts.begin(), parts.size()});
}

}
