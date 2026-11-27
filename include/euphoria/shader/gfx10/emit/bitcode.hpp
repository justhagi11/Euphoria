#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::emit {

struct Usage;
struct Census;
enum class Effect : std::uint8_t;

struct Ty {
    std::uint32_t id{0};

    friend constexpr auto operator<=>(const Ty&, const Ty&) noexcept = default;
};

enum class Tag : std::uint8_t {
    Guard,
    Clamp,
    Counter,
};

struct V {
    enum class Kind : std::uint8_t {
        Global,
        Func,
        Const,
        Inst,
    };

    Kind kind{Kind::Const};
    std::uint32_t index{0};

    static constexpr V Global(std::uint32_t i) noexcept { return V{Kind::Global, i}; }
    static constexpr V Func(std::uint32_t i) noexcept { return V{Kind::Func, i}; }
    static constexpr V Const(std::uint32_t i) noexcept { return V{Kind::Const, i}; }
    static constexpr V Inst(std::uint32_t i) noexcept { return V{Kind::Inst, i}; }

    constexpr bool is(Kind k) const noexcept { return kind == k; }
    constexpr bool is_inst() const noexcept { return kind == Kind::Inst; }
    constexpr bool is_const() const noexcept { return kind == Kind::Const; }
    constexpr std::uint64_t key() const noexcept
    {
        return (static_cast<std::uint64_t>(kind) << 32) | index;
    }

    friend constexpr auto operator<=>(const V&, const V&) noexcept = default;
};

struct VHash {
    std::size_t operator()(V v) const noexcept
    {
        std::uint64_t x = v.key() * 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(x ^ (x >> 29));
    }
};

enum class Bin : std::uint8_t {
    Add = 0,
    Sub = 1,
    Mul = 2,
    UDiv = 3,
    SDiv = 4,
    URem = 5,
    Shl = 7,
    LShr = 8,
    AShr = 9,
    And = 10,
    Or = 11,
    Xor = 12,
};

enum class Rmw : std::uint8_t {
    Xchg = 0,
    Add = 1,
    Sub = 2,
    And = 3,
    Or = 5,
    Xor = 6,
    Max = 7,
    Min = 8,
    UMax = 9,
    UMin = 10,
};

enum class Cast : std::uint8_t {
    Trunc = 0,
    ZExt = 1,
    SExt = 2,
    FpToUi = 3,
    FpToSi = 4,
    UiToFp = 5,
    SiToFp = 6,
    Bitcast = 11,
};

namespace pred {
inline constexpr std::uint8_t FALSE_ = 0;
inline constexpr std::uint8_t OEQ = 1;
inline constexpr std::uint8_t OGT = 2;
inline constexpr std::uint8_t OGE = 3;
inline constexpr std::uint8_t OLT = 4;
inline constexpr std::uint8_t OLE = 5;
inline constexpr std::uint8_t ONE = 6;
inline constexpr std::uint8_t ORD = 7;
inline constexpr std::uint8_t UNO = 8;
inline constexpr std::uint8_t UEQ = 9;
inline constexpr std::uint8_t UGT = 10;
inline constexpr std::uint8_t UGE = 11;
inline constexpr std::uint8_t ULT = 12;
inline constexpr std::uint8_t ULE = 13;
inline constexpr std::uint8_t UNE = 14;
inline constexpr std::uint8_t TRUE_ = 15;
inline constexpr std::uint8_t EQ = 32;
inline constexpr std::uint8_t NE = 33;
inline constexpr std::uint8_t IUGT = 34;
inline constexpr std::uint8_t IUGE = 35;
inline constexpr std::uint8_t IULT = 36;
inline constexpr std::uint8_t IULE = 37;
inline constexpr std::uint8_t SGT = 38;
inline constexpr std::uint8_t SGE = 39;
inline constexpr std::uint8_t SLT = 40;
inline constexpr std::uint8_t SLE = 41;
}

struct TypeDef {
    enum class Kind : std::uint8_t {
        Void,
        Int,
        Float,
        Double,
        Ptr,
        Struct,
        Array,
        Func,
    };

    Kind kind{Kind::Void};
    std::uint32_t a{0};
    std::uint32_t b{0};
    bool named{false};
    std::string name;
    std::vector<Ty> fields;

    friend bool operator==(const TypeDef&, const TypeDef&) = default;
};

struct TypeDefHash {
    std::size_t operator()(const TypeDef& t) const noexcept;
};

struct ConstDef {
    enum class Kind : std::uint8_t {
        Int,
        Float,
        Undef,
        Aggregate,
    };

    Kind kind{Kind::Undef};
    std::int64_t value{0};
    std::vector<std::uint32_t> ids;

