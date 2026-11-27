#pragma once

#include <cstdint>

namespace openscarlett {

enum class ShaderStage : uint32_t {
    Unknown = 0,
    Vertex = 1,
    Pixel = 2,
    Compute = 3,
    Geometry = 4,
    Hull = 5,
    Domain = 6,
    Library = 7,
    Mesh = 8,
    Amplification = 9,
};

}  // namespace openscarlett
