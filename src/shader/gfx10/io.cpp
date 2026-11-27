#include <euphoria/shader/gfx10/io.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::io {
namespace {

struct PsVgpr {
    std::uint8_t count;
    std::optional<Barycentric> kind;
};

constexpr std::array<PsVgpr, 16> kPsVgprs{{
    {2, Barycentric::PerspectiveSample},
    {2, Barycentric::PerspectiveCenter},
    {2, Barycentric::PerspectiveCentroid},
    {3, Barycentric::PerspectivePullModel},
    {2, Barycentric::LinearSample},
    {2, Barycentric::LinearCenter},
    {2, Barycentric::LinearCentroid},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
}};

Input ps_system(std::size_t bit)
{
    switch (bit) {
    case 7:
        return Input::Simple(Input::Kind::LineStipple);
    case 8:
    case 9:
    case 10:
    case 11:
        return Input::Position(static_cast<std::uint8_t>(bit - 8));
    case 12:
        return Input::Simple(Input::Kind::FrontFace);
    case 13:
        return Input::Simple(Input::Kind::Ancillary);
    case 14:
        return Input::Simple(Input::Kind::SampleCoverage);
    default:
        return Input::Simple(Input::Kind::FixedPosition);
    }
}

std::vector<std::pair<std::uint8_t, Input>> ps_vgprs(std::uint32_t address)
{
    std::vector<std::pair<std::uint8_t, Input>> out;
    std::uint8_t next = 0;
    for (std::size_t bit = 0; bit < kPsVgprs.size(); ++bit) {
        if (((address >> bit) & 1u) == 0) {
            continue;
        }
        const PsVgpr& entry = kPsVgprs[bit];
        for (std::uint8_t index = 0; index < entry.count; ++index) {
            const Input input = entry.kind
                                    ? Input::BarycentricInput(*entry.kind, index)
                                    : ps_system(bit);
            out.push_back({static_cast<std::uint8_t>(next + index), input});
        }
        next = static_cast<std::uint8_t>(next + entry.count);
    }
    return out;
}

struct Pair {
    std::optional<Barycentric> barycentric;
    std::vector<Site> first;
};

Interp with_attribute(const Pair& pair, const Attribute& attribute)
{
    Interp interp{};
    interp.attribute = attribute;
    interp.first = pair.first;
    if (pair.barycentric) {
        interp.kind = Interp::Kind::Interpolated;
        interp.barycentric = *pair.barycentric;
    } else {
        interp.kind = Interp::Kind::Weighted;
    }
    return interp;
}

void push_pair(Io& io, Site site, const Pair& pair, const Attribute& attribute)
{
    if (!pair.barycentric) {
        for (const Site& first : pair.first) {
            bool present = false;
            for (const auto& entry : io.interps) {
                if (entry.first == first) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                Interp weighted{};
                weighted.kind = Interp::Kind::Weighted;
                weighted.attribute = attribute;
                io.interps.push_back({first, weighted});
            }
        }
    }
    io.interps.push_back({site, with_attribute(pair, attribute)});
}

struct Halves {
    const ir::Program* program{nullptr};
    const std::unordered_map<ir::Value, Site>* defs{nullptr};
    const std::unordered_map<ir::Value, std::pair<std::size_t, std::size_t>>* phis{nullptr};

    std::expected<void, const char*> sites(ir::Value value, std::vector<ir::Value>& seen,
                                           std::vector<Site>& out) const
    {
        const auto def = defs->find(value);
        if (def != defs->end()) {
            if (std::find(out.begin(), out.end(), def->second) == out.end()) {
                out.push_back(def->second);
            }
            return {};
        }
        const auto phi = phis->find(value);
        if (phi == phis->end()) {
            return std::unexpected("an interpolation without its first half");
        }
        const auto [b, i] = phi->second;
        if (std::find(seen.begin(), seen.end(), value) == seen.end()) {
            seen.push_back(value);
            for (const ir::Value arg : program->blocks[b].phis[i].args) {
                if (auto traced = sites(arg, seen, out); !traced) {
                    return traced;
                }
            }
        }
        return {};
    }

