#include <euphoria/shader/gfx10/emit/bitcode_opt.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::emit {
namespace {

constexpr std::size_t kNoBlock = std::numeric_limits<std::size_t>::max();

constexpr std::uint64_t mask(std::uint32_t bits) noexcept
{
    return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
}

constexpr std::int64_t as_signed(std::uint32_t bits, std::uint64_t x) noexcept
{
    if (bits < 64 && ((x >> (bits - 1)) & 1) != 0) {
        return static_cast<std::int64_t>(x | ~mask(bits));
    }
    return static_cast<std::int64_t>(x);
}

std::uint8_t opposite(std::uint8_t p) noexcept
{
    if (p <= 15) {
        return static_cast<std::uint8_t>(15 - p);
    }
    switch (p) {
    case pred::EQ:
        return pred::NE;
    case pred::NE:
        return pred::EQ;
    case pred::IUGT:
        return pred::IULE;
    case pred::IULE:
        return pred::IUGT;
    case pred::IUGE:
        return pred::IULT;
    case pred::IULT:
        return pred::IUGE;
    case pred::SGT:
        return pred::SLE;
    case pred::SLE:
        return pred::SGT;
    case pred::SGE:
        return pred::SLT;
    default:
        return pred::SGE;
    }
}

using Repl = std::vector<std::optional<V>>;

V resolve(const Repl& repl, V v)
{
    while (v.is_inst()) {
        const auto& r = repl[v.index];
        if (!r) {
            break;
        }
        v = *r;
    }
    return v;
}

using Key = std::vector<std::uint64_t>;

struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept
    {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (const std::uint64_t x : k) {
            h = (h ^ x) * 0x00000100000001b3ull;
            h ^= h >> 31;
        }
        return static_cast<std::size_t>(h);
    }
};

struct CmpKey {
    std::uint8_t p{0};
    V a;
    V b;

    friend bool operator==(const CmpKey&, const CmpKey&) = default;
};

struct CmpKeyHash {
    std::size_t operator()(const CmpKey& k) const noexcept
    {
        const VHash h;
        return h(k.a) * 31 + h(k.b) * 7 + k.p;
    }
};

struct Facts {
    std::unordered_map<V, bool, VHash> bits;
    std::unordered_map<CmpKey, bool, CmpKeyHash> compares;
};

struct Learned {
    std::vector<V> bits;
    std::vector<CmpKey> compares;
};

struct Tree {
    std::vector<std::size_t> order;
    std::vector<std::vector<std::size_t>> children;
    std::vector<std::vector<std::size_t>> preds;
};

bool float_nan(std::uint32_t bits) noexcept
{
    return (bits & 0x7FFFFFFFu) > 0x7F800000u;
}

std::int64_t float_order(std::uint32_t bits) noexcept
{
    const std::int64_t magnitude = bits & 0x7FFFFFFFu;
    return (bits & 0x80000000u) != 0 ? -magnitude : magnitude;
}

class Pass {
public:
    explicit Pass(Module& module)
        : m(module)
    {
    }

    Module& m;

    std::optional<std::uint32_t> int_bits(Ty t) const
    {
        const TypeDef& d = m.types[t.id];
        if (d.kind == TypeDef::Kind::Int) {
            return d.a;
        }
        return std::nullopt;
    }

    std::optional<std::pair<std::uint32_t, std::uint64_t>> int_k(V v) const
    {
        if (!v.is_const()) {
            return std::nullopt;
        }
        const auto& [t, c] = m.consts[v.index];
        if (c.kind != ConstDef::Kind::Int) {
            return std::nullopt;
        }
        const auto bits = int_bits(t);
        if (!bits) {
            return std::nullopt;
        }
        return std::pair{*bits, static_cast<std::uint64_t>(c.value) & mask(*bits)};
    }

    std::optional<std::uint64_t> int_kv(V v) const
    {
        const auto k = int_k(v);
        if (!k) {
            return std::nullopt;
        }
        return k->second;
    }

    V k(Ty t, std::uint32_t bits, std::uint64_t x)
    {
        return m.int_const(t, as_signed(bits, x & mask(bits)));
    }

