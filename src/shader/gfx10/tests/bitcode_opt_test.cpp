#include <euphoria/shader/gfx10/emit/bitcode.hpp>
#include <euphoria/shader/gfx10/emit/bitcode_opt.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

using namespace euphoria::shader::gfx10::emit;

struct Kit {
    Module m;
    std::size_t b{0};
    V pure;
    V reads;
    V writes;
};

Kit kit()
{
    Kit k;
    Module& m = k.m;
    const Ty i32t = m.int_(32);
    const Ty void_t = m.void_();
    const Ty fty = m.func_ty(i32t, {i32t, i32t});
    m.ptr(fty);
    k.pure = m.function("dx.op.binary.i32", fty, Attr::ReadNone, false);
    k.reads = m.function("dx.op.bufferLoad.i32", fty, Attr::ReadOnly, false);
    k.writes = m.function("dx.op.bufferStore.i32", fty, Attr::NoUnwind, false);
    const Ty main_ty = m.func_ty(void_t, {});
    m.ptr(main_ty);
    m.function("main", main_ty, std::nullopt, true);
    k.b = m.new_block();
    return k;
}

std::size_t body(const Module& m, std::size_t b)
{
    return m.blocks[b].body.size();
}

std::vector<std::vector<V>> stores(const Module& m, std::size_t b)
{
    std::vector<std::vector<V>> out;
    for (const std::uint32_t i : m.blocks[b].body) {
        const Op& op = m.insts[i].op;
        if (op.kind == Op::Kind::Call && m.funcs[op.index].name.find("Store") != std::string::npos) {
            out.push_back(op.args);
        }
    }
    return out;
}

void unread_results_without_effect_are_dropped(Context& context)
{
    Kit k = kit();
    Module& m = k.m;
    const V one = m.i32(1);
    const V two = m.i32(2);
    const V used = m.call(k.b, k.reads, {one, two});
    const V dead = m.call(k.b, k.reads, {two, one});
    const V chain = m.bin(k.b, Bin::Add, dead, one);
    m.call(k.b, k.pure, {chain, two});
    const V kept = m.bin(k.b, Bin::Add, used, two);
    m.call(k.b, k.writes, {kept, one});
    m.terminate(k.b, Term::Ret());
    m.optimise(opt::DEAD);
    context.expect(body(m, k.b) == 3, "the load, the add and the store");
    m.write();
}

void branch_conditions_and_phi_cycles(Context& context)
{
    Kit k = kit();
    Module& m = k.m;
    const Ty i32t = m.int_(32);
    const V zero = m.i32(0);
    const V one = m.i32(1);
    const std::size_t head = m.new_block();
    const std::size_t exit = m.new_block();
    m.terminate(k.b, Term::Br(head));
    const V count = m.phi(head, i32t);
    const V idle = m.phi(head, i32t);
    const V next = m.bin(head, Bin::Add, count, one);
    const V idle_next = m.call(head, k.reads, {idle, one});
    const V more = m.cmp(head, pred::IULT, next, one);
    m.terminate(head, Term::CondBr(more, head, exit));
    m.set_phi(count, {{zero, k.b}, {next, head}});
    m.set_phi(idle, {{zero, k.b}, {idle_next, head}});
    m.call(exit, k.writes, {zero, one});
    m.terminate(exit, Term::Ret());
    m.optimise(opt::DEAD);
    context.expect(m.blocks[head].phis.size() == 1, "the counter");
    context.expect(body(m, head) == 2, "the step and the test");
    context.expect(body(m, exit) == 1, "the store");
    m.write();
}

void wave_operations_have_no_effect_of_their_own(Context& context)
{
    Kit k = kit();
    Module& m = k.m;
    const Ty i32t = m.int_(32);
    const Ty fty = m.func_ty(i32t, {i32t, i32t});
    const V first = m.function("dx.op.waveReadLaneFirst.i32", fty, Attr::NoUnwind, false);
    const V barrier = m.function("dx.op.barrier", fty, Attr::NoDuplicate, false);
    const V one = m.i32(1);
    m.call(k.b, first, {one, one});
    m.call(k.b, barrier, {one, one});
    m.terminate(k.b, Term::Ret());
    m.optimise(opt::DEAD);
    context.expect(body(m, k.b) == 1, "the barrier");
    std::vector<std::string> names;
    for (const Func& f : m.funcs) {
        names.push_back(f.name);
    }
    context.expect(names == std::vector<std::string>{"main", "dx.op.barrier"}, "unused declarations dropped");
    const Op& op = m.insts[m.blocks[k.b].body[0]].op;
    context.expect(op.kind == Op::Kind::Call && m.funcs[op.index].name == "dx.op.barrier", "call renumbered");
    m.write();
}

