#include <euphoria/shader/gfx10/ir.hpp>

#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>

namespace euphoria::shader::gfx10::ir {
namespace {

constexpr std::size_t kRegs = 256 + 1 + 256;
using RegSet = std::array<std::uint64_t, (kRegs + 63) / 64>;
using Effects = std::pair<std::vector<Reg>, std::vector<Reg>>;
using Edits = std::vector<std::tuple<std::size_t, std::uint8_t, std::uint8_t>>;

constexpr std::size_t kNoBlock = static_cast<std::size_t>(-1);

constexpr std::array<std::string_view, 19> kDropped{
    "s_nop",
    "s_waitcnt",
    "s_waitcnt_vscnt",
    "s_waitcnt_vmcnt",
    "s_waitcnt_expcnt",
    "s_waitcnt_lgkmcnt",
    "s_waitcnt_depctr",
    "s_wait_idle",
    "s_inst_prefetch",
    "s_clause",
    "s_setprio",
    "s_sleep",
    "s_ttracedata",
    "s_ttracedata_imm",
    "s_incperflevel",
    "s_decperflevel",
    "v_nop",
    "v_pipeflush",
    "v_clrexcp",
};

constexpr std::array<std::string_view, 29> kSccStems{
    "add", "sub", "addc", "subb", "min", "max", "and", "or", "xor", "andn2",
    "orn2", "nand", "nor", "xnor", "lshl", "lshr", "ashr", "bfe", "absdiff", "lshl1",
    "lshl2", "lshl3", "lshl4", "not", "wqm", "bcnt0", "bcnt1", "quadmask", "abs",
};

bool contains(std::string_view text, std::string_view part) noexcept
{
    return text.find(part) != std::string_view::npos;
}

std::size_t index_of(Reg reg) noexcept
{
    switch (reg.kind) {
    case Reg::Kind::S:
        return reg.index;
    case Reg::Kind::Scc:
        return 256;
    case Reg::Kind::V:
        return 257 + static_cast<std::size_t>(reg.index);
    }
    return 0;
}

Reg reg_at(std::size_t index) noexcept
{
    if (index < 256) {
        return Reg::S(static_cast<std::uint8_t>(index));
    }
    if (index == 256) {
        return Reg::Scc();
    }
    return Reg::V(static_cast<std::uint8_t>(index - 257));
}

void insert(RegSet& set, Reg reg) noexcept
{
    const std::size_t index = index_of(reg);
    set[index / 64] |= std::uint64_t{1} << (index % 64);
}

bool contains(const RegSet& set, Reg reg) noexcept
{
    const std::size_t index = index_of(reg);
    return ((set[index / 64] >> (index % 64)) & 1) != 0;
}

std::vector<Reg> members(const RegSet& set)
{
    std::vector<Reg> out;
    for (std::size_t index = 0; index < kRegs; ++index) {
        if (((set[index / 64] >> (index % 64)) & 1) != 0) {
            out.push_back(reg_at(index));
        }
    }
    return out;
}

Value lookup(const std::vector<std::pair<Reg, Value>>& outs, Reg reg) noexcept
{
    const auto found = std::lower_bound(outs.begin(), outs.end(), reg,
                                        [](const std::pair<Reg, Value>& entry, Reg value) {
                                            return entry.first < value;
                                        });
    if (found == outs.end() || found->first != reg) {
        return NONE;
    }
    return found->second;
}

bool writes_scc(Encoding encoding, std::string_view name)
{
    switch (encoding) {
    case Encoding::Sopc:
        return true;
    case Encoding::Sopk:
        return name.starts_with("s_cmpk_") || name == "s_addk_i32";
    default:
        break;
    }
    std::string_view stem;
    const std::size_t first = name.find('_');
    if (first != std::string_view::npos) {
        const std::size_t second = name.find('_', first + 1);
        stem = name.substr(first + 1, second == std::string_view::npos ? std::string_view::npos : second - first - 1);
    }
    return contains(name, "saveexec") || contains(name, "wrexec")
        || std::ranges::find(kSccStems, stem) != kSccStems.end();
}

Operand atomic_result(const Operand& data, std::string_view name)
{
    if (const auto* vector = data.as<Operand::Vector>(); vector != nullptr && contains(name, "cmpswap")) {
        return Operand::Vector{vector->reg, static_cast<std::uint8_t>(vector->dwords / 2)};
    }
    return data;
}

bool add_regs(const Operand& op, std::uint8_t lanes, std::vector<Reg>& out)
{
    if (const auto* s = op.as<Operand::Scalar>()) {
        if (s->code == NULL_REG) {
            return true;
        }
        const unsigned end = static_cast<unsigned>(s->code) + s->dwords;
        const unsigned limit = s->code < PARKED ? 128 : 256;
        if (end > limit) {
            return false;
        }
        for (unsigned code = s->code; code < end; ++code) {
            out.push_back(Reg::S(static_cast<std::uint8_t>(code)));
        }
        return true;
    }
    if (const auto* v = op.as<Operand::Vector>()) {
        if (static_cast<unsigned>(v->reg) + v->dwords > 256) {
            return false;
        }
        for (unsigned index = 0; index < v->dwords; ++index) {
            out.push_back(Reg::V(static_cast<std::uint8_t>(v->reg + index)));
        }
        return true;
    }
    if (const auto* c = op.as<Operand::Const>()) {
        switch (c->code) {
        case 251:
            for (std::uint8_t index = 0; index < lanes; ++index) {
                out.push_back(Reg::S(static_cast<std::uint8_t>(VCC + index)));
            }
            break;
        case 252:
            for (std::uint8_t index = 0; index < lanes; ++index) {
                out.push_back(Reg::S(static_cast<std::uint8_t>(EXEC + index)));
            }
            break;
        case 253:
            out.push_back(Reg::Scc());
            break;
        case 254:
            out.push_back(Reg::S(M0));
            break;
        default:
            break;
        }
    }
    return true;
}

std::expected<std::optional<Effects>, ShaderError> effects(const Instruction& inst, const Fields& fields,
                                                           std::string_view name, std::uint8_t lanes,
                                                           std::uint16_t vgprs)
{
    if (std::ranges::find(kDropped, name) != kDropped.end()) {
        return std::optional<Effects>{};
    }
    const auto unsupported = [&] { return std::unexpected(ShaderError::Unsupported(inst.offset, name.data())); };
    const auto out_of_bounds = [&] {
        return std::unexpected(ShaderError::Isa(inst.offset, "register range out of bounds"));
    };
    const auto mask = [lanes](std::uint8_t code, std::vector<Reg>& out) {
        for (std::uint8_t index = 0; index < lanes; ++index) {
            out.push_back(Reg::S(static_cast<std::uint8_t>(code + index)));
        }
    };
    std::vector<Reg> r;
    std::vector<Reg> w;
#define EUPHORIA_REGS(operand, out)                 \
    do {                                            \
        if (!add_regs((operand), lanes, (out))) {   \
            return out_of_bounds();                 \
        }                                           \
    } while (false)

    const bool vector = !(fields.is<Fields::Sop>() || fields.is<Fields::Sopp>() || fields.is<Fields::Smem>());

    if (const auto* sop = fields.as<Fields::Sop>()) {
        if (name.starts_with("s_movrel") || name.starts_with("s_subvector_loop") || name == "s_call_b64"
            || name == "s_rfe_b64") {
            return unsupported();
        }
        for (const Operand& s : sop->src) {
            EUPHORIA_REGS(s, r);
        }
        if (sop->dst) {
            EUPHORIA_REGS(*sop->dst, w);
            if (name.starts_with("s_bitset") || name.starts_with("s_cmov") || name == "s_addk_i32"
                || name == "s_mulk_i32") {
                EUPHORIA_REGS(*sop->dst, r);
            }
        }
        if (name == "s_addc_u32" || name == "s_subb_u32" || name == "s_cmovk_i32" || name.starts_with("s_cselect_")
            || name.starts_with("s_cmov_")) {
            r.push_back(Reg::Scc());
        }
        if (contains(name, "saveexec") || contains(name, "wrexec")) {
            mask(EXEC, r);
            mask(EXEC, w);
        }
        if (writes_scc(inst.encoding, name)) {
            w.push_back(Reg::Scc());
        }
    } else if (fields.is<Fields::Sopp>()) {
        if (name == "s_cbranch_scc0" || name == "s_cbranch_scc1") {
            r.push_back(Reg::Scc());
        } else if (name == "s_cbranch_vccz" || name == "s_cbranch_vccnz") {
            mask(VCC, r);
        } else if (name == "s_cbranch_execz" || name == "s_cbranch_execnz") {
            mask(EXEC, r);
        } else if (name == "s_sendmsg" || name == "s_sendmsghalt") {
            r.push_back(Reg::S(M0));
        } else if (name == "s_endpgm" || name == "s_endpgm_saved" || name == "s_endpgm_ordered_ps_done"
                   || name == "s_code_end" || name == "s_branch" || name == "s_barrier" || name == "s_icache_inv"
                   || name == "s_wakeup" || name == "s_round_mode" || name == "s_denorm_mode") {
        } else {
            return unsupported();
        }
    } else if (const auto* smem = fields.as<Fields::Smem>()) {
        if (name == "s_memtime" || name == "s_memrealtime" || name == "s_get_waveid_in_workgroup") {
            if (!smem->data) {
                return unsupported();
            }
            EUPHORIA_REGS(*smem->data, w);
        } else if (name == "s_gl1_inv" || name == "s_dcache_inv" || name == "s_dcache_wb") {
        } else if (contains(name, "_load_") || contains(name, "_store_")) {
            EUPHORIA_REGS(smem->base, r);
            EUPHORIA_REGS(smem->soffset, r);
            if (!smem->data) {
                return unsupported();
            }
            if (contains(name, "_load_")) {
                EUPHORIA_REGS(*smem->data, w);
            } else {
                EUPHORIA_REGS(*smem->data, r);
            }
        } else {
            return unsupported();
        }
    } else if (const auto* v = fields.as<Valu>()) {
        bool src_indexed = false;
        bool dst_indexed = false;
        if (name == "v_movrels_b32") {
            src_indexed = true;
        } else if (name == "v_movreld_b32") {
            dst_indexed = true;
        } else if (name == "v_movrelsd_b32" || name == "v_movrelsd_2_b32" || name == "v_swaprel_b32") {
            src_indexed = true;
            dst_indexed = true;
        }
        const auto indexed = [&](const Operand& op, std::vector<Reg>& out) {
            const auto* vec = op.as<Operand::Vector>();
            if (vec == nullptr || static_cast<std::uint16_t>(vec->reg) >= vgprs) {
                return false;
            }
            for (std::uint16_t reg = vec->reg; reg < vgprs; ++reg) {
                out.push_back(Reg::V(static_cast<std::uint8_t>(reg)));
            }
            return true;
        };
        const auto relative_error = [&] {
            return std::unexpected(
                ShaderError::Isa(inst.offset, "relative VGPR base outside the program's VGPRs"));
        };
        for (std::size_t index = 0; index < v->src.size(); ++index) {
            if (index == 0 && src_indexed) {
                if (!indexed(v->src[index], r)) {
                    return relative_error();
                }
            } else {
                EUPHORIA_REGS(v->src[index], r);
            }
        }
        if (name.starts_with("v_cmpx")) {
            mask(EXEC, w);
        } else if (v->dst) {
            if (dst_indexed) {
                if (!indexed(*v->dst, w)) {
                    return relative_error();
                }
            } else {
                EUPHORIA_REGS(*v->dst, w);
            }
        }
        if (v->carry) {
            EUPHORIA_REGS(*v->carry, w);
        }
        if (name == "v_swap_b32") {
            EUPHORIA_REGS(v->src[0], w);
            if (v->dst) {
                EUPHORIA_REGS(*v->dst, r);
            }
        } else if (name == "v_swaprel_b32") {
            if (!indexed(v->src[0], w)) {
                return relative_error();
            }
            if (v->dst && !indexed(*v->dst, r)) {
                return relative_error();
            }
        } else if (name == "v_div_fmas_f32" || name == "v_div_fmas_f64") {
            mask(VCC, r);
        }
        if (name.starts_with("v_interp_") || src_indexed || dst_indexed) {
            r.push_back(Reg::S(M0));
        }
    } else if (const auto* vintrp = fields.as<Fields::Vintrp>()) {
        EUPHORIA_REGS(vintrp->src, r);
        r.push_back(Reg::S(M0));
        EUPHORIA_REGS(vintrp->dst, w);
    } else if (const auto* ds = fields.as<Fields::Ds>()) {
        if (ds->addr) {
            EUPHORIA_REGS(*ds->addr, r);
        }
        for (const Operand& data : ds->data) {
            EUPHORIA_REGS(data, r);
        }
        if (ds->dst) {
            EUPHORIA_REGS(*ds->dst, w);
        }
        if (ds->gds || name.starts_with("ds_gws") || contains(name, "addtid") || name == "ds_ordered_count"
            || name == "ds_append" || name == "ds_consume") {
            r.push_back(Reg::S(M0));
        }
    } else if (const auto* buffer = fields.as<Fields::Buffer>()) {
        if (buffer->offen || buffer->idxen) {
            EUPHORIA_REGS(buffer->addr, r);
        }
        EUPHORIA_REGS(buffer->rsrc, r);
        EUPHORIA_REGS(buffer->soffset, r);
        if (buffer->lds) {
            r.push_back(Reg::S(M0));
        }
        if (buffer->data) {
            const Operand& d = *buffer->data;
            if (contains(name, "_load_")) {
                if (!buffer->lds) {
                    EUPHORIA_REGS(d, w);
                }
            } else if (contains(name, "_store_")) {
                EUPHORIA_REGS(d, r);
            } else if (contains(name, "_atomic_")) {
                EUPHORIA_REGS(d, r);
                if (buffer->cache.glc) {
                    EUPHORIA_REGS(atomic_result(d, name), w);
                }
            } else {
                return unsupported();
            }
        }
    } else if (const auto* image = fields.as<Fields::Image>()) {
        for (const Operand& a : image->addr) {
            EUPHORIA_REGS(a, r);
        }
        EUPHORIA_REGS(image->rsrc, r);
        if (image->sampler) {
            EUPHORIA_REGS(*image->sampler, r);
        }
        if (contains(name, "_store")) {
            EUPHORIA_REGS(image->data, r);
        } else if (contains(name, "_atomic_")) {
            EUPHORIA_REGS(image->data, r);
            if (image->cache.glc) {
                EUPHORIA_REGS(atomic_result(image->data, name), w);
            }
        } else {
            EUPHORIA_REGS(image->data, w);
        }
    } else if (const auto* flat = fields.as<Fields::Flat>()) {
        for (const auto* op : {&flat->addr, &flat->data, &flat->saddr}) {
            if (*op) {
                EUPHORIA_REGS(**op, r);
            }
        }
        if (flat->dst) {
            EUPHORIA_REGS(*flat->dst, w);
        }
    } else if (const auto* exp = fields.as<Fields::Exp>()) {
        for (const auto& s : exp->src) {
            if (s) {
                EUPHORIA_REGS(*s, r);
            }
        }
    }
#undef EUPHORIA_REGS
    if (vector) {
        mask(EXEC, r);
    }
    std::vector<Reg> unique;
    unique.reserve(r.size());
    for (const Reg reg : r) {
        if (std::ranges::find(unique, reg) == unique.end()) {
            unique.push_back(reg);
        }
    }
    return std::optional<Effects>{Effects{std::move(unique), std::move(w)}};
}

void link(std::vector<Block>& blocks)
{
    for (Block& block : blocks) {
        block.preds.clear();
    }
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        for (const std::size_t s : blocks[b].exit.successors()) {
            blocks[s].preds.push_back(b);
        }
    }
}

struct Park {
    bool save;
    std::uint8_t reg;
    std::uint8_t code;
    std::uint32_t lane;
    friend bool operator==(const Park&, const Park&) = default;
};

std::optional<Park> parked(const Instruction& inst, const Fields& fields)
{
    const Valu* v = fields.as<Valu>();
    if (v == nullptr) {
        return std::nullopt;
    }
    if (v->abs != 0 || v->neg != 0 || v->opsel != 0 || v->clamp || v->omod != 0 || v->sdwa || v->dpp) {
        return std::nullopt;
    }
    const auto lane = [](const Operand& op) -> std::optional<std::uint32_t> {
        const auto* c = op.as<Operand::Const>();
        if (c != nullptr && c->code >= 128 && c->code <= 191) {
            return static_cast<std::uint32_t>(c->code) - 128;
        }
        return std::nullopt;
    };
    const auto op = inst.name();
    if (!op || !v->dst || v->src.size() != 2) {
        return std::nullopt;
    }
    if (*op == "v_writelane_b32") {
        const auto* dst = v->dst->as<Operand::Vector>();
        const auto* src = v->src[0].as<Operand::Scalar>();
        if (dst != nullptr && src != nullptr && src->code < 105) {
            if (const auto k = lane(v->src[1])) {
                return Park{true, dst->reg, src->code, *k};
            }
        }
        return std::nullopt;
    }
    if (*op == "v_readlane_b32") {
        const auto* dst = v->dst->as<Operand::Scalar>();
        const auto* src = v->src[0].as<Operand::Vector>();
        if (dst != nullptr && src != nullptr && dst->code < 105) {
            if (const auto k = lane(v->src[1])) {
                return Park{false, src->reg, dst->code, *k};
            }
        }
    }
    return std::nullopt;
}

Edits unspill(std::vector<Instruction>& list, const std::vector<std::size_t>& leaders, bool wave64,
              std::uint8_t lanes, std::uint16_t vgprs)
{
    std::array<bool, 256> stores{};
    std::array<bool, 256> other{};
    std::array<bool, 256> used{};
    std::vector<std::optional<Park>> parks;
    std::vector<std::vector<std::uint8_t>> written;
    std::vector<std::vector<std::uint8_t>> read;
    parks.reserve(list.size());
    written.reserve(list.size());
    read.reserve(list.size());
    for (const Instruction& inst : list) {
        const auto op = inst.name();
        if (!op) {
            return {};
        }
        const Fields fields = inst.fields(wave64);
        const std::optional<Park> park = parked(inst, fields);
        const auto result = effects(inst, fields, *op, lanes, vgprs);
        if (!result) {
            return {};
        }
        std::vector<std::uint8_t> codes;
        std::vector<std::uint8_t> from;
        if (*result) {
            const auto& [reads, writes] = **result;
            for (const Reg reg : writes) {
                if (reg.is_s()) {
                    codes.push_back(reg.index);
                }
            }
            for (const Reg reg : reads) {
                if (reg.is_s()) {
                    from.push_back(reg.index);
                }
            }
            const bool relative = *op == "v_movrels_b32";
            const std::size_t n_reads = reads.size();
            const std::size_t total = reads.size() + writes.size();
            for (std::size_t k = 0; k < total; ++k) {
                const Reg reg = k < n_reads ? reads[k] : writes[k - n_reads];
                if (relative && k < n_reads && reg.is_v()) {
                    continue;
                }
                if (reg.is_s()) {
                    used[reg.index] = true;
                } else if (reg.is_v()) {
                    if (park && park->reg == reg.index) {
                        stores[reg.index] = true;
                    } else {
                        other[reg.index] = true;
                    }
                }
            }
        }
        parks.push_back(park);
        written.push_back(std::move(codes));
        read.push_back(std::move(from));
    }
    for (auto& park : parks) {
        if (park && other[park->reg]) {
            park.reset();
        }
    }
    if (std::ranges::all_of(parks, [](const auto& park) { return !park.has_value(); })) {
        return {};
    }
    const auto leader = [&](std::size_t i) {
        return std::binary_search(leaders.begin(), leaders.end(), list[i].offset);
    };
    std::vector<std::pair<std::uint8_t, std::uint32_t>> slots;
    for (const auto& park : parks) {
        if (park) {
            slots.emplace_back(park->reg, park->lane);
        }
    }
    std::ranges::sort(slots);
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    const auto has = [](const std::vector<std::uint8_t>& codes, unsigned code) {
        return std::ranges::find(codes, static_cast<std::uint8_t>(code)) != codes.end() && code < 256;
    };
    const auto partner = [&](std::size_t i) -> std::optional<std::size_t> {
        if (!parks[i]) {
            return std::nullopt;
        }
        const Park p = *parks[i];
        const unsigned code = p.code;
        if (p.save) {
            for (std::size_t j = i + 1; j < list.size() && !leader(j); ++j) {
                if (parks[j] == Park{true, p.reg, static_cast<std::uint8_t>(code + 1), p.lane + 1}) {
                    return j;
                }
                const bool touches = parks[j] && parks[j]->reg == p.reg
                                  && (parks[j]->lane == p.lane || parks[j]->lane == p.lane + 1);
                if (touches || has(written[j], code) || has(written[j], code + 1)) {
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }
        for (std::size_t j = i + 1; j < list.size() && !leader(j); ++j) {
            if (parks[j] == Park{false, p.reg, static_cast<std::uint8_t>(code + 1), p.lane + 1}) {
                return j;
            }
            const bool touches = parks[j] && parks[j]->save && parks[j]->reg == p.reg
                              && (parks[j]->lane == p.lane || parks[j]->lane == p.lane + 1);
            if (touches || has(written[j], code) || has(written[j], code + 1) || has(read[j], code + 1)) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    };
    const auto slot_in = [&](std::uint8_t reg, std::uint32_t lane) {
        return std::ranges::find(slots, std::pair{reg, lane}) != slots.end();
    };
    std::vector<std::pair<std::uint8_t, std::uint32_t>> pairs;
    for (const auto& [reg, lane] : slots) {
        const bool lower_taken = std::ranges::any_of(pairs, [&](const auto& pair) {
            return pair.first == reg && pair.second + 1 == lane;
        });
        if (!wave64 || lower_taken || !slot_in(reg, lane + 1)) {
            continue;
        }
        std::vector<std::size_t> lower;
        std::size_t upper = 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (parks[i] && parks[i]->reg == reg && parks[i]->lane == lane) {
                lower.push_back(i);
            }
            if (parks[i] && parks[i]->reg == reg && parks[i]->lane == lane + 1) {
                ++upper;
            }
        }
        bool all = true;
        std::vector<std::size_t> distinct;
        for (const std::size_t i : lower) {
            const auto found = partner(i);
            if (!found) {
                all = false;
            } else {
                distinct.push_back(*found);
            }
        }
        std::ranges::sort(distinct);
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        if (all && distinct.size() == lower.size() && upper == lower.size()) {
            pairs.emplace_back(reg, lane);
        }
    }

    const std::size_t past = PARKED;
    std::array<bool, 256> free{};
    for (std::size_t c = 0; c < 105; ++c) {
        free[c] = !used[c];
    }
    for (std::size_t c = 108; c <= 123; ++c) {
        free[c] = !used[c];
    }
    for (std::size_t c = past; c < 255; ++c) {
        free[c] = true;
    }
    std::vector<std::size_t> pair_order;
    for (std::size_t c = 108; c < 123; ++c) {
        pair_order.push_back(c);
    }
    for (std::size_t c = 104; c-- > 0;) {
        pair_order.push_back(c);
    }
    for (std::size_t c = past; c < 254; ++c) {
        pair_order.push_back(c);
    }
    std::vector<std::size_t> single_order;
    for (std::size_t c = 108; c <= 123; ++c) {
        single_order.push_back(c);
    }
    for (std::size_t c = 105; c-- > 0;) {
        single_order.push_back(c);
    }
    for (std::size_t c = past; c < 255; ++c) {
        single_order.push_back(c);
    }

    std::vector<std::pair<std::pair<std::uint8_t, std::uint32_t>, std::uint8_t>> codes;
    for (const auto& [reg, lane] : pairs) {
        const auto at = std::ranges::find_if(pair_order, [&](std::size_t c) { return free[c] && free[c + 1]; });
        if (at == pair_order.end()) {
            return {};
        }
        const std::size_t c = *at;
        free[c] = false;
        free[c + 1] = false;
        codes.push_back({{reg, lane}, static_cast<std::uint8_t>(c)});
        codes.push_back({{reg, lane + 1}, static_cast<std::uint8_t>(c + 1)});
    }
    for (const auto& slot : slots) {
        if (std::ranges::any_of(codes, [&](const auto& entry) { return entry.first == slot; })) {
            continue;
        }
        const auto at = std::ranges::find_if(single_order, [&](std::size_t c) { return free[c]; });
        if (at == single_order.end()) {
            return {};
        }
        free[*at] = false;
        codes.push_back({slot, static_cast<std::uint8_t>(*at)});
    }
    const auto code_of = [&](std::uint8_t reg, std::uint32_t lane) -> std::optional<std::uint8_t> {
        for (const auto& entry : codes) {
            if (entry.first == std::pair{reg, lane}) {
                return entry.second;
            }
        }
        return std::nullopt;
    };
    const auto is_pair = [&](std::uint8_t reg, std::uint32_t lane) {
        return std::ranges::find(pairs, std::pair{reg, lane}) != pairs.end();
    };
    const auto put = [&list](std::size_t i, Encoding encoding, std::uint16_t opcode, std::uint32_t word) {
        const std::size_t offset = list[i].offset;
        list[i] = Instruction{offset, encoding, opcode, {word}};
    };
    const auto field = [](std::uint8_t code) -> std::uint32_t { return code < PARKED ? code : 0u; };
    const auto mov = [&](bool wide, std::uint8_t dst, std::uint8_t src) {
        return 0xBE800000u | (field(dst) << 16) | ((3u + (wide ? 1u : 0u)) << 8) | field(src);
    };
    constexpr std::uint32_t kNop = 0xBF800000u;

    struct Edit {
        std::size_t at;
        bool move;
        bool wide;
        std::uint8_t dst;
        std::uint8_t src;
    };
    std::vector<Edit> edits;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (!parks[i]) {
            continue;
        }
        const Park p = *parks[i];
        const auto slot = code_of(p.reg, p.lane);
        if (!slot) {
            return {};
        }
        if (is_pair(p.reg, p.lane)) {
            const auto j = partner(i);
            if (!j) {
                return {};
            }
            const std::size_t at = p.save ? *j : i;
            const std::size_t gone = p.save ? i : *j;
            const std::uint8_t dst = p.save ? *slot : p.code;
            const std::uint8_t src = p.save ? p.code : *slot;
            edits.push_back({at, true, true, dst, src});
            edits.push_back({gone, false, false, 0, 0});
        } else if (p.lane > 0 && is_pair(p.reg, p.lane - 1)) {
        } else {
            const std::uint8_t dst = p.save ? *slot : p.code;
            const std::uint8_t src = p.save ? p.code : *slot;
            edits.push_back({i, true, false, dst, src});
        }
    }
    Edits moves;
    for (const Edit& edit : edits) {
        if (edit.move) {
            put(edit.at, Encoding::Sop1, static_cast<std::uint16_t>(3 + (edit.wide ? 1 : 0)),
                mov(edit.wide, edit.dst, edit.src));
            moves.emplace_back(edit.at, edit.dst, edit.src);
        } else {
            put(edit.at, Encoding::Sopp, 0, kNop);
        }
    }
    std::ranges::sort(moves);
    return moves;
}

std::pair<std::vector<RegSet>, std::vector<RegSet>> liveness(const std::vector<Block>& blocks,
                                                             const std::vector<std::size_t>& order)
{
    const RegSet empty{};
    std::vector<RegSet> uses(blocks.size(), empty);
    std::vector<RegSet> defs(blocks.size(), empty);
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        for (const Inst& inst : blocks[b].insts) {
            for (const auto& [reg, value] : inst.reads) {
                if (!contains(defs[b], reg)) {
                    insert(uses[b], reg);
                }
            }
            for (const auto& [reg, value] : inst.writes) {
                insert(defs[b], reg);
            }
        }
    }
    std::vector<RegSet> live_in(blocks.size(), empty);
    std::vector<RegSet> live_out(blocks.size(), empty);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const std::size_t b = *it;
            RegSet out = empty;
            for (const std::size_t s : blocks[b].exit.successors()) {
                for (std::size_t word = 0; word < out.size(); ++word) {
                    out[word] |= live_in[s][word];
                }
            }
            RegSet in = empty;
            for (std::size_t word = 0; word < in.size(); ++word) {
                in[word] = uses[b][word] | (out[word] & ~defs[b][word]);
            }
            if (in != live_in[b] || out != live_out[b]) {
                live_in[b] = in;
                live_out[b] = out;
                changed = true;
            }
        }
    }
    return {std::move(live_in), std::move(live_out)};
}