    Tree dominator_tree() const
    {
        const std::size_t n = m.blocks.size();
        const auto succs = [&](std::size_t b) { return successors(m.blocks[b].term); };
        std::vector<bool> seen(n, false);
        std::vector<std::size_t> post;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{0, 0}};
        seen[0] = true;
        while (!stack.empty()) {
            auto& top = stack.back();
            const std::size_t b = top.first;
            const std::size_t at = top.second;
            ++top.second;
            const Succs s = succs(b);
            if (at < s.size()) {
                if (!seen[s[at]]) {
                    seen[s[at]] = true;
                    stack.emplace_back(s[at], 0);
                }
            } else {
                post.push_back(b);
                stack.pop_back();
            }
        }
        std::reverse(post.begin(), post.end());
        Tree tree;
        tree.order = std::move(post);
        std::vector<std::size_t> number(n, kNoBlock);
        for (std::size_t i = 0; i < tree.order.size(); ++i) {
            number[tree.order[i]] = i;
        }
        tree.preds.assign(n, {});
        for (const std::size_t b : tree.order) {
            for (const std::size_t s : succs(b)) {
                tree.preds[s].push_back(b);
            }
        }
        std::vector<std::size_t> idom(n, kNoBlock);
        idom[0] = 0;
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t oi = 1; oi < tree.order.size(); ++oi) {
                const std::size_t b = tree.order[oi];
                std::size_t fresh = kNoBlock;
                for (const std::size_t p : tree.preds[b]) {
                    if (idom[p] == kNoBlock) {
                        continue;
                    }
                    if (fresh == kNoBlock) {
                        fresh = p;
                        continue;
                    }
                    std::size_t x = p;
                    std::size_t y = fresh;
                    while (x != y) {
                        while (number[x] > number[y]) {
                            x = idom[x];
                        }
                        while (number[y] > number[x]) {
                            y = idom[y];
                        }
                    }
                    fresh = x;
                }
                if (idom[b] != fresh) {
                    idom[b] = fresh;
                    changed = true;
                }
            }
        }
        tree.children.assign(n, {});
        for (std::size_t oi = 1; oi < tree.order.size(); ++oi) {
            const std::size_t b = tree.order[oi];
            tree.children[idom[b]].push_back(b);
        }
        return tree;
    }

    void learn(V v, bool holds, Facts& facts, Learned& learned, std::uint32_t depth) const
    {
        if (!v.is_inst()) {
            return;
        }
        if (facts.bits.contains(v)) {
            return;
        }
        facts.bits.emplace(v, holds);
        learned.bits.push_back(v);
        if (depth == 0) {
            return;
        }
        const Op& op = m.insts[v.index].op;
        if (op.kind == Op::Kind::Bin) {
            const V a = op.a;
            const V b = op.b;
            if (op.bin() == Bin::And && holds) {
                learn(a, true, facts, learned, depth - 1);
                learn(b, true, facts, learned, depth - 1);
            } else if (op.bin() == Bin::Or && !holds) {
                learn(a, false, facts, learned, depth - 1);
                learn(b, false, facts, learned, depth - 1);
            } else if (op.bin() == Bin::Xor) {
                const auto ka = int_k(a);
                const auto kb = int_k(b);
                V x;
                std::uint64_t kv = 0;
                if (!ka && kb && kb->first == 1) {
                    x = a;
                    kv = kb->second;
                } else if (ka && ka->first == 1 && !kb) {
                    x = b;
                    kv = ka->second;
                } else {
                    return;
                }
                learn(x, holds != (kv != 0), facts, learned, depth - 1);
            }
        } else if (op.kind == Op::Kind::Cmp) {
            const CmpKey key{opposite(op.code), op.a, op.b};
            if (!facts.compares.contains(key)) {
                facts.compares.emplace(key, !holds);
                learned.compares.push_back(key);
            }
        }
    }

    std::optional<V> simplify(std::uint32_t i, bool& changed)
    {
        const Ty t = m.insts[i].ty;
        const Op& op = m.insts[i].op;
        switch (op.kind) {
        case Op::Kind::Bin: {
            const auto bits_opt = int_bits(t);
            if (!bits_opt) {
                return std::nullopt;
            }
            const std::uint32_t bits = *bits_opt;
            const V a = op.a;
            const V b = op.b;
            const auto ka = int_kv(a);
            const auto kb = int_kv(b);
            const std::uint64_t all = mask(bits);
            const Bin o = op.bin();
            if (ka && kb) {
                const std::uint64_t x = *ka;
                const std::uint64_t y = *kb;
                const bool shift_ok = y < bits;
                std::uint64_t r = 0;
                switch (o) {
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
                    if (!shift_ok) {
                        return std::nullopt;
                    }
                    r = x << y;
                    break;
                case Bin::LShr:
                    if (!shift_ok) {
                        return std::nullopt;
                    }
                    r = x >> y;
                    break;
                case Bin::AShr:
                    if (!shift_ok) {
                        return std::nullopt;
                    }
                    r = static_cast<std::uint64_t>(as_signed(bits, x) >> y);
                    break;
                case Bin::UDiv:
                    if (y == 0) {
                        return std::nullopt;
                    }
                    r = x / y;
                    break;
                case Bin::URem:
                    if (y == 0) {
                        return std::nullopt;
                    }
                    r = x % y;
                    break;
                default:
                    return std::nullopt;
                }
                return k(t, bits, r);
            }
            const auto is = [](const std::optional<std::uint64_t>& v, std::uint64_t want) { return v && *v == want; };
            if ((o == Bin::Add || o == Bin::Sub || o == Bin::Or || o == Bin::Xor || o == Bin::Shl || o == Bin::LShr
                 || o == Bin::AShr)
                && is(kb, 0)) {
                return a;
            }
            if ((o == Bin::Add || o == Bin::Or || o == Bin::Xor) && is(ka, 0)) {
                return b;
            }
            if ((o == Bin::Mul || o == Bin::UDiv) && is(kb, 1)) {
                return a;
            }
            if (o == Bin::Mul && is(ka, 1)) {
                return b;
            }
            if ((o == Bin::Mul || o == Bin::And) && is(kb, 0)) {
                return b;
            }
            if ((o == Bin::Mul || o == Bin::And) && is(ka, 0)) {
                return a;
            }
            if (o == Bin::And && is(kb, all)) {
                return a;
            }
            if (o == Bin::And && is(ka, all)) {
                return b;
            }
            if (o == Bin::Or && is(kb, all)) {
                return b;
            }
            if (o == Bin::Or && is(ka, all)) {
                return a;
            }
            if ((o == Bin::And || o == Bin::Or) && a == b) {
                return a;
            }
            return std::nullopt;
        }
        case Op::Kind::Cmp: {
            const std::uint8_t p = op.code;
            const V a = op.a;
            const V b = op.b;
            if (p <= pred::TRUE_) {
                if (!a.is_const() || !b.is_const()) {
                    return std::nullopt;
                }
                const ConstDef& cx = m.consts[a.index].second;
                const ConstDef& cy = m.consts[b.index].second;
                if (cx.kind != ConstDef::Kind::Float || cy.kind != ConstDef::Kind::Float) {
                    return std::nullopt;
                }
                const auto xb = static_cast<std::uint32_t>(cx.value);
                const auto yb = static_cast<std::uint32_t>(cy.value);
                bool holds = false;
                if (float_nan(xb) || float_nan(yb)) {
                    holds = (p & 8) != 0;
                } else {
                    const std::int64_t x = float_order(xb);
                    const std::int64_t y = float_order(yb);
                    holds = ((p & 1) != 0 && x == y) || ((p & 2) != 0 && x > y) || ((p & 4) != 0 && x < y);
                }
                return m.bool_(holds);
            }
            if (p < pred::EQ || p > pred::SLE) {
                return std::nullopt;
            }
            const auto holds = [p](std::uint64_t x, std::uint64_t y, std::uint32_t bits) {
                const std::int64_t sx = as_signed(bits, x);
                const std::int64_t sy = as_signed(bits, y);
                switch (p) {
                case pred::EQ:
                    return x == y;
                case pred::NE:
                    return x != y;
                case pred::IUGT:
                    return x > y;
                case pred::IUGE:
                    return x >= y;
                case pred::IULT:
                    return x < y;
                case pred::IULE:
                    return x <= y;
                case pred::SGT:
                    return sx > sy;
                case pred::SGE:
                    return sx >= sy;
                case pred::SLT:
                    return sx < sy;
                default:
                    return sx <= sy;
                }
            };
            const auto ka = int_k(a);
            const auto kb = int_k(b);
            if (ka && kb) {
                return m.bool_(holds(ka->second, kb->second, ka->first));
            }
            if (a == b) {
                return m.bool_(holds(0, 0, 32));
            }
            if (p == pred::NE && kb && kb->second == 0 && a.is_inst()) {
                const Op& z = m.insts[a.index].op;
                if (z.kind == Op::Kind::Cast && z.cast() == Cast::ZExt && m.type_of(z.a) == t) {
                    return z.a;
                }
            }
            return std::nullopt;
        }
        case Op::Kind::Select: {
            const V c = op.a;
            V tv = op.b;
            V fv = op.c;
            if (const auto kc = int_k(c)) {
                return kc->second != 0 ? tv : fv;
            }
            bool inner = false;
            while (tv.is_inst()) {
                const Op& x = m.insts[tv.index].op;
                if (x.kind != Op::Kind::Select || x.a != c) {
                    break;
                }
                tv = x.b;
                inner = true;
            }
            while (fv.is_inst()) {
                const Op& x = m.insts[fv.index].op;
                if (x.kind != Op::Kind::Select || x.a != c) {
                    break;
                }
                fv = x.c;
                inner = true;
            }
            if (tv == fv) {
                return tv;
            }
            if (int_bits(t) == 1u) {
                const auto kt = int_kv(tv);
                const auto kf = int_kv(fv);
                if (kt && *kt == 1 && kf && *kf == 0) {
                    return c;
                }
            }
            if (inner) {
                Op& w = m.insts[i].op;
                w.a = c;
                w.b = tv;
                w.c = fv;
                changed = true;
            }
            return std::nullopt;
        }
        case Op::Kind::Cast: {
            const V x = op.a;
            const Ty from = m.type_of(x);
            std::optional<std::pair<Cast, V>> through;
            if (x.is_inst()) {
                const Op& y = m.insts[x.index].op;
                if (y.kind == Op::Kind::Cast) {
                    through = std::pair{y.cast(), y.a};
                }
            }
            const Cast c = op.cast();
            if (c == Cast::Bitcast) {
                if (from == t) {
                    return x;
                }
                if (through && through->first == Cast::Bitcast && m.type_of(through->second) == t) {
                    return through->second;
                }
                if (!x.is_const()) {
                    return std::nullopt;
                }
                const bool to_float = m.types[t.id].kind == TypeDef::Kind::Float;
                const ConstDef cd = m.consts[x.index].second;
                if (cd.kind == ConstDef::Kind::Int && to_float && int_bits(from) == 32u) {
                    return m.f32(static_cast<std::uint32_t>(cd.value));
                }
                if (cd.kind == ConstDef::Kind::Float && int_bits(t) == 32u) {
                    return m.int_const(t, static_cast<std::int32_t>(static_cast<std::uint32_t>(cd.value)));
                }
                return std::nullopt;
            }
            if (c == Cast::ZExt || c == Cast::SExt || c == Cast::Trunc) {
                const auto to = int_bits(t);
                if (!to) {
                    return std::nullopt;
                }
                if (const auto kx = int_k(x)) {
                    const std::uint64_t v = c == Cast::SExt ? static_cast<std::uint64_t>(as_signed(kx->first, kx->second)) : kx->second;
                    return k(t, *to, v);
                }
                if (c == Cast::Trunc && through && (through->first == Cast::ZExt || through->first == Cast::SExt)
                    && m.type_of(through->second) == t) {
                    return through->second;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }
        default:
            return std::nullopt;
        }
    }

    Effect class_of(const std::vector<Effect>& effects, std::uint32_t i) const
    {
        const Op& op = m.insts[i].op;
        switch (op.kind) {
        case Op::Kind::Bin:
        case Op::Kind::Cast:
        case Op::Kind::Cmp:
        case Op::Kind::Select:
        case Op::Kind::Extract:
        case Op::Kind::Gep:
            return Effect::Pure;
        case Op::Kind::Load: {
            V base = op.a;
            if (op.a.is_inst()) {
                const Op& g = m.insts[op.a.index].op;
                if (g.kind == Op::Kind::Gep) {
                    base = g.a;
                }
            }
            if (base.is(V::Kind::Global) && m.globals[base.index].init) {
                return Effect::Pure;
            }
            return Effect::Reads;
        }
        case Op::Kind::Store:
        case Op::Kind::AtomicRmw:
        case Op::Kind::CmpXchg:
            return Effect::Writes;
        case Op::Kind::Phi:
            return Effect::Lanes;
        case Op::Kind::Call:
            return effects[op.index];
        }
        return Effect::Writes;
    }

    std::optional<Key> key_of(std::uint32_t i) const
    {
        const Inst& inst = m.insts[i];
        const Op& op = inst.op;
        switch (op.kind) {
        case Op::Kind::Bin: {
            const Bin o = op.bin();
            const bool commutes = (o == Bin::Add || o == Bin::Mul || o == Bin::And || o == Bin::Or || o == Bin::Xor)
                && int_bits(inst.ty).has_value();
            V a = op.a;
            V b = op.b;
            if (commutes && b < a) {
                std::swap(a, b);
            }
            return Key{0, op.code, a.key(), b.key()};
        }
        case Op::Kind::Cast:
            return Key{1, op.code, inst.ty.id, op.a.key()};
        case Op::Kind::Cmp:
            return Key{2, op.code, op.a.key(), op.b.key()};
        case Op::Kind::Select:
            return Key{3, op.a.key(), op.b.key(), op.c.key()};
        case Op::Kind::Extract:
            return Key{4, op.a.key(), op.index};
        case Op::Kind::Call: {
            Key key{5, op.index};
            for (const V v : op.args) {
                key.push_back(v.key());
            }
            return key;
        }
        case Op::Kind::Gep: {
            Key key{6, op.ty.id, op.a.key()};
            for (const V v : op.args) {
                key.push_back(v.key());
            }
            return key;
        }
        case Op::Kind::Load:
            return Key{7, op.a.key()};
        default:
            return std::nullopt;
        }
    }

    bool merge(std::uint32_t passes)
    {
        if (m.blocks.empty()) {
            return false;
        }
        const std::vector<Effect> effects = m.effects();
        const Tree tree = dominator_tree();
        Repl repl(m.insts.size());
        bool changed = false;
        if ((passes & opt::FOLD) != 0) {
            bool again = true;
            while (again) {
                again = false;
                for (const std::size_t b : tree.order) {
                    for (const std::uint32_t p : m.blocks[b].phis) {
                        if (repl[p]) {
                            continue;
                        }
                        const Op& op = m.insts[p].op;
                        if (op.kind != Op::Kind::Phi) {
                            continue;
                        }
                        std::optional<V> one;
                        bool same = true;
                        for (const auto& in : op.incoming) {
                            const V v = resolve(repl, in.first);
                            if (v == V::Inst(p)) {
                                continue;
                            }
                            if (!one) {
                                one = v;
                            } else if (*one != v) {
                                same = false;
                            }
                        }
                        if (same && one) {
                            repl[p] = *one;
                            again = true;
                            changed = true;
                        }
                    }
                }
            }
        }
        std::unordered_map<Key, V, KeyHash> known;
        std::unordered_map<Key, V, KeyHash> read;
        Facts facts;
        const V yes = m.bool_(true);
        const V no = m.bool_(false);
        struct Step {
            bool leave{false};
            std::size_t block{0};
            std::vector<Key> keys;
            Learned learned;
        };
        std::vector<Step> steps;
        steps.push_back(Step{false, 0, {}, {}});
        const auto known_bit = [&](V v) {
            const auto it = facts.bits.find(v);
            if (it == facts.bits.end()) {
                return v;
            }
            return it->second ? yes : no;
        };
        while (!steps.empty()) {
            Step step = std::move(steps.back());
            steps.pop_back();
            if (step.leave) {
                for (const Key& key : step.keys) {
                    known.erase(key);
                }
                for (const V v : step.learned.bits) {
                    facts.bits.erase(v);
                }
                for (const CmpKey& key : step.learned.compares) {
                    facts.compares.erase(key);
                }
                continue;
            }
            const std::size_t b = step.block;
            std::vector<Key> added;
            Learned learned;
            if ((passes & opt::FACTS) != 0 && tree.preds[b].size() == 1) {
                const std::size_t p = tree.preds[b][0];
                const auto& term = m.blocks[p].term;
                if (term && term->kind == Term::Kind::CondBr && term->t != term->f) {
                    learn(resolve(repl, term->cond), b == term->t, facts, learned, 8);
                }
            }
            read.clear();
            if ((passes & opt::REUSE) != 0 && m.blocks[b].phis.size() > 1) {
                std::unordered_map<Key, V, KeyHash> joins;
                for (const std::uint32_t p : m.blocks[b].phis) {
                    if (repl[p]) {
                        continue;
                    }
                    const Ty t = m.insts[p].ty;
                    const Op& op = m.insts[p].op;
                    if (op.kind != Op::Kind::Phi) {
                        continue;
                    }
                    std::vector<std::pair<std::size_t, V>> pairs;
                    pairs.reserve(op.incoming.size());
                    for (const auto& [v, from] : op.incoming) {
                        pairs.emplace_back(from, resolve(repl, v));
                    }
                    std::sort(pairs.begin(), pairs.end());
                    Key key;
                    key.reserve(pairs.size() * 2 + 2);
                    for (const auto& [from, v] : pairs) {
                        key.push_back(from);
                        key.push_back(v.key());
                    }
                    key.push_back(t.id);
                    key.push_back(V::Global(std::numeric_limits<std::uint32_t>::max()).key());
                    if (const auto it = joins.find(key); it != joins.end()) {
                        repl[p] = it->second;
                        changed = true;
                    } else {
                        joins.emplace(std::move(key), V::Inst(p));
                    }
                }
            }
            std::vector<std::uint32_t> body = std::move(m.blocks[b].body);
            m.blocks[b].body.clear();
            std::vector<std::uint32_t> kept;
            kept.reserve(body.size());
            for (const std::uint32_t i : body) {
                map_operands(m.insts[i].op, [&](V v) {
                    const V r = resolve(repl, v);
                    const V bit = known_bit(r);
                    changed |= bit != r;
                    return bit;
                });
                {
                    const Op& op = m.insts[i].op;
                    if (op.kind == Op::Kind::Cmp) {
                        const auto it = facts.compares.find(CmpKey{op.code, op.a, op.b});
                        if (it != facts.compares.end()) {
                            repl[i] = it->second ? yes : no;
                            changed = true;
                            continue;
                        }
                    }
                }
                if ((passes & opt::FOLD) != 0) {
                    if (const auto v = simplify(i, changed)) {
                        repl[i] = *v;
                        changed = true;
                        continue;
                    }
                }
                const Effect cls = class_of(effects, i);
                if ((passes & opt::REUSE) != 0 && cls == Effect::Pure) {
                    if (auto key = key_of(i)) {
                        if (const auto it = known.find(*key); it != known.end()) {
                            repl[i] = it->second;
                            changed = true;
                            continue;
                        }
                        known.emplace(*key, V::Inst(i));
                        added.push_back(std::move(*key));
                    }
                }
                if ((passes & opt::LOADS) != 0 && (cls == Effect::Reads || cls == Effect::Lanes)) {
                    if (auto key = key_of(i)) {
                        if (const auto it = read.find(*key); it != read.end()) {
                            repl[i] = it->second;
                            changed = true;
                            continue;
                        }
                        read.emplace(std::move(*key), V::Inst(i));
                    }
                }
                if (cls == Effect::Writes) {
                    read.clear();
                }
                kept.push_back(i);
            }
            m.blocks[b].body = std::move(kept);
            if (auto& term = m.blocks[b].term; term && term->kind == Term::Kind::CondBr) {
                term->cond = known_bit(resolve(repl, term->cond));
            }
            steps.push_back(Step{true, b, std::move(added), std::move(learned)});
            const auto& kids = tree.children[b];
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                steps.push_back(Step{false, *it, {}, {}});
            }
        }
        if (!changed) {
            return false;
        }
        for (Block& blk : m.blocks) {
            std::erase_if(blk.phis, [&](std::uint32_t p) { return repl[p].has_value(); });
            if (blk.term && blk.term->kind == Term::Kind::CondBr) {
                blk.term->cond = resolve(repl, blk.term->cond);
            }
        }
        const auto remap = [&](V v) { return resolve(repl, v); };
        for (const Block& blk : m.blocks) {
            for (const std::uint32_t i : blk.phis) {
                map_operands(m.insts[i].op, remap);
            }
            for (const std::uint32_t i : blk.body) {
                map_operands(m.insts[i].op, remap);
            }
        }
        return true;
    }

    bool prune()
    {
        bool changed = false;
        for (std::size_t b = 0; b < m.blocks.size(); ++b) {
            const auto term = m.blocks[b].term;
            if (!term || term->kind != Term::Kind::CondBr) {
                continue;
            }
            if (term->t == term->f) {
                continue;
            }
            const auto kc = int_k(term->cond);
            if (!kc) {
                continue;
            }
            const std::size_t taken = kc->second != 0 ? term->t : term->f;
            const std::size_t other = kc->second != 0 ? term->f : term->t;
            m.blocks[b].term = Term::Br(taken);
            changed = true;
            for (const std::uint32_t p : m.blocks[other].phis) {
                Op& op = m.insts[p].op;
                if (op.kind == Op::Kind::Phi) {
                    std::erase_if(op.incoming, [&](const auto& in) { return in.second == b; });
                }
            }
        }
        if (!changed) {
            return false;
        }
        const std::vector<bool> reached = m.reached();
        for (std::size_t b = 0; b < m.blocks.size(); ++b) {
            if (!reached[b]) {
                continue;
            }
            for (const std::uint32_t p : m.blocks[b].phis) {
                Op& op = m.insts[p].op;
                if (op.kind == Op::Kind::Phi) {
                    std::erase_if(op.incoming, [&](const auto& in) { return !reached[in.second]; });
                }
            }
        }
        return true;
    }

    bool has_effect(const std::vector<Effect>& effects, std::uint32_t i) const
    {
        const Op& op = m.insts[i].op;
        switch (op.kind) {
        case Op::Kind::Store:
        case Op::Kind::AtomicRmw:
        case Op::Kind::CmpXchg:
            return true;
        case Op::Kind::Call:
            return effects[op.index] == Effect::Writes;
        default:
            return false;
        }
    }

    void drop_dead()
    {
        const std::vector<Effect> effects = m.effects();
        const std::vector<bool> reached = m.reached();
        std::vector<bool> alive(m.insts.size(), false);
        std::vector<std::uint32_t> work;
        const auto mark = [&](V v) {
            if (v.is_inst() && !alive[v.index]) {
                alive[v.index] = true;
                work.push_back(v.index);
            }
        };
        for (std::size_t b = 0; b < m.blocks.size(); ++b) {
            if (!reached[b]) {
                continue;
            }
            const Block& blk = m.blocks[b];
            for (const std::uint32_t i : blk.body) {
                if (has_effect(effects, i)) {
                    mark(V::Inst(i));
                }
            }
            if (blk.term && blk.term->kind == Term::Kind::CondBr) {
                mark(blk.term->cond);
            }
        }
        while (!work.empty()) {
            const std::uint32_t i = work.back();
            work.pop_back();
            for_each_operand(m.insts[i].op, mark);
        }
        for (std::size_t b = 0; b < m.blocks.size(); ++b) {
            Block& blk = m.blocks[b];
            if (reached[b]) {
                std::erase_if(blk.body, [&](std::uint32_t i) { return !alive[i]; });
                std::erase_if(blk.phis, [&](std::uint32_t i) { return !alive[i]; });
            } else {
                blk.body.clear();
                blk.phis.clear();
            }
        }
        drop_unused_functions();
        drop_unused_tables();
    }

    void drop_unused_tables()
    {
        std::vector<bool> used;
        used.reserve(m.globals.size());
        for (const Global& g : m.globals) {
            used.push_back(!g.init.has_value());
        }
        if (std::all_of(used.begin(), used.end(), [](bool u) { return u; })) {
            return;
        }
        const auto use = [&](V v) {
            if (v.is(V::Kind::Global)) {
                used[v.index] = true;
            }
        };
        for (const Block& blk : m.blocks) {
            for (const std::uint32_t i : blk.phis) {
                for_each_operand(m.insts[i].op, use);
            }
            for (const std::uint32_t i : blk.body) {
                for_each_operand(m.insts[i].op, use);
            }
        }
        for (const MdDef& md : m.mds) {
            if (md.kind == MdDef::Kind::Value && md.value.is(V::Kind::Global)) {
                used[md.value.index] = true;
            }
        }
        if (std::all_of(used.begin(), used.end(), [](bool u) { return u; })) {
            return;
        }
        std::vector<std::uint32_t> fresh(used.size(), std::numeric_limits<std::uint32_t>::max());
        std::uint32_t next = 0;
        for (std::size_t old = 0; old < used.size(); ++old) {
            if (used[old]) {
                fresh[old] = next++;
            }
        }
        std::vector<Global> kept;
        for (std::size_t old = 0; old < used.size(); ++old) {
            if (used[old]) {
                kept.push_back(std::move(m.globals[old]));
            }
        }
        m.globals = std::move(kept);
        const auto renumber = [&](V v) {
            if (v.is(V::Kind::Global) && used[v.index]) {
                return V::Global(fresh[v.index]);
            }
            return v;
        };
        for (Inst& inst : m.insts) {
            map_operands(inst.op, renumber);
        }
        for (MdDef& md : m.mds) {
            if (md.kind == MdDef::Kind::Value) {
                md.value = renumber(md.value);
            }
        }
    }

    void drop_unused_functions()
    {
        std::vector<bool> used;
        used.reserve(m.funcs.size());
        for (const Func& f : m.funcs) {
            used.push_back(f.body);
        }
        for (const Block& blk : m.blocks) {
            for (const std::uint32_t i : blk.phis) {
                if (m.insts[i].op.kind == Op::Kind::Call) {
                    used[m.insts[i].op.index] = true;
                }
            }
            for (const std::uint32_t i : blk.body) {
                if (m.insts[i].op.kind == Op::Kind::Call) {
                    used[m.insts[i].op.index] = true;
                }
            }
        }
        for (const MdDef& md : m.mds) {
            if (md.kind == MdDef::Kind::Value && md.value.is(V::Kind::Func)) {
                used[md.value.index] = true;
            }
        }
        if (std::all_of(used.begin(), used.end(), [](bool u) { return u; })) {
            return;
        }
        std::vector<std::uint32_t> fresh(used.size(), std::numeric_limits<std::uint32_t>::max());
        std::uint32_t next = 0;
        for (std::size_t old = 0; old < used.size(); ++old) {
            if (used[old]) {
                fresh[old] = next++;
            }
        }
        std::vector<Func> kept;
        for (std::size_t old = 0; old < used.size(); ++old) {
            if (used[old]) {
                kept.push_back(std::move(m.funcs[old]));
            }
        }
        m.funcs = std::move(kept);
        m.func_ids.clear();
        for (std::size_t i = 0; i < m.funcs.size(); ++i) {
            m.func_ids.emplace(m.funcs[i].name, static_cast<std::uint32_t>(i));
        }
        for (Inst& inst : m.insts) {
            if (inst.op.kind == Op::Kind::Call && inst.op.index < used.size() && used[inst.op.index]) {
                inst.op.index = fresh[inst.op.index];
            }
        }
        for (MdDef& md : m.mds) {
            if (md.kind == MdDef::Kind::Value && md.value.is(V::Kind::Func)) {
                md.value = V::Func(fresh[md.value.index]);
            }
        }
    }
};

