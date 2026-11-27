#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>
#include <euphoria/shader/gfx10/structure.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

using ir::Program;
using ir::Reg;
using ir::Value;
using structure::Atom;
using structure::Bool;
using structure::Bools;
using structure::LaneExit;
using structure::Meets;
using structure::Structured;

constexpr std::uint32_t kEndpgm = 0xBF810000u;

std::optional<Program> build_words(const std::vector<std::uint32_t>& words, bool wave64)
{
    auto built = ir::build(bytes(words), wave64, 256);
    if (!built) {
        return std::nullopt;
    }
    return std::move(*built);
}

std::size_t guarded(const Structured& s)
{
    std::size_t count = 0;
    for (const auto& row : s.guards) {
        count += static_cast<std::size_t>(std::ranges::count_if(row, [](const auto& g) { return g.has_value(); }));
    }
    return count;
}

bool only(const std::vector<Atom>& atoms, Atom::Kind kind)
{
    return atoms.size() == 1 && atoms[0].kind == kind;
}

bool meet_anew(const Bools& b, std::vector<std::pair<Bool, bool>> all, std::size_t& budget)
{
    for (const auto& [x, holds] : all) {
        if (x == (holds ? Bool::False : Bool::True)) {
            return false;
        }
    }
    std::erase_if(all, [](const auto& e) { return e.first == Bool::False || e.first == Bool::True; });
    if (all.empty() || budget == 0) {
        return true;
    }
    --budget;
    std::uint32_t v = structure::TERMINAL;
    for (const auto& [x, holds] : all) {
        v = std::min(v, b.top(x));
    }
    for (const bool value : {false, true}) {
        std::vector<std::pair<Bool, bool>> next;
        next.reserve(all.size());
        for (const auto& [x, holds] : all) {
            const auto [lo, hi] = b.split(x, v);
            next.emplace_back(value ? hi : lo, holds);
        }
        if (meet_anew(b, std::move(next), budget)) {
            return true;
        }
    }
    return false;
}

void a_search_that_remembers_answers_as_one_made_anew(Context& context)
{
    Bools b;
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto random = [&](std::size_t n) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::size_t>(state % n);
    };
    std::vector<Bool> f;
    for (std::uint64_t i = 0; i < 12; ++i) {
        f.push_back(b.atom(Atom::Constant(i + 2), 0));
    }
    f.push_back(Bool::False);
    f.push_back(Bool::True);
    for (int i = 0; i < 120; ++i) {
        const Bool x = f[random(f.size())];
        const Bool y = f[random(f.size())];
        Bool made{};
        switch (random(4)) {
        case 0:
            made = b.and_(x, y);
            break;
        case 1:
            made = b.or_(x, y);
            break;
        case 2:
            made = b.xor_(x, y);
            break;
        default:
            made = b.not_(x);
            break;
        }
        f.push_back(made);
    }
    Meets seen;
    std::size_t none = 0;
    std::size_t some = 0;
    std::size_t gave_up = 0;
    std::size_t mismatches = 0;
    for (int round = 0; round < 1500; ++round) {
        std::vector<std::pair<Bool, bool>> all;
        const std::size_t count = 2 + random(4);
        for (std::size_t i = 0; i < count; ++i) {
            const Bool x = f[random(f.size())];
            all.emplace_back(x, random(2) == 0);
        }
        for (const std::size_t budget : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
                                         std::size_t{5}, std::size_t{9}, std::size_t{20}, std::size_t{60},
                                         std::size_t{250}, std::size_t{1000}, structure::MAX_MEET}) {
            std::size_t left = budget;
            const bool anew = meet_anew(b, all, left);
            for (const bool fresh : {false, true}) {
                Meets empty;
                Meets& use = fresh ? empty : seen;
                std::vector<std::uint32_t> work;
                for (const auto& [x, holds] : all) {
                    work.push_back(x.id << 1 | static_cast<std::uint32_t>(holds));
                }
                std::size_t mine = budget;
                const bool found = b.meet_in(0, work, mine, use);
                if (found != anew || !work.empty() || (!found && mine != left)) {
                    ++mismatches;
                }
            }
            if (b.meet_within(all, budget, seen) != anew) {
                ++mismatches;
            }
            if (!anew) {
                ++none;
            } else {
                std::size_t unlimited = std::numeric_limits<std::size_t>::max();
                if (meet_anew(b, all, unlimited)) {
                    ++some;
                } else {
                    ++gave_up;
                }
            }
        }
    }
    context.expect(mismatches == 0, std::format("{} answers differ from the search made anew", mismatches));
    context.expect(none > 100 && some > 100 && gave_up > 100, std::format("{} {} {}", none, some, gave_up));
    context.expect(seen.none.size() > 100, "searches were remembered");
}