void constants_and_identities_fold(Context& context)
{
    Kit k = kit();
    Module& m = k.m;
    const std::size_t b = k.b;
    const Ty i32t = m.int_(32);
    const Ty f32t = m.float_();
    const V zero = m.i32(0);
    const V one = m.i32(1);
    const V all = m.i32(0xFFFFFFFFu);
    const V x = m.call(b, k.reads, {one, one});
    V a = m.bin(b, Bin::Add, x, zero);
    a = m.bin(b, Bin::And, all, a);
    a = m.bin(b, Bin::LShr, a, zero);
    a = m.bin(b, Bin::Mul, a, one);
    a = m.bin(b, Bin::Or, a, a);
    const V f = m.cast(b, Cast::Bitcast, a, f32t);
    a = m.cast(b, Cast::Bitcast, f, i32t);
    V kv = m.i32(262168);
    const V sixteen = m.i32(16);
    kv = m.bin(b, Bin::LShr, kv, sixteen);
    const V k127 = m.i32(127);
    kv = m.bin(b, Bin::And, kv, k127);
    const V minus8 = m.i32(static_cast<std::uint32_t>(-8));
    const V s = m.bin(b, Bin::AShr, minus8, one);
    const V k32 = m.i32(32);
    const V wide = m.bin(b, Bin::Shl, one, k32);
    const V c = m.cmp(b, pred::IULT, zero, one);
    const V sel = m.select(b, c, a, wide);
    const V same = m.select(b, c, sel, sel);
    m.call(b, k.writes, {same, kv});
    m.call(b, k.writes, {s, wide});
    m.terminate(b, Term::Ret());
    m.optimise(opt::FOLD | opt::DEAD);
    context.expect(body(m, b) == 4, std::format("the read, the wide shift and two stores ({})", body(m, b)));
    const V four = m.i32(4);
    const V minus4 = m.i32(static_cast<std::uint32_t>(-4));
    context.expect(stores(m, b) == std::vector<std::vector<V>>{{x, four}, {minus4, wide}}, "folded stores");
    m.write();
}

void float_arithmetic_is_not_folded(Context& context)
{
    Kit k = kit();
    Module& m = k.m;
    const std::size_t b = k.b;
    const Ty f32t = m.float_();
    const Ty fty = m.func_ty(f32t, {f32t, f32t});
    const V input = m.function("dx.op.loadInput.f32", fty, Attr::ReadNone, false);
    const V zero = m.f32(0);
    const V one = m.f32(std::bit_cast<std::uint32_t>(1.0f));
    const V two = m.f32(std::bit_cast<std::uint32_t>(2.0f));
    const V x = m.call(b, input, {zero, zero});
    const V y = m.call(b, input, {one, one});
    const V a = m.bin(b, Bin::Add, x, zero);
    const V p = m.bin(b, Bin::Mul, a, one);
    const V xy = m.bin(b, Bin::Add, x, y);
    const V yx = m.bin(b, Bin::Add, y, x);
    const V nan = m.f32(0x7FC00000u);
    const V lt = m.cmp(b, pred::OLT, one, two);
    const V eq = m.cmp(b, pred::OEQ, nan, nan);
    const V une = m.cmp(b, pred::UNE, nan, one);
    V sel = m.select(b, lt, xy, yx);
    sel = m.select(b, eq, p, sel);
    sel = m.select(b, une, sel, p);
    m.call(b, k.writes, {sel, p});
    m.terminate(b, Term::Ret());
    m.optimise(opt::ALL);
    context.expect(body(m, b) == 6, std::format("six left ({})", body(m, b)));
    context.expect(stores(m, b) == std::vector<std::vector<V>>{{xy, p}}, "store of x + y");
    m.write();
}

struct Diamond {
    Kit k;
    V x;
    V y;
    V c;
    std::size_t then{0};
    std::size_t other{0};
    std::size_t join{0};
};

Diamond diamond()
{
    Diamond d{kit()};
    Module& m = d.k.m;
    const V one = m.i32(1);
    const V two = m.i32(2);
    d.x = m.call(d.k.b, d.k.reads, {one, one});
    d.y = m.call(d.k.b, d.k.reads, {two, two});
    d.c = m.cmp(d.k.b, pred::IULT, d.x, d.y);
    d.then = m.new_block();
    d.other = m.new_block();
    d.join = m.new_block();
    m.terminate(d.k.b, Term::CondBr(d.c, d.then, d.other));
    m.terminate(d.then, Term::Br(d.join));
    m.terminate(d.other, Term::Br(d.join));
    m.terminate(d.join, Term::Ret());
    return d;
}

void a_value_is_reused_below_its_block_and_not_beside_it(Context& context)
{
    Diamond d = diamond();
    Module& m = d.k.m;
    const V top = m.call(d.k.b, d.k.pure, {d.x, d.y});
    m.call(d.k.b, d.k.writes, {top, top});
    const V again = m.call(d.then, d.k.pure, {d.x, d.y});
    const V side = m.bin(d.then, Bin::Add, d.y, d.x);
    m.call(d.then, d.k.writes, {again, side});
    const V beside = m.bin(d.other, Bin::Add, d.x, d.y);
    m.call(d.other, d.k.writes, {beside, beside});
    const V after = m.bin(d.join, Bin::Add, d.x, d.y);
    m.call(d.join, d.k.writes, {after, after});
    m.optimise(opt::REUSE);
    context.expect(stores(m, d.then) == std::vector<std::vector<V>>{{top, side}}, "entry value reused");
    context.expect(body(m, d.then) == 2, "then");
    context.expect(body(m, d.other) == 2, "other");
    context.expect(body(m, d.join) == 2, "join");
    m.write();
}

