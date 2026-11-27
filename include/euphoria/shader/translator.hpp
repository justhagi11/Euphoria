#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace euphoria::shader {

struct LiftResult {
    bool ok{};
    std::vector<std::uint8_t> dxil;
    std::string error;
    std::size_t chunks{};
    std::size_t instructions{};
};

LiftResult lift(std::span<const std::uint8_t> console_shader);

}