void divergent_if_becomes_a_lane_branch(Context& context)
{
    const auto p = build_words(
        {
            0x7D840080,
            0xBE803C6A,
            0xBF880001,
            0x7E020280,
            0x887E007E,
            0x7E040280,
            kEndpgm,
        },
        false);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure(*p);
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    const LaneExit exit = s->exits[0];
    if (!context.expect(exit.kind == LaneExit::Kind::Branch && exit.taken == 2 && exit.not_taken == 1,
                        "block 0 branches to 2, else 1")) {
        return;
    }
    const auto cmp = p->blocks[0].insts[0].write(Reg::S(ir::VCC));
    context.expect(cmp.has_value() && s->bools.node(exit.when) == std::tuple{Atom::Lane(*cmp), Bool::True, Bool::False},
                   "skipped exactly where the compare fails");
    context.expect(guarded(*s) == 0, "no guards");
}

void exec_narrowed_without_a_branch_leaves_a_guard(Context& context)
{
    const auto p = build_words({0x7DA40080, 0x7E020280, kEndpgm}, true);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure(*p);
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    context.expect(!s->guards[0][0].has_value(), "the compare runs in every lane");
    const auto g = s->guards[0][1];
    context.expect(g.has_value() && only(s->bools.support(*g), Atom::Kind::Lane), "guard on the lane's compare");
}

void divergent_loop_exits_per_lane(Context& context)
{
    const auto p = build_words(
        {
            0xBE820480,
            0x4A000081,
            0x7D840080,
            0x8882026A,
            0x8AFE027E,
            0xBF89FFFB,
            0x88FE027E,
            0x7E020280,
            kEndpgm,
        },
        true);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure(*p);
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    context.expect(guarded(*s) == 0, "no guards");
    const LaneExit exit = s->exits[1];
    if (!context.expect(exit.kind == LaneExit::Kind::Branch && exit.taken == 1 && exit.not_taken == 2,
                        "block 1 loops back")) {
        return;
    }
    const auto node = s->bools.node(exit.when);
    context.expect(node.has_value() && std::get<0>(*node).kind == Atom::Kind::Lane && std::get<1>(*node) == Bool::True
                       && std::get<2>(*node) == Bool::False,
                   "back while the lane's compare fails");
}

void a_select_on_the_condition_a_lane_loop_ended_with_takes_its_second_operand(Context& context)
{
    const std::uint32_t cnd = 256 | (256 << 9) | (3 << 18);
    const auto p = build_words(
        {
            0xBE83037E,
            0xBE9C037E,
            0x7E3A0507,
            0x7DA40E1D,
            0xBE9C471C,
            0xBF89FFFC,
            0xBEFE0303,
            0x8503807E,
            0xD5010001,
            cnd,
            kEndpgm,
        },
        false);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure(*p);
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    const ir::Inst* select = nullptr;
    for (const auto& block : p->blocks) {
        for (const auto& inst : block.insts) {
            if (inst.name() == "s_cselect_b32") {
                select = &inst;
            }
        }
    }
    if (!context.expect(select != nullptr, "the select")) {
        return;
    }
    const auto mask = select->write(Reg::S(3));
    const auto found = mask ? s->masks.find(*mask) : s->masks.end();
    context.expect(found != s->masks.end() && found->second == Bool::False, "the select is the empty mask");
}

void scalar_branches_stay_uniform(Context& context)
{
    const auto p = build_words({0xBF060100, 0xBF850001, 0x7E020280, kEndpgm}, true);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure(*p);
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    const LaneExit exit = s->exits[0];
    context.expect(exit.kind == LaneExit::Kind::Branch && only(s->bools.support(exit.when), Atom::Kind::Uniform),
                   "uniform branch");
    context.expect(guarded(*s) == 0, "no guards");
}