void a_branch_taken_says_what_its_condition_is(Context& context)
{
    {
        Diamond d = diamond();
        Module& m = d.k.m;
        const V three = m.i32(3);
        V dv = m.call(d.k.b, d.k.reads, {three, three});
        dv = m.cmp(d.k.b, pred::NE, dv, three);
        m.bin(d.k.b, Bin::And, d.c, dv);
        const V guard = m.bin(d.then, Bin::And, d.c, dv);
        const V old = m.select(d.then, guard, d.x, d.y);
        const V no = m.cmp(d.then, pred::IUGE, d.x, d.y);
        const V fresh = m.select(d.then, no, d.y, old);
        m.call(d.then, d.k.writes, {fresh, fresh});
        const V kept = m.select(d.other, d.c, d.x, d.y);
        m.call(d.other, d.k.writes, {kept, kept});
        const V open = m.select(d.join, d.c, d.x, d.y);
        m.call(d.join, d.k.writes, {open, open});
        m.optimise(opt::FOLD | opt::FACTS);
        context.expect(body(m, d.then) == 2, std::format("then keeps a select and the store ({})", body(m, d.then)));
        const auto then_stores = stores(m, d.then);
        bool ok = false;
        if (!then_stores.empty() && then_stores[0][0].is_inst()) {
            const Op& op = m.insts[then_stores[0][0].index].op;
            ok = op.kind == Op::Kind::Select && op.a == dv && op.b == d.x && op.c == d.y;
        }
        context.expect(ok, "select(d, x, y)");
        context.expect(stores(m, d.other) == std::vector<std::vector<V>>{{d.y, d.y}}, "other stores y");
        context.expect(body(m, d.join) == 2, "join unchanged");
        m.write();
    }
    {
        Diamond d = diamond();
        Module& m = d.k.m;
        const V three = m.i32(3);
        V dv = m.call(d.k.b, d.k.reads, {three, three});
        dv = m.cmp(d.k.b, pred::NE, dv, three);
        const V both = m.bin(d.k.b, Bin::And, d.c, dv);
        const std::size_t inner = m.new_block();
        const std::size_t out = m.new_block();
        m.terminate(d.k.b, Term::CondBr(both, inner, out));
        const V a = m.select(inner, d.c, d.x, d.y);
        const V z = m.select(inner, dv, a, d.y);
        m.call(inner, d.k.writes, {z, z});
        m.terminate(inner, Term::Br(out));
        const V w = m.select(out, dv, d.x, d.y);
        m.call(out, d.k.writes, {w, w});
        m.terminate(out, Term::Ret());
        m.optimise(opt::FOLD | opt::FACTS);
        context.expect(stores(m, inner) == std::vector<std::vector<V>>{{d.x, d.x}}, "both terms held");
        context.expect(body(m, out) == 2, "out unchanged");
        m.write();
    }
}

void a_branch_on_a_constant_goes_one_way(Context& context)
{
    Diamond d = diamond();
    Module& m = d.k.m;
    const Ty i32t = m.int_(32);
    const std::size_t deep = m.new_block();
    const std::size_t never = m.new_block();
    const std::size_t meet = m.new_block();
    m.terminate(d.then, Term::CondBr(d.c, deep, never));
    m.terminate(deep, Term::Br(meet));
    m.call(never, d.k.writes, {d.y, d.y});
    m.terminate(never, Term::Br(meet));
    const V p = m.phi(meet, i32t);
    m.set_phi(p, {{d.x, deep}, {d.y, never}});
    m.call(meet, d.k.writes, {p, p});
    m.terminate(meet, Term::Br(d.join));
    const V q = m.phi(d.join, i32t);
    m.set_phi(q, {{p, meet}, {d.y, d.other}});
    m.call(d.join, d.k.writes, {q, q});
    m.optimise(opt::ALL);
    const auto& term = m.blocks[d.then].term;
    context.expect(term && term->kind == Term::Kind::Br && term->t == deep, "then branches to deep");
    context.expect(stores(m, meet) == std::vector<std::vector<V>>{{d.x, d.x}}, "meet stores x");
    context.expect(m.blocks[meet].phis.empty(), "meet has no phi");
    context.expect(body(m, never) == 0, "emptied: nothing reaches it");
    if (context.expect(!m.blocks[d.join].phis.empty(), "join keeps its phi")) {
        const Op& op = m.insts[m.blocks[d.join].phis[0]].op;
        const std::vector<std::pair<V, std::size_t>> want{{d.x, meet}, {d.y, d.other}};
        context.expect(op.incoming == want, "join incoming");
    }
    m.write();
}

