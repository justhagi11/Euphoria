#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace euphoria::shader {

struct Chunk {
    std::uint32_t fourcc{};
    std::span<const std::uint8_t> data;
    std::string name;
};

struct Container {
    std::span<const std::uint8_t> source;
    std::vector<Chunk> chunks;

    const Chunk* find(std::string_view name) const noexcept;
};

std::optional<Container> parse_container(std::span<const std::uint8_t> data);
std::string fourcc_name(std::uint32_t fourcc);

}
