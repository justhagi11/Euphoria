#include <euphoria/shader/lift_hlsl.hpp>

#include <cstdio>
#include <string>

namespace euphoria::shader {
namespace {

using openscarlett::Rdna2Opcode;
using openscarlett::Rdna2Operand;
using openscarlett::Rdna2OperandKind;

constexpr std::uint8_t kStageVs = 4;
constexpr std::uint8_t kStagePs = 5;

std::string bits(const Rdna2Operand& op)
{
    switch (op.kind) {
    case Rdna2OperandKind::Vgpr:
        return "v[" + std::to_string(op.reg_index) + "]";
    case Rdna2OperandKind::Sgpr:
        return "s[" + std::to_string(op.reg_index) + "]";
    case Rdna2OperandKind::InlineInt:
        return std::to_string(static_cast<std::uint32_t>(op.int_value)) + "u";
    case Rdna2OperandKind::InlineFloat:
    case Rdna2OperandKind::Literal32: {
        char text[32]{};
        std::snprintf(text, sizeof(text), "0x%08xu", op.literal_bits);
        return text;
    }
    default:
        return "0u";
    }
}

std::string f32(const Rdna2Operand& op)
{
    std::string value = "asfloat(" + bits(op) + ")";
    if (op.abs) {
        value = "abs(" + value + ")";
    }
    if (op.neg) {
        value = "-(" + value + ")";
    }
    return value;
}

std::string i32(const Rdna2Operand& op)
{
    if (op.kind == Rdna2OperandKind::Vgpr || op.kind == Rdna2OperandKind::Sgpr) {
        return "asint(" + bits(op) + ")";
    }
    return "asint(" + bits(op) + ")";
}

std::string destination(const Rdna2Operand& op)
{
    if (op.kind == Rdna2OperandKind::Vgpr) {
        return "v[" + std::to_string(op.reg_index) + "]";
    }
    if (op.kind == Rdna2OperandKind::Sgpr) {
        return "s[" + std::to_string(op.reg_index) + "]";
    }
    return {};
}

}

HlslLift lift_to_hlsl(std::uint8_t stage,
                      const std::vector<openscarlett::Rdna2Instruction>& instructions,
                      bool lenient)
{
    HlslLift result;
    result.stage = stage;

    if (stage == 6) {
        result.ok = true;
        result.stub = true;
        result.hlsl =
            "struct V { float4 pos : SV_Position; };\n"
            "[outputtopology(\"triangle\")]\n"
            "[numthreads(1, 1, 1)]\n"
            "void main(out vertices V verts[3], out indices uint3 tris[1]) {\n"
            "  SetMeshOutputCounts(3, 1);\n"
            "  for (int i = 0; i < 3; ++i) { verts[i].pos = float4(0, 0, 0, 1); }\n"
            "  tris[0] = uint3(0, 1, 2);\n"
            "}\n";
        return result;
    }

    if (stage != kStageVs && stage != kStagePs) {
        result.ok = true;
        result.stub = true;
        result.stage = 8;
        result.hlsl = "[numthreads(1, 1, 1)]\nvoid main() {}\n";
        return result;
    }

    std::vector<std::string> body;
    bool exported = false;
    std::string export0 = "0u";
    std::string export1 = "0u";
    std::string export2 = "0u";
    std::string export3 = "0u";

    for (const auto& instruction : instructions) {
        const Rdna2Opcode op = instruction.opcode;
        const Rdna2Operand& d = instruction.dst;
        const Rdna2Operand* s = instruction.src;
        const std::string dst = destination(d);

        switch (op) {
        case Rdna2Opcode::V_NOP:
        case Rdna2Opcode::S_NOP:
        case Rdna2Opcode::S_WAITCNT:
            continue;
        case Rdna2Opcode::S_ENDPGM:
            break;
        case Rdna2Opcode::S_CODE_END:
            if (lenient) {
                ++result.skipped;
                continue;
            }
            result.error = "unsupported opcode: s_code_end";
            return result;
        case Rdna2Opcode::V_SUBREV_F32:
            body.push_back(dst + " = asuint(" + f32(s[1]) + " - " + f32(s[0]) + ");");
            continue;
        case Rdna2Opcode::V_ADD_NC_U32:
            body.push_back(dst + " = " + bits(s[0]) + " + " + bits(s[1]) + ";");
            continue;
        case Rdna2Opcode::V_MUL_LO_U32:
            body.push_back(dst + " = " + bits(s[0]) + " * " + bits(s[1]) + ";");
            continue;
        case Rdna2Opcode::V_CMP_LT_F32:
        case Rdna2Opcode::V_CMP_LE_F32:
        case Rdna2Opcode::V_CMP_GT_F32:
        case Rdna2Opcode::V_CMP_GE_F32:
        case Rdna2Opcode::V_CMP_EQ_F32:
            if (lenient) {
                ++result.skipped;
                continue;
            }
            result.error = std::string("unsupported compare: ")
                         + openscarlett::Rdna2OpcodeName(op);
            return result;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_X:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XY:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZ:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW:
        case Rdna2Opcode::BUFFER_LOAD_DWORD:
            if (lenient) {
                ++result.skipped;
                if (!dst.empty()) {
                    body.push_back(dst + " = 0u;");
                }
                continue;
            }
            result.error = std::string("unsupported opcode: ")
                         + openscarlett::Rdna2OpcodeName(op);
            return result;
        case Rdna2Opcode::EXP: {
            if (!(stage == kStagePs && instruction.exp_target == 0)
                && !(stage == kStageVs && instruction.exp_target == 12)) {
                continue;
            }
            exported = true;
            export0 = bits(s[0]);
            export1 = bits(s[1]);
            export2 = bits(s[2]);
            export3 = bits(s[3]);
            continue;
        }
        case Rdna2Opcode::S_MOV_B32:
            if (!dst.empty()) {
                body.push_back(dst + " = " + bits(s[0]) + ";");
            }
            continue;
        case Rdna2Opcode::S_ADD_U32:
        case Rdna2Opcode::S_SUB_U32:
        case Rdna2Opcode::S_AND_B32:
        case Rdna2Opcode::S_OR_B32:
        case Rdna2Opcode::S_XOR_B32:
        case Rdna2Opcode::S_LSHL_B32:
        case Rdna2Opcode::S_LSHR_B32:
        case Rdna2Opcode::S_ASHR_I32:
        case Rdna2Opcode::S_MUL_I32: {
            if (dst.empty()) {
                continue;
            }
            const char* operator_text = op == Rdna2Opcode::S_ADD_U32 ? "+"
                                      : op == Rdna2Opcode::S_SUB_U32 ? "-"
                                      : op == Rdna2Opcode::S_AND_B32 ? "&"
                                      : op == Rdna2Opcode::S_OR_B32 ? "|"
                                      : op == Rdna2Opcode::S_XOR_B32 ? "^"
                                      : op == Rdna2Opcode::S_LSHL_B32 ? "<<"
                                      : op == Rdna2Opcode::S_LSHR_B32 ? ">>"
                                      : op == Rdna2Opcode::S_ASHR_I32 ? ">>"
                                      : "*";
            body.push_back(dst + " = " + bits(s[0]) + " " + operator_text + " " + bits(s[1]) + ";");
            continue;
        }
        case Rdna2Opcode::V_MOV_B32:
            if (!dst.empty()) {
                body.push_back(dst + " = " + bits(s[0]) + ";");
            }
            continue;
        case Rdna2Opcode::V_AND_B32:
        case Rdna2Opcode::V_OR_B32:
        case Rdna2Opcode::V_XOR_B32:
        case Rdna2Opcode::V_LSHLREV_B32:
        case Rdna2Opcode::V_LSHRREV_B32:
        case Rdna2Opcode::V_ASHRREV_I32: {
            if (dst.empty()) {
                continue;
            }
            const char* operator_text = op == Rdna2Opcode::V_AND_B32 ? "&"
                                      : op == Rdna2Opcode::V_OR_B32 ? "|"
                                      : op == Rdna2Opcode::V_XOR_B32 ? "^"
                                      : ">>";
            body.push_back(dst + " = " + bits(s[1]) + " " + operator_text + " " + bits(s[0]) + ";");
            continue;
        }
        case Rdna2Opcode::V_CVT_F32_I32:
            body.push_back(dst + " = asuint(float(" + i32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_CVT_I32_F32:
            body.push_back(dst + " = asuint(int(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_RCP_F32:
            body.push_back(dst + " = asuint(1.0 / " + f32(s[0]) + ");");
            continue;
        case Rdna2Opcode::V_SQRT_F32:
            body.push_back(dst + " = asuint(sqrt(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_FRACT_F32:
            body.push_back(dst + " = asuint(frac(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_FLOOR_F32:
            body.push_back(dst + " = asuint(floor(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_CEIL_F32:
            body.push_back(dst + " = asuint(ceil(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_TRUNC_F32:
            body.push_back(dst + " = asuint(trunc(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_RNDNE_F32:
            body.push_back(dst + " = asuint(round(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_EXP_F32:
            body.push_back(dst + " = asuint(exp2(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_LOG_F32:
            body.push_back(dst + " = asuint(log2(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_SIN_F32:
            body.push_back(dst + " = asuint(sin(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_COS_F32:
            body.push_back(dst + " = asuint(cos(" + f32(s[0]) + "));");
            continue;
        case Rdna2Opcode::V_ADD_F32:
            body.push_back(dst + " = asuint(" + f32(s[0]) + " + " + f32(s[1]) + ");");
            continue;
        case Rdna2Opcode::V_SUB_F32:
            body.push_back(dst + " = asuint(" + f32(s[0]) + " - " + f32(s[1]) + ");");
            continue;
        case Rdna2Opcode::V_MUL_F32:
            body.push_back(dst + " = asuint(" + f32(s[0]) + " * " + f32(s[1]) + ");");
            continue;
        case Rdna2Opcode::V_MIN_F32:
            body.push_back(dst + " = asuint(min(" + f32(s[0]) + ", " + f32(s[1]) + "));");
            continue;
        case Rdna2Opcode::V_MAX_F32:
            body.push_back(dst + " = asuint(max(" + f32(s[0]) + ", " + f32(s[1]) + "));");
            continue;
        case Rdna2Opcode::V_FMA_F32:
        case Rdna2Opcode::V_MAD_F32:
            body.push_back(dst + " = asuint(mad(" + f32(s[0]) + ", " + f32(s[1]) + ", " + f32(s[2]) + "));");
            continue;
        case Rdna2Opcode::V_FMAC_F32:
            body.push_back(dst + " = asuint(mad(" + f32(d) + ", " + f32(s[0]) + ", " + f32(s[1]) + "));");
            continue;
        case Rdna2Opcode::V_CNDMASK_B32:
            if (lenient) {
                ++result.skipped;
                body.push_back(dst + " = " + bits(s[1]) + ";");
                continue;
            }
            result.error = "v_cndmask requires a known VCC value";
            return result;
        case Rdna2Opcode::S_SWAPPC_B64:
        case Rdna2Opcode::S_SETPC_B64:
        case Rdna2Opcode::S_GETPC_B64:
            if (lenient) {
                ++result.skipped;
                continue;
            }
            result.error = std::string("unsupported opcode: ")
                         + openscarlett::Rdna2OpcodeName(op);
            return result;
        case Rdna2Opcode::S_BUFFER_LOAD_DWORD:
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX2:
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX4:
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX8:
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX16:
        case Rdna2Opcode::S_LOAD_DWORD:
            if (lenient) {
                ++result.skipped;
                if (!dst.empty()) {
                    body.push_back(dst + " = 0u;");
                }
                continue;
            }
            result.error = std::string("unsupported opcode: ")
                         + openscarlett::Rdna2OpcodeName(op);
            return result;
        default: {
            if (lenient) {
                ++result.skipped;
                continue;
            }
            const char* name = openscarlett::Rdna2OpcodeName(op);
            result.error = std::string("unsupported opcode: ") + (name != nullptr ? name : "unknown");
            return result;
        }
        }
    }

    if (!exported) {
        if (!lenient) {
            result.error = "no supported export";
            return result;
        }
        result.ok = true;
        result.stub = true;
        if (stage == kStageVs) {
            result.hlsl = "float4 main(uint id : SV_VertexID) : SV_Position\n{\n"
                          "  return float4(0, 0, 0, 1);\n}\n";
        } else {
            result.hlsl = "float4 main() : SV_Target0\n{\n  return 0;\n}\n";
        }
        return result;
    }

    const bool vs = stage == kStageVs;
    std::string hlsl;
    hlsl += vs ? "float4 main(uint id : SV_VertexID) : SV_Position\n{\n"
               : "float4 main() : SV_Target0\n{\n";
    hlsl += "  uint v[256];\n";
    hlsl += "  uint s[256];\n";
    hlsl += "  for (int i = 0; i < 256; ++i) { v[i] = 0; s[i] = 0; }\n";
    for (const auto& line : body) {
        hlsl += "  " + line + "\n";
    }
    hlsl += "  return float4(asfloat(" + export0 + "), asfloat(" + export1 + "), asfloat("
          + export2 + "), asfloat(" + export3 + "));\n";
    hlsl += "}\n";

    result.ok = true;
    result.partial = result.skipped != 0;
    result.hlsl = std::move(hlsl);
    return result;
}

}