std::atomic<std::int64_t> g_shader_opt{-1};

std::string_view trim(std::string_view s) noexcept
{
    constexpr std::string_view kSpace = " \t\r\n\f\v";
    const auto first = s.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(kSpace);
    return s.substr(first, last - first + 1);
}

}

Effect effect(const Func& f)
{
    std::string_view name = f.name;
    if (name.starts_with("dx.op.")) {
        name.remove_prefix(6);
    }
    if (f.attr == Attr::ReadNone) {
        return Effect::Pure;
    }
    if (f.attr == Attr::ReadOnly) {
        if (name.starts_with("cbufferLoadLegacy") || name.starts_with("getDimensions")) {
            return Effect::Pure;
        }
        if (name.starts_with("wave")) {
            return Effect::Lanes;
        }
        return Effect::Reads;
    }
    if (name.starts_with("wave") || name.starts_with("quad")) {
        return Effect::Lanes;
    }
    return Effect::Writes;
}

std::uint32_t parse_shader_opt(std::optional<std::string_view> value)
{
    if (!value) {
        return opt::DEFAULT;
    }
    std::string_view v = trim(*value);
    int base = 10;
    if (v.starts_with("0x")) {
        v.remove_prefix(2);
        base = 16;
    }
    std::uint32_t n = 0;
    const char* begin = v.data();
    const char* end = v.data() + v.size();
    const auto [ptr, ec] = std::from_chars(begin, end, n, base);
    if (v.empty() || ec != std::errc{} || ptr != end) {
        return opt::DEFAULT;
    }
    return n & opt::ALL;
}

