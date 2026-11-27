#include <euphoria/shader/gfx10/emit/bitcode.hpp>

#include <euphoria/shader/gfx10/emit/bitcode_census.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace euphoria::shader::gfx10::emit {
namespace {

constexpr std::uint64_t kModuleBlock = 8;
constexpr std::uint64_t kParamattrBlock = 9;
constexpr std::uint64_t kParamattrGroupBlock = 10;
constexpr std::uint64_t kConstantsBlock = 11;
constexpr std::uint64_t kFunctionBlock = 12;
constexpr std::uint64_t kValueSymtabBlock = 14;
constexpr std::uint64_t kMetadataBlock = 15;
constexpr std::uint64_t kTypeBlock = 17;

constexpr std::uint64_t kNone = std::numeric_limits<std::uint64_t>::max();

std::size_t mix(std::size_t h, std::uint64_t v) noexcept
{
    h ^= static_cast<std::size_t>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

void chars(std::vector<std::uint64_t>& out, std::string_view s)
{
    for (const char c : s) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
}

std::vector<std::uint64_t> chars(std::string_view s)
{
    std::vector<std::uint64_t> out;
    out.reserve(s.size());
    chars(out, s);
    return out;
}

[[noreturn]] void panic(const char* what)
{
    throw std::logic_error(what);
}

}

std::size_t TypeDefHash::operator()(const TypeDef& t) const noexcept
{
    std::size_t h = static_cast<std::size_t>(t.kind);
    h = mix(h, t.a);
    h = mix(h, t.b);
    h = mix(h, t.named ? 1 : 0);
    h = mix(h, std::hash<std::string>{}(t.name));
    for (const Ty f : t.fields) {
        h = mix(h, f.id);
    }
    return h;
}

std::size_t ConstKeyHash::operator()(const ConstKey& k) const noexcept
{
    std::size_t h = k.ty.id;
    h = mix(h, static_cast<std::uint64_t>(k.c.kind));
    h = mix(h, static_cast<std::uint64_t>(k.c.value));
    for (const std::uint32_t i : k.c.ids) {
        h = mix(h, i);
    }
    return h;
}

Succs successors(const std::optional<Term>& term) noexcept
{
    if (!term) {
        return {};
    }
    switch (term->kind) {
    case Term::Kind::Br:
        return Succs{{term->t, 0}, 1};
    case Term::Kind::CondBr:
        return Succs{{term->t, term->f}, 2};
    case Term::Kind::Ret:
        break;
    }
    return {};
}

std::uint64_t signed_operand(std::int64_t v) noexcept
{
    if (v >= 0) {
        return static_cast<std::uint64_t>(v) << 1;
    }
    const std::uint64_t magnitude = 0 - static_cast<std::uint64_t>(v);
    return (magnitude << 1) | 1;
}

std::uint32_t Writer::width() const noexcept
{
    return stack.empty() ? 2u : stack.back().first;
}

void Writer::emit(std::uint64_t v, std::uint32_t w)
{
    cur |= v << bits;
    bits += w;
    while (bits >= 32) {
        words.push_back(static_cast<std::uint32_t>(cur));
        cur >>= 32;
        bits -= 32;
    }
}

void Writer::vbr(std::uint64_t v, std::uint32_t w)
{
    const std::uint64_t hi = std::uint64_t{1} << (w - 1);
    while (v >= hi) {
        emit((v & (hi - 1)) | hi, w);
        v >>= w - 1;
    }
    emit(v, w);
}

void Writer::align()
{
    if (bits > 0) {
        words.push_back(static_cast<std::uint32_t>(cur));
        cur = 0;
        bits = 0;
    }
}

void Writer::bytes(std::string_view b)
{
    for (const char x : b) {
        emit(static_cast<std::uint8_t>(x), 8);
    }
}

void Writer::enter(std::uint64_t id, std::uint32_t w)
{
    emit(1, width());
    vbr(id, 8);
    vbr(w, 4);
    align();
    words.push_back(0);
    stack.emplace_back(w, words.size());
}

void Writer::exit()
{
    emit(0, width());
    align();
    if (stack.empty()) {
        panic("open block");
    }
    const std::size_t start = stack.back().second;
    stack.pop_back();
    words[start - 1] = static_cast<std::uint32_t>(words.size() - start);
}

void Writer::record(std::uint64_t code, const std::vector<std::uint64_t>& ops)
{
    emit(3, width());
    vbr(code, 6);
    vbr(ops.size(), 6);
    for (const std::uint64_t o : ops) {
        vbr(o, 6);
    }
}

std::vector<std::uint8_t> Writer::finish()
{
    align();
    std::vector<std::uint8_t> out(words.size() * 4);
    if (!words.empty()) {
        std::memcpy(out.data(), words.data(), out.size());
    }
    return out;
}

Ty Module::ty(TypeDef def)
{
    if (const auto it = type_ids.find(def); it != type_ids.end()) {
        return Ty{it->second};
    }
    const auto t = static_cast<std::uint32_t>(types.size());
    types.push_back(def);
    type_ids.emplace(std::move(def), t);
    return Ty{t};
}

Ty Module::void_() { return ty(TypeDef{TypeDef::Kind::Void}); }
Ty Module::int_(std::uint32_t bits) { return ty(TypeDef{TypeDef::Kind::Int, bits}); }
Ty Module::float_() { return ty(TypeDef{TypeDef::Kind::Float}); }
Ty Module::double_() { return ty(TypeDef{TypeDef::Kind::Double}); }
Ty Module::ptr(Ty to) { return ty(TypeDef{TypeDef::Kind::Ptr, to.id, 0}); }
Ty Module::ptr_in(Ty to, std::uint32_t space) { return ty(TypeDef{TypeDef::Kind::Ptr, to.id, space}); }
Ty Module::array(std::uint32_t n, Ty of) { return ty(TypeDef{TypeDef::Kind::Array, n, of.id}); }

Ty Module::named_struct(std::string_view name, std::vector<Ty> fields)
{
    return ty(TypeDef{TypeDef::Kind::Struct, 0, 0, true, std::string{name}, std::move(fields)});
}

Ty Module::func_ty(Ty ret, std::vector<Ty> params)
{
    return ty(TypeDef{TypeDef::Kind::Func, ret.id, 0, false, {}, std::move(params)});
}

bool Module::is_void(Ty t) const
{
    return types[t.id].kind == TypeDef::Kind::Void;
}

V Module::constant(Ty t, ConstDef c)
{
    ConstKey key{t, std::move(c)};
    if (const auto it = const_ids.find(key); it != const_ids.end()) {
        return V::Const(it->second);
    }
    const auto i = static_cast<std::uint32_t>(consts.size());
    consts.emplace_back(t, key.c);
    const_ids.emplace(std::move(key), i);
    return V::Const(i);
}

V Module::int_const(Ty t, std::int64_t v)
{
    return constant(t, ConstDef::Int(v));
}

V Module::i32(std::uint32_t v)
{
    const Ty t = int_(32);
    return int_const(t, static_cast<std::int32_t>(v));
}

V Module::i8(std::uint8_t v)
{
    const Ty t = int_(8);
    return int_const(t, static_cast<std::int8_t>(v));
}

V Module::bool_(bool v)
{
    const Ty t = int_(1);
    return int_const(t, v ? -1 : 0);
}

V Module::f32(std::uint32_t bits)
{
    const Ty t = float_();
    return constant(t, ConstDef::Float(bits));
}

V Module::undef(Ty t)
{
    return constant(t, ConstDef::Undef());
}

V Module::aggregate(Ty t, const std::vector<V>& fields)
{
    std::vector<std::uint32_t> ids;
    ids.reserve(fields.size());
    for (const V f : fields) {
        if (!f.is_const()) {
            panic("aggregate of a non-constant");
        }
        ids.push_back(f.index);
    }
    return constant(t, ConstDef::Aggregate(std::move(ids)));
}

std::optional<std::int64_t> Module::int_value(V v) const
{
    if (!v.is_const()) {
        return std::nullopt;
    }
    const ConstDef& c = consts[v.index].second;
    if (c.kind != ConstDef::Kind::Int) {
        return std::nullopt;
    }
    return c.value;
}

void Module::replace_const(V old, V replacement)
{
    if (old.is_const() && replacement.is_const()) {
        consts[old.index] = consts[replacement.index];
    }
}

V Module::function(std::string_view name, Ty t, std::optional<Attr> attr, bool body)
{
    std::string key{name};
    if (const auto it = func_ids.find(key); it != func_ids.end()) {
        return V::Func(it->second);
    }
    const auto i = static_cast<std::uint32_t>(funcs.size());
    funcs.push_back(Func{key, t, attr, body});
    func_ids.emplace(std::move(key), i);
    return V::Func(i);
}

V Module::global(std::string_view name, Ty t, std::uint32_t space)
{
    const Ty p = ptr_in(t, space);
    globals.push_back(Global{std::string{name}, t, p, space, std::nullopt});
    return V::Global(static_cast<std::uint32_t>(globals.size() - 1));
}

V Module::constant_global(std::string_view name, V init)
{
    if (!init.is_const()) {
        panic("a global initialised with a non-constant");
    }
    const Ty t = consts[init.index].first;
    const V g = global(name, t, 0);
    globals[g.index].init = init.index;
    return g;
}

void Module::tag(V v, Tag t)
{
    if (v.is_inst() && census::enabled()) {
        tags[v.index] = t;
    }
}

std::size_t Module::new_block()
{
    blocks.emplace_back();
    return blocks.size() - 1;
}

Ty Module::type_of(V v) const
{
    switch (v.kind) {
    case V::Kind::Global:
        return globals[v.index].ptr;
    case V::Kind::Const:
        return consts[v.index].first;
    case V::Kind::Inst:
        return insts[v.index].ty;
    case V::Kind::Func: {
        const Ty t = funcs[v.index].ty;
        const auto it = type_ids.find(TypeDef{TypeDef::Kind::Ptr, t.id, 0});
        if (it == type_ids.end()) {
            panic("function pointer type");
        }
        return Ty{it->second};
    }
    }
    panic("value kind");
}

V Module::push(std::size_t block, Ty t, Op op)
{
    const auto i = static_cast<std::uint32_t>(insts.size());
    insts.push_back(Inst{t, std::move(op)});
    blocks[block].body.push_back(i);
    return V::Inst(i);
}

V Module::bin(std::size_t block, Bin op, V a, V b)
{
    const Ty t = type_of(a);
    Op o;
    o.kind = Op::Kind::Bin;
    o.code = static_cast<std::uint8_t>(op);
    o.a = a;
    o.b = b;
    return push(block, t, std::move(o));
}

V Module::cast(std::size_t block, Cast op, V v, Ty to)
{
    Op o;
    o.kind = Op::Kind::Cast;
    o.code = static_cast<std::uint8_t>(op);
    o.a = v;
    return push(block, to, std::move(o));
}

V Module::cmp(std::size_t block, std::uint8_t p, V a, V b)
{
    const Ty t = int_(1);
    Op o;
    o.kind = Op::Kind::Cmp;
    o.code = p;
    o.a = a;
    o.b = b;
    return push(block, t, std::move(o));
}

V Module::select(std::size_t block, V c, V t, V f)
{
    const Ty ty_ = type_of(t);
    Op o;
    o.kind = Op::Kind::Select;
    o.a = c;
    o.b = t;
    o.c = f;
    return push(block, ty_, std::move(o));
}

V Module::extract(std::size_t block, V agg, std::uint32_t index)
{
    const TypeDef& def = types[type_of(agg).id];
    Ty t;
    if (def.kind == TypeDef::Kind::Struct) {
        t = def.fields[index];
    } else if (def.kind == TypeDef::Kind::Array) {
        t = Ty{def.b};
    } else {
        panic("extractvalue from a non-aggregate");
    }
    Op o;
    o.kind = Op::Kind::Extract;
    o.a = agg;
    o.index = index;
    return push(block, t, std::move(o));
}

V Module::call(std::size_t block, V f, std::vector<V> args)
{
    if (!f.is(V::Kind::Func)) {
        panic("call of a non-function");
    }
    const TypeDef& def = types[funcs[f.index].ty.id];
    if (def.kind != TypeDef::Kind::Func) {
        panic("call of a non-function type");
    }
    const Ty ret{def.a};
    Op o;
    o.kind = Op::Kind::Call;
    o.index = f.index;
    o.args = std::move(args);
    return push(block, ret, std::move(o));
}

V Module::phi(std::size_t block, Ty t)
{
    const auto i = static_cast<std::uint32_t>(insts.size());
    Op o;
    o.kind = Op::Kind::Phi;
    insts.push_back(Inst{t, std::move(o)});
    blocks[block].phis.push_back(i);
    return V::Inst(i);
}

V Module::element(std::size_t block, V p, V index)
{
    const TypeDef pd = types[type_of(p).id];
    if (pd.kind != TypeDef::Kind::Ptr) {
        panic("element of a non-pointer");
    }
    const Ty arr{pd.a};
    const TypeDef ad = types[arr.id];
    if (ad.kind != TypeDef::Kind::Array) {
        panic("element of a non-array");
    }
    const Ty t = ptr_in(Ty{ad.b}, pd.b);
    const V zero = i32(0);
    Op o;
    o.kind = Op::Kind::Gep;
    o.ty = arr;
    o.a = p;
    o.args = {zero, index};
    return push(block, t, std::move(o));
}

V Module::load(std::size_t block, V p)
{
    const TypeDef& pd = types[type_of(p).id];
    if (pd.kind != TypeDef::Kind::Ptr) {
        panic("load from a non-pointer");
    }
    const Ty t{pd.a};
    Op o;
    o.kind = Op::Kind::Load;
    o.a = p;
    return push(block, t, std::move(o));
}

void Module::store(std::size_t block, V p, V value)
{
    const Ty v = void_();
    Op o;
    o.kind = Op::Kind::Store;
    o.a = p;
    o.b = value;
    push(block, v, std::move(o));
}

V Module::atomic(std::size_t block, Rmw op, V p, V value)
{
    const Ty t = type_of(value);
    Op o;
    o.kind = Op::Kind::AtomicRmw;
    o.code = static_cast<std::uint8_t>(op);
    o.a = p;
    o.b = value;
    return push(block, t, std::move(o));
}

V Module::cmpxchg(std::size_t block, V p, V expected, V replacement)
{
    const Ty i32t = type_of(replacement);
    const Ty i1 = int_(1);
    const Ty pair = ty(TypeDef{TypeDef::Kind::Struct, 0, 0, false, {}, {i32t, i1}});
    Op o;
    o.kind = Op::Kind::CmpXchg;
    o.a = p;
    o.b = expected;
    o.c = replacement;
    const V r = push(block, pair, std::move(o));
    return extract(block, r, 0);
}

void Module::set_phi(V p, std::vector<std::pair<V, std::size_t>> incoming)
{
    if (!p.is_inst()) {
        panic("set_phi of a non-instruction");
    }
    Op& o = insts[p.index].op;
    o.kind = Op::Kind::Phi;
    o.incoming = std::move(incoming);
}

void Module::terminate(std::size_t block, Term t)
{
    blocks[block].term = t;
}

Md Module::md_str(std::string_view s)
{
    MdDef d;
    d.kind = MdDef::Kind::Str;
    d.str = std::string{s};
    mds.push_back(std::move(d));
    return Md{static_cast<std::uint32_t>(mds.size() - 1)};
}

Md Module::md_value(V v)
{
    MdDef d;
    d.kind = MdDef::Kind::Value;
    d.ty = type_of(v);
    d.value = v;
    mds.push_back(std::move(d));
    return Md{static_cast<std::uint32_t>(mds.size() - 1)};
}

Md Module::md_node(std::vector<std::optional<Md>> ops)
{
    MdDef d;
    d.kind = MdDef::Kind::Node;
    d.ops = std::move(ops);
    mds.push_back(std::move(d));
    return Md{static_cast<std::uint32_t>(mds.size() - 1)};
}

void Module::named_md(std::string_view name, std::vector<Md> nodes)
{
    named.emplace_back(std::string{name}, std::move(nodes));
}

std::vector<bool> Module::live() const
{
    std::vector<bool> alive(insts.size(), false);
    std::vector<std::uint32_t> work;
    const auto mark = [&](V v) {
        if (v.is_inst() && !alive[v.index]) {
            alive[v.index] = true;
            work.push_back(v.index);
        }
    };
    for (const Block& b : blocks) {
        for (const std::uint32_t i : b.body) {
            mark(V::Inst(i));
        }
        if (b.term && b.term->kind == Term::Kind::CondBr) {
            mark(b.term->cond);
        }
    }
    while (!work.empty()) {
        const std::uint32_t i = work.back();
        work.pop_back();
        for_each_operand(insts[i].op, mark);
    }
    return alive;
}

std::vector<std::uint8_t> Module::write() const
{
    Writer w;
    w.bytes(std::string_view{"BC\xC0\xDE", 4});
    w.enter(kModuleBlock, 3);
    w.record(1, {1});

    w.enter(kParamattrGroupBlock, 3);
    const std::vector<std::pair<std::uint64_t, std::vector<std::uint64_t>>> groups{
        {1, {18, 20}},
        {2, {18}},
        {3, {18, 21}},
        {4, {12, 18}},
    };
    for (const auto& [id, kinds] : groups) {
        std::vector<std::uint64_t> r{id, 0xFFFFFFFFull};
        for (const std::uint64_t k : kinds) {
            r.push_back(0);
            r.push_back(k);
        }
        w.record(3, r);
    }
    w.exit();
    w.enter(kParamattrBlock, 3);
    for (std::uint64_t id = 1; id <= 4; ++id) {
        w.record(2, {id});
    }
    w.exit();

    w.enter(kTypeBlock, 4);
    w.record(1, {types.size()});
    for (const TypeDef& t : types) {
        switch (t.kind) {
        case TypeDef::Kind::Void:
            w.record(2, {});
            break;
        case TypeDef::Kind::Float:
            w.record(3, {});
            break;
        case TypeDef::Kind::Double:
            w.record(4, {});
            break;
        case TypeDef::Kind::Int:
            w.record(7, {t.a});
            break;
        case TypeDef::Kind::Ptr:
            w.record(8, {t.a, t.b});
            break;
        case TypeDef::Kind::Array:
            w.record(11, {t.a, t.b});
            break;
        case TypeDef::Kind::Struct: {
            std::vector<std::uint64_t> r{0};
            for (const Ty f : t.fields) {
                r.push_back(f.id);
            }
            if (t.named) {
                w.record(19, chars(t.name));
                w.record(20, r);
            } else {
                w.record(18, r);
            }
            break;
        }
        case TypeDef::Kind::Func: {
            std::vector<std::uint64_t> r{0, t.a};
            for (const Ty p : t.fields) {
                r.push_back(p.id);
            }
            w.record(21, r);
            break;
        }
        }
    }
    w.exit();

    w.record(2, chars("dxil-ms-dx"));
    w.record(3, chars("e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"));
    const std::uint64_t first_const = globals.size() + funcs.size();
    for (const Global& g : globals) {
        std::uint64_t constant_bit = 0;
        std::uint64_t init = 0;
        std::uint64_t linkage = 0;
        if (g.init) {
            constant_bit = 1;
            init = first_const + *g.init + 1;
            linkage = 3;
        }
        w.record(7, {g.ty.id, (static_cast<std::uint64_t>(g.space) << 2) | 2 | constant_bit, init, linkage, 3, 0});
    }
    for (const Func& f : funcs) {
        const std::uint64_t attr = f.attr ? static_cast<std::uint64_t>(*f.attr) : 0;
        w.record(8, {f.ty.id, 0, f.body ? 0u : 1u, 0, attr, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    }

    const auto nglobals = static_cast<std::uint32_t>(globals.size());
    const std::uint32_t nfuncs = nglobals + static_cast<std::uint32_t>(funcs.size());
    w.enter(kConstantsBlock, 4);
    std::optional<Ty> current;
    for (const auto& [t, c] : consts) {
        if (current != t) {
            w.record(1, {t.id});
            current = t;
        }
        switch (c.kind) {
        case ConstDef::Kind::Undef:
            w.record(3, {});
            break;
        case ConstDef::Kind::Int:
            w.record(4, {signed_operand(c.value)});
            break;
        case ConstDef::Kind::Float:
            w.record(6, {static_cast<std::uint32_t>(c.value)});
            break;
        case ConstDef::Kind::Aggregate: {
            std::vector<std::uint64_t> r;
            r.reserve(c.ids.size());
            for (const std::uint32_t i : c.ids) {
                r.push_back(static_cast<std::uint64_t>(nfuncs) + i);
            }
            w.record(7, r);
            break;
        }
        }
    }
    w.exit();

    const std::vector<bool> alive = live();

    const std::size_t n = blocks.size();
    const auto succs = [&](std::size_t b) { return successors(blocks[b].term); };
    std::vector<bool> seen(n, false);
    std::vector<std::size_t> post;
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, 0}};
    seen[0] = true;
    while (!stack.empty()) {
        auto& top = stack.back();
        const std::size_t b = top.first;
        const std::size_t k = top.second;
        ++top.second;
        const Succs s = succs(b);
        if (k < s.size()) {
            if (!seen[s[k]]) {
                seen[s[k]] = true;
                stack.emplace_back(s[k], 0);
            }
        } else {
            post.push_back(b);
            stack.pop_back();
        }
    }
    std::reverse(post.begin(), post.end());
    std::vector<std::uint64_t> pos(n, kNone);
    for (std::size_t i = 0; i < post.size(); ++i) {
        pos[post[i]] = i;
    }
    std::vector<std::vector<std::size_t>> preds(n);
    for (const std::size_t b : post) {
        for (const std::size_t s : succs(b)) {
            preds[s].push_back(b);
        }
    }
    std::vector<std::optional<std::pair<std::size_t, std::size_t>>> fold(n);
    for (const std::size_t b : post) {
        const Block& blk = blocks[b];
        if (blk.term && blk.term->kind == Term::Kind::Br && blk.phis.empty() && blk.body.empty()
            && preds[b].size() == 1) {
            const std::size_t h = blk.term->t;
            const std::size_t p = preds[b][0];
            if (pos[h] < pos[b] && !succs(p).contains(h)) {
                fold[b] = std::pair{p, h};
            }
        }
    }
    std::vector<std::size_t> layout;
    layout.reserve(post.size());
    for (const std::size_t b : post) {
        if (!fold[b]) {
            layout.push_back(b);
        }
    }
    std::fill(pos.begin(), pos.end(), kNone);
    for (std::size_t i = 0; i < layout.size(); ++i) {
        pos[layout[i]] = i;
    }
    const auto target = [&](std::size_t t) { return fold[t] ? pos[fold[t]->second] : pos[t]; };
    const auto from = [&](std::size_t b) { return fold[b] ? pos[fold[b]->first] : pos[b]; };

    const std::uint32_t base = nfuncs + static_cast<std::uint32_t>(consts.size());
    std::vector<std::uint32_t> ids(insts.size(), std::numeric_limits<std::uint32_t>::max());
    std::uint32_t next = base;
    const auto each_written = [&](const Block& b, auto&& f) {
        for (const std::uint32_t i : b.phis) {
            if (alive[i]) {
                f(i);
            }
        }
        for (const std::uint32_t i : b.body) {
            f(i);
        }
    };
    for (const std::size_t lb : layout) {
        each_written(blocks[lb], [&](std::uint32_t i) {
            if (!is_void(insts[i].ty)) {
                ids[i] = next++;
            }
        });
    }
    const auto abs = [&](V v) -> std::uint32_t {
        switch (v.kind) {
        case V::Kind::Global:
            return v.index;
        case V::Kind::Func:
            return nglobals + v.index;
        case V::Kind::Const:
            return nfuncs + v.index;
        case V::Kind::Inst:
            break;
        }
        const std::uint32_t id = ids[v.index];
        if (id == std::numeric_limits<std::uint32_t>::max()) {
            panic("use of a void value");
        }
        return id;
    };

    w.enter(kMetadataBlock, 3);
    for (const MdDef& m : mds) {
        switch (m.kind) {
        case MdDef::Kind::Str:
            w.record(1, chars(m.str));
            break;
        case MdDef::Kind::Value:
            w.record(2, {m.ty.id, abs(m.value)});
            break;
        case MdDef::Kind::Node: {
            std::vector<std::uint64_t> r;
            r.reserve(m.ops.size());
            for (const auto& o : m.ops) {
                r.push_back(o ? static_cast<std::uint64_t>(o->id) + 1 : 0);
            }
            w.record(3, r);
            break;
        }
        }
    }
    for (const auto& [name, nodes] : named) {
        w.record(4, chars(name));
        std::vector<std::uint64_t> r;
        r.reserve(nodes.size());
        for (const Md m : nodes) {
            r.push_back(m.id);
        }
        w.record(10, r);
    }
    w.exit();
    w.enter(kMetadataBlock, 3);
    constexpr std::array<std::string_view, 14> kinds{
        "dbg",
        "tbaa",
        "prof",
        "fpmath",
        "range",
        "tbaa.struct",
        "invariant.load",
        "alias.scope",
        "noalias",
        "nontemporal",
        "llvm.mem.parallel_loop_access",
        "nonnull",
        "dereferenceable",
        "dereferenceable_or_null",
    };
    for (std::size_t id = 0; id < kinds.size(); ++id) {
        std::vector<std::uint64_t> r{id};
        chars(r, kinds[id]);
        w.record(6, r);
    }
    w.exit();

    w.enter(kValueSymtabBlock, 4);
    {
        std::uint64_t i = 0;
        for (const Global& g : globals) {
            std::vector<std::uint64_t> r{i++};
            chars(r, g.name);
            w.record(1, r);
        }
        for (const Func& f : funcs) {
            std::vector<std::uint64_t> r{i++};
            chars(r, f.name);
            w.record(1, r);
        }
    }
    w.exit();

    w.enter(kFunctionBlock, 4);
    w.record(1, {layout.size()});
    std::uint32_t inst_id = base;
    std::vector<std::uint64_t> r;
    for (const std::size_t lb : layout) {
        const Block& b = blocks[lb];
        each_written(b, [&](std::uint32_t i) {
            const Inst& inst = insts[i];
            const auto rel = [&](V v) -> std::uint64_t {
                const std::uint32_t a = abs(v);
                if (a >= inst_id) {
                    panic("forward reference outside a phi");
                }
                return inst_id - a;
            };
            const Op& op = inst.op;
            switch (op.kind) {
            case Op::Kind::Bin:
                w.record(2, {rel(op.a), rel(op.b), op.code});
                break;
            case Op::Kind::Cast:
                w.record(3, {rel(op.a), inst.ty.id, op.code});
                break;
            case Op::Kind::Cmp:
                w.record(28, {rel(op.a), rel(op.b), op.code});
                break;
            case Op::Kind::Select:
                w.record(29, {rel(op.b), rel(op.c), rel(op.a)});
                break;
            case Op::Kind::Extract:
                w.record(26, {rel(op.a), op.index});
                break;
            case Op::Kind::Call:
                r.clear();
                r.push_back(0);
                r.push_back(1u << 15);
                r.push_back(funcs[op.index].ty.id);
                r.push_back(rel(V::Func(op.index)));
                for (const V a : op.args) {
                    r.push_back(rel(a));
                }
                w.record(34, r);
                break;
            case Op::Kind::Phi:
                r.clear();
                r.push_back(inst.ty.id);
                for (const auto& [v, blk] : op.incoming) {
                    r.push_back(signed_operand(static_cast<std::int64_t>(inst_id) - static_cast<std::int64_t>(abs(v))));
                    r.push_back(from(blk));
                }
                w.record(16, r);
                break;
            case Op::Kind::Gep:
                r.clear();
                r.push_back(0);
                r.push_back(op.ty.id);
                r.push_back(rel(op.a));
                for (const V x : op.args) {
                    r.push_back(rel(x));
                }
                w.record(43, r);
                break;
            case Op::Kind::Load:
                w.record(20, {rel(op.a), inst.ty.id, 3, 0});
                break;
            case Op::Kind::Store:
                w.record(44, {rel(op.a), rel(op.b), 3, 0});
                break;
            case Op::Kind::AtomicRmw:
                w.record(38, {rel(op.a), rel(op.b), op.code, 0, 6, 1});
                break;
            case Op::Kind::CmpXchg:
                w.record(46, {rel(op.a), rel(op.b), rel(op.c), 0, 6, 1, 6, 0});
                break;
            }
            if (!is_void(inst.ty)) {
                ++inst_id;
            }
        });
        if (!b.term) {
            panic("unterminated block");
        }
        switch (b.term->kind) {
        case Term::Kind::Ret:
            w.record(10, {});
            break;
        case Term::Kind::Br:
            w.record(11, {target(b.term->t)});
            break;
        case Term::Kind::CondBr: {
            const std::uint32_t a = abs(b.term->cond);
            if (a >= inst_id) {
                panic("forward branch condition");
            }
            w.record(11, {target(b.term->t), target(b.term->f), inst_id - a});
            break;
        }
        }
    }
    w.exit();
    w.exit();
    return w.finish();
}

}
