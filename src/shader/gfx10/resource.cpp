#include "resource_tracer.hpp"

#include <euphoria/shader/gfx10/bvh.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <map>
#include <tuple>

namespace euphoria::shader::gfx10::resource {

bool operator==(const Source::Phi& a, const Source::Phi& b)
{
    return a.block == b.block && a.args == b.args;
}

bool operator==(const Source::Select& a, const Source::Select& b)
{
    return a.scc == b.scc && a.args == b.args;
}

bool operator==(const Source::Lanes& a, const Source::Lanes& b)
{
    return a.mask == b.mask && a.args == b.args;
}

bool operator==(const Source::LanePhi& a, const Source::LanePhi& b)
{
    return a.block == b.block && a.args == b.args;
}

bool operator==(const Source& a, const Source& b)
{
    return a.value == b.value;
}

std::vector<const Source*> Source::leaves() const
{
    std::vector<const Source*> out;
    const auto all = [&out](const std::vector<Source>& args) {
        for (const Source& arg : args) {
            const auto below = arg.leaves();
            out.insert(out.end(), below.begin(), below.end());
        }
    };
    if (const auto* phi = as<Phi>()) {
        all(phi->args);
    } else if (const auto* select = as<Select>()) {
        all(select->args);
    } else if (const auto* lanes = as<Lanes>()) {
        all(lanes->args);
    } else if (const auto* lane_phi = as<LanePhi>()) {
        for (const auto& arg : lane_phi->args) {
            if (arg) {
                const auto below = arg->leaves();
                out.insert(out.end(), below.begin(), below.end());
            }
        }
    } else {
        out.push_back(this);
    }
    return out;
}

namespace {

std::string site_text(const Site& site)
{
    return std::format("({}, {})", site.first, site.second);
}

std::string address_text(const Address& address)
{
    std::string param = address.param == CODE ? "CODE"
                      : address.param == MEMORY ? "MEMORY"
                                                : std::to_string(address.param);
    return std::format("{{param: {}, offset: {:#x}, dynamic: {}}}", param, address.offset,
                       address.dynamic ? std::format("v{}", address.dynamic->id) : std::string{"None"});
}

std::string args_text(const std::vector<Source>& args)
{
    std::string out = "[";
    for (std::size_t n = 0; n < args.size(); ++n) {
        out += (n == 0 ? "" : ", ") + to_string(args[n]);
    }
    return out + "]";
}

}

std::string to_string(const Source& source)
{
    if (const auto* s = source.as<Source::Loaded>()) {
        return std::format("Loaded {{load: {}, first: {}}}", site_text(s->load), s->first);
    }
    if (const auto* s = source.as<Source::Built>()) {
        return std::format("Built {{address: {}, bits: {:#x}, words: [{:#x}, {:#x}]}}", address_text(s->address),
                           s->bits, s->words[0], s->words[1]);
    }
    if (const auto* s = source.as<Source::Constant>()) {
        std::string out = "Constant([";
        for (std::size_t n = 0; n < s->words.size(); ++n) {
            out += std::format("{}{:#x}", n == 0 ? "" : ", ", s->words[n]);
        }
        return out + "])";
    }
    if (const auto* s = source.as<Source::Phi>()) {
        return std::format("Phi {{block: {}, args: {}}}", s->block, args_text(s->args));
    }
    if (const auto* s = source.as<Source::Select>()) {
        return std::format("Select {{scc: v{}, args: {}}}", s->scc.id, args_text(s->args));
    }
    if (const auto* s = source.as<Source::Fetched>()) {
        return std::format("Fetched {{load: {}, first: {}}}", site_text(s->load), s->first);
    }
    if (const auto* s = source.as<Source::Lanes>()) {
        return std::format("Lanes {{mask: v{}, args: {}}}", s->mask.id, args_text(s->args));
    }
    if (const auto* s = source.as<Source::LanePhi>()) {
        std::string out = std::format("LanePhi {{block: {}, args: [", s->block);
        for (std::size_t n = 0; n < s->args.size(); ++n) {
            out += (n == 0 ? "" : ", ") + (s->args[n] ? to_string(*s->args[n]) : std::string{"None"});
        }
        return out + "]}";
    }
    return {};
}

std::optional<std::uint32_t> inline_const(std::uint8_t code) noexcept
{
    const auto f = [](float x) { return std::optional<std::uint32_t>{std::bit_cast<std::uint32_t>(x)}; };
    if (code >= 128 && code <= 192) {
        return static_cast<std::uint32_t>(code) - 128;
    }
    if (code >= 193 && code <= 208) {
        return static_cast<std::uint32_t>(192 - static_cast<std::int32_t>(code));
    }
    switch (code) {
    case 240:
        return f(0.5f);
    case 241:
        return f(-0.5f);
    case 242:
        return f(1.0f);
    case 243:
        return f(-1.0f);
    case 244:
        return f(2.0f);
    case 245:
        return f(-2.0f);
    case 246:
        return f(4.0f);
    case 247:
        return f(-4.0f);
    case 248:
        return 0x3E22F983u;
    default:
        return std::nullopt;
    }
}

namespace detail {

namespace {

constexpr std::string_view RAW[] = {
    "an address not from a root parameter",
    "an address from a root parameter that is no address",
    "an address in an SGPR that is no root parameter",
    "an address loaded from memory",
    "an address from a phi",
};

Reg sreg(std::uint32_t code)
{
    return Reg::S(static_cast<std::uint8_t>(code));
}

Reg vreg(std::uint32_t reg)
{
    return Reg::V(static_cast<std::uint8_t>(reg));
}

bool plain(const Valu& f)
{
    return f.abs == 0 && f.neg == 0 && !f.sdwa && !f.dpp;
}

}

bool named(std::string_view name, std::initializer_list<std::string_view> names)
{
    return std::ranges::find(names, name) != names.end();
}

std::optional<std::uint32_t> lane_of(const Operand& op)
{
    if (const auto* c = op.as<Operand::Const>()) {
        return inline_const(c->code);
    }
    if (const auto* l = op.as<Operand::Literal>()) {
        return l->value;
    }
    return std::nullopt;
}

std::optional<Term> copy_source(const Inst& inst, std::size_t k)
{
    const auto* sop = inst.fields.as<Fields::Sop>();
    if (sop == nullptr) {
        return std::nullopt;
    }
    const std::string_view name = inst.name();
    if (name == "s_movk_i32") {
        if (!sop->imm) {
            return std::nullopt;
        }
        return Term::K(static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(*sop->imm))));
    }
    if (sop->src.empty()) {
        return std::nullopt;
    }
    const Operand& src = sop->src.front();
    const bool mov = name == "s_mov_b32" || name == "s_mov_b64";
    if (const auto* s = src.as<Operand::Scalar>(); s != nullptr && mov) {
        if (s->code != NULL_REG) {
            const auto v = inst.read(sreg(s->code + k));
            return v ? std::optional<Term>{Term::V(*v)} : std::nullopt;
        }
        return Term::K(0);
    }
    if (const auto* l = src.as<Operand::Literal>(); l != nullptr && name == "s_mov_b32") {
        return Term::K(l->value);
    }
    const auto* c = src.as<Operand::Const>();
    if (c == nullptr) {
        return std::nullopt;
    }
    if (name == "s_mov_b32") {
        const auto x = inline_const(c->code);
        return x ? std::optional<Term>{Term::K(*x)} : std::nullopt;
    }
    if (name == "s_brev_b32") {
        const auto x = inline_const(c->code);
        if (!x) {
            return std::nullopt;
        }
        std::uint32_t r = 0;
        for (int bit = 0; bit < 32; ++bit) {
            r |= ((*x >> bit) & 1u) << (31 - bit);
        }
        return Term::K(r);
    }
    if (name == "s_not_b32") {
        const auto x = inline_const(c->code);
        return x ? std::optional<Term>{Term::K(~*x)} : std::nullopt;
    }
    if (name == "s_mov_b64" && c->code >= 128 && c->code <= 208) {
        const auto low = inline_const(c->code);
        if (!low) {
            return std::nullopt;
        }
        return Term::K(k == 0 ? *low : static_cast<std::uint32_t>(static_cast<std::int32_t>(*low) >> 31));
    }
    return std::nullopt;
}