void remove_trivial_phis(std::vector<Block>& blocks, std::uint32_t values)
{
    std::vector<Value> replaced(values);
    for (std::uint32_t index = 0; index < values; ++index) {
        replaced[index] = Value{index};
    }
    const auto find = [&replaced](Value v) {
        while (replaced[v.id] != v) {
            v = replaced[v.id];
        }
        return v;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (Block& block : blocks) {
            std::vector<Phi> kept;
            kept.reserve(block.phis.size());
            for (Phi& phi : block.phis) {
                std::optional<Value> only;
                bool keep = false;
                for (const Value arg : phi.args) {
                    const Value a = find(arg);
                    if (a == phi.value || (only && *only == a)) {
                        continue;
                    }
                    if (only) {
                        keep = true;
                        break;
                    }
                    only = a;
                }
                if (keep || !only) {
                    kept.push_back(std::move(phi));
                    continue;
                }
                replaced[phi.value.id] = *only;
                changed = true;
            }
            block.phis = std::move(kept);
        }
    }
    for (Block& block : blocks) {
        for (Phi& phi : block.phis) {
            for (Value& a : phi.args) {
                a = find(a);
            }
        }
        for (Inst& inst : block.insts) {
            for (auto& [reg, value] : inst.reads) {
                value = find(value);
            }
        }
    }
}