bool is_the_compare(const Structured& s, Value lo)
{
    const auto found = s.masks.find(lo);
    if (found == s.masks.end()) {
        return false;
    }
    return s.bools.node(found->second) == std::tuple{Atom::Lane(lo), Bool::False, Bool::True};
}

void a_mask_whose_high_dword_is_rewritten_is_not_the_compare(Context& context)
{
    for (const std::uint32_t reader : {0x02020480u, 0x50020480u}) {
        const auto p = build_words({0x7D840080, 0xBEEB0380, reader, kEndpgm}, true);
        if (!context.expect(p.has_value(), "build")) {
            return;
        }
        const auto s = structure::structure(*p);
        if (!context.expect(s.has_value(), "structure")) {
            return;
        }
        const auto lo = p->blocks[0].insts[2].read(Reg::S(ir::VCC));
        if (!context.expect(lo.has_value() && p->blocks[0].insts[0].write(Reg::S(ir::VCC)) == lo,
                            "the reader sees the compare's low dword")) {
            return;
        }
        context.expect(!is_the_compare(*s, *lo), std::format("{:#x}: vcc_hi was zeroed after the compare", reader));
    }
}

void a_mask_joined_over_a_back_edge_with_a_rewritten_high_dword_is_not_the_compare(Context& context)
{
    const auto p = build_words({0x7D840080, 0x02020480, 0x7D840280, 0xBEEB0380, 0xBF85FFFC, kEndpgm}, true);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure(*p);
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    const auto lo = p->blocks[1].insts[1].write(Reg::S(ir::VCC));
    context.expect(lo.has_value() && !is_the_compare(*s, *lo), "the joined mask is not the compare");
}

void irreducible_loops_are_split(Context& context)
{
    const auto p = build_words({0xBF060100, 0xBF850001, 0x80028102, 0xBF84FFFE, kEndpgm}, false);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    if (!context.expect(p->blocks.size() > 4, "five blocks")) {
        return;
    }
    const auto& copy = p->blocks[4];
    context.expect(copy.offset == 8 && copy.exit == ir::Exit::Jump(2), "the entry into 8 gets a copy of it");
    context.expect(p->blocks[0].exit.successors() == std::vector<std::size_t>{2, 4}, "entry successors");
    context.expect(p->blocks[1].preds == std::vector<std::size_t>{2}, "block 1 preds");
    context.expect(structure::structure(*p).has_value(), "structures");
}

const ir::Phi* phi_for(const Program& p, Reg reg)
{
    for (const auto& block : p.blocks) {
        for (const auto& phi : block.phis) {
            if (phi.reg == reg) {
                return &phi;
            }
        }
    }
    return nullptr;
}

void check_phi_edges(Context& context, const Structured& s, const ir::Phi& phi, bool needs_phi_atom)
{
    const auto found = s.phi_masks.find(phi.value);
    if (!context.expect(found != s.phi_masks.end(), "edge masks recorded")) {
        return;
    }
    const auto& edges = found->second;
    context.expect(edges.size() == 2, "two edges");
    context.expect(std::ranges::find(edges, Bool::False) != edges.end(), "an empty edge");
    const bool lane = std::ranges::any_of(edges, [&](Bool e) {
        const std::vector<Atom> support = s.bools.support(e);
        if (needs_phi_atom) {
            return std::ranges::find(support, Atom::Phi(phi.value)) != support.end()
                && std::ranges::any_of(support, [](const Atom& a) { return a.kind == Atom::Kind::Lane; });
        }
        return only(support, Atom::Kind::Lane);
    });
    context.expect(lane, "an edge of the lane's compare");
}

void mask_phi_joining_constant_and_compare_records_edge_bools(Context& context)
{
    const std::uint32_t cnd = 258 | (259 << 9) | (2 << 18);
    const auto p = build_words(
        {0xBF068000, 0xBF850002, 0xBE820380, 0xBF820002, 0xD4C20002, 0x00020080, 0xD5010001, cnd, kEndpgm}, false);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure(*p);
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    const ir::Phi* phi = phi_for(*p, Reg::S(2));
    if (!context.expect(phi != nullptr, "mask phi for s2")) {
        return;
    }
    const auto found = s->masks.find(phi->value);
    context.expect(found != s->masks.end()
                       && s->bools.node(found->second) == std::tuple{Atom::Phi(phi->value), Bool::False, Bool::True},
                   "the join is its phi atom");
    check_phi_edges(context, *s, *phi, false);
}