std::expected<Tracer, ShaderError> Tracer::make(const Program& p, std::span<const RootParam> layout, HwStage stage,
                                                const Structured* lanes)
{
    std::size_t n = 0;
    const auto see = [&n](Value v) {
        if (v != ir::NONE) {
            n = std::max<std::size_t>(n, static_cast<std::size_t>(v.id) + 1);
        }
    };
    for (const auto& input : p.inputs) {
        see(input.second);
    }
    for (const auto& block : p.blocks) {
        for (const auto& phi : block.phis) {
            see(phi.value);
        }
        for (const auto& inst : block.insts) {
            for (const auto& write : inst.writes) {
                see(write.second);
            }
        }
    }
    Tracer t(p, layout, stage, lanes);
    t.defs.assign(n, std::nullopt);
    t.phis.assign(n, std::nullopt);
    t.inputs.assign(n, std::nullopt);
    for (const auto& [reg, v] : p.inputs) {
        if (v.id < n) {
            t.inputs[v.id] = reg;
        }
    }
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const auto& block = p.blocks[b];
        for (std::size_t i = 0; i < block.phis.size(); ++i) {
            if (block.phis[i].value.id < n) {
                t.phis[block.phis[i].value.id] = std::pair{b, i};
            }
        }
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            for (const auto& write : block.insts[i].writes) {
                if (write.second.id < n) {
                    t.defs[write.second.id] = Site{b, i};
                }
            }
        }
    }
    t.rpo.assign(p.blocks.size(), static_cast<std::size_t>(-1));
    const auto order = ir::reverse_postorder(p.blocks);
    for (std::size_t k = 0; k < order.size(); ++k) {
        t.rpo[order[k]] = k;
    }
    auto origins = t.propagate_copies(n);
    if (!origins) {
        return std::unexpected(origins.error());
    }
    t.origins = std::move(*origins);
    return t;
}

