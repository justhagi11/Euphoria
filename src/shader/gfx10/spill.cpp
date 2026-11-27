#include <euphoria/shader/gfx10/spill.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::spill {
namespace {

constexpr std::uint32_t kFirst = 16;

std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>> spilled_entries(
    const native::NativeProgram& program)
{
    const auto* compute = std::get_if<native::ComputeIo>(&program.io);
    if (compute == nullptr || program.stage != native::HwStage::Cs) {
        return std::nullopt;
    }
    const std::uint32_t registers = compute->user_sgprs();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;
    for (const native::RootParam& param : program.layout) {
        if (param.sgprs > 0 && static_cast<std::uint32_t>(param.user_data) >= registers) {
            entries.push_back({static_cast<std::uint32_t>(param.user_data), param.sgprs});
        }
    }
    if (entries.empty() || registers < 2) {
        return std::nullopt;
    }
    std::uint32_t lowest = 0xFFFFFFFFu;
    for (const auto& entry : entries) {
        lowest = std::min(lowest, entry.first);
    }
    if (lowest != kFirst) {
        return std::nullopt;
    }
    return entries;
}

bool contains(const std::vector<ir::Value>& values, ir::Value value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::optional<ir::Value> entry_of(const ir::Program& program, ir::Reg reg)
{
    for (const auto& input : program.inputs) {
        if (input.first == reg) {
            return input.second;
        }
    }
    return std::nullopt;
}

std::optional<ir::Value> read_of(const ir::Inst& inst, const Operand& op)
{
    const auto* scalar = op.as<Operand::Scalar>();
    if (scalar == nullptr || scalar->code == NULL_REG) {
        return std::nullopt;
    }
    return inst.read(ir::Reg::S(scalar->code));
}

}

bool is_spilled(const native::NativeProgram& program)
{
    return spilled_entries(program).has_value();
}

std::expected<ir::Program, ShaderError> build(const native::NativeProgram& program)
{
    const std::vector<std::uint8_t> code = program.patched_code();
    auto built = ir::build(code, program.wave64, program.vgprs);
    if (!built) {
        return std::unexpected(built.error());
    }
    user_data(*built, program);
    return built;
}

std::size_t user_data(ir::Program& program, const native::NativeProgram& source)
{
    const auto spilled = spilled_entries(source);
    if (!spilled) {
        return 0;
    }
    const auto low = entry_of(program, ir::Reg::S(0));
    const auto high = entry_of(program, ir::Reg::S(1));
    if (!low || !high) {
        return 0;
    }

    std::vector<ir::Value> lows{*low};
    std::vector<ir::Value> highs;
    std::vector<ir::Value> raw_highs{*high};
    std::vector<std::pair<std::size_t, std::size_t>> forming;
    for (;;) {
        bool changed = false;
        for (std::size_t b = 0; b < program.blocks.size(); ++b) {
            for (std::size_t i = 0; i < program.blocks[b].insts.size(); ++i) {
                const ir::Inst& inst = program.blocks[b].insts[i];
                const auto* fields = inst.fields.as<Fields::Sop>();
                if (fields == nullptr || !fields->dst) {
                    continue;
                }
                const auto* dst = fields->dst->as<Operand::Scalar>();
                if (dst == nullptr) {
                    continue;
                }
                const auto out = inst.write(ir::Reg::S(dst->code));
                if (!out) {
                    continue;
                }
                std::vector<ir::Value>* found = nullptr;
                const std::vector<Operand>& src = fields->src;
                if (inst.name() == "s_mov_b32" && src.size() == 1) {
                    if (const auto value = read_of(inst, src[0])) {
                        if (contains(lows, *value)) {
                            found = &lows;
                        } else if (contains(highs, *value)) {
                            found = &highs;
                        } else if (contains(raw_highs, *value)) {
                            found = &raw_highs;
                        }
                    }
                } else if (inst.name() == "s_and_b32" && src.size() == 2) {
                    const auto masked = [&](const Operand& a, const Operand& m) {
                        const auto* literal = m.as<Operand::Literal>();
                        if (literal == nullptr || literal->value != 0xFF) {
                            return false;
                        }
                        const auto value = read_of(inst, a);
                        return value && contains(raw_highs, *value);
                    };
                    if (masked(src[0], src[1]) || masked(src[1], src[0])) {
                        found = &highs;
                    }
                }
                if (found != nullptr && !contains(*found, *out)) {
                    found->push_back(*out);
                    forming.push_back({b, i});
                    changed = true;
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    if (highs.empty()) {
        return 0;
    }

    std::unordered_set<ir::Value> read_somewhere;
    for (const ir::Block& block : program.blocks) {
        for (const ir::Phi& phi : block.phis) {
            for (const ir::Value arg : phi.args) {
                read_somewhere.insert(arg);
            }
        }
        for (const ir::Inst& inst : block.insts) {
            for (const auto& read : inst.reads) {
                read_somewhere.insert(read.second);
            }
        }
    }
    std::uint32_t next = 0;
    for (const auto& input : program.inputs) {
        next = std::max(next, input.second.id + 1);
    }
    for (const ir::Block& block : program.blocks) {
        for (const ir::Phi& phi : block.phis) {
            next = std::max(next, phi.value.id + 1);
        }
        for (const ir::Inst& inst : block.insts) {
            for (const auto& write : inst.writes) {
                next = std::max(next, write.second.id + 1);
            }
        }
    }

    std::unordered_map<std::uint32_t, ir::Value> replaced;
    std::vector<std::pair<std::size_t, std::size_t>> gone;
    std::vector<std::pair<ir::Reg, ir::Value>> inputs = program.inputs;
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        for (std::size_t i = 0; i < program.blocks[b].insts.size(); ++i) {
            const ir::Inst& inst = program.blocks[b].insts[i];
            const auto* smem = inst.fields.as<Fields::Smem>();
            if (smem == nullptr || !smem->data) {
                continue;
            }
            const auto* first = smem->data->as<Operand::Scalar>();
            const auto* base = smem->base.as<Operand::Scalar>();
            if (first == nullptr || base == nullptr) {
                continue;
            }
            const bool soffset_ok = [&] {
                if (const auto* scalar = smem->soffset.as<Operand::Scalar>()) {
                    return scalar->code == NULL_REG;
                }
                if (const auto* constant = smem->soffset.as<Operand::Const>()) {
                    return constant->code == 128;
                }
                return false;
            }();
            if (!inst.name().starts_with("s_load_dword") || !soffset_ok || smem->offset < 0
                || smem->offset % 4 != 0) {
                continue;
            }
            const auto base_low = inst.read(ir::Reg::S(base->code));
            const auto base_high = inst.read(ir::Reg::S(static_cast<std::uint8_t>(base->code + 1)));
            if (!base_low || !contains(lows, *base_low) || !base_high
                || !contains(highs, *base_high)) {
                continue;
            }
            bool abort = false;
            std::vector<std::pair<ir::Reg, ir::Value>> dwords;
            for (const auto& write : inst.writes) {
                if (!write.first.is_s() || write.first.is_scc()) {
                    abort = true;
                    break;
                }
                if (write.first.index < first->code) {
                    abort = true;
                    break;
                }
                const std::uint32_t k = static_cast<std::uint32_t>(write.first.index - first->code);
                const std::uint32_t index = kFirst
                    + static_cast<std::uint32_t>(smem->offset) / 4 + k;
                bool declared = false;
                for (const auto& entry : *spilled) {
                    if (index >= entry.first && index < entry.first + entry.second) {
                        declared = true;
                        break;
                    }
                }
                if (!declared) {
                    if (read_somewhere.contains(write.second)) {
                        abort = true;
                        break;
                    }
                    continue;
                }
                if (index > 105) {
                    abort = true;
                    break;
                }
                dwords.push_back({ir::Reg::S(static_cast<std::uint8_t>(index)), write.second});
            }
            if (abort) {
                continue;
            }
            const auto input_for = [&inputs](ir::Reg reg) -> std::optional<ir::Value> {
                for (const auto& entry : inputs) {
                    if (entry.first == reg) {
                        return entry.second;
                    }
                }
                return std::nullopt;
            };
            for (const auto& [reg, value] : dwords) {
                const auto existing = input_for(reg);
                if (existing) {
                    replaced.emplace(value.id, *existing);
                    continue;
                }
                const ir::Value fresh{next};
                ++next;
                inputs.push_back({reg, fresh});
                replaced.emplace(value.id, fresh);
            }
            gone.push_back({b, i});
        }
    }
    if (gone.empty()) {
        return 0;
    }
    program.inputs = inputs;
    const auto renamed = [&replaced](ir::Value& value) {
        if (const auto found = replaced.find(value.id); found != replaced.end()) {
            value = found->second;
        }
    };
    for (ir::Block& block : program.blocks) {
        for (ir::Phi& phi : block.phis) {
            for (ir::Value& arg : phi.args) {
                renamed(arg);
            }
        }
        for (ir::Inst& inst : block.insts) {
            for (auto& read : inst.reads) {
                renamed(read.second);
            }
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> drop = gone;
    for (;;) {
        std::unordered_set<ir::Value> used;
        for (std::size_t b = 0; b < program.blocks.size(); ++b) {
            const ir::Block& block = program.blocks[b];
            for (const ir::Phi& phi : block.phis) {
                for (const ir::Value arg : phi.args) {
                    used.insert(arg);
                }
            }
            for (std::size_t i = 0; i < block.insts.size(); ++i) {
                const bool dropped = std::find(drop.begin(), drop.end(), std::pair{b, i})
                                     != drop.end();
                if (dropped) {
                    continue;
                }
                for (const auto& read : block.insts[i].reads) {
                    used.insert(read.second);
                }
            }
        }
        const std::size_t before = drop.size();
        for (const auto& site : forming) {
            const ir::Inst& inst = program.blocks[site.first].insts[site.second];
            if (std::find(drop.begin(), drop.end(), site) != drop.end()) {
                continue;
            }
            bool any_read = false;
            for (const auto& write : inst.writes) {
                if (used.contains(write.second)) {
                    any_read = true;
                    break;
                }
            }
            if (!any_read) {
                drop.push_back(site);
            }
        }
        if (drop.size() == before) {
            break;
        }
    }
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        std::vector<ir::Inst> kept;
        kept.reserve(program.blocks[b].insts.size());
        for (std::size_t i = 0; i < program.blocks[b].insts.size(); ++i) {
            if (std::find(drop.begin(), drop.end(), std::pair{b, i}) == drop.end()) {
                kept.push_back(std::move(program.blocks[b].insts[i]));
            }
        }
        program.blocks[b].insts = std::move(kept);
    }
    std::unordered_set<ir::Value> read;
    for (const ir::Block& block : program.blocks) {
        for (const ir::Phi& phi : block.phis) {
            for (const ir::Value arg : phi.args) {
                read.insert(arg);
            }
        }
        for (const ir::Inst& inst : block.insts) {
            for (const auto& entry : inst.reads) {
                read.insert(entry.second);
            }
        }
    }
    std::vector<std::pair<ir::Reg, ir::Value>> kept_inputs;
    for (const auto& input : program.inputs) {
        const bool pointer = input.first.is_s()
            && (input.first.index == 0 || input.first.index == 1);
        if (!pointer || read.contains(input.second)) {
            kept_inputs.push_back(input);
        }
    }
    program.inputs = std::move(kept_inputs);
    return gone.size();
}

}
