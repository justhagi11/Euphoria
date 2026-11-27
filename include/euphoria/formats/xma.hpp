#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace euphoria::formats::xma {

constexpr std::size_t kPacketBytes = 2048;
constexpr std::size_t kFrameSamples = 512;
constexpr std::size_t kSubframeSamples = 128;

struct Packet {
    std::uint32_t frames;
    std::uint32_t first_frame_bit;
    std::uint32_t skip;
};

struct Frame {
    std::vector<std::uint8_t> data;
    std::size_t bits;
    std::size_t start;
    std::optional<std::size_t> next;
};

std::optional<Packet> packet(std::span<const std::uint8_t> data, std::size_t index) noexcept;
std::optional<Frame> read_frame(std::span<const std::uint8_t> data, std::size_t start);

class Decoder {
public:
    Decoder(std::size_t channels, std::uint32_t sample_rate);
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    std::size_t channels() const noexcept;
    void reset();
    std::expected<void, std::string_view> decode(std::span<const std::uint8_t> data, std::size_t bits,
                                                 std::vector<std::int16_t>& out);
    void flush(std::vector<std::int16_t>& out);

    struct State;

private:
    std::unique_ptr<State> state_;
};

}
