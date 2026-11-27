#pragma once

#include <euphoria/shader/gfx10/resource.hpp>
#include <euphoria/shader/gfx10/structure.hpp>

#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::resource::detail {

using ir::Inst;
using ir::Program;
using ir::Reg;
using ir::Value;
using structure::Bool;
using structure::Structured;

struct Term {
    bool constant{};
    std::uint32_t k{};
    Value v{};

    static constexpr Term K(std::uint32_t c) noexcept { return {true, c, Value{}}; }
    static constexpr Term V(Value value) noexcept { return {false, 0, value}; }

    constexpr bool is_value() const noexcept { return !constant; }

    friend constexpr bool operator==(const Term& a, const Term& b) noexcept
    {
        return a.constant == b.constant && (a.constant ? a.k == b.k : a.v == b.v);
    }
};

struct Sum {
    std::optional<Value> base;
    std::uint32_t offset{};
    std::optional<Value> dynamic;
};

using Known = std::vector<std::pair<Bool, bool>>;
using Why = const char*;

template <typename T>
using Traced = std::expected<T, Why>;

inline constexpr std::size_t MAX_ROUNDS = 64;
inline constexpr std::size_t MAX_JOINS = 8;
inline constexpr std::size_t MAX_COPIES = 64;
inline constexpr std::size_t JOIN_MARKER = static_cast<std::size_t>(-1);

struct Picked {
    Value condition;
    Term set;
    Term clear;
};

std::optional<Term> copy_source(const Inst& inst, std::size_t k);
std::optional<std::uint32_t> lane_of(const Operand& op);
bool named(std::string_view name, std::initializer_list<std::string_view> names);

class Tracer {
public:
    static std::expected<Tracer, ShaderError> make(const Program& p, std::span<const RootParam> layout, HwStage stage,
                                                   const Structured* lanes);

    const Program& p;
    std::span<const RootParam> layout;
    HwStage stage;
    std::vector<std::optional<Site>> defs;
    std::vector<std::optional<std::pair<std::size_t, std::size_t>>> phis;
    std::vector<std::optional<Reg>> inputs;
    std::vector<Term> origins;
    mutable std::vector<Site> lane_copied;
    const Structured* lanes;
    mutable bool read_first_lane{false};
    mutable structure::Meets meets;
    std::vector<std::size_t> rpo;
    mutable std::optional<std::size_t> at;

    Tracer(const Program& program, std::span<const RootParam> params, HwStage hw, const Structured* structured)
        : p(program), layout(params), stage(hw), lanes(structured)
    {
    }

    std::optional<Site> def_site(Value v) const;
    std::optional<std::pair<std::size_t, std::size_t>> phi_at(Value v) const;
    std::optional<Reg> input(Value v) const;

    std::expected<std::vector<Term>, ShaderError> propagate_copies(std::size_t n) const;
    bool meet(std::span<const std::pair<Bool, bool>> conditions) const;
    const Inst& inst(Site site) const;
    const Inst* def(Value v) const;
    Term origin(Value v) const;
    std::optional<Term> term(const Inst& inst, const Operand& op) const;
    std::optional<std::pair<std::size_t, std::uint32_t>> param_of(Reg reg) const;
    std::optional<std::pair<const Inst*, std::array<Term, 2>>> sop2(Value v,
                                                                   std::initializer_list<std::string_view> names) const;
    std::optional<RootIndexedView> root_indexed_view(const Descriptor& descriptor,
                                                     std::span<const std::pair<Site, Address>> scalar) const;
    Sum sum(Term t) const;
    std::optional<Value> code_join(Value v) const;
    bool code_high(Term t) const;
    bool code_high_in(Term t, std::uint32_t depth) const;
    std::optional<Value> code_base(Value v, std::uint32_t depth) const;
    bool is_high(Term t, Value high) const;
    std::optional<Value> first_lane(Value v) const;
    const Inst* getpc(Value lo) const;
    std::optional<std::array<Term, 2>> vadd(Value v) const;
    std::optional<Term> vterm(const Inst& inst, const Operand& op) const;
    Term vorigin(Value v) const;
    Traced<Address> by_address(Why why) const;
    Traced<Address> flat_address(const Inst& inst, const std::optional<Operand>& addr,
                                 const std::optional<Operand>& saddr, std::int16_t offset) const;
    Traced<Address> address(Term lo, Term hi, Sum extra) const;
    bool high_half(Term hi, std::optional<Value> high) const;
    bool carried(Term hi, Value high) const;
    Why why_no_base(std::optional<Value> dynamic) const;
    void leaves(Value v, std::vector<Value>& out) const;
    Traced<Address> smem_address(const Inst& inst, const Operand& base, std::int32_t offset,
                                 const Operand& soffset) const;
    Traced<Source> descriptor(const Inst& inst, const Operand& op, std::size_t at_block) const;
    Traced<Source> classify(std::span<const Term> words, std::vector<std::size_t>& joins, Known& known) const;
    std::optional<std::size_t> only_edge(std::size_t block, Known& known) const;
    bool anywhere(const Source& source) const;
    Term live(Term t) const;
    Traced<std::optional<Source>> phi(std::span<const Term> words, std::vector<std::size_t>& joins,
                                      Known& known) const;
    Traced<std::optional<Source>> select(std::span<const Term> words, std::vector<std::size_t>& joins,
                                         Known& known) const;
    std::optional<Picked> picked(Value v) const;
    std::optional<std::pair<Term, std::uint32_t>> set_bits(Value v) const;
    std::optional<Value> first_lane_source(Value v) const;
    std::optional<Value> first_lane_offset(Site load) const;
    bool kept_per_lane(const Inst& inst, const Operand& op) const;
    std::optional<std::vector<Value>> first_lane_words(const Inst& inst, const Operand& op) const;
    Term lane_term(Term t) const;
    bool varies(Term t) const;
    Traced<Term> lane_value(Term t, const Known& known) const;
    Term vstep(Value v) const;
    std::optional<Term> raw(const Inst& inst, const Operand& op) const;
    Traced<bool> runs(Site site, const Known& known) const;
    Traced<Source> per_lane(std::span<const Term> words, std::vector<std::size_t>& joins, Known& known) const;
    Traced<std::optional<Source>> fetched(std::span<const Term> words, const Known& known) const;
    Traced<std::optional<Source>> lane_select(std::span<const Term> words, std::vector<std::size_t>& joins,
                                              Known& known) const;
    std::optional<Picked> lane_picked(Value v) const;
    Traced<std::optional<Source>> lane_phi(std::span<const Term> words, std::vector<std::size_t>& joins,
                                           Known& known) const;
    bool reaches(std::size_t from, std::size_t to) const;
    Traced<Waterfall> waterfall(std::size_t b, std::span<const std::size_t> accesses,
                                std::span<const Value> reads) const;
    std::optional<Source> loaded(std::span<const Term> words) const;
    std::optional<Source> lane_copies(std::span<const Term> words) const;
    bool scalar_copies(Value v, std::vector<Term>& out) const;
    bool base_from_entry(Site site) const;
    std::optional<Term> restored(const Inst& inst) const;
    std::optional<std::optional<Term>> spilled(Value v, std::uint32_t lane, std::vector<Value>& seen) const;
};

}