void a_read_is_reused_within_its_block_until_a_store(Context& context)
{
    Diamond d = diamond();
    Module& m = d.k.m;
    const Ty i32t = m.int_(32);
    const V one = m.i32(1);
    const V again = m.call(d.k.b, d.k.reads, {one, one});
    m.call(d.k.b, d.k.writes, {again, d.y});
    const V after = m.call(d.k.b, d.k.reads, {one, one});
    const V twice = m.call(d.k.b, d.k.reads, {one, one});
    const V sum = m.bin(d.k.b, Bin::Add, after, twice);
    m.call(d.k.b, d.k.writes, {sum, sum});
    const V below = m.call(d.then, d.k.reads, {one, one});
    m.call(d.then, d.k.writes, {below, below});
    const Ty cfty = m.func_ty(i32t, {i32t, i32t});
    const V cb = m.function("dx.op.cbufferLoadLegacy.i32", cfty, Attr::ReadOnly, false);
    const V top = m.call(d.k.b, cb, {one, one});
    m.call(d.k.b, d.k.writes, {top, top});
    const V under = m.call(d.then, cb, {one, one});
    m.call(d.then, d.k.writes, {under, under});
    m.optimise(opt::REUSE | opt::LOADS);
    const auto s = stores(m, d.k.b);
    if (!context.expect(s.size() == 3, "three stores in the entry")) {
        return;
    }
    context.expect(s[0] == std::vector<V>{d.x, d.y}, "the second read is the first");
    bool ok = false;
    if (s[1][0].is_inst()) {
        const Op& op = m.insts[s[1][0].index].op;
        ok = op.kind == Op::Kind::Bin && op.bin() == Bin::Add && op.a == after && op.b == after;
    }
    context.expect(ok, "read again after the store, once");
    context.expect(s[2] == std::vector<V>{top, top}, "constant buffer read");
    context.expect(stores(m, d.then) == std::vector<std::vector<V>>{{below, below}, {top, top}}, "then stores");
    m.write();
}

void a_join_of_one_value_and_two_equal_joins(Context& context)
{
    Diamond d = diamond();
    Module& m = d.k.m;
    const Ty i32t = m.int_(32);
    const V one = m.phi(d.join, i32t);
    m.set_phi(one, {{d.x, d.then}, {d.x, d.other}});
    const V p = m.phi(d.join, i32t);
    m.set_phi(p, {{d.x, d.then}, {d.y, d.other}});
    const V q = m.phi(d.join, i32t);
    m.set_phi(q, {{d.y, d.other}, {d.x, d.then}});
    m.call(d.join, d.k.writes, {one, p});
    const V sum = m.bin(d.join, Bin::Add, p, q);
    m.call(d.join, d.k.writes, {sum, q});
    m.optimise(opt::FOLD | opt::REUSE);
    context.expect(m.blocks[d.join].phis == std::vector<std::uint32_t>{p.index}, "one phi left");
    const auto s = stores(m, d.join);
    if (context.expect(s.size() == 2, "two stores")) {
        context.expect(s[0] == std::vector<V>{d.x, p}, "first store");
        context.expect(s[1][1] == p, "q is p");
    }
    m.write();
}

using Trace = std::vector<std::pair<std::string, std::vector<std::uint64_t>>>;

constexpr std::uint64_t mask(std::uint32_t bits)
{
    return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
}

constexpr std::int64_t as_signed(std::uint32_t bits, std::uint64_t x)
{
    if (bits < 64 && ((x >> (bits - 1)) & 1) != 0) {
        return static_cast<std::int64_t>(x | ~mask(bits));
    }
    return static_cast<std::int64_t>(x);
}

std::optional<std::uint32_t> int_bits(const Module& m, Ty t)
{
    const TypeDef& d = m.types[t.id];
    if (d.kind == TypeDef::Kind::Int) {
        return d.a;
    }
    return std::nullopt;
}

