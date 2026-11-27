#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace euphoria::shader {

struct XboxShader {
    std::uint8_t stage{};
    std::uint32_t wave{32};
    std::uint32_t vgprs{};
    std::uint32_t sgprs{};
    std::vector<std::uint32_t> code;
};

std::optional<XboxShader> extract_xbox_shader(std::span<const std::uint8_t> container);
const char* stage_name(std::uint8_t stage) noexcept;

}