std::optional<Site> Tracer::def_site(Value v) const
{
    return v.id < defs.size() ? defs[v.id] : std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>> Tracer::phi_at(Value v) const
{
    return v.id < phis.size() ? phis[v.id] : std::nullopt;
}

std::optional<Reg> Tracer::input(Value v) const
{
    return v.id < inputs.size() ? inputs[v.id] : std::nullopt;
}

std::expected<std::vector<Term>, ShaderError> Tracer::propagate_copies(std::size_t n) const
{
    std::vector<std::optional<Term>> o(n);
    const auto get = [&o](Value v) { return v.id < o.size() ? o[v.id] : std::nullopt; };
    for (const auto& input : p.inputs) {
        if (input.second.id < n) {
            o[input.second.id] = Term::V(input.second);
        }
    }
    const auto order = ir::reverse_postorder(p.blocks);
    for (std::size_t round = 0; round < MAX_ROUNDS; ++round) {
        bool changed = false;
        for (const std::size_t b : order) {
            const auto& block = p.blocks[b];
            for (const auto& phi : block.phis) {
                if (phi.value.id >= n) {
                    continue;
                }
                const Term own = Term::V(phi.value);
                std::optional<Term> first;
                bool agree = true;
                for (const Value a : phi.args) {
                    const auto t = get(a);
                    if (!t || *t == own) {
                        continue;
                    }
                    if (!first) {
                        first = t;
                    } else if (!(*t == *first)) {
                        agree = false;
                    }
                }
                std::optional<Term> next;
                if (first) {
                    next = agree ? *first : own;
                }
                if (next && next != o[phi.value.id]) {
                    o[phi.value.id] = next;
                    changed = true;
                }
            }
            for (const auto& inst : block.insts) {
                for (std::size_t k = 0; k < inst.writes.size(); ++k) {
                    const Value v = inst.writes[k].second;
                    if (v.id >= n) {
                        continue;
                    }
                    auto moved = copy_source(inst, k);
                    if (!moved) {
                        moved = restored(inst);
                    }
                    std::optional<Term> next;
                    if (!moved) {
                        next = Term::V(v);
                    } else if (moved->constant) {
                        next = *moved;
                    } else {
                        next = get(moved->v);
                    }
                    if (next && next != o[v.id]) {
                        o[v.id] = next;
                        changed = true;
                    }
                }
            }
        }
        if (!changed) {
            std::vector<Term> out(n);
            for (std::size_t v = 0; v < n; ++v) {
                out[v] = o[v].value_or(Term::V(Value{static_cast<std::uint32_t>(v)}));
            }
            return out;
        }
    }
    return std::unexpected(ShaderError::Unsupported(0, "copies that do not settle"));
}

bool Tracer::meet(std::span<const std::pair<Bool, bool>> conditions) const
{
    return lanes->bools.meet_seen(conditions, meets);
}

const Inst& Tracer::inst(Site site) const
{
    return p.blocks[site.first].insts[site.second];
}

const Inst* Tracer::def(Value v) const
{
    const auto site = def_site(v);
    return site ? &inst(*site) : nullptr;
}

Term Tracer::origin(Value v) const
{
    return v.id < origins.size() ? origins[v.id] : Term::V(v);
}

std::optional<Term> Tracer::term(const Inst& in, const Operand& op) const
{
    if (const auto* s = op.as<Operand::Scalar>()) {
        if (s->code == NULL_REG) {
            return Term::K(0);
        }
        const auto v = in.read(Reg::S(s->code));
        return v ? std::optional<Term>{origin(*v)} : std::nullopt;
    }
    if (const auto* c = op.as<Operand::Const>()) {
        const auto x = inline_const(c->code);
        return x ? std::optional<Term>{Term::K(*x)} : std::nullopt;
    }
    if (const auto* l = op.as<Operand::Literal>()) {
        return Term::K(l->value);
    }
    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::uint32_t>> Tracer::param_of(Reg reg) const
{
    if (!reg.is_s()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < layout.size(); ++i) {
        const std::uint32_t first = user_sgpr(stage, layout[i].user_data);
        if (reg.index < first) {
            continue;
        }
        const std::uint32_t dword = reg.index - first;
        if (dword < layout[i].sgprs) {
            return std::pair{i, dword};
        }
    }
    return std::nullopt;
}

std::optional<std::pair<const Inst*, std::array<Term, 2>>> Tracer::sop2(
    Value v, std::initializer_list<std::string_view> names) const
{
    const Inst* in = def(v);
    if (in == nullptr || !named(in->name(), names) || in->writes.empty() || in->writes.front().second != v) {
        return std::nullopt;
    }
    const auto* sop = in->fields.as<Fields::Sop>();
    if (sop == nullptr || sop->src.size() < 2) {
        return std::nullopt;
    }
    const auto a = term(*in, sop->src[0]);
    if (!a) {
        return std::nullopt;
    }
    const auto b = term(*in, sop->src[1]);
    if (!b) {
        return std::nullopt;
    }
    return std::pair{in, std::array<Term, 2>{*a, *b}};
}

std::optional<RootIndexedView> Tracer::root_indexed_view(const Descriptor& descriptor,
                                                         std::span<const std::pair<Site, Address>> scalar) const
{
    if (descriptor.role != Role::Resource) {
        return std::nullopt;
    }
    const auto* loaded = descriptor.source.as<Source::Loaded>();
    if (loaded == nullptr) {
        return std::nullopt;
    }
    const auto found = std::ranges::lower_bound(scalar, loaded->load, {}, [](const auto& s) { return s.first; });
    if (found == scalar.end() || found->first != loaded->load) {
        return std::nullopt;
    }
    const Address address = found->second;
    if (address.param >= layout.size()) {
        return std::nullopt;
    }
    const RootParam& base = layout[address.param];
    if (base.root != RootType::Table || !address.dynamic) {
        return std::nullopt;
    }
    const Value dynamic = *address.dynamic;
    Value in = dynamic;
    std::uint8_t shift = 0;
    if (const auto s = sop2(dynamic, {"s_lshl_b32"});
        s && s->second[0].is_value() && s->second[1].constant) {
        in = s->second[0].v;
        shift = static_cast<std::uint8_t>(s->second[1].k & 31);
    }
    const auto reg = input(in);
    if (!reg) {
        return std::nullopt;
    }
    const auto param = param_of(*reg);
    if (!param || param->first >= layout.size()) {
        return std::nullopt;
    }
    const RootParam& constant = layout[param->first];
    if (constant.root != RootType::Constants) {
        return std::nullopt;
    }
    return RootIndexedView{
        .table_slot = base.slot,
        .constant_slot = constant.slot,
        .constant_dword = param->second,
        .byte_offset = address.offset + static_cast<std::uint32_t>(loaded->first) * 4,
        .shift = shift,
    };
}

Sum Tracer::sum(Term t) const
{
    if (t.constant) {
        return Sum{std::nullopt, t.k, std::nullopt};
    }
    const Value v = t.v;
    const Sum dynamic{std::nullopt, 0, v};
    if (const auto reg = input(v)) {
        const auto param = param_of(*reg);
        if (param && param->second == 0) {
            return Sum{v, 0, std::nullopt};
        }
        return dynamic;
    }
    if (getpc(v) != nullptr) {
        return Sum{v, 0, std::nullopt};
    }
    if (const auto pc = code_join(v)) {
        return Sum{*pc, 0, v};
    }
    if (const auto lane = first_lane(v)) {
        Sum per_lane = sum(vorigin(*lane));
        if (per_lane.base || !read_first_lane) {
            return per_lane;
        }
        return dynamic;
    }
    std::optional<std::array<Term, 2>> terms;
    if (const auto s = sop2(v, {"s_add_u32", "s_add_i32"})) {
        terms = s->second;
    } else {
        terms = vadd(v);
    }
    if (!terms) {
        return dynamic;
    }
    const Sum x = sum((*terms)[0]);
    const Sum y = sum((*terms)[1]);
    if ((x.base && y.base) || (x.dynamic && y.dynamic)) {
        return dynamic;
    }
    return Sum{x.base ? x.base : y.base, x.offset + y.offset, x.dynamic ? x.dynamic : y.dynamic};
}

std::optional<Value> Tracer::code_join(Value v) const
{
    const auto at_phi = phi_at(v);
    if (!at_phi) {
        return std::nullopt;
    }
    std::optional<Value> base;
    for (const Value a : p.blocks[at_phi->first].phis[at_phi->second].args) {
        const Term t = vorigin(a);
        if (t.constant) {
            return std::nullopt;
        }
        if (t.v == v) {
            continue;
        }
        const auto pc = code_base(t.v, 4);
        if (!pc) {
            return std::nullopt;
        }
        if (!base) {
            base = pc;
        }
    }
    return base;
}

bool Tracer::code_high(Term t) const
{
    return code_high_in(t, 4);
}

bool Tracer::code_high_in(Term t, std::uint32_t depth) const
{
    if (t.constant) {
        return false;
    }
    const Value x = t.v;
    if (const Inst* in = def(x);
        in != nullptr && in->name() == "s_getpc_b64" && in->writes.size() > 1 && in->writes[1].second == x) {
        return true;
    }
    if (depth == 0) {
        return false;
    }
    const auto s = sop2(x, {"s_addc_u32"});
    return s && (code_high_in(s->second[0], depth - 1) || code_high_in(s->second[1], depth - 1));
}

std::optional<Value> Tracer::code_base(Value v, std::uint32_t depth) const
{
    if (getpc(v) != nullptr) {
        return v;
    }
    if (const auto lane = first_lane(v)) {
        const Term t = vorigin(*lane);
        if (t.constant) {
            return std::nullopt;
        }
        return code_base(t.v, depth);
    }
    if (depth == 0 || phi_at(v)) {
        return std::nullopt;
    }
    std::optional<std::array<Term, 2>> terms;
    if (const auto s = sop2(v, {"s_add_u32", "s_add_i32"})) {
        terms = s->second;
    } else {
        terms = vadd(v);
    }
    if (!terms) {
        return std::nullopt;
    }
    for (const Term& t : *terms) {
        if (t.constant) {
            continue;
        }
        if (const auto found = code_base(t.v, depth - 1)) {
            return found;
        }
    }
    return std::nullopt;
}

bool Tracer::is_high(Term t, Value high) const
{
    if (t.constant) {
        return false;
    }
    if (t.v == high) {
        return true;
    }
    const auto s = sop2(t.v, {"s_addc_u32"});
    return s && (s->second[0] == Term::V(high) || s->second[1] == Term::V(high));
}

std::optional<Value> Tracer::first_lane(Value v) const
{
    const Inst* in = def(v);
    if (in == nullptr || in->name() != "v_readfirstlane_b32" || in->writes.empty() || in->writes.front().second != v) {
        return std::nullopt;
    }
    const auto* f = in->fields.as<Valu>();
    if (f == nullptr || !plain(*f) || f->src.empty()) {
        return std::nullopt;
    }
    if (const auto* vec = f->src.front().as<Operand::Vector>()) {
        return in->read(Reg::V(vec->reg));
    }
    return std::nullopt;
}

const Inst* Tracer::getpc(Value lo) const
{
    const Inst* in = def(lo);
    if (in != nullptr && in->name() == "s_getpc_b64" && !in->writes.empty() && in->writes.front().second == lo) {
        return in;
    }
    return nullptr;
}

std::optional<std::array<Term, 2>> Tracer::vadd(Value v) const
{
    const Inst* in = def(v);
    if (in == nullptr) {
        return std::nullopt;
    }
    const auto* f = in->fields.as<Valu>();
    if (f == nullptr) {
        return std::nullopt;
    }
    if (!named(in->name(), {"v_add_co_u32", "v_add_nc_u32"}) || !f->dst) {
        return std::nullopt;
    }
    const auto* dst = f->dst->as<Operand::Vector>();
    if (dst == nullptr || in->write(Reg::V(dst->reg)) != v || !plain(*f) || f->src.size() < 2) {
        return std::nullopt;
    }
    const auto a = vterm(*in, f->src[0]);
    if (!a) {
        return std::nullopt;
    }
    const auto b = vterm(*in, f->src[1]);
    if (!b) {
        return std::nullopt;
    }
    return std::array<Term, 2>{*a, *b};
}

std::optional<Term> Tracer::vterm(const Inst& in, const Operand& op) const
{
    if (const auto* vec = op.as<Operand::Vector>()) {
        const auto v = in.read(Reg::V(vec->reg));
        return v ? std::optional<Term>{vorigin(*v)} : std::nullopt;
    }
    return term(in, op);
}

Term Tracer::vorigin(Value v) const
{
    if (const Inst* in = def(v); in != nullptr && in->name() == "v_mov_b32") {
        if (const auto* f = in->fields.as<Valu>(); f != nullptr && plain(*f) && !f->src.empty()) {
            if (const auto t = vterm(*in, f->src.front())) {
                return *t;
            }
        }
    }
    return origin(v);
}

Traced<Address> Tracer::by_address(Why why) const
{
    if (stage == HwStage::Cs && std::ranges::find(RAW, std::string_view{why}) != std::end(RAW)) {
        return Address{MEMORY, 0, std::nullopt};
    }
    return std::unexpected(why);
}

Traced<Address> Tracer::flat_address(const Inst& in, const std::optional<Operand>& addr,
                                     const std::optional<Operand>& saddr, std::int16_t offset) const
{
    const auto* vec = addr ? addr->as<Operand::Vector>() : nullptr;
    if (vec == nullptr) {
        return std::unexpected("a global address not in VGPRs");
    }
    const auto off = static_cast<std::uint32_t>(static_cast<std::int32_t>(offset));
    const auto* s = saddr ? saddr->as<Operand::Scalar>() : nullptr;
    if (s != nullptr && s->code != NULL_REG) {
        const auto lo = in.read(sreg(s->code));
        const auto hi = in.read(sreg(s->code + 1u));
        if (!lo || !hi) {
            return std::unexpected("an address not in SGPRs");
        }
        const auto v = in.read(Reg::V(vec->reg));
        if (!v) {
            return std::unexpected("a global address");
        }
        Sum extra = sum(vorigin(*v));
        if (extra.base) {
            return std::unexpected("an offset from a root parameter");
        }
        extra.offset += off;
        return address(origin(*lo), origin(*hi), extra);
    }
    const auto lo = in.read(vreg(vec->reg));
    const auto hi = in.read(vreg(vec->reg + 1u));
    if (!lo || !hi) {
        return std::unexpected("a global address");
    }
    return address(vorigin(*lo), vorigin(*hi), Sum{std::nullopt, off, std::nullopt});
}

Traced<Address> Tracer::address(Term lo, Term hi, Sum extra) const
{
    const Term lived = live(hi);
    if (lived.constant) {
        hi = lived;
    } else if (input(lived.v)) {
        hi = lived;
    }
    const Sum s = sum(lo);
    if (s.dynamic && extra.dynamic) {
        return std::unexpected("an address with two computed terms");
    }
    if (!s.base) {
        return std::unexpected(why_no_base(s.dynamic));
    }
    const Value base = *s.base;
    if (const Inst* pc = getpc(base)) {
        const std::optional<Value> high = pc->writes.size() > 1 ? std::optional<Value>{pc->writes[1].second}
                                                                : std::nullopt;
        if (!high_half(hi, high) && !(high && carried(hi, *high))) {
            return std::unexpected("an address with an untracked high half");
        }
        const bool joined = s.dynamic && code_join(*s.dynamic).has_value();
        const std::uint32_t first = joined ? 0 : static_cast<std::uint32_t>(pc->machine.offset) + 4;
        return Address{CODE, first + s.offset + extra.offset, s.dynamic ? s.dynamic : extra.dynamic};
    }
    const auto reg = input(base);
    if (!reg) {
        return std::unexpected("an address not from a root parameter");
    }
    const auto param = param_of(*reg);
    if (!param) {
        return std::unexpected("an address not from a root parameter");
    }
    std::optional<Value> high;
    switch (layout[param->first].root) {
    case RootType::Table:
    case RootType::ResourceHeap:
    case RootType::SamplerHeap:
        break;
    case RootType::Descriptor: {
        const Reg high_reg = sreg(user_sgpr(stage, layout[param->first].user_data) + 1u);
        const auto found = std::ranges::find_if(p.inputs, [&](const auto& i) { return i.first == high_reg; });
        if (found == p.inputs.end()) {
            return std::unexpected("a root descriptor without its high half");
        }
        high = found->second;
        break;
    }
    default:
        return std::unexpected("an address from a root parameter that is no address");
    }
    const std::optional<Value> dynamic = s.dynamic ? s.dynamic : extra.dynamic;
    if (!high_half(hi, high) && !(dynamic && high && carried(hi, *high))) {
        return std::unexpected("an address with an untracked high half");
    }
    return Address{param->first, s.offset + extra.offset, dynamic};
}

bool Tracer::high_half(Term hi, std::optional<Value> high) const
{
    const Term want = high ? Term::V(*high) : Term::K(0);
    if (hi == want) {
        return true;
    }
    if (hi.constant) {
        return false;
    }
    const auto s = sop2(hi.v, {"s_addc_u32"});
    if (!s) {
        return false;
    }
    const auto& [a, b] = s->second;
    return (a == want && b == Term::K(0)) || (b == want && a == Term::K(0));
}

bool Tracer::carried(Term hi, Value high) const
{
    if (hi.constant) {
        return false;
    }
    const Value v = hi.v;
    if (v == high) {
        return true;
    }
    const auto high_of = [&](Term t) { return is_high(t, high) || code_high(t); };
    const auto ci_of = [&](Value x, auto&& accept) {
        const Inst* in = def(x);
        if (in == nullptr) {
            return false;
        }
        const auto* f = in->fields.as<Valu>();
        if (f == nullptr || in->name() != "v_add_co_ci_u32") {
            return false;
        }
        const std::size_t count = std::min<std::size_t>(2, f->src.size());
        for (std::size_t k = 0; k < count; ++k) {
            const auto t = vterm(*in, f->src[k]);
            if (t && accept(*t)) {
                return true;
            }
        }
        return false;
    };
    if (const auto at_phi = phi_at(v)) {
        for (const Value a : p.blocks[at_phi->first].phis[at_phi->second].args) {
            const Term t = vorigin(a);
            if (t == Term::V(v) || high_of(t)) {
                continue;
            }
            if (t.constant) {
                return false;
            }
            Value x = t.v;
            if (const auto lane = first_lane(x)) {
                const Term y = vorigin(*lane);
                if (y.constant) {
                    return false;
                }
                x = y.v;
            }
            if (!ci_of(x, high_of)) {
                return false;
            }
        }
        return true;
    }
    if (const auto lane = first_lane(v)) {
        return carried(vorigin(*lane), high);
    }
    if (const auto s = sop2(v, {"s_addc_u32"})) {
        return carried(s->second[0], high) || carried(s->second[1], high);
    }
    return ci_of(v, [&](Term t) { return carried(t, high); });
}

Why Tracer::why_no_base(std::optional<Value> dynamic) const
{
    if (!dynamic) {
        return "a constant address";
    }
    const Value v = *dynamic;
    if (input(v)) {
        return "an address in an SGPR that is no root parameter";
    }
    std::vector<Value> all;
    leaves(v, all);
    for (const Value l : all) {
        if (const Inst* in = def(l); in != nullptr && in->name() == "s_getpc_b64") {
            return "an address relative to the program counter";
        }
    }
    const Inst* in = def(v);
    if (in == nullptr) {
        return "an address from a phi";
    }
    if (in->name().starts_with("s_load_")) {
        return "an address loaded from memory";
    }
    return "an address not from a root parameter";
}

void Tracer::leaves(Value v, std::vector<Value>& out) const
{
    if (const auto s = sop2(v, {"s_add_u32", "s_add_i32"})) {
        for (const Term& t : s->second) {
            if (t.is_value()) {
                leaves(t.v, out);
            }
        }
        return;
    }
    out.push_back(v);
}

Traced<Address> Tracer::smem_address(const Inst& in, const Operand& base, std::int32_t offset,
                                     const Operand& soffset) const
{
    const auto* s = base.as<Operand::Scalar>();
    if (s == nullptr) {
        return std::unexpected("an address not in SGPRs");
    }
    const auto lo = in.read(sreg(s->code));
    const auto hi = in.read(sreg(s->code + 1u));
    if (!lo || !hi) {
        return std::unexpected("an address not in SGPRs");
    }
    const auto off = term(in, soffset);
    if (!off) {
        return std::unexpected("an unknown offset operand");
    }
    Sum extra = sum(*off);
    if (extra.base) {
        return std::unexpected("an offset from a root parameter");
    }
    extra.offset += static_cast<std::uint32_t>(offset);
    return address(origin(*lo), origin(*hi), extra);
}

std::optional<Term> Tracer::restored(const Inst& in) const
{
    const auto* v = in.fields.as<Valu>();
    if (v == nullptr || in.name() != "v_readlane_b32" || v->src.size() != 2) {
        return std::nullopt;
    }
    const auto* vec = v->src[0].as<Operand::Vector>();
    if (vec == nullptr) {
        return std::nullopt;
    }
    const auto value = in.read(Reg::V(vec->reg));
    const auto lane = lane_of(v->src[1]);
    if (!value || !lane) {
        return std::nullopt;
    }
    std::vector<Value> seen;
    const auto found = spilled(*value, *lane, seen);
    if (!found) {
        return std::nullopt;
    }
    return *found;
}

std::optional<std::optional<Term>> Tracer::spilled(Value v, std::uint32_t lane, std::vector<Value>& seen) const
{
    if (const auto at_phi = phi_at(v)) {
        if (std::ranges::find(seen, v) != seen.end()) {
            return std::optional<Term>{};
        }
        seen.push_back(v);
        std::optional<Term> found;
        for (const Value a : p.blocks[at_phi->first].phis[at_phi->second].args) {
            const auto m = spilled(a, lane, seen);
            if (!m) {
                return std::nullopt;
            }
            if (*m) {
                if (found && !(*found == **m)) {
                    return std::nullopt;
                }
                found = *m;
            }
        }
        return found;
    }
    const Inst* in = def(v);
    if (in == nullptr) {
        return std::nullopt;
    }
    const auto* w = in->fields.as<Valu>();
    if (w == nullptr || !w->dst || w->src.size() != 2) {
        return std::nullopt;
    }
    const auto* dst = w->dst->as<Operand::Vector>();
    if (dst == nullptr || in->name() != "v_writelane_b32") {
        return std::nullopt;
    }
    const auto at = lane_of(w->src[1]);
    if (!at) {
        return std::nullopt;
    }
    if (*at != lane) {
        const auto old = in->read(Reg::V(dst->reg));
        if (!old) {
            return std::nullopt;
        }
        return spilled(*old, lane, seen);
    }
    const Operand& value = w->src[0];
    if (const auto* s = value.as<Operand::Scalar>(); s != nullptr && s->code != NULL_REG) {
        const auto x = in->read(Reg::S(s->code));
        if (!x) {
            return std::nullopt;
        }
        return std::optional<Term>{Term::V(*x)};
    }
    if (const auto* l = value.as<Operand::Literal>()) {
        return std::optional<Term>{Term::K(l->value)};
    }
    if (const auto* c = value.as<Operand::Const>()) {
        const auto x = inline_const(c->code);
        if (!x) {
            return std::nullopt;
        }
        return std::optional<Term>{Term::K(*x)};
    }
    return std::nullopt;
}

}

std::expected<Resources, ShaderError> track(const ir::Program& p, std::span<const RootParam> layout, HwStage stage,
                                            const structure::Structured* lanes)
{
    using detail::Term;
    auto made = detail::Tracer::make(p, layout, stage, lanes);
    if (!made) {
        return std::unexpected(made.error());
    }
    const detail::Tracer& t = *made;
    Resources out;
    for (const auto& [reg, v] : p.inputs) {
        if (const auto param = t.param_of(reg)) {
            out.params.emplace_back(v, param->first, param->second);
        }
    }
    std::map<std::size_t, std::pair<std::vector<std::size_t>, std::vector<ir::Value>>> first_lane_at;
    std::map<std::size_t, std::tuple<std::vector<std::size_t>, std::vector<ir::Value>, std::vector<Site>>> offset_at;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        t.at = b;
        const auto& block = p.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const ir::Inst& inst = block.insts[i];
            const Site site{b, i};
            const auto fail = [&inst](const char* what) {
                return ShaderError::Unsupported(inst.machine.offset, what);
            };
            std::vector<ir::Value> first_lane;
            std::vector<std::pair<ir::Value, Site>> offset;
            const auto descriptor = [&](const Operand& op, Role role) -> std::optional<ShaderError> {
                auto source = t.descriptor(inst, op, b);
                if (!source) {
                    return fail(source.error());
                }
                if (!t.kept_per_lane(inst, op)) {
                    if (auto words = t.first_lane_words(inst, op)) {
                        first_lane.insert(first_lane.end(), words->begin(), words->end());
                    }
                }
                if (const auto* loaded = source->as<Source::Loaded>()) {
                    const bool scalar = std::ranges::any_of(out.scalar, [&](const auto& s) {
                        return s.first == loaded->load;
                    });
                    if (scalar) {
                        if (const auto v = t.first_lane_offset(loaded->load)) {
                            offset.emplace_back(*v, loaded->load);
                        }
                    }
                }
                out.descriptors.push_back(Descriptor{site, role, std::move(*source)});
                return std::nullopt;
            };
            const std::string_view name = inst.name();
            std::optional<ShaderError> error;
            if (const auto* image = inst.fields.as<Fields::Image>()) {
                if (name.starts_with("image_bvh")) {
                    const auto word = [&](std::uint8_t k) -> std::optional<Term> {
                        const auto* s = image->rsrc.as<Operand::Scalar>();
                        if (s == nullptr || k >= s->dwords) {
                            return std::nullopt;
                        }
                        const auto v = inst.read(ir::Reg::S(static_cast<std::uint8_t>(s->code + k)));
                        return v ? std::optional<Term>{t.origin(*v)} : std::nullopt;
                    };
                    const auto word1 = word(1);
                    const auto word3 = word(3);
                    if (word1 && word3 && word1->constant && word3->constant
                        && (word3->k >> 28) == bvh::DESCRIPTOR_TYPE) {
                        out.bvh.emplace_back(site, BvhNodes::Memory(word1->k, word3->k));
                    } else {
                        error = descriptor(image->rsrc, Role::Resource);
                        if (!error) {
                            out.bvh.emplace_back(site, BvhNodes::Resource());
                        }
                    }
                } else {
                    error = descriptor(image->rsrc, Role::Resource);
                    if (!error && image->sampler) {
                        error = descriptor(*image->sampler, Role::Sampler);
                    }
                }
            } else if (const auto* buffer = inst.fields.as<Fields::Buffer>()) {
                if (!name.ends_with("_inv")) {
                    error = descriptor(buffer->rsrc, Role::Resource);
                }
            } else if (const auto* smem = inst.fields.as<Fields::Smem>()) {
                if (name.starts_with("s_buffer_")) {
                    error = descriptor(smem->base, Role::Resource);
                } else if (name.find("_load_") != std::string_view::npos
                           || name.find("_store_") != std::string_view::npos) {
                    auto address = t.smem_address(inst, smem->base, smem->offset, smem->soffset);
                    if (!address) {
                        address = t.by_address(address.error());
                    }
                    if (!address) {
                        return std::unexpected(fail(address.error()));
                    }
                    out.scalar.emplace_back(site, *address);
                }
            } else if (const auto* flat = inst.fields.as<Fields::Flat>()) {
                if (name.starts_with("global_")) {
                    auto address = t.flat_address(inst, flat->addr, flat->saddr, flat->offset);
                    if (!address) {
                        address = t.by_address(address.error());
                    }
                    if (!address) {
                        return std::unexpected(fail(address.error()));
                    }
                    out.scalar.emplace_back(site, *address);
                }
            }
            if (error) {
                return std::unexpected(*error);
            }
            if (!first_lane.empty()) {
                auto& [accesses, reads] = first_lane_at[b];
                accesses.push_back(i);
                reads.insert(reads.end(), first_lane.begin(), first_lane.end());
            }
            if (!offset.empty()) {
                auto& [accesses, reads, loads] = offset_at[b];
                accesses.push_back(i);
                for (const auto& [v, load] : offset) {
                    reads.push_back(v);
                    loads.push_back(load);
                }
            }
        }
    }
    t.at.reset();
    if (lanes == nullptr) {
        return out;
    }
    const auto unique = [](std::vector<ir::Value>& reads) {
        std::ranges::sort(reads);
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
    };
    for (auto& [block, entry] : first_lane_at) {
        auto& [accesses, reads] = entry;
        unique(reads);
        auto proof = t.waterfall(block, accesses, reads);
        if (!proof) {
            return std::unexpected(ShaderError::Unsupported(t.inst({block, accesses.front()}).machine.offset,
                                                            proof.error()));
        }
        out.waterfall.compares.insert(out.waterfall.compares.end(), proof->compares.begin(), proof->compares.end());
        out.waterfall.reads.insert(out.waterfall.reads.end(), proof->reads.begin(), proof->reads.end());
    }
    for (auto& [block, entry] : offset_at) {
        auto& [accesses, reads, loads] = entry;
        unique(reads);
        if (auto proof = t.waterfall(block, accesses, reads)) {
            out.waterfall.compares.insert(out.waterfall.compares.end(), proof->compares.begin(),
                                          proof->compares.end());
            out.waterfall.reads.insert(out.waterfall.reads.end(), proof->reads.begin(), proof->reads.end());
            out.waterfall.loads.insert(out.waterfall.loads.end(), loads.begin(), loads.end());
        }
    }
    out.lane_copied = std::move(t.lane_copied);
    std::ranges::sort(out.lane_copied);
    out.lane_copied.erase(std::unique(out.lane_copied.begin(), out.lane_copied.end()), out.lane_copied.end());
    for (const Descriptor& d : out.descriptors) {
        if (auto view = t.root_indexed_view(d, out.scalar)) {
            out.root_indexed_views.push_back(*view);
        }
    }
    return out;
}

}
