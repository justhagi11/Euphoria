#include "resource_tracer.hpp"

#include <algorithm>

namespace euphoria::shader::gfx10::resource::detail {
namespace {

using structure::LaneExit;

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

template <typename T>
bool contains(const std::vector<T>& list, const T& item)
{
    return std::ranges::find(list, item) != list.end();
}

}

Traced<Source> Tracer::descriptor(const Inst& in, const Operand& op, std::size_t at_block) const
{
    const auto* s = op.as<Operand::Scalar>();
    if (s == nullptr) {
        return std::unexpected("a descriptor not in SGPRs");
    }
    std::vector<Term> words;
    for (std::uint32_t k = 0; k < s->dwords; ++k) {
        const auto v = in.read(sreg(s->code + k));
        if (!v) {
            return std::unexpected("a descriptor not in SGPRs");
        }
        words.push_back(origin(*v));
    }
    Known known;
    if (lanes != nullptr) {
        known.emplace_back(lanes->reach[at_block], true);
    }
    std::vector<std::size_t> joins;
    return classify(words, joins, known);
}

Traced<Source> Tracer::classify(std::span<const Term> words, std::vector<std::size_t>& joins, Known& known) const
{
    auto joined = phi(words, joins, known);
    if (!joined) {
        return std::unexpected(joined.error());
    }
    if (*joined) {
        return std::move(**joined);
    }
    auto selected = select(words, joins, known);
    if (!selected) {
        return std::unexpected(selected.error());
    }
    if (*selected) {
        return std::move(**selected);
    }
    const auto read = [&](const Term& w) -> std::optional<Value> {
        return w.constant ? std::nullopt : first_lane_source(w.v);
    };
    bool all_read = true;
    bool any_read = false;
    std::vector<Term> sources;
    for (const Term& w : words) {
        if (const auto v = read(w)) {
            any_read = true;
            sources.push_back(Term::V(*v));
        } else {
            all_read = false;
        }
    }
    if (all_read) {
        if (auto source = lane_copies(words)) {
            return std::move(*source);
        }
        return per_lane(sources, joins, known);
    }
    if (any_read) {
        return std::unexpected("a descriptor partly read from the first lane");
    }
    if (std::ranges::all_of(words, [](const Term& w) { return w.constant; })) {
        Source::Constant constant;
        for (const Term& w : words) {
            constant.words.push_back(w.k);
        }
        return Source{std::move(constant)};
    }
    if (auto source = loaded(words)) {
        return std::move(*source);
    }
    if (words.size() == 4 && words[2].constant && words[3].constant) {
        Term hi = words[1];
        std::uint32_t bits = 0;
        while (hi.is_value()) {
            const auto more = set_bits(hi.v);
            if (!more) {
                break;
            }
            hi = more->first;
            bits |= more->second;
        }
        const auto address = this->address(words[0], hi, Sum{});
        if (!address) {
            return std::unexpected(address.error());
        }
        return Source{Source::Built{*address, bits, {words[2].k, words[3].k}}};
    }
    const bool loaded_word = std::ranges::any_of(words, [&](const Term& w) {
        if (w.constant) {
            return false;
        }
        const Inst* d = def(w.v);
        return d != nullptr && d->name().starts_with("s_load_");
    });
    return std::unexpected(loaded_word ? "a descriptor changed after loading" : "a descriptor neither loaded nor built");
}

std::optional<std::size_t> Tracer::only_edge(std::size_t block, Known& known) const
{
    if (lanes == nullptr) {
        return std::nullopt;
    }
    const auto& preds = p.blocks[block].preds;
    if (preds.size() < 2 || std::ranges::any_of(preds, [&](std::size_t q) { return rpo[q] >= rpo[block]; })) {
        return std::nullopt;
    }
    std::optional<std::size_t> only;
    for (std::size_t j = 0; j < preds.size(); ++j) {
        const std::size_t q = preds[j];
        const std::size_t n = known.size();
        known.emplace_back(lanes->reach[q], true);
        const LaneExit& exit = lanes->exits[q];
        if (exit.kind == LaneExit::Kind::Branch && exit.taken != exit.not_taken) {
            known.emplace_back(exit.when, exit.taken == block);
        }
        const bool possible = meet(known);
        known.resize(n);
        if (possible) {
            if (only) {
                return std::nullopt;
            }
            only = j;
        }
    }
    return only;
}

bool Tracer::anywhere(const Source& source) const
{
    if (source.is<Source::Constant>()) {
        return true;
    }
    if (const auto* built = source.as<Source::Built>()) {
        return !built->address.dynamic;
    }
    if (const auto* loaded_source = source.as<Source::Loaded>()) {
        const Inst& in = inst(loaded_source->load);
        if (const auto* smem = in.fields.as<Fields::Smem>()) {
            const auto a = smem_address(in, smem->base, smem->offset, smem->soffset);
            return a && !a->dynamic;
        }
    }
    return false;
}

Term Tracer::live(Term t) const
{
    if (!at || lanes == nullptr) {
        return t;
    }
    Known known{{lanes->reach[*at], true}};
    for (std::size_t round = 0; round < MAX_JOINS; ++round) {
        if (t.constant) {
            break;
        }
        const auto at_phi = phi_at(t.v);
        if (!at_phi) {
            break;
        }
        const auto j = only_edge(at_phi->first, known);
        if (!j) {
            break;
        }
        t = origin(p.blocks[at_phi->first].phis[at_phi->second].args[*j]);
    }
    return t;
}

Traced<std::optional<Source>> Tracer::phi(std::span<const Term> words, std::vector<std::size_t>& joins,
                                          Known& known) const
{
    const auto phi_of = [&](const Term& w) { return w.constant ? std::nullopt : phi_at(w.v); };
    std::optional<std::size_t> found;
    for (const Term& w : words) {
        if (const auto at_phi = phi_of(w)) {
            if (!found) {
                found = at_phi->first;
            } else if (at_phi->first != *found) {
                return std::unexpected("a descriptor joined in several blocks");
            }
        }
    }
    if (!found) {
        return std::optional<Source>{};
    }
    const std::size_t block = *found;
    if (contains(joins, block)) {
        return std::unexpected("a descriptor that changes in a loop");
    }
    if (joins.size() >= MAX_JOINS) {
        return std::unexpected("a descriptor joined too often");
    }
    joins.push_back(block);
    const auto& block_phis = p.blocks[block].phis;
    const auto edge = [&](std::size_t j) {
        std::vector<Term> out;
        for (const Term& w : words) {
            const auto at_phi = phi_of(w);
            out.push_back(at_phi ? origin(block_phis[at_phi->second].args[j]) : w);
        }
        return out;
    };
    Source::Phi joined_source{block, {}};
    std::optional<Why> failed;
    for (std::size_t j = 0; j < p.blocks[block].preds.size(); ++j) {
        auto arg = classify(edge(j), joins, known);
        if (!arg) {
            failed = arg.error();
            break;
        }
        joined_source.args.push_back(std::move(*arg));
    }
    Traced<Source> source = Source{};
    if (!failed) {
        source = Source{std::move(joined_source)};
    } else {
        source = std::unexpected(*failed);
        if (const auto j = only_edge(block, known)) {
            auto one = classify(edge(*j), joins, known);
            if (one && anywhere(*one)) {
                source = std::move(*one);
            }
        }
    }
    joins.pop_back();
    if (!source) {
        return std::unexpected(source.error());
    }
    return std::optional<Source>{std::move(*source)};
}

Traced<std::optional<Source>> Tracer::select(std::span<const Term> words, std::vector<std::size_t>& joins,
                                             Known& known) const
{
    std::vector<std::optional<Picked>> picks;
    for (const Term& w : words) {
        picks.push_back(w.constant ? std::nullopt : picked(w.v));
    }
    std::optional<Value> scc;
    for (const auto& pick : picks) {
        if (pick) {
            if (!scc) {
                scc = pick->condition;
            } else if (pick->condition != *scc) {
                return std::unexpected("a descriptor chosen by several conditions");
            }
        }
    }
    if (!scc) {
        return std::optional<Source>{};
    }
    if (joins.size() >= MAX_JOINS) {
        return std::unexpected("a descriptor joined too often");
    }
    joins.push_back(JOIN_MARKER);
    const auto side = [&](bool set) {
        std::vector<Term> out;
        for (std::size_t k = 0; k < words.size(); ++k) {
            out.push_back(picks[k] ? (set ? picks[k]->set : picks[k]->clear) : words[k]);
        }
        return out;
    };
    Source::Select selected{*scc, {}};
    std::optional<Why> failed;
    for (const bool set : {true, false}) {
        auto arg = classify(side(set), joins, known);
        if (!arg) {
            failed = arg.error();
            break;
        }
        selected.args.push_back(std::move(*arg));
    }
    joins.pop_back();
    if (failed) {
        return std::unexpected(*failed);
    }
    return std::optional<Source>{Source{std::move(selected)}};
}

std::optional<Picked> Tracer::picked(Value v) const
{
    const Inst* in = def(v);
    if (in == nullptr) {
        return std::nullopt;
    }
    const auto* sop = in->fields.as<Fields::Sop>();
    if (sop == nullptr) {
        return std::nullopt;
    }
    const auto position = std::ranges::find_if(in->writes, [&](const auto& w) { return w.second == v; });
    if (position == in->writes.end()) {
        return std::nullopt;
    }
    const auto k = static_cast<std::uint32_t>(position - in->writes.begin());
    const std::string_view name = in->name();
    bool wide = false;
    if (name == "s_cselect_b32" || name == "s_cmov_b32") {
        wide = false;
    } else if (name == "s_cselect_b64" || name == "s_cmov_b64") {
        wide = true;
    } else {
        return std::nullopt;
    }
    const auto dword = [&](const Operand& op) -> std::optional<Term> {
        if (const auto* s = op.as<Operand::Scalar>()) {
            if (s->code == NULL_REG) {
                return Term::K(0);
            }
            const auto x = in->read(sreg(s->code + k));
            return x ? std::optional<Term>{origin(*x)} : std::nullopt;
        }
        if (const auto* c = op.as<Operand::Const>()) {
            if (!wide) {
                const auto x = inline_const(c->code);
                return x ? std::optional<Term>{Term::K(*x)} : std::nullopt;
            }
            if (c->code >= 128 && c->code <= 208) {
                const auto low = inline_const(c->code);
                if (!low) {
                    return std::nullopt;
                }
                return Term::K(k == 0 ? *low : static_cast<std::uint32_t>(static_cast<std::int32_t>(*low) >> 31));
            }
            return std::nullopt;
        }
        if (const auto* l = op.as<Operand::Literal>(); l != nullptr && !wide) {
            return Term::K(l->value);
        }
        return std::nullopt;
    };
    if (sop->src.empty()) {
        return std::nullopt;
    }
    const auto set = dword(sop->src[0]);
    if (!set) {
        return std::nullopt;
    }
    std::optional<Term> clear;
    if (name.starts_with("s_cmov")) {
        if (!sop->dst) {
            return std::nullopt;
        }
        clear = dword(*sop->dst);
    } else {
        if (sop->src.size() < 2) {
            return std::nullopt;
        }
        clear = dword(sop->src[1]);
    }
    if (!clear) {
        return std::nullopt;
    }
    const auto scc = in->read(Reg::Scc());
    if (!scc) {
        return std::nullopt;
    }
    return Picked{*scc, *set, *clear};
}

std::optional<std::pair<Term, std::uint32_t>> Tracer::set_bits(Value v) const
{
    if (const auto s = sop2(v, {"s_or_b32"})) {
        const auto& [a, b] = s->second;
        if (b.constant) {
            return std::pair{a, b.k};
        }
        if (a.constant) {
            return std::pair{b, a.k};
        }
    }
    const Inst* in = def(v);
    if (in == nullptr) {
        return std::nullopt;
    }
    const auto* sop = in->fields.as<Fields::Sop>();
    if (sop == nullptr || !sop->dst || sop->src.empty()) {
        return std::nullopt;
    }
    const auto* dst = sop->dst->as<Operand::Scalar>();
    if (dst == nullptr) {
        return std::nullopt;
    }
    const auto first = term(*in, sop->src[0]);
    if (!first) {
        return std::nullopt;
    }
    if (in->name() == "s_bitset1_b32" && first->constant) {
        const auto old = in->read(Reg::S(dst->code));
        if (!old) {
            return std::nullopt;
        }
        return std::pair{origin(*old), 1u << (first->k & 31)};
    }
    return std::nullopt;
}

std::optional<Value> Tracer::first_lane_source(Value v) const
{
    const Inst* in = def(v);
    if (in == nullptr) {
        return std::nullopt;
    }
    const auto* f = in->fields.as<Valu>();
    if (f == nullptr || in->name() != "v_readfirstlane_b32" || f->sdwa || f->dpp || f->src.empty()) {
        return std::nullopt;
    }
    const auto* vec = f->src.front().as<Operand::Vector>();
    if (vec == nullptr) {
        return std::nullopt;
    }
    return in->read(Reg::V(vec->reg));
}

std::optional<Value> Tracer::first_lane_offset(Site load) const
{
    const Inst& in = inst(load);
    const auto* smem = in.fields.as<Fields::Smem>();
    if (smem == nullptr) {
        return std::nullopt;
    }
    read_first_lane = true;
    const auto address = smem_address(in, smem->base, smem->offset, smem->soffset);
    read_first_lane = false;
    if (!address || !address->dynamic) {
        return std::nullopt;
    }
    const Term t = origin(*address->dynamic);
    if (t.is_value() && first_lane_source(t.v)) {
        return t.v;
    }
    return std::nullopt;
}

bool Tracer::kept_per_lane(const Inst& in, const Operand& op) const
{
    const auto* s = op.as<Operand::Scalar>();
    if (s == nullptr) {
        return false;
    }
    std::vector<Term> words;
    for (std::uint32_t k = 0; k < s->dwords; ++k) {
        const auto v = in.read(sreg(s->code + k));
        if (!v) {
            return false;
        }
        words.push_back(origin(*v));
    }
    return lane_copies(words).has_value();
}

std::optional<std::vector<Value>> Tracer::first_lane_words(const Inst& in, const Operand& op) const
{
    const auto* s = op.as<Operand::Scalar>();
    if (s == nullptr) {
        return std::nullopt;
    }
    std::vector<Value> out;
    for (std::uint32_t k = 0; k < s->dwords; ++k) {
        const auto v = in.read(sreg(s->code + k));
        if (!v) {
            return std::nullopt;
        }
        const Term t = origin(*v);
        if (t.constant || !first_lane_source(t.v)) {
            return std::nullopt;
        }
        out.push_back(t.v);
    }
    return out;
}

Term Tracer::lane_term(Term t) const
{
    for (std::size_t round = 0; round < MAX_JOINS * 2; ++round) {
        if (t.constant) {
            break;
        }
        const Term next = vorigin(t.v);
        if (next == t) {
            break;
        }
        t = next;
    }
    return t;
}

bool Tracer::varies(Term t) const
{
    if (t.constant) {
        return false;
    }
    const Value v = t.v;
    std::optional<Reg> reg = input(v);
    if (!reg) {
        if (const auto at_phi = phi_at(v)) {
            reg = p.blocks[at_phi->first].phis[at_phi->second].reg;
        } else if (const Inst* in = def(v)) {
            const auto w = std::ranges::find_if(in->writes, [&](const auto& x) { return x.second == v; });
            if (w != in->writes.end()) {
                reg = w->first;
            }
        }
    }
    return reg && reg->is_v();
}

Traced<Term> Tracer::lane_value(Term t, const Known& known) const
{
    for (std::size_t round = 0; round < MAX_JOINS * 2; ++round) {
        if (t.constant) {
            break;
        }
        const Value v = t.v;
        const Term next = vstep(v);
        if (next == t) {
            break;
        }
        if (varies(t)) {
            if (const auto site = def_site(v)) {
                const auto ran = runs(*site, known);
                if (!ran) {
                    return std::unexpected(ran.error());
                }
            }
        }
        t = next;
    }
    return t;
}

Term Tracer::vstep(Value v) const
{
    if (const Inst* in = def(v); in != nullptr && in->name() == "v_mov_b32") {
        if (const auto* f = in->fields.as<Valu>(); f != nullptr && plain(*f) && !f->src.empty()) {
            if (const auto t = raw(*in, f->src.front())) {
                return *t;
            }
        }
    }
    return origin(v);
}

std::optional<Term> Tracer::raw(const Inst& in, const Operand& op) const
{
    if (const auto* vec = op.as<Operand::Vector>()) {
        const auto v = in.read(Reg::V(vec->reg));
        return v ? std::optional<Term>{Term::V(*v)} : std::nullopt;
    }
    return term(in, op);
}

Traced<bool> Tracer::runs(Site site, const Known& known) const
{
    if (lanes == nullptr || site.first >= lanes->guards.size() || site.second >= lanes->guards[site.first].size()) {
        return true;
    }
    const auto& guard = lanes->guards[site.first][site.second];
    if (!guard) {
        return true;
    }
    Known skips = known;
    skips.emplace_back(lanes->reach[site.first], true);
    skips.emplace_back(*guard, false);
    if (meet(skips)) {
        return std::unexpected("a per-lane descriptor written where some lanes may keep the old one");
    }
    return true;
}

Traced<Source> Tracer::per_lane(std::span<const Term> input_words, std::vector<std::size_t>& joins,
                                Known& known) const
{
    std::vector<Term> words;
    for (const Term& w : input_words) {
        const auto value = lane_value(w, known);
        if (!value) {
            return std::unexpected(value.error());
        }
        words.push_back(*value);
    }
    const auto lane = std::ranges::find_if(words, [&](const Term& w) { return varies(w); });
    if (lane == words.end()) {
        return classify(words, joins, known);
    }
    auto source = fetched(words, known);
    if (!source) {
        return std::unexpected(source.error());
    }
    if (*source) {
        return std::move(**source);
    }
    source = lane_select(words, joins, known);
    if (!source) {
        return std::unexpected(source.error());
    }
    if (*source) {
        return std::move(**source);
    }
    source = lane_phi(words, joins, known);
    if (!source) {
        return std::unexpected(source.error());
    }
    if (*source) {
        return std::move(**source);
    }
    const Value v = lane->v;
    if (input(v)) {
        return std::unexpected("a descriptor read from the first lane of an input VGPR");
    }
    const Inst* in = def(v);
    if (in == nullptr) {
        return std::unexpected("a descriptor read from the first lane of a phi");
    }
    const std::string_view name = in->name();
    if (name.starts_with("ds_read")) {
        return std::unexpected("a descriptor read from the first lane of an LDS read");
    }
    if (name.starts_with("buffer_load")) {
        return std::unexpected("a descriptor read from the first lane of a buffer load");
    }
    if (name.starts_with("global_load") || name.starts_with("flat_load")) {
        return std::unexpected("a descriptor read from the first lane of a partial or mixed load");
    }
    return std::unexpected("a descriptor read from the first lane of computed VGPRs");
}

Traced<std::optional<Source>> Tracer::fetched(std::span<const Term> words, const Known& known) const
{
    std::vector<std::pair<Site, std::size_t>> sites;
    for (const Term& w : words) {
        if (w.constant) {
            return std::optional<Source>{};
        }
        const auto site = def_site(w.v);
        if (!site) {
            return std::optional<Source>{};
        }
        const Inst& in = inst(*site);
        if (!in.name().starts_with("global_load_dword")) {
            return std::optional<Source>{};
        }
        std::optional<std::size_t> k;
        std::size_t n = 0;
        for (const auto& write : in.writes) {
            if (!write.first.is_v()) {
                continue;
            }
            if (write.second == w.v) {
                k = n;
                break;
            }
            ++n;
        }
        if (!k) {
            return std::optional<Source>{};
        }
        sites.emplace_back(*site, *k);
    }
    const auto address = [&](Site site) -> Traced<Address> {
        const Inst& in = inst(site);
        const auto* flat = in.fields.as<Fields::Flat>();
        if (flat == nullptr) {
            return std::unexpected("a global load");
        }
        return flat_address(in, flat->addr, flat->saddr, flat->offset);
    };
    const auto [load, first] = sites.front();
    const auto a0 = address(load);
    if (!a0) {
        return std::unexpected(a0.error());
    }
    for (std::size_t k = 0; k < sites.size(); ++k) {
        const auto& [site, dword] = sites[k];
        const auto ran = runs(site, known);
        if (!ran) {
            return std::unexpected(ran.error());
        }
        const auto a = address(site);
        if (!a) {
            return std::unexpected(a.error());
        }
        const std::uint32_t want = a0->offset + 4 * static_cast<std::uint32_t>(first + k);
        if (a->param != a0->param || a->dynamic != a0->dynamic
            || a->offset + 4 * static_cast<std::uint32_t>(dword) != want) {
            return std::unexpected("a descriptor read from the first lane of loads that do not line up");
        }
    }
    return std::optional<Source>{Source{Source::Fetched{load, static_cast<std::uint8_t>(first)}}};
}

Traced<std::optional<Source>> Tracer::lane_select(std::span<const Term> words, std::vector<std::size_t>& joins,
                                                  Known& known) const
{
    std::vector<std::optional<Picked>> picks;
    for (const Term& w : words) {
        picks.push_back(w.constant ? std::nullopt : lane_picked(w.v));
    }
    std::optional<Value> mask;
    for (const auto& pick : picks) {
        if (pick) {
            if (!mask) {
                mask = pick->condition;
            } else if (pick->condition != *mask) {
                return std::unexpected("a descriptor chosen per lane by several masks");
            }
        }
    }
    if (!mask) {
        return std::optional<Source>{};
    }
    for (std::size_t k = 0; k < words.size(); ++k) {
        if (words[k].is_value() && picks[k]) {
            if (const auto site = def_site(words[k].v)) {
                const auto ran = runs(*site, known);
                if (!ran) {
                    return std::unexpected(ran.error());
                }
            }
        }
    }
    if (joins.size() >= MAX_JOINS) {
        return std::unexpected("a descriptor joined too often");
    }
    joins.push_back(JOIN_MARKER);
    std::optional<Bool> bit;
    if (lanes != nullptr) {
        if (const auto found = lanes->masks.find(*mask); found != lanes->masks.end()) {
            bit = found->second;
        }
    }
    const auto side = [&](bool set) {
        std::vector<Term> out;
        for (std::size_t k = 0; k < words.size(); ++k) {
            out.push_back(picks[k] ? (set ? picks[k]->set : picks[k]->clear) : words[k]);
        }
        return out;
    };
    Source::Lanes picked_source{*mask, {}};
    std::optional<Why> failed;
    for (const bool set : {true, false}) {
        const std::size_t n = known.size();
        if (bit) {
            known.emplace_back(*bit, set);
        }
        auto arg = per_lane(side(set), joins, known);
        known.resize(n);
        if (!arg) {
            failed = arg.error();
            break;
        }
        picked_source.args.push_back(std::move(*arg));
    }
    joins.pop_back();
    if (failed) {
        return std::unexpected(*failed);
    }
    return std::optional<Source>{Source{std::move(picked_source)}};
}

std::optional<Picked> Tracer::lane_picked(Value v) const
{
    const Inst* in = def(v);
    if (in == nullptr) {
        return std::nullopt;
    }
    const auto* f = in->fields.as<Valu>();
    if (f == nullptr || !f->dst) {
        return std::nullopt;
    }
    const auto* dst = f->dst->as<Operand::Vector>();
    if (dst == nullptr) {
        return std::nullopt;
    }
    if (in->name() != "v_cndmask_b32" || in->write(Reg::V(dst->reg)) != v || !plain(*f) || f->clamp || f->omod != 0) {
        return std::nullopt;
    }
    if (f->src.size() != 3) {
        return std::nullopt;
    }
    const auto* s = f->src[2].as<Operand::Scalar>();
    if (s == nullptr) {
        return std::nullopt;
    }
    const auto mask = in->read(Reg::S(s->code));
    if (!mask) {
        return std::nullopt;
    }
    const auto set = raw(*in, f->src[1]);
    if (!set) {
        return std::nullopt;
    }
    const auto clear = raw(*in, f->src[0]);
    if (!clear) {
        return std::nullopt;
    }
    return Picked{*mask, *set, *clear};
}

Traced<std::optional<Source>> Tracer::lane_phi(std::span<const Term> words, std::vector<std::size_t>& joins,
                                               Known& known) const
{
    const auto phi_of = [&](const Term& w) { return w.constant ? std::nullopt : phi_at(w.v); };
    std::optional<std::size_t> found;
    for (const Term& w : words) {
        if (const auto at_phi = phi_of(w)) {
            if (!found) {
                found = at_phi->first;
            } else if (at_phi->first != *found) {
                return std::unexpected("a descriptor joined in several blocks");
            }
        }
    }
    if (!found) {
        return std::optional<Source>{};
    }
    const std::size_t block = *found;
    if (contains(joins, block)) {
        return std::unexpected("a descriptor that changes in a loop");
    }
    const auto& preds = p.blocks[block].preds;
    if (std::ranges::any_of(preds, [&](std::size_t q) { return rpo[q] >= rpo[block]; })) {
        return std::unexpected("a descriptor read from the first lane of a phi at a loop header");
    }
    if (lanes == nullptr) {
        return std::unexpected("a descriptor read from the first lane of a phi, without the lanes' paths");
    }
    for (const Term& w : words) {
        if (w.is_value() && !phi_of(w)) {
            if (const auto site = def_site(w.v); site && (site->first == block || reaches(block, site->first))) {
                return std::unexpected("a descriptor partly joined by control flow");
            }
        }
    }
    if (joins.size() >= MAX_JOINS) {
        return std::unexpected("a descriptor joined too often");
    }
    joins.push_back(block);
    const auto& block_phis = p.blocks[block].phis;
    Source::LanePhi joined_source{block, {}};
    for (std::size_t j = 0; j < preds.size(); ++j) {
        const std::size_t q = preds[j];
        const std::size_t n = known.size();
        known.emplace_back(lanes->reach[q], true);
        const LaneExit& exit = lanes->exits[q];
        if (exit.kind == LaneExit::Kind::Branch && exit.taken != exit.not_taken) {
            known.emplace_back(exit.when, exit.taken == block);
        }
        Traced<std::optional<Source>> arg = std::optional<Source>{};
        if (meet(known)) {
            std::vector<Term> edge;
            for (const Term& w : words) {
                const auto at_phi = phi_of(w);
                edge.push_back(at_phi ? Term::V(block_phis[at_phi->second].args[j]) : w);
            }
            auto traced = per_lane(edge, joins, known);
            if (traced) {
                arg = std::optional<Source>{std::move(*traced)};
            } else {
                arg = std::unexpected(traced.error());
            }
        }
        known.resize(n);
        if (!arg) {
            joins.pop_back();
            return std::unexpected(arg.error());
        }
        joined_source.args.push_back(std::move(*arg));
    }
    joins.pop_back();
    return std::optional<Source>{Source{std::move(joined_source)}};
}

bool Tracer::reaches(std::size_t from, std::size_t to) const
{
    std::vector<bool> seen(p.blocks.size(), false);
    std::vector<std::size_t> stack = p.blocks[from].exit.successors();
    while (!stack.empty()) {
        const std::size_t x = stack.back();
        stack.pop_back();
        if (x == to) {
            return true;
        }
        if (seen[x]) {
            continue;
        }
        seen[x] = true;
        const auto next = p.blocks[x].exit.successors();
        stack.insert(stack.end(), next.begin(), next.end());
    }
    return false;
}

Traced<Waterfall> Tracer::waterfall(std::size_t b, std::span<const std::size_t> accesses,
                                    std::span<const Value> reads) const
{
    constexpr Why LOOP = "a descriptor read from the first lane outside a waterfall loop";
    constexpr Why SPLIT = "a waterfall loop nested in another or split over blocks";
    constexpr Why SELECT = "a waterfall loop whose compares do not select the access's lanes";
    constexpr Why RETRY = "a waterfall loop whose retry does not keep the lanes still waiting";
    const std::size_t access = accesses.front();
    const auto& block = p.blocks[b];
    if (lanes == nullptr || block.insts.empty()) {
        return std::unexpected(LOOP);
    }
    const Inst& exit = block.insts.back();
    if (block.exit.kind != ir::Exit::Kind::Branch || block.exit.taken != b || block.exit.not_taken == b
        || exit.name() != "s_cbranch_execnz") {
        return std::unexpected(LOOP);
    }
    const auto mask = [&](std::optional<Value> low, std::optional<Value> high) -> std::optional<Bool> {
        if (!low) {
            return std::nullopt;
        }
        std::optional<Value> pair_high;
        if (p.wave64) {
            if (!high) {
                return std::nullopt;
            }
            pair_high = high;
        }
        return lanes->mask_bits(*low, pair_high);
    };
    const auto exec = [&](const Inst& in) {
        return mask(in.read(Reg::S(ir::EXEC)), in.read(Reg::S(ir::EXEC + 1)));
    };
    for (const Value v : reads) {
        const auto site = def_site(v);
        if (!site || site->first != b) {
            return std::unexpected(SPLIT);
        }
    }
    const auto selected = exec(block.insts[access]);
    if (!selected) {
        return std::unexpected(SELECT);
    }
    for (const std::size_t i : accesses) {
        if (exec(block.insts[i]) != selected) {
            return std::unexpected(SELECT);
        }
    }
    const auto retry = exec(exit);
    if (!retry) {
        return std::unexpected(RETRY);
    }
    const auto first = def_site(reads.front());
    if (!first) {
        return std::unexpected(LOOP);
    }
    const auto remaining = exec(inst(*first));
    if (!remaining) {
        return std::unexpected(SELECT);
    }
    for (const Value v : reads) {
        const auto site = def_site(v);
        if (!site || site->second >= access || exec(inst(*site)) != remaining) {
            return std::unexpected(SELECT);
        }
    }
    const std::pair<Bool, bool> reach{lanes->reach[b], true};
    const auto meets_all = [&](std::initializer_list<std::pair<Bool, bool>> conditions) {
        return meet(std::span<const std::pair<Bool, bool>>{conditions.begin(), conditions.size()});
    };
    Waterfall out;
    Known required{{*remaining, true}};
    for (std::size_t i = 0; i < access; ++i) {
        const Inst& in = block.insts[i];
        const auto* f = in.fields.as<Valu>();
        if (f == nullptr) {
            continue;
        }
        const std::string_view name = in.name();
        std::string_view ty;
        if (name.starts_with("v_cmp_eq_")) {
            ty = name.substr(9);
        } else if (name.starts_with("v_cmpx_eq_")) {
            ty = name.substr(10);
        } else {
            continue;
        }
        std::uint32_t dwords = 0;
        if (ty == "u32" || ty == "i32") {
            dwords = 1;
        } else if (ty == "u64" || ty == "i64") {
            dwords = 2;
        } else {
            continue;
        }
        if (!plain(*f)) {
            continue;
        }
        const auto pair = [&](const Operand& first_op, const Operand& own) -> std::optional<std::vector<Value>> {
            const auto* s = first_op.as<Operand::Scalar>();
            const auto* vec = own.as<Operand::Vector>();
            if (s == nullptr || vec == nullptr) {
                return std::nullopt;
            }
            std::vector<Value> xs;
            for (std::uint32_t k = 0; k < dwords; ++k) {
                const auto sv = in.read(sreg(s->code + k));
                if (!sv) {
                    return std::nullopt;
                }
                const Term t = origin(*sv);
                if (t.constant) {
                    return std::nullopt;
                }
                const Value x = t.v;
                const auto lane = in.read(vreg(vec->reg + k));
                if (!lane) {
                    return std::nullopt;
                }
                const auto read_site = def_site(x);
                if (!read_site) {
                    return std::nullopt;
                }
                if (std::ranges::find(reads, x) == reads.end() || read_site->second >= i) {
                    return std::nullopt;
                }
                const auto source = first_lane_source(x);
                if (!source || !(lane_term(Term::V(*source)) == lane_term(Term::V(*lane)))) {
                    return std::nullopt;
                }
                xs.push_back(x);
            }
            return xs;
        };
        if (f->src.size() < 2) {
            continue;
        }
        auto xs = pair(f->src[0], f->src[1]);
        if (!xs) {
            xs = pair(f->src[1], f->src[0]);
        }
        if (!xs) {
            continue;
        }
        std::uint32_t code = 0;
        if (name.starts_with("v_cmpx_")) {
            code = ir::EXEC;
        } else if (const auto* d = f->dst ? f->dst->as<Operand::Scalar>() : nullptr) {
            code = d->code;
        } else {
            continue;
        }
        const auto matched = mask(in.write(sreg(code)), in.write(sreg(code + 1)));
        if (!matched) {
            continue;
        }
        if (meets_all({reach, {*selected, true}, {*matched, false}})) {
            continue;
        }
        required.emplace_back(*matched, true);
        out.compares.emplace_back(b, i);
        for (const Value x : *xs) {
            if (!contains(out.reads, x)) {
                out.reads.push_back(x);
            }
        }
    }
    if (std::ranges::any_of(reads, [&](Value v) { return !contains(out.reads, v); })
        || meets_all({reach, {*selected, true}, {*remaining, false}})) {
        return std::unexpected(SELECT);
    }
    required.push_back(reach);
    required.emplace_back(*selected, false);
    if (meet(required)) {
        return std::unexpected(SELECT);
    }
    if (meets_all({reach, {*retry, true}, {*remaining, false}}) || meets_all({reach, {*retry, true}, {*selected, true}})
        || meets_all({reach, {*remaining, true}, {*selected, false}, {*retry, false}})) {
        return std::unexpected(RETRY);
    }
    return out;
}

std::optional<Source> Tracer::loaded(std::span<const Term> words) const
{
    if (words.empty() || words.front().constant) {
        return std::nullopt;
    }
    const Value v0 = words.front().v;
    const auto load = def_site(v0);
    if (!load) {
        return std::nullopt;
    }
    const Inst& in = inst(*load);
    if (!in.name().starts_with("s_load_")) {
        return std::nullopt;
    }
    const auto position = std::ranges::find_if(in.writes, [&](const auto& w) { return w.second == v0; });
    if (position == in.writes.end()) {
        return std::nullopt;
    }
    const auto first = static_cast<std::size_t>(position - in.writes.begin());
    for (std::size_t k = 0; k < words.size(); ++k) {
        if (first + k >= in.writes.size() || !(words[k] == Term::V(in.writes[first + k].second))) {
            return std::nullopt;
        }
    }
    return Source{Source::Loaded{*load, static_cast<std::uint8_t>(first)}};
}

std::optional<Source> Tracer::lane_copies(std::span<const Term> words) const
{
    std::vector<Site> loads;
    std::optional<std::size_t> first;
    for (std::size_t k = 0; k < words.size(); ++k) {
        const Term& w = words[k];
        if (w.constant) {
            return std::nullopt;
        }
        const auto lane = first_lane(w.v);
        if (!lane) {
            return std::nullopt;
        }
        std::vector<Term> copies;
        if (!scalar_copies(*lane, copies)) {
            return std::nullopt;
        }
        bool found = false;
        for (const Term& c : copies) {
            if (c.constant) {
                continue;
            }
            const auto site = def_site(c.v);
            if (!site) {
                continue;
            }
            const Inst& load = inst(*site);
            if (!load.name().starts_with("s_load_")) {
                continue;
            }
            const auto position = std::ranges::find_if(load.writes, [&](const auto& x) { return x.second == c.v; });
            if (position == load.writes.end()) {
                return std::nullopt;
            }
            const auto dword = static_cast<std::size_t>(position - load.writes.begin());
            if (dword < k) {
                return std::nullopt;
            }
            const std::size_t f = dword - k;
            if (!first) {
                first = f;
            }
            if (*first != f) {
                return std::nullopt;
            }
            if (!contains(loads, *site)) {
                loads.push_back(*site);
            }
            found = true;
        }
        if (!found) {
            return std::nullopt;
        }
    }
    const auto address = [&](Site site) -> std::optional<Address> {
        const Inst& in = inst(site);
        const auto* smem = in.fields.as<Fields::Smem>();
        if (smem == nullptr) {
            return std::nullopt;
        }
        auto a = smem_address(in, smem->base, smem->offset, smem->soffset);
        return a ? std::optional<Address>{*a} : std::nullopt;
    };
    if (loads.empty()) {
        return std::nullopt;
    }
    const auto a = address(loads.front());
    if (!a || a->dynamic) {
        return std::nullopt;
    }
    for (const Site& l : loads) {
        if (address(l) != a) {
            return std::nullopt;
        }
    }
    std::ranges::sort(loads);
    const auto load = std::ranges::find_if(loads, [&](const Site& l) { return base_from_entry(l); });
    if (load == loads.end() || !first) {
        return std::nullopt;
    }
    lane_copied.insert(lane_copied.end(), loads.begin(), loads.end());
    return Source{Source::Loaded{*load, static_cast<std::uint8_t>(*first)}};
}

bool Tracer::scalar_copies(Value start, std::vector<Term>& out) const
{
    std::vector<Value> seen;
    std::vector<Value> work{start};
    while (!work.empty()) {
        const Value v = work.back();
        work.pop_back();
        if (contains(seen, v)) {
            continue;
        }
        if (seen.size() == MAX_COPIES) {
            return false;
        }
        seen.push_back(v);
        if (const auto at_phi = phi_at(v)) {
            const auto& args = p.blocks[at_phi->first].phis[at_phi->second].args;
            work.insert(work.end(), args.begin(), args.end());
            continue;
        }
        const Inst* in = def(v);
        if (in == nullptr) {
            continue;
        }
        if (in->name().starts_with("global_load_")) {
            return false;
        }
        const auto* f = in->fields.as<Valu>();
        if (f == nullptr) {
            continue;
        }
        std::size_t sources = 0;
        if (in->name() == "v_mov_b32") {
            sources = 1;
        } else if (in->name() == "v_cndmask_b32") {
            sources = 2;
        } else {
            continue;
        }
        if (!plain(*f)) {
            continue;
        }
        for (std::size_t k = 0; k < sources && k < f->src.size(); ++k) {
            const Operand& op = f->src[k];
            if (const auto* vec = op.as<Operand::Vector>()) {
                if (const auto x = in->read(Reg::V(vec->reg))) {
                    work.push_back(*x);
                }
            } else if (const auto* s = op.as<Operand::Scalar>(); s != nullptr && s->code != NULL_REG) {
                if (const auto x = in->read(Reg::S(s->code))) {
                    out.push_back(origin(*x));
                }
            }
        }
        if (f->dst) {
            if (const auto* vec = f->dst->as<Operand::Vector>()) {
                if (const auto x = in->read(Reg::V(vec->reg))) {
                    work.push_back(*x);
                }
            }
        }
    }
    return true;
}

bool Tracer::base_from_entry(Site site) const
{
    const Inst& in = inst(site);
    const auto* smem = in.fields.as<Fields::Smem>();
    if (smem == nullptr) {
        return false;
    }
    const auto* base = smem->base.as<Operand::Scalar>();
    if (base == nullptr || !smem->soffset.is_scalar(NULL_REG)) {
        return false;
    }
    auto v = in.read(Reg::S(base->code));
    if (!v) {
        return false;
    }
    for (;;) {
        if (input(*v)) {
            return true;
        }
        const Inst* d = def(*v);
        if (d == nullptr) {
            return false;
        }
        const auto position = std::ranges::find_if(d->writes, [&](const auto& w) { return w.second == *v; });
        if (position == d->writes.end()) {
            return false;
        }
        const auto moved = copy_source(*d, static_cast<std::size_t>(position - d->writes.begin()));
        if (!moved || moved->constant) {
            return false;
        }
        v = moved->v;
    }
}

}
