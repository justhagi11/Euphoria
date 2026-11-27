#pragma once

#include <cstdint>
#include <vector>

namespace openscarlett {

struct Xs2ShaderContainer;

bool ExtractXdatDxilChunkToXs2(const std::vector<std::uint8_t>& dxil_data,
                               Xs2ShaderContainer& out_xs2);

}