Trace run(const Module& m, std::span<const std::uint32_t> inputs)
{
    std::vector<std::optional<std::uint64_t>> vals(m.insts.size());
    std::array<std::uint64_t, 16> memory{};
    Trace trace;
    const auto eval = [&](V v) -> std::uint64_t {
        if (v.is_inst()) {
            if (!vals[v.index]) {
                throw std::logic_error("a value defined before its use");
            }
            return *vals[v.index];
        }
        if (v.is_const()) {
            const auto& [t, c] = m.consts[v.index];
            const auto bits = int_bits(m, t);
            if (c.kind == ConstDef::Kind::Int && bits) {
                return static_cast<std::uint64_t>(c.value) & mask(*bits);
            }
            if (c.kind == ConstDef::Kind::Float) {
                return static_cast<std::uint32_t>(c.value);
            }
            return 0;
        }
        throw std::logic_error("a global or function as a value");
    };
    std::size_t b = 0;
    std::size_t from = static_cast<std::size_t>(-1);
    for (int step = 0; step < 4000; ++step) {
        const Block& blk = m.blocks[b];
        std::vector<std::pair<std::uint32_t, std::uint64_t>> joined;
        for (const std::uint32_t p : blk.phis) {
            const Op& op = m.insts[p].op;
            if (op.kind != Op::Kind::Phi) {
                throw std::logic_error("a join");
            }
            const auto it = std::find_if(op.incoming.begin(), op.incoming.end(), [&](const auto& x) { return x.second == from; });
            if (it == op.incoming.end()) {
                throw std::logic_error("a value for the edge taken");
            }
            joined.emplace_back(p, eval(it->first));
        }
        for (const auto& [p, x] : joined) {
            vals[p] = x;
        }
        for (const std::uint32_t i : blk.body) {
            const Inst& inst = m.insts[i];
            const auto bits = int_bits(m, inst.ty);
            const auto as_float = [](std::uint64_t x) { return std::bit_cast<float>(static_cast<std::uint32_t>(x)); };
            std::uint64_t r = 0;
            const Op& op = inst.op;
            switch (op.kind) {
            case Op::Kind::Bin: {
                const std::uint64_t x = eval(op.a);
                const std::uint64_t y = eval(op.b);
                if (bits) {
                    switch (op.bin()) {
                    case Bin::Add:
                        r = x + y;
                        break;
                    case Bin::Sub:
                        r = x - y;
                        break;
                    case Bin::Mul:
                        r = x * y;
                        break;
                    case Bin::And:
                        r = x & y;
                        break;
                    case Bin::Or:
                        r = x | y;
                        break;
                    case Bin::Xor:
                        r = x ^ y;
                        break;
                    case Bin::Shl:
                        r = y >= 64 ? 0 : x << y;
                        break;
                    case Bin::LShr:
                        r = y >= 64 ? 0 : x >> y;
                        break;
                    case Bin::AShr:
                        r = static_cast<std::uint64_t>(as_signed(*bits, x) >> std::min<std::uint64_t>(y, 63));
                        break;
                    default:
                        throw std::logic_error("no divisions here");
                    }
                    r &= mask(*bits);
                } else {
                    const float fx = as_float(x);
                    const float fy = as_float(y);
                    float fr = 0.0f;
                    switch (op.bin()) {
                    case Bin::Add:
                        fr = fx + fy;
                        break;
                    case Bin::Sub:
                        fr = fx - fy;
                        break;
                    default:
                        fr = fx * fy;
                        break;
                    }
                    r = std::bit_cast<std::uint32_t>(fr);
                }
                break;
            }
            case Op::Kind::Cmp: {
                const auto wide = int_bits(m, m.type_of(op.a));
                const std::uint64_t x = eval(op.a);
                const std::uint64_t y = eval(op.b);
                const std::uint8_t p = op.code;
                bool holds = false;
                if (wide) {
                    const std::int64_t sx = as_signed(*wide, x);
                    const std::int64_t sy = as_signed(*wide, y);
                    switch (p) {
                    case pred::EQ:
                        holds = x == y;
                        break;
                    case pred::NE:
                        holds = x != y;
                        break;
                    case pred::IUGT:
                        holds = x > y;
                        break;
                    case pred::IUGE:
                        holds = x >= y;
                        break;
                    case pred::IULT:
                        holds = x < y;
                        break;
                    case pred::IULE:
                        holds = x <= y;
                        break;
                    case pred::SGT:
                        holds = sx > sy;
                        break;
                    case pred::SGE:
                        holds = sx >= sy;
                        break;
                    case pred::SLT:
                        holds = sx < sy;
                        break;
                    default:
                        holds = sx <= sy;
                        break;
                    }
                } else {
                    const auto xb = static_cast<std::uint32_t>(x);
                    const auto yb = static_cast<std::uint32_t>(y);
                    const auto nan = [](std::uint32_t v) { return (v & 0x7FFFFFFFu) > 0x7F800000u; };
                    const auto order = [](std::uint32_t v) {
                        const std::int64_t mag = v & 0x7FFFFFFFu;
                        return (v & 0x80000000u) != 0 ? -mag : mag;
                    };
                    if (nan(xb) || nan(yb)) {
                        holds = (p & 8) != 0;
                    } else {
                        const std::int64_t ox = order(xb);
                        const std::int64_t oy = order(yb);
                        holds = ((p & 1) != 0 && ox == oy) || ((p & 2) != 0 && ox > oy) || ((p & 4) != 0 && ox < oy);
                    }
                }
                r = holds ? 1 : 0;
                break;
            }
            case Op::Kind::Select:
                r = eval(op.a) != 0 ? eval(op.b) : eval(op.c);
                break;
            case Op::Kind::Cast: {
                const auto from_bits = int_bits(m, m.type_of(op.a));
                const std::uint64_t x = eval(op.a);
                const Cast c = op.cast();
                if (c == Cast::Bitcast || c == Cast::ZExt) {
                    r = x;
                } else if (c == Cast::SExt && from_bits && bits) {
                    r = static_cast<std::uint64_t>(as_signed(*from_bits, x)) & mask(*bits);
                } else if (c == Cast::Trunc && bits) {
                    r = x & mask(*bits);
                } else {
                    throw std::logic_error("a cast the generator does not make");
                }
                break;
            }
            case Op::Kind::Call: {
                std::vector<std::uint64_t> a;
                for (const V v : op.args) {
                    a.push_back(eval(v));
                }
                const std::string& name = m.funcs[op.index].name;
                if (name == "dx.op.loadInput.i32") {
                    r = inputs[(a[0] ^ a[1]) % inputs.size()];
                } else if (name == "dx.op.binary.i32") {
                    r = std::min(a[0], a[1]);
                } else if (name == "dx.op.cbufferLoadLegacy.i32") {
                    r = std::rotl(inputs[a[0] % inputs.size()], static_cast<int>(a[1] & 31));
                } else if (name == "dx.op.bufferLoad.i32") {
                    r = memory[(a[0] ^ a[1]) % 16];
                } else if (name == "dx.op.waveReadLaneFirst.i32") {
                    r = a[0] ^ a[1];
                } else {
                    memory[a[0] % 16] = a[1];
                    trace.emplace_back(name, a);
                    r = 0;
                }
                break;
            }
            default:
                throw std::logic_error("an instruction the generator does not make");
            }
            vals[i] = r;
        }
        from = b;
        if (!blk.term) {
            throw std::logic_error("a terminator");
        }
        switch (blk.term->kind) {
        case Term::Kind::Ret:
            trace.emplace_back("ret", std::vector<std::uint64_t>{});
            return trace;
        case Term::Kind::Br:
            b = blk.term->t;
            break;
        case Term::Kind::CondBr:
            b = eval(blk.term->cond) != 0 ? blk.term->t : blk.term->f;
            break;
        }
    }
    trace.emplace_back("still running", std::vector<std::uint64_t>{});
    return trace;
}