    template <typename BarycentricOf>
    std::expected<Pair, const char*> pair(const Operand& attr, std::optional<ir::Value> accumulator,
                                          std::optional<ir::Value> j,
                                          BarycentricOf barycentric) const
    {
        std::vector<Site> first;
        if (!accumulator) {
            return std::unexpected("an interpolation without its first half");
        }
        std::vector<ir::Value> seen;
        if (auto traced = sites(*accumulator, seen, first); !traced) {
            return std::unexpected(traced.error());
        }
        std::optional<Barycentric> kind;
        if (const auto found = barycentric(j); found && found->second == 1) {
            kind = found->first;
        }
        for (const Site& site : first) {
            const ir::Inst& inst = program->blocks[site.first].insts[site.second];
            const Operand* half_attr = nullptr;
            std::optional<ir::Value> i;
            if (const auto* fields = inst.fields.as<Fields::Vintrp>()) {
                if (inst.name() != "v_interp_p1_f32") {
                    return std::unexpected("an interpolation without its first half");
                }
                const auto* source = fields->src.as<Operand::Vector>();
                if (source == nullptr) {
                    return std::unexpected("an interpolation without its first half");
                }
                half_attr = &fields->attr;
                i = inst.read(ir::Reg::V(source->reg));
            } else if (const auto* fields = inst.fields.as<Fields::Valu>()) {
                if (inst.name() != "v_interp_p1ll_f16" || fields->src.size() < 2) {
                    return std::unexpected("an interpolation without its first half");
                }
                const auto* source = fields->src[1].as<Operand::Vector>();
                if (source == nullptr) {
                    return std::unexpected("an interpolation without its first half");
                }
                half_attr = &fields->src[0];
                i = inst.read(ir::Reg::V(source->reg));
            } else {
                return std::unexpected("an interpolation without its first half");
            }
            if (!(*half_attr == attr)) {
                return std::unexpected("interpolation halves of different attributes");
            }
            if (kind) {
                const auto found = barycentric(i);
                if (!found || found->first != *kind || found->second != 0) {
                    kind = std::nullopt;
                }
            }
        }
        Pair result{};
        result.barycentric = kind;
        result.first = std::move(first);
        return result;
    }
};

std::uint32_t offset_of(const ir::Inst& inst)
{
    return inst.machine.offset;
}

}