Program rename(std::vector<Block> blocks, bool wave64, std::uint16_t vgprs)
{
    const std::vector<std::size_t> order = reverse_postorder(blocks);
    const auto [live_in, live_out] = liveness(blocks, order);
    std::uint32_t values = 0;
    const auto new_value = [&values] { return Value{values++}; };
    std::vector<std::pair<Reg, Value>> inputs;
    std::vector<std::vector<std::pair<Reg, Value>>> outs(blocks.size());
    std::vector<Value> current(kRegs, NONE);
    for (const std::size_t b : order) {
        std::ranges::fill(current, NONE);
        Block& block = blocks[b];
        for (const Reg reg : members(live_in[b])) {
            Value value;
            if (block.preds.empty()) {
                value = new_value();
                inputs.emplace_back(reg, value);
            } else if (block.preds.size() == 1) {
                value = lookup(outs[block.preds[0]], reg);
            } else {
                value = new_value();
                block.phis.push_back(Phi{reg, value, {}});
            }
            current[index_of(reg)] = value;
        }
        for (Inst& inst : block.insts) {
            for (auto& [reg, value] : inst.reads) {
                value = current[index_of(reg)];
            }
            for (auto& [reg, value] : inst.writes) {
                value = new_value();
                current[index_of(reg)] = value;
            }
        }
        for (const Reg reg : members(live_out[b])) {
            outs[b].emplace_back(reg, current[index_of(reg)]);
        }
    }
    for (Block& block : blocks) {
        for (Phi& phi : block.phis) {
            phi.args.clear();
            for (const std::size_t p : block.preds) {
                phi.args.push_back(lookup(outs[p], phi.reg));
            }
        }
    }
    remove_trivial_phis(blocks, values);
    return Program{wave64, vgprs, std::move(blocks), std::move(inputs)};
}