class Gen {
public:
    Kit k;
    std::uint64_t rng{0};
    std::vector<V> regs;
    std::vector<V> conds;
    std::size_t cur{0};
    V input;
    V table;
    V wave;
    std::optional<std::pair<V, V>> last;

    std::uint64_t next(std::uint64_t n)
    {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (rng >> 11) % n;
    }

    V reg()
    {
        const auto n = static_cast<std::size_t>(next(regs.size()));
        return regs[n];
    }

    V value()
    {
        if (next(4) == 0) {
            constexpr std::array<std::uint32_t, 7> kValues{0, 1, 2, 31, 0xFFFFFFFFu, 0x80000000u, 7};
            const std::uint32_t kv = kValues[static_cast<std::size_t>(next(7))];
            return k.m.i32(kv);
        }
        return reg();
    }

    V cond()
    {
        const std::size_t c = cur;
        V made;
        const std::uint64_t pick = next(6);
        if (pick == 0 && !conds.empty()) {
            const auto n = static_cast<std::size_t>(next(conds.size()));
            return conds[n];
        }
        if (pick == 1 && conds.size() > 1) {
            const auto a = static_cast<std::size_t>(next(conds.size()));
            const auto b = static_cast<std::size_t>(next(conds.size()));
            constexpr std::array<Bin, 3> kOps{Bin::And, Bin::Or, Bin::Xor};
            const Bin op = kOps[static_cast<std::size_t>(next(3))];
            made = k.m.bin(c, op, conds[a], conds[b]);
        } else if (pick == 2 && !conds.empty()) {
            const auto n = static_cast<std::size_t>(next(conds.size()));
            const V t = k.m.bool_(true);
            made = k.m.bin(c, Bin::Xor, conds[n], t);
        } else {
            const auto p = static_cast<std::uint8_t>(pred::EQ + next(10));
            const V x = reg();
            const V y = value();
            made = k.m.cmp(c, p, x, y);
        }
        conds.push_back(made);
        return made;
    }

    void statement()
    {
        const std::size_t c = cur;
        const auto d = static_cast<std::size_t>(next(regs.size()));
        const Ty i32t = k.m.int_(32);
        const Ty f32t = k.m.float_();
        V r;
        const std::uint64_t pick = next(14);
        if (pick <= 3) {
            constexpr std::array<Bin, 6> kOps{Bin::Add, Bin::Sub, Bin::Mul, Bin::And, Bin::Or, Bin::Xor};
            const Bin op = kOps[static_cast<std::size_t>(next(6))];
            const V x = value();
            const V y = value();
            r = k.m.bin(c, op, x, y);
        } else if (pick == 4) {
            constexpr std::array<Bin, 3> kOps{Bin::Shl, Bin::LShr, Bin::AShr};
            const Bin op = kOps[static_cast<std::size_t>(next(3))];
            const V x = value();
            const auto by = static_cast<std::uint32_t>(next(32));
            const V byv = k.m.i32(by);
            r = k.m.bin(c, op, x, byv);
        } else if (pick == 5 || pick == 6) {
            const V g = cond();
            const V x = value();
            r = k.m.select(cur, g, x, regs[d]);
        } else if (pick == 7) {
            const V x = value();
            const V y = value();
            const std::array<V, 4> fs{k.pure, input, table, wave};
            const V f = fs[static_cast<std::size_t>(next(4))];
            r = k.m.call(c, f, {x, y});
        } else if (pick == 8 || pick == 9) {
            const bool again = next(2) == 0;
            std::pair<V, V> xy;
            if (last && again) {
                xy = *last;
            } else {
                const V x = value();
                const V y = value();
                xy = {x, y};
            }
            last = xy;
            r = k.m.call(c, k.reads, {xy.first, xy.second});
        } else if (pick == 10) {
            V x = value();
            const V y = value();
            const bool there = next(2) == 0;
            if (last && there) {
                x = k.m.bin(c, Bin::Xor, last->first, last->second);
            }
            k.m.call(c, k.writes, {x, y});
            return;
        } else if (pick == 11) {
            const V x0 = value();
            const V y0 = value();
            const V x = k.m.cast(c, Cast::Bitcast, x0, f32t);
            const V y = k.m.cast(c, Cast::Bitcast, y0, f32t);
            constexpr std::array<Bin, 3> kOps{Bin::Add, Bin::Sub, Bin::Mul};
            const Bin op = kOps[static_cast<std::size_t>(next(3))];
            const V f = k.m.bin(c, op, x, y);
            if (next(2) == 0) {
                const auto p = static_cast<std::uint8_t>(next(16));
                const V t = k.m.cmp(c, p, f, y);
                conds.push_back(t);
            }
            r = k.m.cast(c, Cast::Bitcast, f, i32t);
        } else if (pick == 12) {
            const V g = cond();
            r = k.m.cast(cur, Cast::ZExt, g, i32t);
        } else {
            const V x = value();
            const V f = k.m.cast(c, Cast::Bitcast, x, f32t);
            r = k.m.cast(c, Cast::Bitcast, f, i32t);
        }
        regs[d] = r;
    }

