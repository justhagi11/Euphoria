#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <format>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

using ir::Exit;
using ir::Inst;
using ir::Program;
using ir::Reg;
using ir::Value;

constexpr std::uint32_t kEndpgm = 0xBF810000u;

std::optional<Program> build_words(const std::vector<std::uint32_t>& words, bool wave64)
{
    auto built = ir::build(bytes(words), wave64, 256);
    if (!built) {
        return std::nullopt;
    }
    return std::move(*built);
}

std::optional<Value> input(const Program& p, Reg reg)
{
    for (const auto& [r, value] : p.inputs) {
        if (r == reg) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<Program> skip(const std::vector<std::uint32_t>& body, const std::vector<std::uint32_t>& after)
{
    std::vector<std::uint32_t> w{
        0xBE940306,
        0x7D880000,
        0xBE8C246A,
        0xBF880000u | static_cast<std::uint32_t>(body.size()),
    };
    w.insert(w.end(), body.begin(), body.end());
    w.push_back(0xBEFE040C);
    w.insert(w.end(), after.begin(), after.end());
    w.push_back(kEndpgm);
    return build_words(w, true);
}

std::size_t skips(const Program& p)
{
    std::size_t count = 0;
    for (const auto& block : p.blocks) {
        if (!block.insts.empty() && block.insts.back().name() == "s_cbranch_execz") {
            ++count;
        }
    }
    return count;
}

void skips_left_out(Context& context)
{
    constexpr std::uint32_t kSet = 0xBE860385;
    constexpr std::uint32_t kUse = 0x7E020206;
    constexpr std::uint32_t kBack = 0xBE860314;
    constexpr std::uint32_t kRead = 0x7E040206;

    const auto kept = skip({kSet, kUse}, {kRead});
    if (context.expect(kept.has_value(), "kept builds")) {
        context.expect(skips(*kept) == 1, "kept has one skip");
        context.expect(!ir::without_skips(*kept, false), "kept: shallow keeps the branch");
        context.expect(!ir::without_skips(*kept, true), "kept: deep keeps the branch");
    }
    const auto back = skip({kSet, kUse, kBack}, {kRead});
    if (context.expect(back.has_value(), "back builds")) {
        context.expect(!ir::without_skips(*back, false), "back: shallow keeps the branch");
        const auto deep = ir::without_skips(*back, true);
        context.expect(deep && skips(*deep) == 0, "back: deep leaves the branch out");
    }
    const auto unread = skip({kSet, kUse}, {});
    if (context.expect(unread.has_value(), "unread builds")) {
        const auto shallow = ir::without_skips(*unread, false);
        const auto deep = ir::without_skips(*unread, true);
        context.expect(shallow && skips(*shallow) == 0, "unread: shallow leaves the branch out");
        context.expect(deep && skips(*deep) == 0, "unread: deep leaves the branch out");
    }
    const auto idle = skip({kSet, kUse}, {0xBEFE0406, 0xBEFE040C, 0x7E040200});
    if (context.expect(idle.has_value(), "idle builds")) {
        context.expect(!ir::without_skips(*idle, false), "idle: shallow keeps the branch");
        const auto deep = ir::without_skips(*idle, true);
        context.expect(deep && skips(*deep) == 0, "idle: deep leaves the branch out");
    }
}

void joins_get_phis_only_where_values_differ(Context& context)
{
    const std::vector<std::uint32_t> code{0xBF060100, 0xBF850001, 0xBE820380, 0x7E000202, 0x7E020200, kEndpgm};
    for (const bool wave64 : {false, true}) {
        const auto built = build_words(code, wave64);
        if (!context.expect(built && built->blocks.size() == 3, std::format("three blocks (wave64 {})", wave64))) {
            continue;
        }
        const Program& p = *built;
        const auto& b0 = p.blocks[0];
        const auto& b1 = p.blocks[1];
        const auto& b2 = p.blocks[2];
        context.expect(b0.exit == Exit::Branch(2, 1), "b0 branches to 2 or 1");
        context.expect(b1.exit == Exit::Jump(2), "b1 jumps to 2");
        context.expect(b2.preds == std::vector<std::size_t>{0, 1}, "b2 preds are 0, 1");
        context.expect(b0.insts.size() >= 2 && b0.insts[1].read(Reg::Scc()) == b0.insts[0].write(Reg::Scc()),
                       "the branch reads the compare's SCC");
        if (!context.expect(b2.phis.size() == 1, "one phi")) {
            continue;
        }
        const auto& phi = b2.phis[0];
        context.expect(phi.reg == Reg::S(2), "phi of s2");
        const auto s2_in = input(p, Reg::S(2));
        const auto s2_set = b1.insts.empty() ? std::nullopt : b1.insts[0].write(Reg::S(2));
        context.expect(s2_in && s2_set && phi.args == std::vector<Value>{*s2_in, *s2_set}, "phi args");
        if (!context.expect(b2.insts.size() == 3, "three instructions in the join")) {
            continue;
        }
        const Inst& mov0 = b2.insts[0];
        const Inst& mov1 = b2.insts[1];
        const Inst& end = b2.insts[2];
        context.expect(mov0.read(Reg::S(2)) == phi.value, "v_mov reads the phi");
        context.expect(mov1.read(Reg::S(0)) == input(p, Reg::S(0)), "s0 is the input");
        context.expect(mov1.read(Reg::V(1)) == input(p, Reg::V(1)), "the old v1 is read");
        context.expect(mov1.read(Reg::S(ir::EXEC)) == input(p, Reg::S(ir::EXEC)), "EXEC is read");
        context.expect(mov1.read(Reg::S(ir::EXEC + 1)).has_value() == wave64, "EXEC_HI is read in wave64");
        context.expect(end.name() == "s_endpgm" && b2.exit == Exit::End(), "ends");
    }
}

void in_place_scalar_ops_read_their_destination(Context& context)
{
    const auto built = build_words({0xBE8D1D94, 0xB78E0010, 0xBE8F0380, kEndpgm}, false);
    if (!context.expect(built && built->blocks[0].insts.size() == 4, "four instructions")) {
        return;
    }
    const Program& p = *built;
    const auto& insts = p.blocks[0].insts;
    context.expect(insts[0].read(Reg::S(13)) == input(p, Reg::S(13)), "s_bitset1_b32 reads s13");
    context.expect(insts[1].read(Reg::S(14)) == input(p, Reg::S(14)), "s_addk_i32 reads s14");
    context.expect(!insts[2].read(Reg::S(15)), "s_mov_b32 does not read s15");
}

void loops_carry_values_through_phis(Context& context)
{
    const auto built = build_words({0xBE800380, 0x80008100, 0xBF078400, 0xBF85FFFD, kEndpgm}, true);
    if (!context.expect(built && built->blocks.size() == 3, "three blocks")) {
        return;
    }
    const Program& p = *built;
    context.expect(p.inputs.empty(), "no inputs");
    const auto& b0 = p.blocks[0];
    const auto& b1 = p.blocks[1];
    const auto& b2 = p.blocks[2];
    context.expect(b1.preds == std::vector<std::size_t>{0, 1}, "loop preds");
    context.expect(b1.exit == Exit::Branch(1, 2), "loop exit");
    if (!context.expect(b1.phis.size() == 1 && b1.insts.size() >= 2, "one phi")) {
        return;
    }
    const auto& phi = b1.phis[0];
    const Inst& add = b1.insts[0];
    context.expect(phi.reg == Reg::S(0), "phi of s0");
    const auto first = b0.insts[0].write(Reg::S(0));
    const auto again = add.write(Reg::S(0));
    context.expect(first && again && phi.args == std::vector<Value>{*first, *again}, "phi args");
    context.expect(add.read(Reg::S(0)) == phi.value, "add reads the phi");
    context.expect(b1.insts[1].read(Reg::S(0)) == add.write(Reg::S(0)), "compare reads the sum");
    context.expect(!b2.insts.empty() && b2.insts[0].name() == "s_endpgm", "exit ends");
}

void drops_scheduling_and_follows_far_jumps(Context& context)
{
    const auto built =
        build_words({0xBF800000, 0xBE881F00, 0x8008FF08, 20, 0x82098009, 0xBE802008, 0xFFFFFFFFu, kEndpgm}, false);
    if (!context.expect(built && built->blocks.size() >= 2, "builds")) {
        return;
    }
    const Program& p = *built;
    std::string names;
    for (const Inst& inst : p.blocks[0].insts) {
        names += std::string{inst.name()} + " ";
    }
    context.expect_eq(names, "s_getpc_b64 s_add_u32 s_addc_u32 s_setpc_b64 ");
    context.expect(p.blocks[0].exit == Exit::Jump(1), "far jump to block 1");
    context.expect(p.blocks[1].offset == 28, "block 1 at 28");
    const auto& i = p.blocks[0].insts;
    context.expect(i.size() == 4 && i[2].read(Reg::Scc()) == i[1].write(Reg::Scc()), "s_addc_u32 reads the carry");
}

void reports_what_it_cannot_express(Context& context)
{
    const auto built = ir::build(bytes(std::vector<std::uint32_t>{0xBE802E01, kEndpgm}), false, 256);
    context.expect(!built && built.error() == ShaderError::Unsupported(0, "s_movrels_b32"),
                   built ? "built" : built.error().message());
}

void relative_vgpr_access_spans_up_to_the_vgpr_count(Context& context)
{
    const std::vector<std::uint8_t> code = bytes(std::vector<std::uint32_t>{0x7E048501, 0x7E008704, kEndpgm});
    const auto built = ir::build(code, false, 6);
    if (!context.expect(built && built->blocks[0].insts.size() == 3, "builds")) {
        return;
    }
    const auto vgprs = [](const std::vector<std::pair<Reg, Value>>& regs) {
        std::vector<std::uint8_t> out;
        for (const auto& [reg, value] : regs) {
            if (reg.is_v()) {
                out.push_back(reg.index);
            }
        }
        return out;
    };
    const Inst& movreld = built->blocks[0].insts[0];
    const Inst& movrels = built->blocks[0].insts[1];
    context.expect(vgprs(movreld.writes) == std::vector<std::uint8_t>{2, 3, 4, 5}, "movreld writes v2-v5");
    context.expect(vgprs(movreld.reads) == std::vector<std::uint8_t>{1, 2, 3, 4, 5}, "movreld reads v1-v5");
    context.expect(movreld.read(Reg::S(ir::M0)).has_value(), "movreld reads M0");
    context.expect(vgprs(movrels.writes) == std::vector<std::uint8_t>{0}, "movrels writes v0");
    context.expect(vgprs(movrels.reads) == std::vector<std::uint8_t>{4, 5, 0}, "movrels reads v4, v5, v0");
    context.expect(movrels.read(Reg::V(4)) == movreld.write(Reg::V(4)), "movrels sees movreld's v4");
    context.expect(!ir::build(code, false, 4), "v4 is past 4 VGPRs");
}

}

void register_ir_tests()
{
    register_case("ir", "a_skip_is_left_out_when_the_scalars_it_changes_come_back_or_are_not_read", skips_left_out);
    register_case("ir", "joins_get_phis_only_where_values_differ", joins_get_phis_only_where_values_differ);
    register_case("ir", "in_place_scalar_ops_read_their_destination", in_place_scalar_ops_read_their_destination);
    register_case("ir", "loops_carry_values_through_phis", loops_carry_values_through_phis);
    register_case("ir", "drops_scheduling_and_follows_far_jumps", drops_scheduling_and_follows_far_jumps);
    register_case("ir", "reports_what_it_cannot_express", reports_what_it_cannot_express);
    register_case("ir", "relative_vgpr_access_spans_up_to_the_vgpr_count",
                  relative_vgpr_access_spans_up_to_the_vgpr_count);
}

}