std::optional<std::pair<std::vector<bool>, std::vector<std::size_t>>> multi_entry_loop(
    const std::vector<Block>& blocks, const std::vector<bool>& inside)
{
    const auto reach = [&](std::size_t from, bool forward) {
        std::vector<bool> seen(blocks.size(), false);
        std::vector<std::size_t> stack{from};
        while (!stack.empty()) {
            const std::size_t b = stack.back();
            stack.pop_back();
            const std::vector<std::size_t> next = forward ? blocks[b].exit.successors() : blocks[b].preds;
            for (const std::size_t n : next) {
                if (inside[n] && !seen[n]) {
                    seen[n] = true;
                    stack.push_back(n);
                }
            }
        }
        return seen;
    };
    std::vector<bool> done(blocks.size(), false);
    for (std::size_t v = 0; v < blocks.size(); ++v) {
        if (!inside[v] || done[v]) {
            continue;
        }
        const std::vector<bool> ahead = reach(v, true);
        const std::vector<bool> behind = reach(v, false);
        std::vector<bool> region(blocks.size());
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            region[b] = ahead[b] && behind[b];
        }
        if (!region[v]) {
            continue;
        }
        std::vector<std::size_t> entries;
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            if (!region[b]) {
                continue;
            }
            done[b] = true;
            if (std::ranges::any_of(blocks[b].preds, [&](std::size_t p) { return !region[p]; })) {
                entries.push_back(b);
            }
        }
        if (entries.size() == 1) {
            std::vector<bool> nested = region;
            nested[entries[0]] = false;
            if (auto found = multi_entry_loop(blocks, nested)) {
                return found;
            }
        } else if (entries.size() > 1) {
            return std::pair{std::move(region), std::move(entries)};
        }
    }
    return std::nullopt;
}