std::expected<Io, ShaderError> raise(const ir::Program& program,
                                     const native::NativeProgram& source)
{
    std::vector<std::pair<ir::Reg, Input>> loaded;
    const std::vector<native::PsInput>* ps_inputs = nullptr;
    static const std::vector<native::VsOutput> no_outputs;
    const std::vector<native::VsOutput>* params = &no_outputs;

    if (const auto* ps = std::get_if<native::PsIo>(&source.io);
        ps != nullptr && source.stage == native::HwStage::Ps) {
        for (const auto& [reg, input] : ps_vgprs(ps->input_addr)) {
            loaded.push_back({ir::Reg::V(reg), input});
        }
        ps_inputs = &ps->inputs;
    } else if (const auto* vs = std::get_if<native::VsIo>(&source.io);
               vs != nullptr && source.stage == native::HwStage::Vs) {
        loaded.push_back({ir::Reg::V(0), Input::Simple(Input::Kind::VertexId)});
        loaded.push_back({ir::Reg::V(2), Input::Simple(Input::Kind::PrimitiveId)});
        loaded.push_back({ir::Reg::V(3), Input::Simple(Input::Kind::InstanceId)});
        for (const native::VsInput& input : vs->inputs) {
            for (std::uint8_t channel = 0; channel < 4; ++channel) {
                if (((input.mask >> channel) & 1u) == 0) {
                    continue;
                }
                loaded.push_back({ir::Reg::V(static_cast<std::uint8_t>(input.vgpr + channel)),
                                  Input::Attribute(input.row, channel)});
            }
        }
        params = &vs->outputs;
    } else if (const auto* cs = std::get_if<native::ComputeIo>(&source.io);
               cs != nullptr && source.stage == native::HwStage::Cs) {
        const auto [ids, info] = cs->system_sgprs();
        const std::uint8_t thread_vgprs = cs->thread_id_vgprs();
        for (std::uint8_t c = 0; c < thread_vgprs; ++c) {
            loaded.push_back({ir::Reg::V(c), Input::ThreadIdInGroup(c)});
        }
        for (std::size_t c = 0; c < ids.size(); ++c) {
            if (ids[c]) {
                loaded.push_back(
                    {ir::Reg::S(*ids[c]), Input::GroupId(static_cast<std::uint8_t>(c))});
            }
        }
        if (info) {
            loaded.push_back({ir::Reg::S(*info), Input::Simple(Input::Kind::GroupInfo)});
        }
    } else if (const auto* ms = std::get_if<native::MeshIo>(&source.io);
               ms != nullptr && source.stage == native::HwStage::Ms) {
        std::vector<ir::Value> group;
        for (const auto& input : program.inputs) {
            if (input.first == ir::Reg::V(5)) {
                group.push_back(input.second);
            }
        }
        const auto no_lanes = [&program](std::size_t q, std::size_t b) {
            const ir::Block& block = program.blocks[q];
            if (block.insts.empty() || block.exit.kind != ir::Exit::Kind::Branch) {
                return false;
            }
            const std::string_view name = block.insts.back().name();
            if (name == "s_cbranch_execz") {
                return block.exit.taken == b && block.exit.not_taken != b;
            }
            if (name == "s_cbranch_execnz") {
                return block.exit.not_taken == b && block.exit.taken != b;
            }
            return false;
        };
        bool added = true;
        while (added) {
            added = false;
            for (std::size_t b = 0; b < program.blocks.size() && !added; ++b) {
                const ir::Block& block = program.blocks[b];
                for (const ir::Phi& phi : block.phis) {
                    bool carried = false;
                    for (std::size_t n = 0; n < phi.args.size() && n < block.preds.size(); ++n) {
                        if (std::find(group.begin(), group.end(), phi.args[n]) != group.end()
                            && !no_lanes(block.preds[n], b)) {
                            carried = true;
                            break;
                        }
                    }
                    if (carried
                        && std::find(group.begin(), group.end(), phi.value) == group.end()) {
                        group.push_back(phi.value);
                        added = true;
                        break;
                    }
                }
            }
        }
        for (const ir::Block& block : program.blocks) {
            for (const ir::Inst& inst : block.insts) {
                bool reads_group = false;
                for (const auto& read : inst.reads) {
                    if (read.first == ir::Reg::V(5)
                        && std::find(group.begin(), group.end(), read.second) != group.end()) {
                        reads_group = true;
                        break;
                    }
                }
                if (!reads_group || inst.name() == "v_readfirstlane_b32") {
                    continue;
                }
                bool per_lane = !inst.write(ir::Reg::V(5)).has_value();
                for (const ir::Reg reg : ir::operand_reads(program, inst)) {
                    if (reg == ir::Reg::V(5)) {
                        per_lane = true;
                    }
                }
                if (per_lane) {
                    return std::unexpected(ShaderError::Unsupported(
                        offset_of(inst), "the mesh group input read per lane"));
                }
            }
        }
        loaded.push_back({ir::Reg::V(5), Input::Simple(Input::Kind::MeshGroup)});
        loaded.push_back({ir::Reg::S(3), Input::Simple(Input::Kind::MergedWaveInfo)});
        params = &ms->outputs;
    } else {
        return std::unexpected(ShaderError::Unsupported(0, "I/O of this stage"));
    }

    std::vector<std::pair<ir::Value, Input>> inputs;
    for (const auto& input : program.inputs) {
        for (const auto& entry : loaded) {
            if (entry.first == input.first) {
                inputs.push_back({input.second, entry.second});
                break;
            }
        }
    }
    const auto input_of = [&inputs](ir::Value value) -> std::optional<Input> {
        for (const auto& entry : inputs) {
            if (entry.first == value) {
                return entry.second;
            }
        }
        return std::nullopt;
    };
    const auto attribute = [&](const ir::Inst& inst,
                               const Operand& op) -> std::expected<Attribute, ShaderError> {
        const auto* attr = op.as<Operand::Attr>();
        if (attr == nullptr) {
            return std::unexpected(
                ShaderError::Unsupported(offset_of(inst), "an interpolation operand"));
        }
        for (const native::PsInput& input : *ps_inputs) {
            if (input.attr == attr->index) {
                return Attribute{input.usage, input.row, attr->chan, attr->high};
            }
        }
        return std::unexpected(ShaderError::Unsupported(
            offset_of(inst), "an interpolated attribute the stage data does not list"));
    };
    const auto barycentric = [&input_of](std::optional<ir::Value> value)
        -> std::optional<std::pair<Barycentric, std::uint8_t>> {
        if (!value) {
            return std::nullopt;
        }
        const auto found = input_of(*value);
        if (!found || found->kind != Input::Kind::BarycentricValue) {
            return std::nullopt;
        }
        return std::pair{found->barycentric, found->index};
    };

    Io result{};
    result.inputs = inputs;
    if (const auto* cs = std::get_if<native::ComputeIo>(&source.io)) {
        result.initialized_sgprs = cs->initialized_sgprs();
    }

    std::unordered_map<ir::Value, Site> defs;
    std::unordered_map<ir::Value, std::pair<std::size_t, std::size_t>> phis;
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const ir::Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            for (const auto& write : block.insts[i].writes) {
                defs.emplace(write.second, Site{b, i});
            }
        }
        for (std::size_t i = 0; i < block.phis.size(); ++i) {
            phis.emplace(block.phis[i].value, std::pair{b, i});
        }
    }
    const Halves halves{&program, &defs, &phis};

    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const ir::Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const ir::Inst& inst = block.insts[i];
            const Site site{b, i};
            const std::uint32_t at = offset_of(inst);
            const std::string_view name = inst.name();
            if (const auto* fields = inst.fields.as<Fields::Vintrp>()) {
                auto attr = attribute(inst, fields->attr);
                if (!attr) {
                    return std::unexpected(attr.error());
                }
                const auto* source_vector = fields->src.as<Operand::Vector>();
                if (name == "v_interp_mov_f32") {
                    const auto* vertex = fields->src.as<Operand::Const>();
                    if (vertex == nullptr) {
                        return std::unexpected(
                            ShaderError::Unsupported(at, "an interpolation form"));
                    }
                    Interp interp{};
                    interp.kind = Interp::Kind::Vertex;
                    interp.attribute = *attr;
                    interp.vertex = vertex->code;
                    result.interps.push_back({site, interp});
                } else if (name == "v_interp_p1_f32") {
                } else if (name == "v_interp_p2_f32" && source_vector != nullptr) {
                    const auto* destination = fields->dst.as<Operand::Vector>();
                    if (destination == nullptr) {
                        return std::unexpected(
                            ShaderError::Unsupported(at, "an interpolation operand"));
                    }
                    const auto pair = halves.pair(
                        fields->attr, inst.read(ir::Reg::V(destination->reg)),
                        inst.read(ir::Reg::V(source_vector->reg)), barycentric);
                    if (!pair) {
                        return std::unexpected(ShaderError::Unsupported(at, pair.error()));
                    }
                    push_pair(result, site, *pair, *attr);
                } else {
                    return std::unexpected(ShaderError::Unsupported(at, "an interpolation form"));
                }
                continue;
            }
            if (const auto* fields = inst.fields.as<Fields::Valu>();
                fields != nullptr && name.starts_with("v_interp_")) {
                if (name == "v_interp_p1ll_f16") {
                    continue;
                }
                if (name != "v_interp_p2_f16" || fields->src.empty()) {
                    return std::unexpected(ShaderError::Unsupported(at, "an interpolation form"));
                }
                auto attr = attribute(inst, fields->src[0]);
                if (!attr) {
                    return std::unexpected(attr.error());
                }
                const auto read = [&](std::size_t k) -> std::optional<ir::Value> {
                    if (k >= fields->src.size()) {
                        return std::nullopt;
                    }
                    const auto* vector = fields->src[k].as<Operand::Vector>();
                    if (vector == nullptr) {
                        return std::nullopt;
                    }
                    return inst.read(ir::Reg::V(vector->reg));
                };
                const auto pair =
                    halves.pair(fields->src[0], read(2), read(1), barycentric);
                if (!pair) {
                    return std::unexpected(ShaderError::Unsupported(at, pair.error()));
                }
                push_pair(result, site, *pair, *attr);
                continue;
            }
            if (const auto* fields = inst.fields.as<Fields::Exp>()) {
                Target target{};
                const std::uint32_t value_target = fields->target;
                if (value_target <= 7) {
                    target.kind = Target::Kind::Color;
                    target.index = static_cast<std::uint8_t>(value_target);
                } else if (value_target == 8) {
                    target.kind = Target::Kind::Depth;
                } else if (value_target == 9) {
                    target.kind = Target::Kind::Null;
                } else if (value_target >= 12 && value_target <= 16) {
                    target.kind = Target::Kind::Position;
                    target.index = static_cast<std::uint8_t>(value_target - 12);
                } else if (value_target == 20) {
                    target.kind = Target::Kind::Primitive;
                } else if (value_target == 32 && params->empty()) {
                    target.kind = Target::Kind::Unread;
                } else if (value_target >= 32 && value_target <= 63) {
                    const auto found = std::find_if(
                        params->begin(), params->end(), [&](const native::VsOutput& output) {
                            return (output.param & 0x7F) == value_target - 32;
                        });
                    if (found == params->end()) {
                        return std::unexpected(ShaderError::Unsupported(
                            at, "a parameter export the stage data does not list"));
                    }
                    target.usage = found->usage;
                    target.row = found->row;
                    target.kind = (found->param & 0x80) != 0 ? Target::Kind::PrimitiveParam
                                                             : Target::Kind::Param;
                } else {
                    return std::unexpected(ShaderError::Unsupported(at, "an export target"));
                }
                Export entry{};
                entry.site = site;
                entry.target = target;
                for (std::size_t k = 0; k < fields->src.size() && k < 4; ++k) {
                    if (!fields->src[k]) {
                        continue;
                    }
                    const auto* vector = fields->src[k]->as<Operand::Vector>();
                    if (vector != nullptr) {
                        entry.values[k] = inst.read(ir::Reg::V(vector->reg));
                    }
                }
                entry.compressed = fields->compr;
                entry.done = fields->done;
                result.exports.push_back(std::move(entry));
            }
        }
    }
    return result;
}

}