    void code(std::uint32_t depth)
    {
        const std::uint64_t count = 1 + next(6);
        for (std::uint64_t n = 0; n < count; ++n) {
            const std::uint64_t pick = next(8);
            if ((pick == 0 || pick == 1) && depth > 0) {
                branch(depth - 1);
            } else if (pick == 2 && depth > 0) {
                repeat(depth - 1);
            } else {
                statement();
            }
        }
    }

    void branch(std::uint32_t depth)
    {
        const V c = cond();
        const Ty i32t = k.m.int_(32);
        const std::size_t then = k.m.new_block();
        const std::size_t other = k.m.new_block();
        const std::size_t join = k.m.new_block();
        k.m.terminate(cur, Term::CondBr(c, then, other));
        std::vector<V> before = regs;
        const std::size_t known = conds.size();
        cur = then;
        last.reset();
        code(depth);
        const std::vector<V> then_regs = std::exchange(regs, std::move(before));
        const std::size_t then_end = cur;
        k.m.terminate(then_end, Term::Br(join));
        conds.resize(known);
        cur = other;
        last.reset();
        if (next(3) != 0) {
            code(depth);
        }
        const std::size_t other_end = cur;
        k.m.terminate(other_end, Term::Br(join));
        conds.resize(known);
        for (std::size_t r = 0; r < then_regs.size(); ++r) {
            const V p = k.m.phi(join, i32t);
            k.m.set_phi(p, {{then_regs[r], then_end}, {regs[r], other_end}});
            regs[r] = p;
        }
        cur = join;
        last.reset();
    }

    void repeat(std::uint32_t depth)
    {
        const Ty i32t = k.m.int_(32);
        const std::size_t head = k.m.new_block();
        const std::size_t exit = k.m.new_block();
        const std::size_t before = cur;
        k.m.terminate(before, Term::Br(head));
        const std::vector<V> entering = regs;
        std::vector<V> joins;
        for (std::size_t r = 0; r < regs.size(); ++r) {
            joins.push_back(k.m.phi(head, i32t));
        }
        const V count = k.m.phi(head, i32t);
        regs = joins;
        const std::size_t known = conds.size();
        cur = head;
        last.reset();
        code(depth);
        const V one = k.m.i32(1);
        const V step = k.m.bin(cur, Bin::Add, count, one);
        const auto limit_n = static_cast<std::uint32_t>(1 + next(3));
        const V limit = k.m.i32(limit_n);
        V more = k.m.cmp(cur, pred::IULT, step, limit);
        if (next(2) == 0) {
            const V c = cond();
            more = k.m.bin(cur, Bin::And, more, c);
        }
        const std::size_t latch = cur;
        k.m.terminate(latch, Term::CondBr(more, head, exit));
        for (std::size_t r = 0; r < joins.size(); ++r) {
            k.m.set_phi(joins[r], {{entering[r], before}, {regs[r], latch}});
        }
        const V zero = k.m.i32(0);
        k.m.set_phi(count, {{zero, before}, {step, latch}});
        conds.resize(known);
        cur = exit;
        last.reset();
    }
};

Module random_program(std::uint64_t seed)
{
    Gen g;
    g.k = kit();
    Module& m = g.k.m;
    const Ty i32t = m.int_(32);
    const Ty fty = m.func_ty(i32t, {i32t, i32t});
    g.input = m.function("dx.op.loadInput.i32", fty, Attr::ReadNone, false);
    g.table = m.function("dx.op.cbufferLoadLegacy.i32", fty, Attr::ReadOnly, false);
    g.wave = m.function("dx.op.waveReadLaneFirst.i32", fty, Attr::NoUnwind, false);
    const std::size_t b = g.k.b;
    g.rng = (seed * 0x9E3779B97F4A7C15ull) | 1;
    g.cur = b;
    for (std::uint32_t r = 0; r < 5; ++r) {
        const V x = m.i32(r);
        const V y = m.i32(r * 3);
        g.regs.push_back(m.call(b, g.input, {x, y}));
    }
    g.code(3);
    g.code(2);
    for (std::size_t r = 0; r < g.regs.size(); ++r) {
        const V at = m.i32(static_cast<std::uint32_t>(r));
        m.call(g.cur, g.k.writes, {at, g.regs[r]});
    }
    m.terminate(g.cur, Term::Ret());
    return std::move(g.k.m);
}