void retarget(Exit& exit, const std::function<std::size_t(std::size_t)>& to)
{
    switch (exit.kind) {
    case Exit::Kind::Jump:
        exit.target = to(exit.target);
        break;
    case Exit::Kind::Branch:
        exit.taken = to(exit.taken);
        exit.not_taken = to(exit.not_taken);
        break;
    default:
        break;
    }
}

std::expected<void, ShaderError> split_irreducible(std::vector<Block>& blocks)
{
    const std::size_t limit = 4 * blocks.size() + 64;
    while (true) {
        const std::vector<std::size_t> order = reverse_postorder(blocks);
        std::vector<std::size_t> index(blocks.size(), kNoBlock);
        for (std::size_t i = 0; i < order.size(); ++i) {
            index[order[i]] = i;
        }
        const auto edge = irreducible_edge(blocks, order, index);
        if (!edge) {
            return {};
        }
        const std::size_t from = edge->first;
        auto found = multi_entry_loop(blocks, std::vector<bool>(blocks.size(), true));
        if (!found || blocks.size() >= limit) {
            return std::unexpected(ShaderError::Unsupported(blocks[from].offset, "irreducible control flow"));
        }
        std::vector<bool>& region = found->first;
        std::vector<std::size_t>& entries = found->second;
        std::ranges::stable_sort(entries, [&](std::size_t a, std::size_t b) { return index[a] < index[b]; });
        const std::size_t header = entries[0];
        const std::size_t entry = entries[1];

        std::vector<std::size_t> copy(blocks.size(), kNoBlock);
        std::vector<std::size_t> copied;
        std::vector<std::size_t> stack{entry};
        while (!stack.empty()) {
            const std::size_t b = stack.back();
            stack.pop_back();
            if (copy[b] != kNoBlock) {
                continue;
            }
            copy[b] = blocks.size() + copied.size();
            copied.push_back(b);
            for (const std::size_t s : blocks[b].exit.successors()) {
                if (region[s] && s != header) {
                    stack.push_back(s);
                }
            }
        }
        for (const std::size_t b : copied) {
            Block block = blocks[b];
            retarget(block.exit, [&](std::size_t s) { return copy[s] == kNoBlock ? s : copy[s]; });
            blocks.push_back(std::move(block));
        }
        std::vector<std::size_t> outside;
        for (const std::size_t p : blocks[entry].preds) {
            if (!region[p]) {
                outside.push_back(p);
            }
        }
        for (const std::size_t p : outside) {
            retarget(blocks[p].exit, [&](std::size_t s) { return s == entry ? copy[entry] : s; });
        }
        link(blocks);
    }
}

