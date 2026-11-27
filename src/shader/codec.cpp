#include <euphoria/shader/codec.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace euphoria::shader {
namespace {

constexpr std::size_t kMaxRaw = 256U * 1024U * 1024U;
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kMatchMinimum = 3;
constexpr std::size_t kMatchMaximum = 130;
constexpr std::size_t kLiteralMaximum = 128;
constexpr std::uint32_t kWindow = 65535;
constexpr std::uint8_t kMatchFlag = 0x80;

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

std::uint16_t get_u16(std::span<const std::uint8_t> data, std::size_t offset)
{
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

std::uint32_t get_u32(std::span<const std::uint8_t> data, std::size_t offset)
{
    return static_cast<std::uint32_t>(data[offset])
         | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
         | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
         | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

std::uint64_t get_u64(std::span<const std::uint8_t> data, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | data[offset + static_cast<std::size_t>(index)];
    }
    return value;
}

std::uint32_t hash3(const std::uint8_t* data) noexcept
{
    const std::uint32_t value = static_cast<std::uint32_t>(data[0])
                              | (static_cast<std::uint32_t>(data[1]) << 8)
                              | (static_cast<std::uint32_t>(data[2]) << 16);
    return ((value * 2654435761U) >> 16) & 0xFFFF;
}

}

std::uint64_t hash64(std::span<const std::uint8_t> data) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> out;
    out.reserve(data.size() / 2 + 16);

    std::array<std::int32_t, 1 << 16> table{};
    table.fill(-1);

    std::size_t literal_start = 0;
    const auto flush_literals = [&](std::size_t end) {
        while (literal_start < end) {
            const std::size_t length = std::min<std::size_t>(kLiteralMaximum, end - literal_start);
            out.push_back(static_cast<std::uint8_t>(length - 1));
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(literal_start),
                       data.begin() + static_cast<std::ptrdiff_t>(literal_start + length));
            literal_start += length;
        }
    };

    std::size_t position = 0;
    while (position + kMatchMinimum <= data.size()) {
        const std::uint32_t slot = hash3(data.data() + position);
        const std::int32_t candidate = table[slot];
        table[slot] = static_cast<std::int32_t>(position);

        std::size_t best_length = 0;
        std::size_t best_distance = 0;
        if (candidate >= 0) {
            const std::size_t distance = position - static_cast<std::size_t>(candidate);
            if (distance <= kWindow) {
                const std::size_t limit =
                    std::min<std::size_t>(kMatchMaximum, data.size() - position);
                std::size_t length = 0;
                while (length < limit
                       && data[static_cast<std::size_t>(candidate) + length]
                              == data[position + length]) {
                    ++length;
                }
                if (length >= kMatchMinimum) {
                    best_length = length;
                    best_distance = distance;
                }
            }
        }

        if (best_length == 0) {
            ++position;
            continue;
        }

        flush_literals(position);
        out.push_back(static_cast<std::uint8_t>(kMatchFlag | (best_length - kMatchMinimum)));
        put_u16(out, static_cast<std::uint16_t>(best_distance - 1));
        position += best_length;
        literal_start = position;
    }

    flush_literals(data.size());
    return out;
}

std::optional<std::vector<std::uint8_t>> decompress(std::span<const std::uint8_t> stored,
                                                    std::uint32_t raw_size)
{
    if (raw_size > kMaxRaw) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> out(raw_size);
    std::size_t position = 0;
    std::size_t written = 0;

    while (written < raw_size) {
        if (position >= stored.size()) {
            return std::nullopt;
        }

        const std::uint8_t token = stored[position++];
        if ((token & kMatchFlag) != 0) {
            const std::size_t length = static_cast<std::size_t>(token & 0x7F) + kMatchMinimum;
            if (position + 2 > stored.size()) {
                return std::nullopt;
            }
            const std::size_t distance = static_cast<std::size_t>(get_u16(stored, position)) + 1;
            position += 2;
            if (distance > written || written + length > raw_size) {
                return std::nullopt;
            }
            if (distance >= length) {
                std::memcpy(out.data() + written, out.data() + written - distance, length);
            } else {
                for (std::size_t index = 0; index < length; ++index) {
                    out[written + index] = out[written - distance + index];
                }
            }
            written += length;
        } else {
            const std::size_t length = static_cast<std::size_t>(token & 0x7F) + 1;
            if (position + length > stored.size() || written + length > raw_size) {
                return std::nullopt;
            }
            std::memcpy(out.data() + written, stored.data() + position, length);
            position += length;
            written += length;
        }
    }

    return position == stored.size()
             ? std::optional<std::vector<std::uint8_t>>(std::move(out))
             : std::nullopt;
}

std::vector<std::uint8_t> encode(std::span<const std::uint8_t> data)
{
    const std::vector<std::uint8_t> payload = compress(data);

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload.size());
    put_u32(out, kCodecMagic);
    put_u16(out, kCodecVersion);
    out.push_back(kMethodLz);
    out.push_back(0);
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    put_u64(out, hash64(data));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<std::vector<std::uint8_t>> decode(std::span<const std::uint8_t> stored)
{
    if (stored.size() < kHeaderSize || get_u32(stored, 0) != kCodecMagic
        || get_u16(stored, 4) != kCodecVersion || stored[6] != kMethodLz) {
        return std::nullopt;
    }

    const std::uint32_t raw_size = get_u32(stored, 8);
    const std::uint32_t payload_size = get_u32(stored, 12);
    const std::uint64_t expected = get_u64(stored, 16);

    if (stored.size() != kHeaderSize + payload_size) {
        return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> out =
        decompress(stored.subspan(kHeaderSize, payload_size), raw_size);
    if (!out) {
        return std::nullopt;
    }
    if (hash64(*out) != expected) {
        return std::nullopt;
    }
    return out;
}

}
