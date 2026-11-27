#include <euphoria/shader/gfx10/dxil.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

std::vector<std::uint8_t> fake_bitcode()
{
    std::vector<std::uint8_t> bitcode{0x42, 0x43, 0xC0, 0xDE};
    bitcode.resize(16, 0xAA);
    return bitcode;
}

std::pair<std::uint8_t, std::uint8_t> sm(std::uint8_t major, std::uint8_t minor)
{
    return {major, minor};
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void parses_standard_layout(Context& check)
{
    const auto bytes = build_standard(0, sm(6, 0), 0x100, fake_bitcode());
    const auto p = DxilProgram::parse(bytes);
    check.expect(p.has_value(), "standard program parses");
    if (!p) {
        return;
    }
    check.expect(p->kind == ShaderKind::Pixel, "kind is pixel");
    check.expect(p->shader_model == sm(6, 0), "shader model 6.0");
    check.expect(p->dxil_version == 0x100, "dxil version");
    check.expect(p->is_standard_layout(), "standard layout");
    check.expect(p->xbox_extras() == XboxExtras{}, "no Xbox extras");
}

void detects_xbox_layout(Context& check)
{
    const auto bitcode = fake_bitcode();
    const std::array<std::uint8_t, 8> xdat{'X', 'D', 'A', 'T', 1, 0, 0, 0};
    const std::array<std::uint8_t, 8> trailer{'x', 'b', 'o', 'p', 2, 0, 0, 0};
    const std::size_t size = 24 + xdat.size() + bitcode.size() + trailer.size();
    std::vector<std::uint8_t> bytes;
    put_u32(bytes, (5u << 16) | 0x65);
    put_u32(bytes, static_cast<std::uint32_t>(size / 4));
    bytes.insert(bytes.end(), {'D', 'X', 'I', 'L'});
    put_u32(bytes, 0x105);
    put_u32(bytes, static_cast<std::uint32_t>(16 + xdat.size()));
    put_u32(bytes, static_cast<std::uint32_t>(bitcode.size()));
    bytes.insert(bytes.end(), xdat.begin(), xdat.end());
    bytes.insert(bytes.end(), bitcode.begin(), bitcode.end());
    bytes.insert(bytes.end(), trailer.begin(), trailer.end());

    const auto p = DxilProgram::parse(bytes);
    check.expect(p.has_value(), "Xbox program parses");
    if (!p) {
        return;
    }
    check.expect(p->kind == ShaderKind::Compute, "kind is compute");
    check.expect(p->shader_model == sm(6, 5), "shader model 6.5");
    check.expect(!p->is_standard_layout(), "not the standard layout");
    check.expect(p->xbox_extras() == XboxExtras{.xdat = true, .xbop = true}, "XDAT and xbop found");
    check.expect(std::ranges::equal(p->bitcode, bitcode), "bitcode located");
    check.expect(std::ranges::equal(p->prefix, xdat), "prefix is the XDAT table");
    check.expect(std::ranges::equal(p->trailer, trailer), "trailer follows the bitcode");
}

void rejects_missing_bitcode_magic(Context& check)
{
    const std::array<std::uint8_t, 16> zeros{};
    const auto bytes = build_standard(0, sm(6, 0), 0x100, zeros);
    const auto p = DxilProgram::parse(bytes);
    check.expect(!p && p.error() == ShaderError::Malformed("bitcode lacks LLVM magic"), "missing magic is rejected");
}

void rejects_bad_sizes(Context& check)
{
    auto bytes = build_standard(0, sm(6, 0), 0x100, fake_bitcode());
    auto oversized = bytes;
    oversized[4] = 0xFF;
    check.expect(!DxilProgram::parse(oversized), "program size beyond the part is rejected");
    auto no_magic = bytes;
    no_magic[8] = 'X';
    check.expect(!DxilProgram::parse(no_magic), "missing DXIL magic is rejected");
    auto past_end = bytes;
    past_end[20] = 0xFF;
    check.expect(!DxilProgram::parse(past_end), "bitcode past the end is rejected");
    auto too_small = bytes;
    too_small[16] = 0;
    check.expect(!DxilProgram::parse(too_small), "bitcode inside the header is rejected");
    const auto unknown = DxilProgram::parse(build_standard(0x42, sm(6, 6), 0x106, fake_bitcode()));
    check.expect(unknown && is_unknown(unknown->kind) && static_cast<std::uint16_t>(unknown->kind) == 0x42, "unknown kinds keep their value");
}

}

void register_dxil_tests()
{
    register_case("dxil", "parses_standard_layout", parses_standard_layout);
    register_case("dxil", "detects_xbox_layout", detects_xbox_layout);
    register_case("dxil", "rejects_missing_bitcode_magic", rejects_missing_bitcode_magic);
    register_case("dxil", "rejects_bad_sizes", rejects_bad_sizes);
}

}