std::uint32_t shader_opt()
{
    std::int64_t passes = g_shader_opt.load(std::memory_order_relaxed);
    if (passes < 0) {
        const char* env = std::getenv("EUPHORIA_SHADER_OPT");
        passes = parse_shader_opt(env != nullptr ? std::optional<std::string_view>{env} : std::nullopt);
        g_shader_opt.store(passes, std::memory_order_relaxed);
    }
    return static_cast<std::uint32_t>(passes);
}

void set_shader_opt(std::uint32_t passes)
{
    g_shader_opt.store(passes & opt::ALL, std::memory_order_relaxed);
}

void Module::optimise(std::uint32_t passes)
{
    Pass pass{*this};
    if ((passes & (opt::FOLD | opt::REUSE | opt::FACTS | opt::LOADS)) != 0) {
        for (int round = 0; round < 4; ++round) {
            bool changed = pass.merge(passes);
            if ((passes & opt::FOLD) != 0) {
                changed |= pass.prune();
            }
            if (!changed) {
                break;
            }
        }
    }
    if ((passes & opt::DEAD) != 0) {
        pass.drop_dead();
    }
}

std::vector<Effect> Module::effects() const
{
    std::vector<Effect> out;
    out.reserve(funcs.size());
    for (const Func& f : funcs) {
        out.push_back(effect(f));
    }
    return out;
}

