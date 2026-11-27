#pragma once

#include <array>
#include <cstdint>

namespace euphoria::shader::gfx10::emit {

struct StaticSampler {
    std::uint32_t filter{0};
    std::array<std::uint32_t, 3> address{};
    float mip_lod_bias{0.0f};
    std::uint32_t max_anisotropy{0};
    std::uint32_t comparison{0};
    std::uint32_t border{0};
    float min_lod{0.0f};
    float max_lod{0.0f};

    static StaticSampler from_words(std::array<std::uint32_t, 4> w) noexcept;

    friend bool operator==(const StaticSampler&, const StaticSampler&) = default;
};

}
