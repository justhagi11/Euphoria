#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <openscarlett/gpu/rdna2_disasm.h>

namespace euphoria::shader {

struct HlslLift {
    bool ok{};
    bool partial{};
    bool stub{};
    std::uint32_t skipped{};
    std::string hlsl;
    std::string error;
    std::uint8_t stage{};
};

HlslLift lift_to_hlsl(std::uint8_t stage, const std::vector<openscarlett::Rdna2Instruction>& instructions,
                      bool lenient = false);

}
