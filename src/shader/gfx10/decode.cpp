#include <euphoria/shader/gfx10/decode.hpp>

#include <euphoria/shader/gfx10/opcodes.hpp>

#include <algorithm>
#include <array>
#include <map>

namespace euphoria::shader::gfx10 {
namespace {

ShaderError isa(std::size_t offset, const char* reason) noexcept
{
    return ShaderError::Isa(offset, reason);
}

bool is_mk_ak(std::string_view name) noexcept
{
    constexpr std::array<std::string_view, 8> kNames{
        "v_madmk_f32", "v_madak_f32", "v_fmamk_f32", "v_fmaak_f32",
        "v_madmk_f16", "v_madak_f16", "v_fmamk_f16", "v_fmaak_f16",
    };
    return std::ranges::find(kNames, name) != kNames.end();
}

std::expected<Instruction, ShaderError> instruction(std::span<const std::uint8_t> code, std::size_t offset)
{
    const auto word = [&](std::size_t index) -> std::expected<std::uint32_t, ShaderError> {
        const std::size_t at = offset + 4 * index;
        if (at + 4 > code.size() || at < offset) {
            return std::unexpected(isa(offset, "instruction runs past the end of the code"));
        }
        return static_cast<std::uint32_t>(code[at]) | (static_cast<std::uint32_t>(code[at + 1]) << 8)
             | (static_cast<std::uint32_t>(code[at + 2]) << 16)
             | (static_cast<std::uint32_t>(code[at + 3]) << 24);
    };

    const auto first = word(0);
    if (!first) {
        return std::unexpected(first.error());
    }
    const std::uint32_t w = *first;
    Encoding encoding{};
    std::uint32_t opcode = 0;
    std::size_t len = 1;

    if ((w >> 31) == 0) {
        const std::uint32_t src0 = w & 0x1FF;
        const std::size_t extra =
            (src0 == 0xFF || src0 == 0xF9 || src0 == 0xFA || src0 == 0xE9 || src0 == 0xEA) ? 1 : 0;
        const std::uint32_t top = w >> 25;
        if (top == 0x3F) {
            encoding = Encoding::Vop1;
            opcode = (w >> 9) & 0xFF;
            len = 1 + extra;
        } else if (top == 0x3E) {
            encoding = Encoding::Vopc;
            opcode = (w >> 17) & 0xFF;
            len = 1 + extra;
        } else {
            encoding = Encoding::Vop2;
            opcode = top;
            const auto op_name = name(Encoding::Vop2, static_cast<std::uint16_t>(top));
            const std::size_t mk_ak = op_name && is_mk_ak(*op_name) ? 1 : 0;
            len = 1 + std::max(extra, mk_ak);
        }
    } else if ((w >> 30) == 0b10) {
        const auto literal = [](std::initializer_list<std::uint32_t> sources) -> std::size_t {
            return 1 + (std::ranges::find(sources, 0xFFu) != sources.end() ? 1 : 0);
        };
        const std::uint32_t top = w >> 23;
        if (top == 0x17D) {
            encoding = Encoding::Sop1;
            opcode = (w >> 8) & 0xFF;
            len = literal({w & 0xFF});
        } else if (top == 0x17E) {
            encoding = Encoding::Sopc;
            opcode = (w >> 16) & 0x7F;
            len = literal({w & 0xFF, (w >> 8) & 0xFF});
        } else if (top == 0x17F) {
            encoding = Encoding::Sopp;
            opcode = (w >> 16) & 0x7F;
            len = 1;
        } else if ((w >> 28) == 0b1011) {
            encoding = Encoding::Sopk;
            opcode = (w >> 23) & 0x1F;
            const bool imm32 = name(Encoding::Sopk, static_cast<std::uint16_t>(opcode)) == "s_setreg_imm32_b32";
            len = 1 + (imm32 ? 1 : 0);
        } else {
            encoding = Encoding::Sop2;
            opcode = (w >> 23) & 0x7F;
            len = literal({w & 0xFF, (w >> 8) & 0xFF});
        }
    } else {
        const auto vop3 = [](std::uint32_t w1) -> std::size_t {
            const bool literal = (w1 & 0x1FF) == 0xFF || ((w1 >> 9) & 0x1FF) == 0xFF
                              || ((w1 >> 18) & 0x1FF) == 0xFF;
            return 2 + (literal ? 1 : 0);
        };
        switch (w >> 26) {
        case 0x32:
            encoding = Encoding::Vintrp;
            opcode = (w >> 16) & 0x3;
            len = 1;
            break;
        case 0x33:
        case 0x35: {
            const auto second = word(1);
            if (!second) {
                return std::unexpected(second.error());
            }
            if ((w >> 26) == 0x33) {
                encoding = Encoding::Vop3p;
                opcode = (w >> 16) & 0x7F;
            } else {
                encoding = Encoding::Vop3;
                opcode = (w >> 16) & 0x3FF;
            }
            len = vop3(*second);
            break;
        }
        case 0x36:
            encoding = Encoding::Ds;
            opcode = (w >> 18) & 0xFF;
            len = 2;
            break;
        case 0x37:
            switch ((w >> 14) & 3) {
            case 0:
                encoding = Encoding::Flat;
                break;
            case 1:
                encoding = Encoding::Scratch;
                break;
            case 2:
                encoding = Encoding::Global;
                break;
            default:
                return std::unexpected(isa(offset, "reserved FLAT segment"));
            }
            opcode = (w >> 18) & 0x7F;
            len = 2;
            break;
        case 0x38:
            encoding = Encoding::Mubuf;
            opcode = ((w >> 18) & 0x7F) | (((w >> 25) & 1) << 7);
            len = 2;
            break;
        case 0x3A: {
            const auto second = word(1);
            if (!second) {
                return std::unexpected(second.error());
            }
            encoding = Encoding::Mtbuf;
            opcode = ((w >> 16) & 0x7) | (((*second >> 21) & 1) << 3);
            len = 2;
            break;
        }
        case 0x3C:
            encoding = Encoding::Mimg;
            opcode = ((w >> 18) & 0x7F) | ((w & 1) << 7);
            len = 2 + ((w >> 1) & 3);
            break;
        case 0x3D:
            encoding = Encoding::Smem;
            opcode = (w >> 18) & 0xFF;
            len = 2;
            break;
        case 0x3E:
            encoding = Encoding::Exp;
            opcode = 0;
            len = 2;
            break;
        default:
            return std::unexpected(isa(offset, "unknown encoding"));
        }
    }

    Instruction inst;
    inst.offset = offset;
    inst.encoding = encoding;
    inst.opcode = static_cast<std::uint16_t>(opcode);
    inst.words.reserve(len);
    for (std::size_t index = 0; index < len; ++index) {
        const auto value = word(index);
        if (!value) {
            return std::unexpected(value.error());
        }
        inst.words.push_back(*value);
    }
    return inst;
}

std::expected<Flow, ShaderError> flow(const Instruction& inst, std::size_t next, std::size_t len)
{
    const auto target = [&]() -> std::expected<std::size_t, ShaderError> {
        const std::int64_t delta = 4 * static_cast<std::int64_t>(static_cast<std::int16_t>(inst.words[0] & 0xFFFF));
        const std::int64_t at = static_cast<std::int64_t>(next) + delta;
        if (at < 0 || static_cast<std::size_t>(at) >= len) {
            return std::unexpected(isa(inst.offset, "branch target outside the code"));
        }
        return static_cast<std::size_t>(at);
    };
    const std::string_view op = inst.name().value_or("");
    switch (inst.encoding) {
    case Encoding::Sopp:
        if (op == "s_endpgm" || op == "s_endpgm_saved" || op == "s_endpgm_ordered_ps_done"
            || op == "s_code_end") {
            return Flow::Stop();
        }
        if (op == "s_branch") {
            return target().transform(Flow::Jump);
        }
        if (op.starts_with("s_cbranch_")) {
            return target().transform(Flow::Branch);
        }
        break;
    case Encoding::Sopk:
        if (op == "s_call_b64") {
            return target().transform(Flow::Branch);
        }
        break;
    case Encoding::Sop1:
        if (op == "s_setpc_b64") {
            return Flow::Stop();
        }
        break;
    default:
        break;
    }
    return Flow::Next();
}

std::expected<std::optional<std::size_t>, ShaderError> far_jump(const Before& before, const Instruction& setpc,
                                                                std::size_t len)
{
    const std::uint32_t n = setpc.words[0] & 0xFF;
    std::optional<std::int64_t> delta;
    std::size_t at = setpc.offset;
    for (int step = 0; step < 4; ++step) {
        const Instruction* prev = before(at);
        if (prev == nullptr || prev->end() != at) {
            break;
        }
        const std::uint32_t w = prev->words[0];
        const std::uint32_t sdst = (w >> 16) & 0x7F;
        const std::uint32_t ssrc0 = w & 0xFF;
        const std::uint32_t ssrc1 = (w >> 8) & 0xFF;
        const auto op = prev->name();
        if (prev->encoding == Encoding::Sop2 && op && (*op == "s_add_u32" || *op == "s_sub_u32")
            && sdst == n && ssrc0 == n && ssrc1 == 0xFF && !delta) {
            const std::int64_t literal = static_cast<std::int32_t>(prev->words[1]);
            delta = *op == "s_add_u32" ? literal : -literal;
        } else if (prev->encoding == Encoding::Sop1 && op == "s_getpc_b64" && sdst == n) {
            if (!delta) {
                break;
            }
            const std::int64_t target = static_cast<std::int64_t>(prev->offset) + 4 + *delta;
            if (target < 0 || static_cast<std::size_t>(target) >= len) {
                return std::unexpected(isa(setpc.offset, "far jump target outside the code"));
            }
            return std::optional<std::size_t>{static_cast<std::size_t>(target)};
        }
        at = prev->offset;
    }
    return std::optional<std::size_t>{};
}

}

const char* encoding_name(Encoding encoding) noexcept
{
    constexpr std::array<const char*, 20> kNames{
        "SOP2", "SOPK", "SOP1", "SOPC", "SOPP", "SMEM", "VOP2", "VOP1", "VOPC", "VOP3",
        "VOP3P", "VINTRP", "DS", "MUBUF", "MTBUF", "MIMG", "EXP", "FLAT", "GLOBAL", "SCRATCH",
    };
    const auto index = static_cast<std::size_t>(encoding);
    return index < kNames.size() ? kNames[index] : "?";
}

std::optional<std::string_view> name(Encoding encoding, std::uint16_t opcode) noexcept
{
    OpcodeTable table;
    std::uint16_t op = opcode;
    switch (encoding) {
    case Encoding::Sop2:
        table = opcodes::SOP2;
        break;
    case Encoding::Sopk:
        table = opcodes::SOPK;
        break;
    case Encoding::Sop1:
        table = opcodes::SOP1;
        break;
    case Encoding::Sopc:
        table = opcodes::SOPC;
        break;
    case Encoding::Sopp:
        table = opcodes::SOPP;
        break;
    case Encoding::Smem:
        table = opcodes::SMEM;
        break;
    case Encoding::Vop2:
        table = opcodes::VOP2;
        break;
    case Encoding::Vop1:
        table = opcodes::VOP1;
        break;
    case Encoding::Vopc:
        table = opcodes::VOPC;
        break;
    case Encoding::Vop3:
        if (opcode < 0x100) {
            table = opcodes::VOPC;
        } else if (opcode < 0x140) {
            table = opcodes::VOP2;
            op = static_cast<std::uint16_t>(opcode - 0x100);
        } else if (opcode >= 0x180 && opcode < 0x200) {
            table = opcodes::VOP1;
            op = static_cast<std::uint16_t>(opcode - 0x180);
        } else {
            table = opcodes::VOP3;
        }
        break;
    case Encoding::Vop3p:
        table = opcodes::VOP3P;
        break;
    case Encoding::Vintrp:
        table = opcodes::VINTRP;
        break;
    case Encoding::Ds:
        table = opcodes::DS;
        break;
    case Encoding::Mubuf:
        table = opcodes::MUBUF;
        break;
    case Encoding::Mtbuf:
        table = opcodes::MTBUF;
        break;
    case Encoding::Mimg:
        table = opcodes::MIMG;
        break;
    case Encoding::Exp:
        return std::string_view{"exp"};
    case Encoding::Flat:
        table = opcodes::FLAT;
        break;
    case Encoding::Global:
        table = opcodes::GLOBAL;
        break;
    case Encoding::Scratch:
        table = opcodes::SCRATCH;
        break;
    }
    return opcodes::lookup(table, op);
}

std::expected<std::vector<Instruction>, ShaderError> decode(std::span<const std::uint8_t> code)
{
    if (code.size() % 4 != 0) {
        return std::unexpected(isa(code.size(), "code size is not a multiple of 4"));
    }
    std::map<std::size_t, Instruction> found;
    std::vector<std::size_t> pending{0};
    const Before before = [&found](std::size_t at) -> const Instruction* {
        auto it = found.lower_bound(at);
        if (it == found.begin()) {
            return nullptr;
        }
        return &std::prev(it)->second;
    };
    while (!pending.empty()) {
        std::size_t offset = pending.back();
        pending.pop_back();
        while (!found.contains(offset)) {
            auto inst = instruction(code, offset);
            if (!inst) {
                return std::unexpected(inst.error());
            }
            const std::size_t next = inst->end();
            const auto next_flow = control(*inst, code.size(), before);
            if (!next_flow) {
                return std::unexpected(next_flow.error());
            }
            found.emplace(offset, std::move(*inst));
            bool stop = false;
            switch (next_flow->kind) {
            case Flow::Kind::Next:
                offset = next;
                break;
            case Flow::Kind::Stop:
                stop = true;
                break;
            case Flow::Kind::Jump:
                offset = next_flow->target;
                break;
            case Flow::Kind::Branch:
                pending.push_back(next_flow->target);
                offset = next;
                break;
            }
            if (stop) {
                break;
            }
        }
    }
    std::vector<Instruction> list;
    list.reserve(found.size());
    for (auto& [offset, inst] : found) {
        list.push_back(std::move(inst));
    }
    for (std::size_t index = 1; index < list.size(); ++index) {
        if (list[index - 1].end() > list[index].offset) {
            return std::unexpected(isa(list[index].offset, "a branch lands inside an instruction"));
        }
    }
    return list;
}

std::expected<Flow, ShaderError> control(const Instruction& inst, std::size_t len, const Before& before)
{
    const auto result = flow(inst, inst.end(), len);
    if (!result) {
        return result;
    }
    if (result->kind == Flow::Kind::Stop && inst.name() == "s_setpc_b64") {
        const auto target = far_jump(before, inst, len);
        if (!target) {
            return std::unexpected(target.error());
        }
        return *target ? Flow::Jump(**target) : Flow::Stop();
    }
    return result;
}

}
