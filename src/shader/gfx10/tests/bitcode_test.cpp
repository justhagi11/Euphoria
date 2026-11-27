#include <euphoria/shader/gfx10/emit/bitcode.hpp>
#include <euphoria/shader/gfx10/emit/bitcode_census.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <tuple>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

using namespace euphoria::shader::gfx10::emit;

Module synthetic(std::size_t regs, std::size_t stages, std::size_t read)
{
    Module m;
    const Ty i32t = m.int_(32);
    const Ty void_t = m.void_();
    const Ty fty = m.func_ty(i32t, {i32t, i32t});
    const V f = m.function("dx.op.synthetic", fty, Attr::ReadNone, false);
    const Ty main_ty = m.func_ty(void_t, {});
    m.function("main", main_ty, std::nullopt, true);
    std::size_t b = m.new_block();
    std::vector<V> cur;
    for (std::size_t r = 0; r < regs; ++r) {
        cur.push_back(m.i32(static_cast<std::uint32_t>(r)));
    }
    for (std::size_t s = 0; s < stages; ++s) {
        const V c = m.cmp(b, pred::EQ, cur[0], cur[s % regs]);
        const std::size_t t = m.new_block();
        const std::size_t e = m.new_block();
        const std::size_t j = m.new_block();
        m.terminate(b, Term::CondBr(c, t, e));
        const V x = m.call(t, f, {cur[s % regs], cur[(s + 1) % regs]});
        m.terminate(t, Term::Br(j));
        m.terminate(e, Term::Br(j));
        std::vector<V> next;
        for (std::size_t r = 0; r < regs; ++r) {
            const V p = m.phi(j, i32t);
            const V tv = r == s % regs ? x : cur[r];
            m.set_phi(p, {{tv, t}, {cur[r], e}});
            next.push_back(p);
        }
        cur = std::move(next);
        b = j;
    }
    const std::size_t head = m.new_block();
    const std::size_t body = m.new_block();
    const std::size_t latch = m.new_block();
    const std::size_t exit = m.new_block();
    m.terminate(b, Term::Br(head));
    std::vector<V> phis;
    for (std::size_t r = 0; r < regs; ++r) {
        phis.push_back(m.phi(head, i32t));
    }
    const V one = m.i32(1);
    std::vector<V> stepped;
    for (const V p : phis) {
        stepped.push_back(m.bin(body, Bin::Add, p, one));
    }
    const V c = m.cmp(body, pred::IULT, stepped[0], cur[0]);
    m.terminate(head, Term::Br(body));
    m.terminate(body, Term::CondBr(c, latch, exit));
    m.terminate(latch, Term::Br(head));
    for (std::size_t r = 0; r < regs; ++r) {
        m.set_phi(phis[r], {{cur[r], b}, {stepped[r], latch}});
    }
    V acc = stepped[0];
    for (std::size_t i = 1; i < std::max<std::size_t>(read, 1); ++i) {
        acc = m.bin(exit, Bin::Add, acc, stepped[i]);
    }
    m.call(exit, f, {acc, cur[0]});
    m.terminate(exit, Term::Ret());
    return m;
}

std::uint64_t fnv(std::span<const std::uint8_t> bytes)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const std::uint8_t b : bytes) {
        h = (h ^ b) * 0x00000100000001b3ull;
    }
    return h;
}

std::vector<bool> live_by_passes(const Module& m)
{
    std::vector<bool> live(m.insts.size(), false);
    for (const Block& b : m.blocks) {
        for (const std::uint32_t i : b.body) {
            live[i] = true;
        }
        if (b.term && b.term->kind == Term::Kind::CondBr && b.term->cond.is_inst()) {
            live[b.term->cond.index] = true;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < m.insts.size(); ++i) {
            if (!live[i]) {
                continue;
            }
            for_each_operand(m.insts[i].op, [&](V v) {
                if (v.is_inst() && !live[v.index]) {
                    live[v.index] = true;
                    changed = true;
                }
            });
        }
    }
    return live;
}

void vbr_and_blocks(Context& context)
{
    Writer w;
    w.vbr(5, 6);
    w.vbr(100, 6);
    context.expect(w.cur == (5u | (0b100100u << 6) | (3u << 12)), "vbr chunks");
    Writer blocks;
    blocks.enter(8, 3);
    blocks.record(1, {1});
    blocks.exit();
    const std::vector<std::uint8_t> bytes = blocks.finish();
    context.expect(bytes.size() % 4 == 0, "whole words");
    const std::uint32_t length = bytes.size() >= 8
        ? static_cast<std::uint32_t>(bytes[4]) | (static_cast<std::uint32_t>(bytes[5]) << 8)
            | (static_cast<std::uint32_t>(bytes[6]) << 16) | (static_cast<std::uint32_t>(bytes[7]) << 24)
        : 0;
    context.expect(length == 1, "block length word");
    context.expect(signed_operand(-3) == 7, "signed -3");
    context.expect(signed_operand(2) == 4, "signed 2");
}

