#include <euphoria/shader/gfx10/emit/container.hpp>
#include <euphoria/shader/gfx10/emit/sampler.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <array>
#include <format>
#include <string>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

using namespace euphoria::shader::gfx10::emit;

std::string hex(std::span<const std::uint8_t> bytes)
{
    std::string out;
    for (const std::uint8_t b : bytes) {
        out += std::format("{:02x}", b);
    }
    return out;
}

void md5_core_matches_the_standard(Context& context)
{
    std::array<std::uint8_t, 64> block{};
    block[0] = 'a';
    block[1] = 'b';
    block[2] = 'c';
    block[3] = 0x80;
    block[56] = 24;
    std::array<std::uint32_t, 4> state{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    md5_block(state, block.data());
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t s : state) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(s >> (8 * i)));
        }
    }
    context.expect_eq(hex(bytes), "900150983cd24fb0d6963f7d28e17f72");
}

void container_layout_and_hash(Context& context)
{
    const std::vector<std::uint8_t> a{1, 2, 3, 4};
    const std::vector<std::uint8_t> b(70, 0x5A);
    const std::array<ContainerPart, 2> parts{part("SFI0", a), part("DXIL", b)};
    const std::vector<std::uint8_t> out = build(parts);
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(out[at]) | (static_cast<std::uint32_t>(out[at + 1]) << 8)
            | (static_cast<std::uint32_t>(out[at + 2]) << 16) | (static_cast<std::uint32_t>(out[at + 3]) << 24);
    };
    context.expect(out.size() == 0x20 + 8 + 8 + 4 + 8 + 70, "total size");
    context.expect(out[0] == 'D' && out[1] == 'X' && out[2] == 'B' && out[3] == 'C', "magic");
    context.expect(u32(20) == 1, "version");
    context.expect(u32(24) == out.size(), "size field");
    context.expect(u32(28) == 2, "part count");
    context.expect(u32(32) == 0x28 && u32(36) == 0x28 + 12, "part offsets");
    const auto digest = hash(std::span<const std::uint8_t>{out}.subspan(20));
    context.expect(std::equal(digest.begin(), digest.end(), out.begin() + 4), "stored digest");
    for (std::size_t n : {0u, 1u, 55u, 56u, 63u, 64u, 119u, 120u, 200u}) {
        std::vector<std::uint8_t> data(n, 0x33);
        const auto h1 = hash(data);
        if (n > 0) {
            data[n / 2] ^= 1;
        }
        const auto h2 = hash(data);
        context.expect(n == 0 || h1 != h2, std::format("hash of {} bytes sees a change", n));
    }
}

void signature_rows_and_names(Context& context)
{
    const std::vector<SigElement> els{
        SigElement{"TEXCOORD", 1, 0, 9, 2, 1u, 0, 2, 3, false},
        SigElement{"SV_Position", 0, 3, 9, 4, 0u, 0, 4, 15, false},
        SigElement{"TEXCOORD", 2, 0, 9, 2, std::nullopt, 0, 1, 0, false},
    };
    const std::vector<std::uint8_t> s = signature(els, true);
    context.expect(s.size() == 8 + 32 * 3 + 9 + 12 + 9, std::format("size {}", s.size()));
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(s[at]) | (static_cast<std::uint32_t>(s[at + 1]) << 8)
            | (static_cast<std::uint32_t>(s[at + 2]) << 16) | (static_cast<std::uint32_t>(s[at + 3]) << 24);
    };
    context.expect(u32(0) == 3 && u32(4) == 8, "header");
    context.expect(u32(8 + 20) == 0 && u32(8 + 12) == 1, "row 0 first: SV_Position");
    context.expect(u32(40 + 20) == 1 && u32(40 + 4) == 8 + 96, "row 1: first TEXCOORD name");
    context.expect(u32(72 + 20) == 0xFFFFFFFFu, "unallocated last");
    context.expect(s[40 + 24] == 0x3 && s[40 + 25] == 0x3, "mask and read mask");
}

void console_compiled_samplers_decode(Context& context)
{
    const StaticSampler point = StaticSampler::from_words({0x3088, 0x00540100, 0x14000080, 0x80000000});
    const StaticSampler want{0, {1, 2, 3}, 0.5f, 1, 4, 2, 1.0f, 5.25f};
    context.expect(point == want, "point sampler");
    const StaticSampler aniso = StaticSampler::from_words({0x369E, 0x00FFF000, 0x18F03E00, 0x80000000});
    context.expect(aniso.filter == 0x55 && aniso.address == std::array<std::uint32_t, 3>{4, 5, 3} && aniso.max_anisotropy == 8,
                   "anisotropic sampler");
    context.expect(aniso.mip_lod_bias == -2.0f && aniso.max_lod == 4095.0f / 256.0f, "anisotropic lod");
    const StaticSampler compare = StaticSampler::from_words({0x3006, 0x00FFF000, 0x14500000, 0x40000000});
    context.expect(compare.filter == 0x14 && compare.address == std::array<std::uint32_t, 3>{4, 1, 1} && compare.border == 1,
                   "comparison sampler");
    const StaticSampler minimum = StaticSampler::from_words({0x20003003, 0x00FFF000, 0x18500000, 0x80000000});
    context.expect(minimum.filter == 0x115 && minimum.address[0] == 5, "minimum sampler");
    const StaticSampler mag_mip = StaticSampler::from_words({0x3002, 0, 0x18100000, 0x80000000});
    context.expect(mag_mip.filter == 0x05 && mag_mip.max_lod == 0.0f, "mag mip sampler");
    const StaticSampler max_aniso = StaticSampler::from_words({0x40003800, 0x00FFF000, 0x18F00F00, 0x80000000});
    context.expect(max_aniso.filter == 0x1D5 && max_aniso.max_anisotropy == 16 && max_aniso.mip_lod_bias == 15.0f,
                   "maximum anisotropic sampler");
}

}

void register_container_tests()
{
    register_case("container", "md5_core_matches_the_standard", md5_core_matches_the_standard);
    register_case("container", "container_layout_and_hash", container_layout_and_hash);
    register_case("container", "signature_rows_and_names", signature_rows_and_names);
    register_case("sampler", "console_compiled_samplers_decode", console_compiled_samplers_decode);
}

}