void loop_mask_accumulator_from_zero_tracks_phi_and_edges(Context& context)
{
    const std::uint32_t cnd = 258 | (259 << 9) | (2 << 18);
    const auto p = build_words({0xBE820480, 0xBE840380, 0xD5010001, cnd, 0x7D840080, 0x8882026A, 0x80048104,
                                0xBF0A8404, 0xBF85FFF9, 0xD5010004, cnd, kEndpgm},
                               true);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure(*p);
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    const ir::Phi* phi = phi_for(*p, Reg::S(2));
    if (!context.expect(phi != nullptr, "loop mask phi for s[2:3]")) {
        return;
    }
    context.expect(s->masks.contains(phi->value), "the phi is a mask");
    check_phi_edges(context, *s, *phi, true);
}

void loop_mask_phi_reset_to_constant_on_back_edge(Context& context)
{
    const std::uint32_t cnd = 258 | (259 << 9) | (2 << 18);
    const auto p = build_words({0xD4C20002, 0x00020080, 0xBE840380, 0xD5010001, cnd, 0xBE820380, 0x80048104,
                                0xBF0A8404, 0xBF85FFFA, kEndpgm},
                               false);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure(*p);
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    const ir::Phi* phi = phi_for(*p, Reg::S(2));
    if (!context.expect(phi != nullptr, "loop mask phi for s2")) {
        return;
    }
    context.expect(s->masks.contains(phi->value), "the phi is a mask");
    check_phi_edges(context, *s, *phi, false);
}

void wide_branches_keep_exec_as_a_guard(Context& context)
{
    const auto p = build_words(
        {
            0x7D840080,
            0xBE803C6A,
            0xBF880001,
            0x7E020280,
            0x887E007E,
            0x7E040280,
            kEndpgm,
        },
        false);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const auto s = structure::structure_wide(*p, {0});
    if (!context.expect(s.has_value(), "structure")) {
        return;
    }
    context.expect(s->wide.contains(0), "block 0 is wide");
    context.expect(s->wave_any[0] == std::optional<bool>{false}, "execz is taken when no lane is set");
    context.expect(s->guards[1][0].has_value(), "the move inside keeps EXEC as its guard");
}

}

void register_structure_tests()
{
    register_case("structure", "a_search_that_remembers_answers_as_one_made_anew",
                  a_search_that_remembers_answers_as_one_made_anew);
    register_case("structure", "divergent_if_becomes_a_lane_branch", divergent_if_becomes_a_lane_branch);
    register_case("structure", "exec_narrowed_without_a_branch_leaves_a_guard",
                  exec_narrowed_without_a_branch_leaves_a_guard);
    register_case("structure", "divergent_loop_exits_per_lane", divergent_loop_exits_per_lane);
    register_case("structure", "a_select_on_the_condition_a_lane_loop_ended_with_takes_its_second_operand",
                  a_select_on_the_condition_a_lane_loop_ended_with_takes_its_second_operand);
    register_case("structure", "scalar_branches_stay_uniform", scalar_branches_stay_uniform);
    register_case("structure", "a_mask_whose_high_dword_is_rewritten_is_not_the_compare",
                  a_mask_whose_high_dword_is_rewritten_is_not_the_compare);
    register_case("structure", "a_mask_joined_over_a_back_edge_with_a_rewritten_high_dword_is_not_the_compare",
                  a_mask_joined_over_a_back_edge_with_a_rewritten_high_dword_is_not_the_compare);
    register_case("structure", "irreducible_loops_are_split", irreducible_loops_are_split);
    register_case("structure", "mask_phi_joining_constant_and_compare_records_edge_bools",
                  mask_phi_joining_constant_and_compare_records_edge_bools);
    register_case("structure", "loop_mask_accumulator_from_zero_tracks_phi_and_edges",
                  loop_mask_accumulator_from_zero_tracks_phi_and_edges);
    register_case("structure", "loop_mask_phi_reset_to_constant_on_back_edge",
                  loop_mask_phi_reset_to_constant_on_back_edge);
    register_case("structure", "wide_branches_keep_exec_as_a_guard", wide_branches_keep_exec_as_a_guard);
}

}