std::vector<bool> Module::reached() const
{
    std::vector<bool> seen(blocks.size(), false);
    if (blocks.empty()) {
        return seen;
    }
    std::vector<std::size_t> stack{0};
    seen[0] = true;
    while (!stack.empty()) {
        const std::size_t b = stack.back();
        stack.pop_back();
        for (const std::size_t s : successors(blocks[b].term)) {
            if (!seen[s]) {
                seen[s] = true;
                stack.push_back(s);
            }
        }
    }
    return seen;
}

Usage Module::usage() const
{
    Usage u;
    const std::vector<bool> reach = reached();
    const auto wide = [&](Ty t) {
        const TypeDef& d = types[t.id];
        return d.kind == TypeDef::Kind::Int && d.a == 64;
    };
    const auto annotation = [&](V h) -> std::optional<std::pair<std::int64_t, std::int64_t>> {
        if (!h.is_inst()) {
            return std::nullopt;
        }
        const Op& op = insts[h.index].op;
        if (op.kind != Op::Kind::Call) {
            return std::nullopt;
        }
        if (funcs[op.index].name != "dx.op.annotateHandle") {
            return std::nullopt;
        }
        if (op.args.size() <= 2 || !op.args[2].is_const()) {
            return std::nullopt;
        }
        const ConstDef& props = consts[op.args[2].index].second;
        if (props.kind != ConstDef::Kind::Aggregate || props.ids.size() < 2) {
            return std::nullopt;
        }
        const ConstDef& f0 = consts[props.ids[0]].second;
        const ConstDef& f1 = consts[props.ids[1]].second;
        if (f0.kind != ConstDef::Kind::Int || f1.kind != ConstDef::Kind::Int) {
            return std::nullopt;
        }
        return std::pair{f0.value, f1.value};
    };
    const auto int_k = [&](V v) -> std::optional<std::uint64_t> {
        if (!v.is_const()) {
            return std::nullopt;
        }
        const auto& [t, c] = consts[v.index];
        if (c.kind != ConstDef::Kind::Int || types[t.id].kind != TypeDef::Kind::Int) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(c.value) & mask(types[t.id].a);
    };
    const auto visit = [&](std::uint32_t i) {
        const Inst& inst = insts[i];
        u.int64 |= wide(inst.ty);
        for_each_operand(inst.op, [&](V v) { u.int64 |= wide(type_of(v)); });
        if (inst.op.kind != Op::Kind::Call) {
            return;
        }
        const std::vector<V>& args = inst.op.args;
        std::string_view name = funcs[inst.op.index].name;
        if (name.starts_with("dx.op.")) {
            name.remove_prefix(6);
        }
        if (name.starts_with("wave") || name.starts_with("quad")) {
            u.wave_ops = true;
        } else if (name == "createHandleFromHeap") {
            const auto s = args.size() > 2 ? int_k(args[2]) : std::nullopt;
            if (s && *s == 0) {
                u.heap_views = true;
            } else {
                u.heap_samplers = true;
            }
        } else if (name.starts_with("atomic") && name.ends_with(".i64")) {
            u.atomic64 = true;
        } else if (name.starts_with("textureLoad") || name.starts_with("bufferLoad")) {
            const auto a = args.size() > 1 ? annotation(args[1]) : std::nullopt;
            u.typed_uav_loads |= !a || ((a->first & 0x1000) != 0 && ((a->second >> 8) & 0xff) > 1);
        }
    };
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        if (!reach[b]) {
            continue;
        }
        for (const std::uint32_t i : blocks[b].phis) {
            visit(i);
        }
        for (const std::uint32_t i : blocks[b].body) {
            visit(i);
        }
    }
    return u;
}

}