    static ConstDef Int(std::int64_t v) { return ConstDef{Kind::Int, v, {}}; }
    static ConstDef Float(std::uint32_t bits) { return ConstDef{Kind::Float, static_cast<std::int64_t>(bits), {}}; }
    static ConstDef Undef() { return ConstDef{Kind::Undef, 0, {}}; }
    static ConstDef Aggregate(std::vector<std::uint32_t> ids) { return ConstDef{Kind::Aggregate, 0, std::move(ids)}; }

    friend bool operator==(const ConstDef&, const ConstDef&) = default;
};

struct ConstKey {
    Ty ty;
    ConstDef c;

    friend bool operator==(const ConstKey&, const ConstKey&) = default;
};

struct ConstKeyHash {
    std::size_t operator()(const ConstKey& k) const noexcept;
};

struct Global {
    std::string name;
    Ty ty;
    Ty ptr;
    std::uint32_t space{0};
    std::optional<std::uint32_t> init;
};

struct Op {
    enum class Kind : std::uint8_t {
        Bin,
        Cast,
        Cmp,
        Select,
        Extract,
        Call,
        Phi,
        Gep,
        Load,
        Store,
        AtomicRmw,
        CmpXchg,
    };

    Kind kind{Kind::Bin};
    std::uint8_t code{0};
    V a;
    V b;
    V c;
    std::uint32_t index{0};
    Ty ty;
    std::vector<V> args;
    std::vector<std::pair<V, std::size_t>> incoming;

    Bin bin() const noexcept { return static_cast<Bin>(code); }
    Cast cast() const noexcept { return static_cast<Cast>(code); }
    Rmw rmw() const noexcept { return static_cast<Rmw>(code); }
};

template <class F>
void for_each_operand(const Op& op, F&& f)
{
    switch (op.kind) {
    case Op::Kind::Bin:
    case Op::Kind::Cmp:
    case Op::Kind::Store:
    case Op::Kind::AtomicRmw:
        f(op.a);
        f(op.b);
        break;
    case Op::Kind::Cast:
    case Op::Kind::Extract:
    case Op::Kind::Load:
        f(op.a);
        break;
    case Op::Kind::Select:
    case Op::Kind::CmpXchg:
        f(op.a);
        f(op.b);
        f(op.c);
        break;
    case Op::Kind::Call:
        for (const V v : op.args) {
            f(v);
        }
        break;
    case Op::Kind::Phi:
        for (const auto& x : op.incoming) {
            f(x.first);
        }
        break;
    case Op::Kind::Gep:
        f(op.a);
        for (const V v : op.args) {
            f(v);
        }
        break;
    }
}

template <class F>
void map_operands(Op& op, F&& f)
{
    switch (op.kind) {
    case Op::Kind::Bin:
    case Op::Kind::Cmp:
    case Op::Kind::Store:
    case Op::Kind::AtomicRmw:
        op.a = f(op.a);
        op.b = f(op.b);
        break;
    case Op::Kind::Cast:
    case Op::Kind::Extract:
    case Op::Kind::Load:
        op.a = f(op.a);
        break;
    case Op::Kind::Select:
    case Op::Kind::CmpXchg:
        op.a = f(op.a);
        op.b = f(op.b);
        op.c = f(op.c);
        break;
    case Op::Kind::Call:
        for (V& v : op.args) {
            v = f(v);
        }
        break;
    case Op::Kind::Phi:
        for (auto& x : op.incoming) {
            x.first = f(x.first);
        }
        break;
    case Op::Kind::Gep:
        op.a = f(op.a);
        for (V& v : op.args) {
            v = f(v);
        }
        break;
    }
}

struct Inst {
    Ty ty;
    Op op;
};

struct Term {
    enum class Kind : std::uint8_t {
        Br,
        CondBr,
        Ret,
    };

    Kind kind{Kind::Ret};
    V cond;
    std::size_t t{0};
    std::size_t f{0};

    static constexpr Term Br(std::size_t target) noexcept { return Term{Kind::Br, V{}, target, 0}; }
    static constexpr Term CondBr(V c, std::size_t t, std::size_t f) noexcept { return Term{Kind::CondBr, c, t, f}; }
    static constexpr Term Ret() noexcept { return Term{Kind::Ret, V{}, 0, 0}; }
};

struct Block {
    std::vector<std::uint32_t> phis;
    std::vector<std::uint32_t> body;
    std::optional<Term> term;
};

enum class Attr : std::uint8_t {
    ReadNone = 1,
    NoUnwind = 2,
    ReadOnly = 3,
    NoDuplicate = 4,
};

struct Func {
    std::string name;
    Ty ty;
    std::optional<Attr> attr;
    bool body{false};
};

struct Md {
    std::uint32_t id{0};

    friend constexpr bool operator==(const Md&, const Md&) noexcept = default;
};

struct MdDef {
    enum class Kind : std::uint8_t {
        Str,
        Value,
        Node,
    };