bool ends_program(const Program& p, std::size_t b)
{
    return p.blocks[b].exit == Exit::End()
        && std::ranges::all_of(p.blocks[b].insts, [](const Inst& inst) { return inst.fields.is<Fields::Sopp>(); });
}

std::optional<Program> relink(std::vector<Block> blocks, const Program& p)
{
    for (Block& block : blocks) {
        block.phis.clear();
    }
    link(blocks);
    if (!split_irreducible(blocks)) {
        return std::nullopt;
    }
    return rename(std::move(blocks), p.wave64, p.vgprs);
}

}

std::vector<std::size_t> Exit::successors() const
{
    switch (kind) {
    case Kind::Jump:
        return {target};
    case Kind::Branch:
        return {taken, not_taken};
    default:
        return {};
    }
}

std::optional<Value> Inst::read(Reg reg) const noexcept
{
    for (const auto& [r, value] : reads) {
        if (r == reg) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<Value> Inst::write(Reg reg) const noexcept
{
    for (const auto& [r, value] : writes) {
        if (r == reg) {
            return value;
        }
    }
    return std::nullopt;
}

std::expected<Program, ShaderError> build(std::span<const std::uint8_t> code, bool wave64, std::uint16_t vgprs)
{
    auto decoded = decode(code);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    std::vector<Instruction> list = std::move(*decoded);
    const std::uint8_t lanes = wave64 ? 2 : 1;

    std::vector<Flow> flows;
    flows.reserve(list.size());
    std::vector<std::size_t> leaders{0};
    const Before before = [&list](std::size_t at) -> const Instruction* {
        const auto it = std::partition_point(list.begin(), list.end(),
                                             [at](const Instruction& inst) { return inst.offset < at; });
        return it == list.begin() ? nullptr : &*std::prev(it);
    };
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto flow = control(list[i], code.size(), before);
        if (!flow) {
            return std::unexpected(flow.error());
        }
        if (flow->has_target()) {
            leaders.push_back(flow->target);
        }
        if (flow->kind != Flow::Kind::Next && i + 1 < list.size()) {
            leaders.push_back(list[i + 1].offset);
        }
        flows.push_back(*flow);
    }
    std::ranges::sort(leaders);
    leaders.erase(std::unique(leaders.begin(), leaders.end()), leaders.end());
    const Edits parked_moves = unspill(list, leaders, wave64, lanes, vgprs);
    const auto block_at = [&leaders](std::size_t offset) -> std::expected<std::size_t, ShaderError> {
        const auto it = std::lower_bound(leaders.begin(), leaders.end(), offset);
        if (it == leaders.end() || *it != offset) {
            return std::unexpected(ShaderError::Isa(offset, "jump into a block"));
        }
        return static_cast<std::size_t>(it - leaders.begin());
    };

    std::vector<Block> blocks;
    blocks.reserve(leaders.size());
    for (const std::size_t offset : leaders) {
        Block block;
        block.offset = offset;
        block.exit = Exit::End();
        blocks.push_back(std::move(block));
    }
    std::vector<std::optional<std::size_t>> next_offsets(list.size());
    for (std::size_t i = 0; i + 1 < list.size(); ++i) {
        next_offsets[i] = list[i + 1].offset;
    }
    for (std::size_t n = 0; n < list.size(); ++n) {
        Instruction& machine = list[n];
        const Flow flow = flows[n];
        const std::optional<std::size_t> next = next_offsets[n];
        const std::size_t b =
            static_cast<std::size_t>(std::upper_bound(leaders.begin(), leaders.end(), machine.offset) - leaders.begin()) - 1;
        const auto op = machine.name();
        if (!op) {
            return std::unexpected(ShaderError::Unsupported(machine.offset, "unknown opcode"));
        }
        const std::string_view name = *op;
        Fields fields = machine.fields(wave64);
        const auto parked = std::lower_bound(parked_moves.begin(), parked_moves.end(), n,
                                             [](const auto& entry, std::size_t value) {
                                                 return std::get<0>(entry) < value;
                                             });
        if (parked != parked_moves.end() && std::get<0>(*parked) == n) {
            if (auto* sop = fields.as<Fields::Sop>(); sop != nullptr && sop->dst && !sop->src.empty()) {
                auto* to = sop->dst->as<Operand::Scalar>();
                auto* from = sop->src.front().as<Operand::Scalar>();
                if (to != nullptr && from != nullptr) {
                    to->code = std::get<1>(*parked);
                    from->code = std::get<2>(*parked);
                }
            }
        }
        const auto fallthrough = [&]() -> std::expected<std::size_t, ShaderError> {
            if (!next) {
                return std::unexpected(ShaderError::Isa(machine.offset, "falls off the code"));
            }
            return block_at(*next);
        };
        const bool ends = flow.kind != Flow::Kind::Next || !next
                       || std::binary_search(leaders.begin(), leaders.end(), *next);
        if (ends) {
            switch (flow.kind) {
            case Flow::Kind::Next: {
                const auto to = fallthrough();
                if (!to) {
                    return std::unexpected(to.error());
                }
                blocks[b].exit = Exit::Jump(*to);
                break;
            }
            case Flow::Kind::Jump: {
                const auto to = block_at(flow.target);
                if (!to) {
                    return std::unexpected(to.error());
                }
                blocks[b].exit = Exit::Jump(*to);
                break;
            }
            case Flow::Kind::Branch: {
                const auto taken = block_at(flow.target);
                if (!taken) {
                    return std::unexpected(taken.error());
                }
                const auto not_taken = fallthrough();
                if (!not_taken) {
                    return std::unexpected(not_taken.error());
                }
                blocks[b].exit = Exit::Branch(*taken, *not_taken);
                break;
            }
            case Flow::Kind::Stop:
                blocks[b].exit = name == "s_setpc_b64" ? Exit::Indirect() : Exit::End();
                break;
            }
        }
        auto result = effects(machine, fields, name, lanes, vgprs);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result) {
            auto& [reads, writes] = **result;
            for (const Reg reg : writes) {
                if (reg.is_v() && std::ranges::find(reads, reg) == reads.end()) {
                    reads.push_back(reg);
                }
            }
            Inst inst;
            inst.machine = std::move(machine);
            inst.fields = std::move(fields);
            for (const Reg reg : reads) {
                inst.reads.emplace_back(reg, NONE);
            }
            for (const Reg reg : writes) {
                inst.writes.emplace_back(reg, NONE);
            }
            blocks[b].insts.push_back(std::move(inst));
        }
    }
    link(blocks);
    if (!blocks[0].preds.empty()) {
        return std::unexpected(ShaderError::Unsupported(0, "a branch to the program entry"));
    }
    if (auto split = split_irreducible(blocks); !split) {
        return std::unexpected(split.error());
    }
    return rename(std::move(blocks), wave64, vgprs);
}

