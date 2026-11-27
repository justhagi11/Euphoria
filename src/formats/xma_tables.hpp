#pragma once

#include <array>
#include <cstdint>

namespace euphoria::formats::xma::tables {

struct Code {
    std::uint16_t symbol;
    std::uint8_t length;
};

extern const std::array<std::uint16_t, 28> kCriticalFreq;
extern const std::array<Code, 121> kScale;
extern const std::array<Code, 120> kScaleRl;
extern const std::array<std::uint8_t, 120> kScaleRlRun;
extern const std::array<std::uint8_t, 120> kScaleRlLevel;
extern const std::array<std::uint8_t, 272> kCoef0Lens;
extern const std::array<std::uint16_t, 272> kCoef0Syms;
extern const std::array<Code, 244> kCoef1;
extern const std::array<std::uint16_t, 272> kCoef0Run;
extern const std::array<float, 272> kCoef0Level;
extern const std::array<std::uint16_t, 244> kCoef1Run;
extern const std::array<float, 244> kCoef1Level;
extern const std::array<std::uint8_t, 127> kVec4Lens;
extern const std::array<std::uint16_t, 127> kVec4Syms;
extern const std::array<Code, 137> kVec2;
extern const std::array<Code, 101> kVec1;

}