    Kind kind{Kind::Str};
    std::string str;
    Ty ty;
    V value;
    std::vector<std::optional<Md>> ops;
};

struct Succs {
    std::array<std::size_t, 2> to{};
    std::size_t len{0};

    const std::size_t* begin() const noexcept { return to.data(); }
    const std::size_t* end() const noexcept { return to.data() + len; }
    std::size_t size() const noexcept { return len; }
    bool empty() const noexcept { return len == 0; }
    std::size_t operator[](std::size_t i) const noexcept { return to[i]; }
    bool contains(std::size_t b) const noexcept
    {
        for (std::size_t i = 0; i < len; ++i) {
            if (to[i] == b) {
                return true;
            }
        }
        return false;
    }
};

Succs successors(const std::optional<Term>& term) noexcept;

std::uint64_t signed_operand(std::int64_t v) noexcept;

class Writer {
public:
    std::vector<std::uint32_t> words;
    std::uint64_t cur{0};
    std::uint32_t bits{0};
    std::vector<std::pair<std::uint32_t, std::size_t>> stack;

    std::uint32_t width() const noexcept;
    void emit(std::uint64_t v, std::uint32_t width);
    void vbr(std::uint64_t v, std::uint32_t width);
    void align();
    void bytes(std::string_view b);
    void enter(std::uint64_t id, std::uint32_t width);
    void exit();
    void record(std::uint64_t code, const std::vector<std::uint64_t>& ops);
    std::vector<std::uint8_t> finish();
};

class Module {
public:
    std::vector<TypeDef> types;
    std::unordered_map<TypeDef, std::uint32_t, TypeDefHash> type_ids;
    std::vector<std::pair<Ty, ConstDef>> consts;
    std::unordered_map<ConstKey, std::uint32_t, ConstKeyHash> const_ids;
    std::vector<Global> globals;
    std::vector<Func> funcs;
    std::unordered_map<std::string, std::uint32_t> func_ids;
    std::vector<Inst> insts;
    std::vector<Block> blocks;
    std::vector<MdDef> mds;
    std::vector<std::pair<std::string, std::vector<Md>>> named;
    std::unordered_map<std::uint32_t, Tag> tags;

    Ty ty(TypeDef def);
    Ty void_();
    Ty int_(std::uint32_t bits);
    Ty float_();
    Ty double_();
    Ty ptr(Ty to);
    Ty ptr_in(Ty to, std::uint32_t space);
    Ty array(std::uint32_t n, Ty of);
    Ty named_struct(std::string_view name, std::vector<Ty> fields);
    Ty func_ty(Ty ret, std::vector<Ty> params);

    bool is_void(Ty t) const;

    V constant(Ty ty, ConstDef c);
    V int_const(Ty ty, std::int64_t v);
    V i32(std::uint32_t v);
    V i8(std::uint8_t v);
    V bool_(bool v);
    V f32(std::uint32_t bits);
    V undef(Ty ty);
    V aggregate(Ty ty, const std::vector<V>& fields);
    std::optional<std::int64_t> int_value(V v) const;
    void replace_const(V old, V replacement);

    V function(std::string_view name, Ty ty, std::optional<Attr> attr, bool body);
    V global(std::string_view name, Ty ty, std::uint32_t space);
    V constant_global(std::string_view name, V init);
    void tag(V v, Tag tag);

    std::size_t new_block();
    Ty type_of(V v) const;

    V bin(std::size_t block, Bin op, V a, V b);
    V cast(std::size_t block, Cast op, V v, Ty to);
    V cmp(std::size_t block, std::uint8_t pred, V a, V b);
    V select(std::size_t block, V c, V t, V f);
    V extract(std::size_t block, V agg, std::uint32_t index);
    V call(std::size_t block, V f, std::vector<V> args);
    V phi(std::size_t block, Ty ty);
    V element(std::size_t block, V ptr, V index);
    V load(std::size_t block, V ptr);
    void store(std::size_t block, V ptr, V value);
    V atomic(std::size_t block, Rmw op, V ptr, V value);
    V cmpxchg(std::size_t block, V ptr, V expected, V replacement);

    void set_phi(V phi, std::vector<std::pair<V, std::size_t>> incoming);
    void terminate(std::size_t block, Term t);

    Md md_str(std::string_view s);
    Md md_value(V v);
    Md md_node(std::vector<std::optional<Md>> ops);
    void named_md(std::string_view name, std::vector<Md> nodes);

    std::vector<bool> live() const;
    std::vector<std::uint8_t> write() const;

    void optimise(std::uint32_t passes);
    std::vector<Effect> effects() const;
    std::vector<bool> reached() const;
    Usage usage() const;

    Census census() const;

private:
    V push(std::size_t block, Ty ty, Op op);
};

}