std::expected<Program, ShaderError> thread_edges(const Program& p,
                                                 std::span<const std::tuple<std::size_t, std::size_t, std::size_t>> edges)
{
    std::vector<Block> blocks = p.blocks;
    for (Block& block : blocks) {
        block.phis.clear();
    }
    for (const auto& [pred, join, dest] : edges) {
        Block copy = blocks[join];
        copy.exit = Exit::Jump(dest);
        const std::size_t target = blocks.size();
        Exit& exit = blocks[pred].exit;
        if (exit.kind == Exit::Kind::Jump) {
            exit.target = target;
        } else if (exit.kind == Exit::Kind::Branch) {
            if (exit.taken == join) {
                exit.taken = target;
            }
            if (exit.not_taken == join) {
                exit.not_taken = target;
            }
        }
        blocks.push_back(std::move(copy));
    }
    link(blocks);
    if (auto split = split_irreducible(blocks); !split) {
        return std::unexpected(split.error());
    }
    return rename(std::move(blocks), p.wave64, p.vgprs);
}

std::optional<Program> without_early_outs(const Program& p)
{
    std::vector<Block> blocks = p.blocks;
    bool any = false;
    for (Block& block : blocks) {
        if (block.exit.kind != Exit::Kind::Branch) {
            continue;
        }
        const std::size_t taken = block.exit.taken;
        const std::size_t not_taken = block.exit.not_taken;
        if (ends_program(p, taken) && !block.insts.empty() && block.insts.back().name() == "s_cbranch_execz") {
            block.insts.pop_back();
            block.exit = Exit::Jump(not_taken);
            any = true;
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return relink(std::move(blocks), p);
}

std::optional<Program> without_skips(const Program& p, bool deep)
{
    const std::size_t n = p.blocks.size();
    std::unordered_map<Value, Value> copy;
    std::unordered_set<Value> live;
    const auto idle = [](const Inst& inst, const std::unordered_set<Value>& live_values) {
        const std::string_view name = inst.name();
        return inst.fields.is<Fields::Sop>()
            && !(name.starts_with("s_setpc") || name.starts_with("s_swappc") || name.starts_with("s_call")
                 || name.starts_with("s_rfe") || name.starts_with("s_movrel") || name.starts_with("s_subvector"))
            && std::ranges::all_of(inst.writes, [&](const auto& w) { return !live_values.contains(w.second); });
    };
    for (const Block& block : p.blocks) {
        for (const Inst& inst : block.insts) {
            const std::string_view name = inst.name();
            if ((name == "s_mov_b32" || name == "s_mov_b64") && inst.reads.size() == inst.writes.size()
                && std::ranges::all_of(inst.reads, [](const auto& r) { return r.first.is_s(); })
                && std::ranges::all_of(inst.writes, [](const auto& w) { return w.first.is_s(); })) {
                for (std::size_t k = 0; k < inst.writes.size(); ++k) {
                    copy[inst.writes[k].second] = inst.reads[k].second;
                }
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Block& block : p.blocks) {
            for (const Inst& inst : block.insts) {
                if (!idle(inst, live)) {
                    for (const auto& r : inst.reads) {
                        changed |= live.insert(r.second).second;
                    }
                }
            }
            for (const Phi& phi : block.phis) {
                if (live.contains(phi.value)) {
                    for (const Value a : phi.args) {
                        changed |= live.insert(a).second;
                    }
                }
            }
        }
    }
    const auto root = [&](Value v) {
        while (deep) {
            const auto it = copy.find(v);
            if (it == copy.end()) {
                break;
            }
            v = it->second;
        }
        return v;
    };
    const auto skipped = [&](std::size_t b, std::size_t target, std::size_t first) -> bool {
        std::vector<bool> inside(n, false);
        std::vector<std::size_t> stack{first};
        while (!stack.empty()) {
            const std::size_t x = stack.back();
            stack.pop_back();
            if (x == target || inside[x]) {
                continue;
            }
            if (x == b) {
                return false;
            }
            inside[x] = true;
            const Exit& exit = p.blocks[x].exit;
            if (exit.kind == Exit::Kind::End || exit.kind == Exit::Kind::Indirect) {
                return false;
            }
            for (const std::size_t s : exit.successors()) {
                stack.push_back(s);
            }
        }
        for (std::size_t x = 0; x < n; ++x) {
            if (!inside[x]) {
                continue;
            }
            if (std::ranges::any_of(p.blocks[x].preds, [&](std::size_t q) { return q != b && !inside[q]; })) {
                return false;
            }
        }
        const Block& join = p.blocks[target];
        for (const Phi& phi : join.phis) {
            bool scalar = false;
            if (phi.reg.is_s()) {
                scalar = phi.reg.index != 126 && phi.reg.index != 127;
            } else if (phi.reg.is_scc()) {
                scalar = true;
            }
            if (!scalar || (deep && !live.contains(phi.value))) {
                continue;
            }
            std::vector<Value> before;
            for (std::size_t k = 0; k < join.preds.size() && k < phi.args.size(); ++k) {
                if (join.preds[k] == b) {
                    before.push_back(root(phi.args[k]));
                }
            }
            for (std::size_t k = 0; k < join.preds.size() && k < phi.args.size(); ++k) {
                if (inside[join.preds[k]] && std::ranges::find(before, root(phi.args[k])) == before.end()) {
                    return false;
                }
            }
        }
        if (!deep && std::ranges::any_of(join.preds, [&](std::size_t q) { return q != b && !inside[q]; })) {
            return false;
        }
        return true;
    };
    std::vector<Block> blocks = p.blocks;
    bool any = false;
    for (std::size_t b = 0; b < n; ++b) {
        const Exit& exit = p.blocks[b].exit;
        if (exit.kind != Exit::Kind::Branch) {
            continue;
        }
        if (p.blocks[b].insts.empty() || p.blocks[b].insts.back().name() != "s_cbranch_execz") {
            continue;
        }
        if (ends_program(p, exit.taken) || skipped(b, exit.taken, exit.not_taken)) {
            blocks[b].insts.pop_back();
            blocks[b].exit = Exit::Jump(exit.not_taken);
            any = true;
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return relink(std::move(blocks), p);
}

std::vector<Reg> operand_reads(const Program& p, const Inst& inst)
{
    const std::uint8_t lanes = p.wave64 ? 2 : 1;
    const auto result = effects(inst.machine, inst.fields, inst.name(), lanes, p.vgprs);
    if (!result || !*result) {
        return {};
    }
    return (**result).first;
}

std::vector<std::size_t> reverse_postorder(std::span<const Block> blocks)
{
    std::vector<std::size_t> post;
    if (blocks.empty()) {
        return post;
    }
    std::vector<bool> seen(blocks.size(), false);
    post.reserve(blocks.size());
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, 0}};
    seen[0] = true;
    while (!stack.empty()) {
        auto& top = stack.back();
        const std::size_t b = top.first;
        const std::size_t i = top.second;
        ++top.second;
        const std::vector<std::size_t> successors = blocks[b].exit.successors();
        if (i < successors.size()) {
            const std::size_t s = successors[i];
            if (!seen[s]) {
                seen[s] = true;
                stack.emplace_back(s, 0);
            }
        } else {
            post.push_back(b);
            stack.pop_back();
        }
    }
    std::ranges::reverse(post);
    return post;
}

std::optional<std::pair<std::size_t, std::size_t>> irreducible_edge(std::span<const Block> blocks,
                                                                    std::span<const std::size_t> order,
                                                                    std::span<const std::size_t> index)
{
    if (order.empty()) {
        return std::nullopt;
    }
    const std::size_t entry = order.front();
    const std::vector<std::size_t> idom = idoms(blocks, order, index);
    for (const std::size_t b : order) {
        for (const std::size_t s : blocks[b].exit.successors()) {
            if (index[s] > index[b]) {
                continue;
            }
            std::size_t d = b;
            while (d != s && d != entry) {
                d = idom[d];
            }
            if (d != s) {
                return std::pair{b, s};
            }
        }
    }
    return std::nullopt;
}

std::vector<std::size_t> idoms(std::span<const Block> blocks, std::span<const std::size_t> order,
                               std::span<const std::size_t> index)
{
    std::vector<std::size_t> idom(blocks.size(), kNoBlock);
    if (order.empty()) {
        return idom;
    }
    const std::size_t entry = order.front();
    idom[entry] = entry;
    const auto intersect = [&](std::size_t a, std::size_t b) {
        while (a != b) {
            while (index[a] > index[b]) {
                a = idom[a];
            }
            while (index[b] > index[a]) {
                b = idom[b];
            }
        }
        return a;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t k = 1; k < order.size(); ++k) {
            const std::size_t b = order[k];
            std::size_t next = kNoBlock;
            for (const std::size_t q : blocks[b].preds) {
                if (idom[q] == kNoBlock) {
                    continue;
                }
                next = next == kNoBlock ? q : intersect(q, next);
            }
            if (idom[b] != next) {
                idom[b] = next;
                changed = true;
            }
        }
    }
    return idom;
}

}
