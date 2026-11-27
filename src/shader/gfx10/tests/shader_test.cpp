#include <euphoria/shader/gfx10/emit/container.hpp>
#include <euphoria/shader/gfx10/emit/shader.hpp>
#include <euphoria/shader/gfx10/emit/validator.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <string>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

using namespace euphoria::shader::gfx10::emit;

std::vector<std::uint8_t> constant_color()
{
    Shader s{true};
    s.outputs.push_back(SigElement{"SV_Target", 0, 16, 9, 0, 0u, 0, 4, 15, false});
    const std::size_t b = s.entry;
    const std::array<float, 4> colour{1.0f, 0.5f, 0.25f, 1.0f};
    for (std::size_t c = 0; c < colour.size(); ++c) {
        const V a0 = s.m.i32(0);
        const V a1 = s.m.i32(0);
        const V a2 = s.m.i8(static_cast<std::uint8_t>(c));
        const V a3 = s.m.f32(std::bit_cast<std::uint32_t>(colour[c]));
        s.op(b, "storeOutput.f32", Attr::NoUnwind, s.t.void_, 5, {a0, a1, a2, a3});
    }
    s.m.terminate(b, Term::Ret());
    return s.finish();
}

std::vector<std::uint8_t> compute_store()
{
    Shader s{false};
    s.threads = std::array<std::uint32_t, 3>{64, 1, 1};
    const std::size_t b = s.entry;
    const V h = s.root_uav_handle(0);
    const V zero = s.m.i32(0);
    const V tid = s.op(b, "threadId.i32", Attr::ReadNone, s.t.i32, 93, {zero});
    const V two = s.m.i32(2);
    const V byte = s.m.bin(b, Bin::Shl, tid, two);
    const V three = s.m.i32(3);
    const V scaled = s.m.bin(b, Bin::Mul, tid, three);
    const V base = s.m.i32(0x0ABC0000u);
    const V value = s.m.bin(b, Bin::Add, scaled, base);
    const V iu = s.m.undef(s.t.i32);
    const V mask = s.m.i8(1);
    const V align = s.m.i32(4);
    s.op(b, "rawBufferStore.i32", Attr::NoUnwind, s.t.void_, 140, {h, byte, iu, value, iu, iu, iu, mask, align});
    s.m.terminate(b, Term::Ret());
    return s.finish();
}

std::uint32_t u32_at(const std::vector<std::uint8_t>& c, std::size_t at)
{
    return static_cast<std::uint32_t>(c[at]) | (static_cast<std::uint32_t>(c[at + 1]) << 8)
        | (static_cast<std::uint32_t>(c[at + 2]) << 16) | (static_cast<std::uint32_t>(c[at + 3]) << 24);
}

std::vector<std::string> part_names(const std::vector<std::uint8_t>& c)
{
    std::vector<std::string> names;
    const std::uint32_t count = u32_at(c, 28);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t at = u32_at(c, 32 + 4 * i);
        names.emplace_back(reinterpret_cast<const char*>(&c[at]), 4);
    }
    return names;
}

void check_container(Context& context, const std::vector<std::uint8_t>& c, const std::vector<std::string>& want)
{
    if (!context.expect(c.size() > 64 && c[0] == 'D' && c[1] == 'X' && c[2] == 'B' && c[3] == 'C', "a container")) {
        return;
    }
    context.expect(u32_at(c, 24) == c.size(), "size field");
    context.expect(part_names(c) == want, "parts in order");
    const auto digest = hash(std::span<const std::uint8_t>{c}.subspan(20));
    context.expect(std::equal(digest.begin(), digest.end(), c.begin() + 4), "hash");
}

void validate_with_dll(Context& context, std::vector<std::uint8_t> c, std::string_view what)
{
    auto validator = Validator::load(Validator::default_path());
    if (!validator) {
        context.expect(true, "no dxil.dll; skipped");
        return;
    }
    const std::vector<std::uint8_t> ours(c.begin() + 4, c.begin() + 20);
    const auto result = validator->validate(c);
    if (!context.expect(result.has_value(), std::format("{} rejected: {}", what, result ? std::string{} : result.error()))) {
        return;
    }
    context.expect(std::equal(ours.begin(), ours.end(), c.begin() + 4), std::format("{}: our hash differs from the validator's", what));
}

void constant_color_container(Context& context)
{
    check_container(context, constant_color(), {"SFI0", "ISG1", "OSG1", "PSV0", "HASH", "DXIL"});
}

void constant_color_validates(Context& context)
{
    validate_with_dll(context, constant_color(), "constant colour");
}

void compute_store_validates(Context& context)
{
    const std::vector<std::uint8_t> c = compute_store();
    check_container(context, c, {"SFI0", "ISG1", "OSG1", "PSV0", "HASH", "DXIL"});
    validate_with_dll(context, c, "compute store");
}

void heap_handles_clamp_and_flag(Context& context)
{
    Shader s{false};
    s.threads = std::array<std::uint32_t, 3>{32, 1, 1};
    const std::size_t b = s.entry;
    const V zero = s.m.i32(0);
    const V tid = s.op(b, "threadId.i32", Attr::ReadNone, s.t.i32, 93, {zero});
    const std::uint32_t old_limit = view_heap_limit();
    set_view_heap_limit(1000);
    const V h = s.heap_handle(b, tid, false, true, {11 | 0x1000, 0});
    set_view_heap_limit(old_limit);
    const auto clamp_name = std::ranges::find_if(s.m.funcs, [](const Func& f) { return f.name == "dx.op.binary.i32"; });
    context.expect(clamp_name != s.m.funcs.end(), "index clamped by UMin");
    const V iu = s.m.undef(s.t.i32);
    const V mask = s.m.i8(1);
    const V align = s.m.i32(4);
    s.op(b, "rawBufferStore.i32", Attr::NoUnwind, s.t.void_, 140, {h, zero, iu, tid, iu, iu, iu, mask, align});
    s.m.terminate(b, Term::Ret());
    context.expect((s.flags & flags::RESOURCE_HEAP) != 0, "resource heap flag");
    context.expect(s.nonuniform_handles == 1, "non-uniform count");
    const std::vector<std::uint8_t> c = s.finish();
    check_container(context, c, {"SFI0", "ISG1", "OSG1", "PSV0", "HASH", "DXIL"});
    const std::uint32_t sfi0 = u32_at(c, u32_at(c, 32) + 8);
    context.expect((sfi0 & 0x2000000) != 0, "SFI0 resource heap feature");
    validate_with_dll(context, c, "heap store");
}

}

void register_shader_tests()
{
    register_case("shader", "constant_color_container", constant_color_container);
    register_case("shader", "constant_color_validates", constant_color_validates);
    register_case("shader", "compute_store_validates", compute_store_validates);
    register_case("shader", "heap_handles_clamp_and_flag", heap_handles_clamp_and_flag);
}

}