void worklist_liveness_matches_passes(Context& context)
{
    for (const auto& [regs, stages, read] : std::vector<std::tuple<std::size_t, std::size_t, std::size_t>>{
             {1, 0, 1}, {1, 1, 1}, {4, 3, 1}, {8, 17, 3}, {16, 40, 16}}) {
        const Module m = synthetic(regs, stages, read);
        context.expect(m.live() == live_by_passes(m), std::format("{} regs x {} stages", regs, stages));
    }
}

void unread_phis_are_dropped(Context& context)
{
    Module m;
    const Ty i32t = m.int_(32);
    const std::size_t entry = m.new_block();
    const std::size_t join = m.new_block();
    const V a = m.i32(1);
    const V b = m.i32(2);
    m.terminate(entry, Term::Br(join));
    const V read = m.phi(join, i32t);
    const V unread = m.phi(join, i32t);
    const V cyc_a = m.phi(join, i32t);
    const V cyc_b = m.phi(join, i32t);
    m.set_phi(read, {{a, entry}});
    m.set_phi(unread, {{b, entry}});
    m.set_phi(cyc_a, {{cyc_b, entry}});
    m.set_phi(cyc_b, {{cyc_a, entry}});
    const V sum = m.bin(join, Bin::Add, read, b);
    m.terminate(join, Term::Ret());
    const std::vector<bool> live = m.live();
    context.expect(live[read.index] && live[sum.index], "read phi and sum live");
    context.expect(!live[unread.index] && !live[cyc_a.index] && !live[cyc_b.index], "unread phis dead");
    context.expect(live == live_by_passes(m), "matches passes");
}

void successors_of_each_terminator(Context& context)
{
    context.expect(successors(std::nullopt).empty(), "none");
    context.expect(successors(Term::Ret()).empty(), "ret");
    const Succs br = successors(Term::Br(7));
    context.expect(br.size() == 1 && br[0] == 7, "br");
    const Succs cond = successors(Term::CondBr(V::Const(0), 3, 5));
    context.expect(cond.size() == 2 && cond[0] == 3 && cond[1] == 5, "condbr");
}

void written_bytes_unchanged(Context& context)
{
    struct Pin {
        std::size_t regs;
        std::size_t stages;
        std::size_t read;
        std::size_t len;
        std::uint64_t hash;
    };
    for (const Pin& pin : {Pin{64, 64, 64, 38916, 0x9586730cf9534a45ull}, Pin{128, 256, 32, 295952, 0x89e5de2d64f08785ull}}) {
        const std::vector<std::uint8_t> bytes = synthetic(pin.regs, pin.stages, pin.read).write();
        const std::uint64_t h = fnv(bytes);
        context.expect(bytes.size() == pin.len && h == pin.hash,
                       std::format("{}x{}: {} bytes fnv {:#018x}, expected {} bytes fnv {:#018x}", pin.regs, pin.stages,
                                   bytes.size(), h, pin.len, pin.hash));
    }
}

void census_counts_written_instructions(Context& context)
{
    const bool was = census::enabled();
    census::set_enabled(true);
    Module m = synthetic(2, 1, 2);
    const std::vector<std::uint8_t> bytes = m.write();
    census::note(m, bytes);
    std::vector<std::uint8_t> wrapped{'D', 'X', 'I', 'L'};
    wrapped.insert(wrapped.end(), bytes.begin(), bytes.end());
    const auto counted = census::take(wrapped);
    census::set_enabled(was);
    if (!context.expect(counted.has_value(), "census found the module")) {
        return;
    }
    const auto at = [](std::string_view name) {
        return static_cast<std::size_t>(std::find(census::KINDS.begin(), census::KINDS.end(), name) - census::KINDS.begin());
    };
    context.expect(counted->counts[at("blocks")] == 8, std::format("blocks {}", counted->counts[at("blocks")]));
    context.expect(counted->counts[at("io_other")] == 2, "two calls");
    context.expect(counted->counts[at("phi")] == 4, std::format("phis {}", counted->counts[at("phi")]));
    context.expect(!census::take(wrapped).has_value(), "taken once");
}

}

void register_bitcode_tests()
{
    register_case("bitcode", "vbr_and_blocks", vbr_and_blocks);
    register_case("bitcode", "worklist_liveness_matches_passes", worklist_liveness_matches_passes);
    register_case("bitcode", "unread_phis_are_dropped", unread_phis_are_dropped);
    register_case("bitcode", "successors_of_each_terminator", successors_of_each_terminator);
    register_case("bitcode", "written_bytes_unchanged", written_bytes_unchanged);
    register_case("bitcode", "census_counts_written_instructions", census_counts_written_instructions);
}

}
