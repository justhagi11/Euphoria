#include <euphoria/shader/gfx10/emit/sampler.hpp>

#include <algorithm>

namespace euphoria::shader::gfx10::emit {
namespace {

constexpr std::uint32_t bits(std::uint32_t word, std::uint32_t at, std::uint32_t n) noexcept
{
    return (word >> at) & ((1u << n) - 1);
}

constexpr std::uint32_t address_mode(std::uint32_t m) noexcept
{
    constexpr std::array<std::uint32_t, 8> kModes{1, 2, 3, 5, 3, 5, 4, 5};
    return kModes[m];
}

float lod(std::uint32_t v) noexcept
{
    return static_cast<float>(v) / 256.0f;
}

}

StaticSampler StaticSampler::from_words(std::array<std::uint32_t, 4> w) noexcept
{
    const std::uint32_t mag = bits(w[2], 20, 2);
    const std::uint32_t min = bits(w[2], 22, 2);
    const std::uint32_t mip = bits(w[2], 26, 2);
    std::uint32_t reduction = 0;
    switch (bits(w[0], 29, 2)) {
    case 1:
        reduction = 2;
        break;
    case 2:
        reduction = 3;
        break;
    default:
        reduction = 0;
        break;
    }
    std::uint32_t filter = 0;
    if (mag >= 2 || min >= 2) {
        filter = 0x55 | (reduction << 7);
    } else {
        filter = ((min & 1) << 4) | ((mag & 1) << 2) | (mip == 2 ? 1u : 0u) | (reduction << 7);
    }
    float min_lod = 0.0f;
    float max_lod = 0.0f;
    if (mip != 0) {
        min_lod = lod(bits(w[1], 0, 12));
        max_lod = lod(bits(w[1], 12, 12));
    }
    StaticSampler s;
    s.filter = filter;
    s.address = {address_mode(bits(w[0], 0, 3)), address_mode(bits(w[0], 3, 3)), address_mode(bits(w[0], 6, 3))};
    s.mip_lod_bias = static_cast<float>(static_cast<std::int32_t>(bits(w[2], 0, 14) << 18) >> 18) / 256.0f;
    s.max_anisotropy = 1u << std::min(bits(w[0], 9, 3), 4u);
    s.comparison = bits(w[0], 12, 3) + 1;
    s.border = bits(w[3], 30, 2) % 3;
    s.min_lod = min_lod;
    s.max_lod = max_lod;
    return s;
}

}
