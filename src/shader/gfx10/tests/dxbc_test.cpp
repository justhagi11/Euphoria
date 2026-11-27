#include <euphoria/shader/gfx10/dxbc.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

void round_trips_parts(Context& check)
{
    const std::array<std::uint8_t, 4> sfi0{1, 2, 3, 4};
    const std::array<std::uint8_t, 20> hash = [] {
        std::array<std::uint8_t, 20> bytes{};
        bytes.fill(9);
        return bytes;
    }();
    const auto bytes = build({{FourCC::SFI0, sfi0}, {FourCC::HASH, hash}});
    const auto c = Container::parse(bytes);
    check.expect(c.has_value(), "container parses");
    if (!c) {
        return;
    }
    check.expect(c->parts.size() == 2, "two parts");
    const Part* first = c->part(FourCC::SFI0);
    check.expect(first != nullptr && std::ranges::equal(first->data, sfi0), "SFI0 data round trips");
    const Part* second = c->part(FourCC::HASH);
    check.expect(second != nullptr && second->data.size() == 20, "HASH has 20 bytes");
    check.expect(c->part(FourCC::DXIL) == nullptr, "no DXIL part");
    check.expect(FourCC::SFI0.to_string() == "SFI0", "FourCC prints as text");
    check.expect(FourCC{{'A', 0, 0x7F, 'B'}}.to_string() == "A..B", "non-graphic FourCC bytes print as dots");
}

void rejects_bad_input(Context& check)
{
    const std::array<std::uint8_t, 4> nope{'n', 'o', 'p', 'e'};
    const auto not_dxbc = Container::parse(nope);
    check.expect(!not_dxbc && not_dxbc.error() == ShaderError::NotDxbc(), "short input is not DXBC");
    const std::array<std::uint8_t, 8> zeros{};
    auto bytes = build({{FourCC::SFI0, zeros}});
    const std::size_t last = bytes.size() - 1;
    bytes[0x20] = 0xFF;
    check.expect(!Container::parse(bytes), "corrupt part offset is rejected");
    bytes.resize(last);
    check.expect(!Container::parse(bytes), "truncated container is rejected");
}

void reads_little_endian(Context& check)
{
    const std::array<std::uint8_t, 9> data{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    check.expect(u16_at(data, 0) == std::uint16_t{0x0201}, "u16_at");
    check.expect(u32_at(data, 1) == std::uint32_t{0x05040302}, "u32_at");
    check.expect(u64_at(data, 1) == std::uint64_t{0x0908070605040302}, "u64_at");
    check.expect(!u32_at(data, 6) && u32_at(data, 6).error() == ShaderError::Malformed("truncated"), "u32_at past the end is truncated");
    check.expect(!u64_at(data, ~std::size_t{0}), "u64_at with a huge offset fails");
}

}

void register_dxbc_tests()
{
    register_case("dxbc", "round_trips_parts", round_trips_parts);
    register_case("dxbc", "rejects_bad_input", rejects_bad_input);
    register_case("dxbc", "reads_little_endian", reads_little_endian);
}

}