void every_pass_leaves_what_random_programs_store(Context& context)
{
    std::size_t removed = 0;
    std::size_t failures = 0;
    for (std::uint64_t seed = 1; seed <= 400 && failures < 5; ++seed) {
        const Module plain = random_program(seed);
        std::vector<std::vector<std::uint32_t>> inputs;
        for (std::uint32_t n = 0; n < 6; ++n) {
            std::vector<std::uint32_t> row;
            for (std::uint32_t i = 0; i < 7; ++i) {
                switch (n) {
                case 0:
                    row.push_back(0);
                    break;
                case 1:
                    row.push_back(i);
                    break;
                case 2:
                    row.push_back(0xFFFFFFFFu - i);
                    break;
                default:
                    row.push_back(std::rotl((static_cast<std::uint32_t>(seed) ^ n) * 0x9E3779B9u, static_cast<int>(i * 5)) ^ (i << 28));
                    break;
                }
            }
            inputs.push_back(std::move(row));
        }
        std::vector<Trace> expected;
        for (const auto& in : inputs) {
            expected.push_back(run(plain, in));
        }
        std::size_t written = 0;
        for (const Block& blk : plain.blocks) {
            written += blk.body.size() + blk.phis.size();
        }
        for (const std::uint32_t passes : {opt::DEAD, opt::FOLD, opt::REUSE, opt::FACTS, opt::LOADS, opt::FOLD | opt::FACTS,
                                           opt::FOLD | opt::REUSE | opt::DEAD, opt::ALL}) {
            Module m = random_program(seed);
            m.optimise(passes);
            for (std::size_t n = 0; n < inputs.size(); ++n) {
                if (run(m, inputs[n]) != expected[n]) {
                    context.expect(false, std::format("seed {}, passes {}", seed, passes));
                    ++failures;
                    break;
                }
            }
            if (passes == opt::ALL) {
                std::size_t left = 0;
                for (const Block& blk : m.blocks) {
                    left += blk.body.size() + blk.phis.size();
                }
                removed += written - left;
            }
            m.write();
        }
    }
    context.expect(removed > 4000, std::format("{} instructions removed", removed));
}

void the_clean_up_switch_parses_or_runs_every_pass(Context& context)
{
    context.expect(opt::DEFAULT == 63, "the default passes");
    context.expect(parse_shader_opt(std::nullopt) == opt::DEFAULT, "unset");
    context.expect(parse_shader_opt("0") == 0, "0");
    context.expect(parse_shader_opt(" 1 ") == 1, "spaces");
    context.expect(parse_shader_opt("0x1") == 1, "hex");
    context.expect(parse_shader_opt("127") == opt::ALL, "127");
    context.expect(parse_shader_opt("0xffffffff") == opt::ALL, "all bits");
    context.expect(parse_shader_opt("on") == opt::DEFAULT, "unreadable");
}

}

void register_bitcode_opt_tests()
{
    register_case("bitcode_opt", "unread_results_without_effect_are_dropped", unread_results_without_effect_are_dropped);
    register_case("bitcode_opt", "branch_conditions_and_phi_cycles", branch_conditions_and_phi_cycles);
    register_case("bitcode_opt", "wave_operations_have_no_effect_of_their_own", wave_operations_have_no_effect_of_their_own);
    register_case("bitcode_opt", "constants_and_identities_fold", constants_and_identities_fold);
    register_case("bitcode_opt", "float_arithmetic_is_not_folded", float_arithmetic_is_not_folded);
    register_case("bitcode_opt", "a_value_is_reused_below_its_block_and_not_beside_it",
                  a_value_is_reused_below_its_block_and_not_beside_it);
    register_case("bitcode_opt", "a_branch_taken_says_what_its_condition_is", a_branch_taken_says_what_its_condition_is);
    register_case("bitcode_opt", "a_branch_on_a_constant_goes_one_way", a_branch_on_a_constant_goes_one_way);
    register_case("bitcode_opt", "a_read_is_reused_within_its_block_until_a_store",
                  a_read_is_reused_within_its_block_until_a_store);
    register_case("bitcode_opt", "a_join_of_one_value_and_two_equal_joins", a_join_of_one_value_and_two_equal_joins);
    register_case("bitcode_opt", "every_pass_leaves_what_random_programs_store", every_pass_leaves_what_random_programs_store);
    register_case("bitcode_opt", "the_clean_up_switch_parses_or_runs_every_pass", the_clean_up_switch_parses_or_runs_every_pass);
}

}
