#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace openscarlett {

namespace {

uint32_t ReadLe32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

uint32_t FloatToBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

float BitsToFloat(uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

int32_t SignExtend(uint32_t value, unsigned bits) {
    const uint32_t shift = 32u - bits;
    return static_cast<int32_t>(value << shift) >> shift;
}

// Public AMD RDNA2 / GFX10.3 inline float constants (codes 240..248)
constexpr uint32_t kInlineFloatBits[9] = {
    0x3F000000u,  // 240:  0.5f
    0xBF000000u,  // 241: -0.5f
    0x3F800000u,  // 242:  1.0f
    0xBF800000u,  // 243: -1.0f
    0x40000000u,  // 244:  2.0f
    0xC0000000u,  // 245: -2.0f
    0x40800000u,  // 246:  4.0f
    0xC0800000u,  // 247: -4.0f
    0x3E22F983u,  // 248:  1.0f / (2.0f * pi)
};

// ---------------------------------------------------------------------------
// GFX10.3 / RDNA2 Opcode Tables (Public AMD RDNA2 ISA Reference Guide)
// ---------------------------------------------------------------------------
Rdna2Opcode DecodeSop2Opcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_ADD_U32;
        case 0x01: return Rdna2Opcode::S_SUB_U32;
        case 0x02: return Rdna2Opcode::S_ADD_I32;
        case 0x03: return Rdna2Opcode::S_SUB_I32;
        case 0x04: return Rdna2Opcode::S_ADDC_U32;
        case 0x05: return Rdna2Opcode::S_SUBB_U32;
        case 0x06: return Rdna2Opcode::S_MIN_I32;
        case 0x07: return Rdna2Opcode::S_MIN_U32;
        case 0x08: return Rdna2Opcode::S_MAX_I32;
        case 0x09: return Rdna2Opcode::S_MAX_U32;
        case 0x0A: return Rdna2Opcode::S_CSELECT_B32;
        case 0x0E: return Rdna2Opcode::S_AND_B32;
        case 0x10: return Rdna2Opcode::S_OR_B32;
        case 0x12: return Rdna2Opcode::S_XOR_B32;
        case 0x14: return Rdna2Opcode::S_ANDN2_B32;
        case 0x16: return Rdna2Opcode::S_OR_B32;       // GFX10.3 s_orn2_b32
        case 0x1E: return Rdna2Opcode::S_LSHL_B32;
        case 0x20: return Rdna2Opcode::S_LSHR_B32;
        case 0x22: return Rdna2Opcode::S_ASHR_I32;
        case 0x26: return Rdna2Opcode::S_ABSDIFF_I32;
        case 0x27: return Rdna2Opcode::S_BFE_U32;
        case 0x28: return Rdna2Opcode::S_BFE_I32;
        case 0x2C: return Rdna2Opcode::S_MUL_I32;
        case 0x2E: return Rdna2Opcode::S_ADD_U32;      // GFX10.3 s_lshl1_add_u32
        case 0x2F: return Rdna2Opcode::S_ADD_U32;      // GFX10.3 s_lshl2_add_u32
        case 0x30: return Rdna2Opcode::S_ADD_U32;      // GFX10.3 s_lshl3_add_u32
        case 0x31: return Rdna2Opcode::S_ADD_U32;      // GFX10.3 s_lshl4_add_u32
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSop2Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_ADD_U32:     return 0x00;
        case Rdna2Opcode::S_SUB_U32:     return 0x01;
        case Rdna2Opcode::S_ADD_I32:     return 0x02;
        case Rdna2Opcode::S_SUB_I32:     return 0x03;
        case Rdna2Opcode::S_ADDC_U32:    return 0x04;
        case Rdna2Opcode::S_SUBB_U32:    return 0x05;
        case Rdna2Opcode::S_MIN_I32:     return 0x06;
        case Rdna2Opcode::S_MIN_U32:     return 0x07;
        case Rdna2Opcode::S_MAX_I32:     return 0x08;
        case Rdna2Opcode::S_MAX_U32:     return 0x09;
        case Rdna2Opcode::S_CSELECT_B32: return 0x0A;
        case Rdna2Opcode::S_AND_B32:     return 0x0E;
        case Rdna2Opcode::S_OR_B32:      return 0x10;
        case Rdna2Opcode::S_XOR_B32:     return 0x12;
        case Rdna2Opcode::S_ANDN2_B32:   return 0x14;
        case Rdna2Opcode::S_LSHL_B32:    return 0x1E;
        case Rdna2Opcode::S_LSHR_B32:    return 0x20;
        case Rdna2Opcode::S_ASHR_I32:    return 0x22;
        case Rdna2Opcode::S_ABSDIFF_I32: return 0x26;
        case Rdna2Opcode::S_BFE_U32:     return 0x27;
        case Rdna2Opcode::S_BFE_I32:     return 0x28;
        case Rdna2Opcode::S_MUL_I32:     return 0x2C;
        default:                         return 0xFFFF;
    }
}

Rdna2Opcode DecodeSop1Opcode(uint16_t raw) {
    switch (raw) {
        case 0x03: return Rdna2Opcode::S_MOV_B32;
        case 0x04: return Rdna2Opcode::S_MOV_B32;      // GFX10.3 s_mov_b64 (e.g. FTexTG exec save)
        case 0x07: return Rdna2Opcode::S_NOT_B32;
        case 0x08: return Rdna2Opcode::S_BCNT1_I32_B32;
        case 0x09: return Rdna2Opcode::S_BCNT1_I32_B64;
        case 0x0A: return Rdna2Opcode::S_MOV_B32;      // GFX10.3 s_wqm_b32 (exec quad mode)
        case 0x0B: return Rdna2Opcode::S_MOV_B32;      // GFX10.3 s_wqm_b64
        case 0x0C: return Rdna2Opcode::S_MOV_B32;      // GFX10.3 s_quadmask_b32
        case 0x0D: return Rdna2Opcode::S_BREV_B32;
        case 0x15: return Rdna2Opcode::S_FLBIT_I32_B32;
        case 0x16: return Rdna2Opcode::S_FLBIT_I32_B64;
        case 0x17: return Rdna2Opcode::S_FLBIT_I32;
        case 0x18: return Rdna2Opcode::S_FLBIT_I32_I64;
        case 0x1D: return Rdna2Opcode::S_SETPC_B64;
        case 0x1E: return Rdna2Opcode::S_SWAPPC_B64;
        case 0x1F: return Rdna2Opcode::S_GETPC_B64;
        case 0x20: return Rdna2Opcode::S_SETPC_B64;    // GFX10.3 hardware s_setpc_b64
        case 0x21: return Rdna2Opcode::S_SWAPPC_B64;   // GFX10.3 hardware s_swappc_b64 (VS fetch call)
        case 0x2C: return Rdna2Opcode::S_MOV_B32;      // GFX10.3 s_sendmsg_rtn_b32
        case 0x2D: return Rdna2Opcode::S_MOV_B32;      // GFX10.3 s_sendmsg_rtn_b64
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSop1Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_MOV_B32:       return 0x03;
        case Rdna2Opcode::S_NOT_B32:       return 0x07;
        case Rdna2Opcode::S_BCNT1_I32_B32: return 0x08;
        case Rdna2Opcode::S_BCNT1_I32_B64: return 0x09;
        case Rdna2Opcode::S_BREV_B32:      return 0x0D;
        case Rdna2Opcode::S_FLBIT_I32_B32: return 0x15;
        case Rdna2Opcode::S_FLBIT_I32_B64: return 0x16;
        case Rdna2Opcode::S_FLBIT_I32:     return 0x17;
        case Rdna2Opcode::S_FLBIT_I32_I64: return 0x18;
        case Rdna2Opcode::S_SETPC_B64:     return 0x1D;
        case Rdna2Opcode::S_SWAPPC_B64:    return 0x1E;
        case Rdna2Opcode::S_GETPC_B64:     return 0x1F;
        default:                           return 0xFFFF;
    }
}

Rdna2Opcode DecodeSopcOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_CMP_EQ_I32;
        case 0x01: return Rdna2Opcode::S_CMP_LG_I32;
        case 0x02: return Rdna2Opcode::S_CMP_GT_I32;
        case 0x03: return Rdna2Opcode::S_CMP_GE_I32;
        case 0x04: return Rdna2Opcode::S_CMP_LT_I32;
        case 0x05: return Rdna2Opcode::S_CMP_LE_I32;
        case 0x06: return Rdna2Opcode::S_CMP_EQ_U32;
        case 0x07: return Rdna2Opcode::S_CMP_LG_U32;
        case 0x08: return Rdna2Opcode::S_CMP_GT_U32;
        case 0x09: return Rdna2Opcode::S_CMP_GE_U32;
        case 0x0A: return Rdna2Opcode::S_CMP_LT_U32;
        case 0x0B: return Rdna2Opcode::S_CMP_LE_U32;
        case 0x12: return Rdna2Opcode::S_CMP_EQ_U64;
        case 0x13: return Rdna2Opcode::S_CMP_LG_U64;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSopcOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_CMP_EQ_I32: return 0x00;
        case Rdna2Opcode::S_CMP_LG_I32: return 0x01;
        case Rdna2Opcode::S_CMP_GT_I32: return 0x02;
        case Rdna2Opcode::S_CMP_GE_I32: return 0x03;
        case Rdna2Opcode::S_CMP_LT_I32: return 0x04;
        case Rdna2Opcode::S_CMP_LE_I32: return 0x05;
        case Rdna2Opcode::S_CMP_EQ_U32: return 0x06;
        case Rdna2Opcode::S_CMP_LG_U32: return 0x07;
        case Rdna2Opcode::S_CMP_GT_U32: return 0x08;
        case Rdna2Opcode::S_CMP_GE_U32: return 0x09;
        case Rdna2Opcode::S_CMP_LT_U32: return 0x0A;
        case Rdna2Opcode::S_CMP_LE_U32: return 0x0B;
        case Rdna2Opcode::S_CMP_EQ_U64: return 0x12;
        case Rdna2Opcode::S_CMP_LG_U64: return 0x13;
        default:                        return 0xFFFF;
    }
}

Rdna2Opcode DecodeSopkOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_MOVK_I32;
        case 0x01: return Rdna2Opcode::S_VERSION;
        case 0x0F: return Rdna2Opcode::S_ADDK_I32;
        case 0x10: return Rdna2Opcode::S_MULK_I32;
        case 0x12: return Rdna2Opcode::S_MOVK_I32;     // GFX10.3 s_getreg_b32
        case 0x13: return Rdna2Opcode::S_VERSION;      // GFX10.3 s_setreg_b32
        case 0x14: return Rdna2Opcode::S_VERSION;      // GFX10.3 s_setreg_imm32_b32 (2 dwords)
        case 0x17: return Rdna2Opcode::S_WAITCNT_VSCNT;
        case 0x18: return Rdna2Opcode::S_WAITCNT_VMCNT;
        case 0x19: return Rdna2Opcode::S_WAITCNT_EXPCNT;
        case 0x1A: return Rdna2Opcode::S_WAITCNT_LGKMCNT;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSopkOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_MOVK_I32:        return 0x00;
        case Rdna2Opcode::S_VERSION:         return 0x01;
        case Rdna2Opcode::S_ADDK_I32:        return 0x0F;
        case Rdna2Opcode::S_MULK_I32:        return 0x10;
        case Rdna2Opcode::S_WAITCNT_VSCNT:   return 0x17;
        case Rdna2Opcode::S_WAITCNT_VMCNT:   return 0x18;
        case Rdna2Opcode::S_WAITCNT_EXPCNT:  return 0x19;
        case Rdna2Opcode::S_WAITCNT_LGKMCNT: return 0x1A;
        default:                             return 0xFFFF;
    }
}

Rdna2Opcode DecodeSoppOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_NOP;
        case 0x01: return Rdna2Opcode::S_ENDPGM;
        case 0x02: return Rdna2Opcode::S_BRANCH;
        case 0x04: return Rdna2Opcode::S_CBRANCH_SCC0;
        case 0x05: return Rdna2Opcode::S_CBRANCH_SCC1;
        case 0x06: return Rdna2Opcode::S_CBRANCH_VCCZ;
        case 0x07: return Rdna2Opcode::S_CBRANCH_VCCNZ;
        case 0x08: return Rdna2Opcode::S_CBRANCH_EXECZ;
        case 0x09: return Rdna2Opcode::S_CBRANCH_EXECNZ;
        case 0x0A: return Rdna2Opcode::S_BARRIER;
        case 0x0C: return Rdna2Opcode::S_WAITCNT;
        case 0x0E: return Rdna2Opcode::S_NOP;          // s_sleep
        case 0x0F: return Rdna2Opcode::S_NOP;          // s_setprio
        case 0x10: return Rdna2Opcode::S_SENDMSG;
        case 0x12: return Rdna2Opcode::S_NOP;          // s_trap
        case 0x13: return Rdna2Opcode::S_ICACHE_INV;
        case 0x16: return Rdna2Opcode::S_TTRACEDATA;
        case 0x1B: return Rdna2Opcode::S_ENDPGM;       // s_endpgm_saved
        case 0x1E: return Rdna2Opcode::S_ENDPGM;       // s_endpgm_ordered_ps_done
        case 0x1F: return Rdna2Opcode::S_CODE_END;
        case 0x20: return Rdna2Opcode::S_NOP;          // s_inst_prefetch
        case 0x21: return Rdna2Opcode::S_NOP;          // s_clause
        case 0x22: return Rdna2Opcode::S_NOP;          // s_wait_idle
        case 0x23: return Rdna2Opcode::S_NOP;          // s_waitcnt_depctr
        case 0x24: return Rdna2Opcode::S_NOP;          // s_round_mode
        case 0x25: return Rdna2Opcode::S_NOP;          // s_denorm_mode
        case 0x28: return Rdna2Opcode::S_TTRACEDATA_IMM;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSoppOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_NOP:            return 0x00;
        case Rdna2Opcode::S_ENDPGM:         return 0x01;
        case Rdna2Opcode::S_BRANCH:         return 0x02;
        case Rdna2Opcode::S_CBRANCH_SCC0:   return 0x04;
        case Rdna2Opcode::S_CBRANCH_SCC1:   return 0x05;
        case Rdna2Opcode::S_CBRANCH_VCCZ:   return 0x06;
        case Rdna2Opcode::S_CBRANCH_VCCNZ:  return 0x07;
        case Rdna2Opcode::S_CBRANCH_EXECZ:  return 0x08;
        case Rdna2Opcode::S_CBRANCH_EXECNZ: return 0x09;
        case Rdna2Opcode::S_BARRIER:        return 0x0A;
        case Rdna2Opcode::S_WAITCNT:        return 0x0C;
        case Rdna2Opcode::S_SENDMSG:        return 0x10;
        case Rdna2Opcode::S_ICACHE_INV:     return 0x13;
        case Rdna2Opcode::S_TTRACEDATA:     return 0x16;
        case Rdna2Opcode::S_CODE_END:       return 0x1F;
        case Rdna2Opcode::S_TTRACEDATA_IMM: return 0x28;
        default:                            return 0xFFFF;
    }
}

Rdna2Opcode DecodeVop2Opcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::V_CNDMASK_B32;  // GFX9/Xbox & GFX10 VOP2 op=0x00 v_cndmask_b32_e32
        case 0x01: return Rdna2Opcode::V_CNDMASK_B32;
        case 0x02: return Rdna2Opcode::V_DOT2_F32_F16; // GFX10.3 v_dot2c_f32_f16_e32
        case 0x03: return Rdna2Opcode::V_ADD_F32;
        case 0x04: return Rdna2Opcode::V_SUB_F32;
        case 0x05: return Rdna2Opcode::V_SUBREV_F32;
        case 0x06: return Rdna2Opcode::V_FMAC_F32;     // GFX10.3 v_fmac_legacy_f32
        case 0x07: return Rdna2Opcode::V_MUL_F32;  // GFX10.3 v_mul_legacy_f32
        case 0x08: return Rdna2Opcode::V_MUL_F32;
        case 0x09: return Rdna2Opcode::V_MUL_LO_U32;   // GFX10.3 v_mul_i32_i24
        case 0x0B: return Rdna2Opcode::V_MUL_LO_U32;   // GFX10.3 v_mul_u32_u24
        case 0x0F: return Rdna2Opcode::V_MIN_F32;
        case 0x10: return Rdna2Opcode::V_MAX_F32;
        case 0x11: return Rdna2Opcode::V_MIN_I32;
        case 0x12: return Rdna2Opcode::V_MAX_I32;
        case 0x13: return Rdna2Opcode::V_MIN_U32;
        case 0x14: return Rdna2Opcode::V_MAX_U32;
        case 0x16: return Rdna2Opcode::V_LSHRREV_B32;
        case 0x17: return Rdna2Opcode::V_ASHRREV_I32;
        case 0x18: return Rdna2Opcode::V_LSHLREV_B32;
        case 0x1A: return Rdna2Opcode::V_LSHLREV_B32;  // GFX10.3 native V_LSHLREV_B32 encoding
        case 0x1B: return Rdna2Opcode::V_AND_B32;
        case 0x1C: return Rdna2Opcode::V_OR_B32;
        case 0x1D: return Rdna2Opcode::V_XOR_B32;
        case 0x1F: return Rdna2Opcode::V_MAC_F32;
        case 0x20: return Rdna2Opcode::V_MADMK_F32;    // GFX10.3 native v_madmk_f32
        case 0x21: return Rdna2Opcode::V_MADMK_F32;
        case 0x22: return Rdna2Opcode::V_MADAK_F32;
        case 0x25: return Rdna2Opcode::V_ADD_NC_U32;
        case 0x26: return Rdna2Opcode::V_SUB_NC_U32;
        case 0x27: return Rdna2Opcode::V_SUB_NC_U32;   // GFX10.3 v_subrev_nc_u32
        case 0x28: return Rdna2Opcode::V_ADD_NC_U32;   // GFX10.3 v_add_co_ci_u32_e32
        case 0x29: return Rdna2Opcode::V_SUB_NC_U32;   // GFX10.3 v_sub_co_ci_u32_e32
        case 0x2A: return Rdna2Opcode::V_SUB_NC_U32;   // GFX10.3 v_subrev_co_ci_u32_e32
        case 0x2B: return Rdna2Opcode::V_FMAC_F32;
        case 0x2C: return Rdna2Opcode::V_MADMK_F32;  // GFX10.3 v_fmamk_f32 (mandatory 32-bit literal K)
        case 0x2D: return Rdna2Opcode::V_MADAK_F32;  // GFX10.3 v_fmaak_f32 (mandatory 32-bit literal K)
        case 0x2F: return Rdna2Opcode::V_CVT_PKRTZ_F16_F32;  // GFX10.3 v_cvt_pkrtz_f16_f32
        case 0x36: return Rdna2Opcode::V_FMAC_F16;
        case 0x37: return Rdna2Opcode::V_FMAMK_F16;
        case 0x38: return Rdna2Opcode::V_FMAAK_F16;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeVop2Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_CNDMASK_B32: return 0x01;
        case Rdna2Opcode::V_ADD_F32:     return 0x03;
        case Rdna2Opcode::V_SUB_F32:     return 0x04;
        case Rdna2Opcode::V_SUBREV_F32:  return 0x05;
        case Rdna2Opcode::V_MUL_F32:     return 0x08;
        case Rdna2Opcode::V_MIN_F32:     return 0x0F;
        case Rdna2Opcode::V_MAX_F32:     return 0x10;
        case Rdna2Opcode::V_MIN_I32:     return 0x11;
        case Rdna2Opcode::V_MAX_I32:     return 0x12;
        case Rdna2Opcode::V_MIN_U32:     return 0x13;
        case Rdna2Opcode::V_MAX_U32:     return 0x14;
        case Rdna2Opcode::V_LSHRREV_B32: return 0x16;
        case Rdna2Opcode::V_ASHRREV_I32: return 0x17;
        case Rdna2Opcode::V_LSHLREV_B32: return 0x18;
        case Rdna2Opcode::V_AND_B32:     return 0x1B;
        case Rdna2Opcode::V_OR_B32:      return 0x1C;
        case Rdna2Opcode::V_XOR_B32:     return 0x1D;
        case Rdna2Opcode::V_MAC_F32:     return 0x1F;
        case Rdna2Opcode::V_MADMK_F32:   return 0x21;
        case Rdna2Opcode::V_MADAK_F32:   return 0x22;
        case Rdna2Opcode::V_ADD_NC_U32:  return 0x25;
        case Rdna2Opcode::V_SUB_NC_U32:  return 0x26;
        case Rdna2Opcode::V_FMAC_F32:    return 0x2B;
        case Rdna2Opcode::V_FMAC_F16:    return 0x36;
        case Rdna2Opcode::V_FMAMK_F16:   return 0x37;
        case Rdna2Opcode::V_FMAAK_F16:   return 0x38;
        default:                         return 0xFFFF;
    }
}

Rdna2Opcode DecodeVop1Opcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::V_NOP;
        case 0x01: return Rdna2Opcode::V_MOV_B32;
        case 0x02: return Rdna2Opcode::V_READFIRSTLANE_B32;
        case 0x05: return Rdna2Opcode::V_CVT_F32_I32;
        case 0x06: return Rdna2Opcode::V_CVT_F32_U32;
        case 0x07: return Rdna2Opcode::V_CVT_U32_F32;
        case 0x08: return Rdna2Opcode::V_CVT_I32_F32;
        case 0x20: return Rdna2Opcode::V_FRACT_F32;
        case 0x21: return Rdna2Opcode::V_TRUNC_F32;
        case 0x22: return Rdna2Opcode::V_CEIL_F32;
        case 0x23: return Rdna2Opcode::V_RNDNE_F32;
        case 0x24: return Rdna2Opcode::V_FLOOR_F32;
        case 0x25: return Rdna2Opcode::V_EXP_F32;
        case 0x27: return Rdna2Opcode::V_LOG_F32;
        case 0x2A: return Rdna2Opcode::V_RCP_F32;
        case 0x2E: return Rdna2Opcode::V_RSQ_F32;
        case 0x33: return Rdna2Opcode::V_SQRT_F32;
        case 0x35: return Rdna2Opcode::V_SIN_F32;
        case 0x36: return Rdna2Opcode::V_COS_F32;
        case 0x63: return Rdna2Opcode::V_CVT_NORM_I16_F16;
        case 0x64: return Rdna2Opcode::V_CVT_NORM_U16_F16;
        case 0x65: return Rdna2Opcode::V_SWAP_B32;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeVop1Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_NOP:               return 0x00;
        case Rdna2Opcode::V_MOV_B32:           return 0x01;
        case Rdna2Opcode::V_READFIRSTLANE_B32: return 0x02;
        case Rdna2Opcode::V_CVT_F32_I32:       return 0x05;
        case Rdna2Opcode::V_CVT_F32_U32:       return 0x06;
        case Rdna2Opcode::V_CVT_U32_F32:       return 0x07;
        case Rdna2Opcode::V_CVT_I32_F32:       return 0x08;
        case Rdna2Opcode::V_FRACT_F32:         return 0x20;
        case Rdna2Opcode::V_TRUNC_F32:         return 0x21;
        case Rdna2Opcode::V_CEIL_F32:          return 0x22;
        case Rdna2Opcode::V_RNDNE_F32:         return 0x23;
        case Rdna2Opcode::V_FLOOR_F32:         return 0x24;
        case Rdna2Opcode::V_EXP_F32:           return 0x25;
        case Rdna2Opcode::V_LOG_F32:           return 0x27;
        case Rdna2Opcode::V_RCP_F32:           return 0x2A;
        case Rdna2Opcode::V_RSQ_F32:           return 0x2E;
        case Rdna2Opcode::V_SQRT_F32:          return 0x33;
        case Rdna2Opcode::V_SIN_F32:           return 0x35;
        case Rdna2Opcode::V_COS_F32:           return 0x36;
        case Rdna2Opcode::V_CVT_NORM_I16_F16:  return 0x63;
        case Rdna2Opcode::V_CVT_NORM_U16_F16:  return 0x64;
        case Rdna2Opcode::V_SWAP_B32:          return 0x65;
        default:                               return 0xFFFF;
    }
}

Rdna2Opcode DecodeVopcOpcode(uint16_t raw) {
    switch (raw) {
        // GFX10.3 native V_CMP_*_F32 (0x00..0x0F)
        case 0x00: return Rdna2Opcode::V_CMP_F_F32;
        case 0x01: return Rdna2Opcode::V_CMP_LT_F32;
        case 0x02: return Rdna2Opcode::V_CMP_EQ_F32;
        case 0x03: return Rdna2Opcode::V_CMP_LE_F32;
        case 0x04: return Rdna2Opcode::V_CMP_GT_F32;
        case 0x05: return Rdna2Opcode::V_CMP_LG_F32;
        case 0x06: return Rdna2Opcode::V_CMP_GE_F32;
        case 0x07: return Rdna2Opcode::V_CMP_O_F32;
        case 0x08: return Rdna2Opcode::V_CMP_U_F32;
        case 0x09: return Rdna2Opcode::V_CMP_NGE_F32;
        case 0x0A: return Rdna2Opcode::V_CMP_NLG_F32;
        case 0x0B: return Rdna2Opcode::V_CMP_NGT_F32;
        case 0x0C: return Rdna2Opcode::V_CMP_NLE_F32;
        case 0x0D: return Rdna2Opcode::V_CMP_NEQ_F32;
        case 0x0E: return Rdna2Opcode::V_CMP_NLT_F32;
        case 0x0F: return Rdna2Opcode::V_CMP_TRU_F32;
        case 0x10: return Rdna2Opcode::V_CMP_F_F32;
        case 0x11: return Rdna2Opcode::V_CMP_LT_F32;
        case 0x12: return Rdna2Opcode::V_CMP_EQ_F32;
        case 0x13: return Rdna2Opcode::V_CMP_LE_F32;
        case 0x14: return Rdna2Opcode::V_CMP_GT_F32;
        case 0x15: return Rdna2Opcode::V_CMP_LG_F32;
        case 0x16: return Rdna2Opcode::V_CMP_GE_F32;
        case 0x17: return Rdna2Opcode::V_CMP_O_F32;
        case 0x18: return Rdna2Opcode::V_CMP_U_F32;
        case 0x19: return Rdna2Opcode::V_CMP_NGE_F32;
        case 0x1A: return Rdna2Opcode::V_CMP_NLG_F32;
        case 0x1B: return Rdna2Opcode::V_CMP_NGT_F32;
        case 0x1C: return Rdna2Opcode::V_CMP_NLE_F32;
        case 0x1D: return Rdna2Opcode::V_CMP_NEQ_F32;
        case 0x1E: return Rdna2Opcode::V_CMP_NLT_F32;
        case 0x1F: return Rdna2Opcode::V_CMP_TRU_F32;
        case 0x31: return Rdna2Opcode::V_CMPX_LT_F32;
        case 0x32: return Rdna2Opcode::V_CMPX_EQ_F32;
        case 0x33: return Rdna2Opcode::V_CMPX_LE_F32;
        case 0x34: return Rdna2Opcode::V_CMPX_GT_F32;
        case 0x35: return Rdna2Opcode::V_CMPX_LG_F32;
        case 0x36: return Rdna2Opcode::V_CMPX_GE_F32;
        case 0x81: return Rdna2Opcode::V_CMP_LT_I32;
        case 0x82: return Rdna2Opcode::V_CMP_EQ_I32;
        case 0x83: return Rdna2Opcode::V_CMP_GE_I32;   // GFX10.3 v_cmp_le_i32
        case 0x84: return Rdna2Opcode::V_CMP_GT_I32;
        case 0x85: return Rdna2Opcode::V_CMP_EQ_I32;   // GFX10.3 v_cmp_lg_i32 (ne)
        case 0x86: return Rdna2Opcode::V_CMP_GE_I32;
        case 0x91: return Rdna2Opcode::V_CMPX_LT_I32;
        case 0x92: return Rdna2Opcode::V_CMPX_EQ_I32;
        case 0x93: return Rdna2Opcode::V_CMPX_GE_I32;  // GFX10.3 v_cmpx_le_i32
        case 0x94: return Rdna2Opcode::V_CMPX_GT_I32;
        case 0x95: return Rdna2Opcode::V_CMPX_EQ_I32;  // GFX10.3 v_cmpx_lg_i32 (ne)
        case 0x96: return Rdna2Opcode::V_CMPX_GE_I32;
        // 64-bit signed integer compares
        case 0xA1: return Rdna2Opcode::V_CMP_LT_I64;
        case 0xA2: return Rdna2Opcode::V_CMP_EQ_I64;
        case 0xA3: return Rdna2Opcode::V_CMP_LE_I64;
        case 0xA4: return Rdna2Opcode::V_CMP_GT_I64;
        case 0xA5: return Rdna2Opcode::V_CMP_LG_I64;
        case 0xA6: return Rdna2Opcode::V_CMP_GE_I64;
        case 0xC1: return Rdna2Opcode::V_CMP_LT_U32;
        case 0xC2: return Rdna2Opcode::V_CMP_EQ_U32;
        case 0xC3: return Rdna2Opcode::V_CMP_GE_U32;   // GFX10.3 v_cmp_le_u32
        case 0xC4: return Rdna2Opcode::V_CMP_GT_U32;
        case 0xC5: return Rdna2Opcode::V_CMP_EQ_U32;   // GFX10.3 v_cmp_lg_u32 (ne)
        case 0xC6: return Rdna2Opcode::V_CMP_GE_U32;
        // GFX10 split F16 compare range (lower half 0xC8..0xCF; upper half 0xD8..0xDF)
        case 0xC9: return Rdna2Opcode::V_CMP_LT_F16;
        case 0xCA: return Rdna2Opcode::V_CMP_EQ_F16;
        case 0xCB: return Rdna2Opcode::V_CMP_LE_F16;
        case 0xCC: return Rdna2Opcode::V_CMP_GT_F16;
        case 0xCD: return Rdna2Opcode::V_CMP_LG_F16;
        case 0xCE: return Rdna2Opcode::V_CMP_GE_F16;
        case 0xD1: return Rdna2Opcode::V_CMPX_LT_U32;
        case 0xD2: return Rdna2Opcode::V_CMPX_EQ_U32;
        case 0xD3: return Rdna2Opcode::V_CMPX_GE_U32;  // GFX10.3 v_cmpx_le_u32
        case 0xD4: return Rdna2Opcode::V_CMPX_GT_U32;
        case 0xD5: return Rdna2Opcode::V_CMPX_EQ_U32;  // GFX10.3 v_cmpx_lg_u32 (ne)
        case 0xD6: return Rdna2Opcode::V_CMPX_GE_U32;
        case 0xD8: return Rdna2Opcode::V_CMP_U_F16;
        case 0xD9: return Rdna2Opcode::V_CMP_NGE_F16;
        case 0xDA: return Rdna2Opcode::V_CMP_NLG_F16;
        case 0xDB: return Rdna2Opcode::V_CMP_NGT_F16;
        case 0xDC: return Rdna2Opcode::V_CMP_NLE_F16;
        case 0xDD: return Rdna2Opcode::V_CMP_NEQ_F16;
        case 0xDE: return Rdna2Opcode::V_CMP_NLT_F16;
        case 0xDF: return Rdna2Opcode::V_CMP_TRU_F16;
        // 64-bit unsigned integer compares
        case 0xE1: return Rdna2Opcode::V_CMP_LT_U64;
        case 0xE2: return Rdna2Opcode::V_CMP_EQ_U64;
        case 0xE3: return Rdna2Opcode::V_CMP_LE_U64;
        case 0xE4: return Rdna2Opcode::V_CMP_GT_U64;
        case 0xE5: return Rdna2Opcode::V_CMP_LG_U64;
        case 0xE6: return Rdna2Opcode::V_CMP_GE_U64;
        // GFX10 split F16 V_CMPX range (lower half 0xE8..0xEF; upper half 0xF8..0xFF)
        case 0xE9: return Rdna2Opcode::V_CMPX_LT_F16;
        case 0xEA: return Rdna2Opcode::V_CMPX_EQ_F16;
        case 0xEB: return Rdna2Opcode::V_CMPX_LE_F16;
        case 0xEC: return Rdna2Opcode::V_CMPX_GT_F16;
        case 0xED: return Rdna2Opcode::V_CMPX_LG_F16;
        case 0xEE: return Rdna2Opcode::V_CMPX_GE_F16;
        case 0xFD: return Rdna2Opcode::V_CMPX_NEQ_F16;
        case 0xFE: return Rdna2Opcode::V_CMPX_NLT_F16;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeVopcOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_CMP_F_F32:    return 0x10;
        case Rdna2Opcode::V_CMP_LT_F32:   return 0x11;
        case Rdna2Opcode::V_CMP_EQ_F32:   return 0x12;
        case Rdna2Opcode::V_CMP_LE_F32:   return 0x13;
        case Rdna2Opcode::V_CMP_GT_F32:   return 0x14;
        case Rdna2Opcode::V_CMP_LG_F32:   return 0x15;
        case Rdna2Opcode::V_CMP_GE_F32:   return 0x16;
        case Rdna2Opcode::V_CMP_O_F32:    return 0x17;
        case Rdna2Opcode::V_CMP_U_F32:    return 0x18;
        case Rdna2Opcode::V_CMP_NGE_F32:  return 0x19;
        case Rdna2Opcode::V_CMP_NLG_F32:  return 0x1A;
        case Rdna2Opcode::V_CMP_NGT_F32:  return 0x1B;
        case Rdna2Opcode::V_CMP_NLE_F32:  return 0x1C;
        case Rdna2Opcode::V_CMP_NEQ_F32:  return 0x1D;
        case Rdna2Opcode::V_CMP_NLT_F32:  return 0x1E;
        case Rdna2Opcode::V_CMP_TRU_F32:  return 0x1F;
        case Rdna2Opcode::V_CMPX_LT_F32:  return 0x31;
        case Rdna2Opcode::V_CMPX_EQ_F32:  return 0x32;
        case Rdna2Opcode::V_CMPX_LE_F32:  return 0x33;
        case Rdna2Opcode::V_CMPX_GT_F32:  return 0x34;
        case Rdna2Opcode::V_CMPX_LG_F32:  return 0x35;
        case Rdna2Opcode::V_CMPX_GE_F32:  return 0x36;
        case Rdna2Opcode::V_CMP_LT_I32:   return 0x81;
        case Rdna2Opcode::V_CMP_EQ_I32:   return 0x82;
        case Rdna2Opcode::V_CMP_GT_I32:   return 0x84;
        case Rdna2Opcode::V_CMP_GE_I32:   return 0x86;
        case Rdna2Opcode::V_CMPX_LT_I32:  return 0x91;
        case Rdna2Opcode::V_CMPX_EQ_I32:  return 0x92;
        case Rdna2Opcode::V_CMPX_GT_I32:  return 0x94;
        case Rdna2Opcode::V_CMPX_GE_I32:  return 0x96;
        case Rdna2Opcode::V_CMP_LT_U32:   return 0xC1;
        case Rdna2Opcode::V_CMP_EQ_U32:   return 0xC2;
        case Rdna2Opcode::V_CMP_GT_U32:   return 0xC4;
        case Rdna2Opcode::V_CMP_GE_U32:   return 0xC6;
        case Rdna2Opcode::V_CMP_LT_F16:   return 0xC9;
        case Rdna2Opcode::V_CMP_EQ_F16:   return 0xCA;
        case Rdna2Opcode::V_CMP_LE_F16:   return 0xCB;
        case Rdna2Opcode::V_CMP_GT_F16:   return 0xCC;
        case Rdna2Opcode::V_CMP_LG_F16:   return 0xCD;
        case Rdna2Opcode::V_CMP_GE_F16:   return 0xCE;
        case Rdna2Opcode::V_CMPX_LT_U32:  return 0xD1;
        case Rdna2Opcode::V_CMPX_EQ_U32:  return 0xD2;
        case Rdna2Opcode::V_CMPX_GT_U32:  return 0xD4;
        case Rdna2Opcode::V_CMPX_GE_U32:  return 0xD6;
        case Rdna2Opcode::V_CMP_U_F16:    return 0xD8;
        case Rdna2Opcode::V_CMP_NGE_F16:  return 0xD9;
        case Rdna2Opcode::V_CMP_NLG_F16:  return 0xDA;
        case Rdna2Opcode::V_CMP_NGT_F16:  return 0xDB;
        case Rdna2Opcode::V_CMP_NLE_F16:  return 0xDC;
        case Rdna2Opcode::V_CMP_NEQ_F16:  return 0xDD;
        case Rdna2Opcode::V_CMP_NLT_F16:  return 0xDE;
        case Rdna2Opcode::V_CMP_TRU_F16:  return 0xDF;
        case Rdna2Opcode::V_CMPX_LT_F16:  return 0xE9;
        case Rdna2Opcode::V_CMPX_EQ_F16:  return 0xEA;
        case Rdna2Opcode::V_CMPX_LE_F16:  return 0xEB;
        case Rdna2Opcode::V_CMPX_GT_F16:  return 0xEC;
        case Rdna2Opcode::V_CMPX_LG_F16:  return 0xED;
        case Rdna2Opcode::V_CMPX_GE_F16:  return 0xEE;
        case Rdna2Opcode::V_CMPX_NEQ_F16: return 0xFD;
        case Rdna2Opcode::V_CMPX_NLT_F16: return 0xFE;
        case Rdna2Opcode::V_CMP_LT_I64:   return 0xA1;
        case Rdna2Opcode::V_CMP_EQ_I64:   return 0xA2;
        case Rdna2Opcode::V_CMP_LE_I64:   return 0xA3;
        case Rdna2Opcode::V_CMP_GT_I64:   return 0xA4;
        case Rdna2Opcode::V_CMP_LG_I64:   return 0xA5;
        case Rdna2Opcode::V_CMP_GE_I64:   return 0xA6;
        case Rdna2Opcode::V_CMP_LT_U64:   return 0xE1;
        case Rdna2Opcode::V_CMP_EQ_U64:   return 0xE2;
        case Rdna2Opcode::V_CMP_LE_U64:   return 0xE3;
        case Rdna2Opcode::V_CMP_GT_U64:   return 0xE4;
        case Rdna2Opcode::V_CMP_LG_U64:   return 0xE5;
        case Rdna2Opcode::V_CMP_GE_U64:   return 0xE6;
        default:                          return 0xFFFF;
    }
}

Rdna2Opcode DecodeVop3Opcode(uint16_t raw, uint8_t& out_src_count) {
    // Native 3-operand / wave / bitfield VOP3 opcodes (GFX10.3)
    switch (raw) {
        case 0x100: out_src_count = 3; return Rdna2Opcode::V_CNDMASK_B32;   // GFX10 v_cndmask_b32_e64 (op=0x100)
        case 0x101: out_src_count = 3; return Rdna2Opcode::V_CNDMASK_B32;
        case 0x128: out_src_count = 3; return Rdna2Opcode::V_ADD_NC_U32;    // GFX10.3 v_add_co_ci_u32_e64
        case 0x129: out_src_count = 3; return Rdna2Opcode::V_SUB_NC_U32;    // GFX10.3 v_sub_co_ci_u32_e64
        case 0x12A: out_src_count = 3; return Rdna2Opcode::V_SUB_NC_U32;    // GFX10.3 v_subrev_co_ci_u32_e64
        case 0x140: out_src_count = 3; return Rdna2Opcode::V_MAD_F32;       // GFX10.3 v_mad_legacy_f32
        case 0x141: out_src_count = 3; return Rdna2Opcode::V_MAD_F32;
        case 0x142: out_src_count = 3; return Rdna2Opcode::V_ADD3_U32;      // GFX10.3 v_mad_i32_i24
        case 0x143: out_src_count = 3; return Rdna2Opcode::V_ADD3_U32;      // GFX10.3 v_mad_u32_u24
        case 0x148: out_src_count = 3; return Rdna2Opcode::V_BFE_U32;
        case 0x149: out_src_count = 3; return Rdna2Opcode::V_BFE_I32;
        case 0x14A: out_src_count = 3; return Rdna2Opcode::V_BFI_B32;
        case 0x14B: out_src_count = 3; return Rdna2Opcode::V_FMA_F32;
        case 0x14C: out_src_count = 3; return Rdna2Opcode::V_ALIGNBYTE_B32;
        case 0x14F: out_src_count = 3; return Rdna2Opcode::V_ALIGNBIT_B32;
        case 0x151: out_src_count = 3; return Rdna2Opcode::V_MIN3_F32;
        case 0x152: out_src_count = 3; return Rdna2Opcode::V_MIN3_I32;
        case 0x153: out_src_count = 3; return Rdna2Opcode::V_MIN3_U32;
        case 0x154: out_src_count = 3; return Rdna2Opcode::V_MAX3_F32;
        case 0x155: out_src_count = 3; return Rdna2Opcode::V_MAX3_I32;
        case 0x156: out_src_count = 3; return Rdna2Opcode::V_MAX3_U32;
        case 0x157: out_src_count = 3; return Rdna2Opcode::V_MED3_F32;
        case 0x158: out_src_count = 3; return Rdna2Opcode::V_MED3_I32;
        case 0x159: out_src_count = 3; return Rdna2Opcode::V_MED3_U32;
        case 0x162: out_src_count = 3; return Rdna2Opcode::V_DIV_FMAS_F32;
        case 0x169: out_src_count = 2; return Rdna2Opcode::V_MUL_LO_U32;
        case 0x16C: out_src_count = 3; return Rdna2Opcode::V_MAD_U64_U32;
        case 0x16D: out_src_count = 3; return Rdna2Opcode::V_MAD_I64_I32;
        case 0x179: out_src_count = 3; return Rdna2Opcode::V_FMA_F32;       // GFX10.3 v_fma_legacy_f32
        case 0x303: out_src_count = 2; return Rdna2Opcode::V_ADD_NC_U16;
        case 0x304: out_src_count = 2; return Rdna2Opcode::V_SUB_NC_U16;
        case 0x305: out_src_count = 2; return Rdna2Opcode::V_MBCNT_LO_U32_B32;
        case 0x306: out_src_count = 2; return Rdna2Opcode::V_MBCNT_HI_U32_B32;
        case 0x309: out_src_count = 2; return Rdna2Opcode::V_MIN_U16;
        case 0x30A: out_src_count = 2; return Rdna2Opcode::V_MAX_U16;
        case 0x30B: out_src_count = 2; return Rdna2Opcode::V_MIN_I16;
        case 0x30C: out_src_count = 2; return Rdna2Opcode::V_MAX_I16;
        case 0x30D: out_src_count = 2; return Rdna2Opcode::V_ADD_NC_U32;    // GFX10.3 v_add_co_u32_e64
        case 0x30E: out_src_count = 2; return Rdna2Opcode::V_SUB_NC_U32;    // GFX10.3 v_sub_co_u32_e64
        case 0x30F: out_src_count = 2; return Rdna2Opcode::V_SUB_NC_U32;    // GFX10.3 v_subrev_co_u32_e64
        case 0x312: out_src_count = 2; return Rdna2Opcode::V_MUL_LO_U16;
        case 0x313: out_src_count = 2; return Rdna2Opcode::V_ASHRREV_I16;
        case 0x314: out_src_count = 2; return Rdna2Opcode::V_LSHLREV_B16;
        case 0x315: out_src_count = 2; return Rdna2Opcode::V_LSHRREV_B16;
        case 0x340: out_src_count = 3; return Rdna2Opcode::V_MAD_U16;
        case 0x346: out_src_count = 3; return Rdna2Opcode::V_LSHL_ADD_U32;
        case 0x34B: out_src_count = 3; return Rdna2Opcode::V_FMA_F16;
        case 0x360: out_src_count = 2; return Rdna2Opcode::V_READLANE_B32;
        case 0x366: out_src_count = 2; return Rdna2Opcode::V_CVT_PKNORM_I16_F16;
        case 0x367: out_src_count = 2; return Rdna2Opcode::V_CVT_PKNORM_U16_F16;
        case 0x368: out_src_count = 2; return Rdna2Opcode::V_CVT_PKNORM_I16_F32;
        case 0x369: out_src_count = 2; return Rdna2Opcode::V_CVT_PKNORM_U16_F32;
        case 0x36D: out_src_count = 3; return Rdna2Opcode::V_ADD3_U32;
        case 0x36E: out_src_count = 3; return Rdna2Opcode::V_LSHL_OR_B32;   // GFX10.3 v_and_or_b32
        case 0x36F: out_src_count = 3; return Rdna2Opcode::V_LSHL_OR_B32;
        case 0x370: out_src_count = 3; return Rdna2Opcode::V_LSHL_OR_B32;   // GFX10.3 v_or3_b32
        case 0x371: out_src_count = 3; return Rdna2Opcode::V_ADD3_U32;      // GFX10.3 v_xad_u32
        case 0x372: out_src_count = 3; return Rdna2Opcode::V_LSHL_ADD_U32;  // GFX10.3 v_lshl_add_u32
        case 0x373: out_src_count = 3; return Rdna2Opcode::V_DOT2_F32_F16;
        case 0x374: out_src_count = 3; return Rdna2Opcode::V_LSHL_ADD_U32;  // GFX10.3 v_add_lshl_u32
        case 0x377: out_src_count = 3; return Rdna2Opcode::V_PERMLANE16_B32;
        case 0x378: out_src_count = 3; return Rdna2Opcode::V_PERMLANEX16_B32;
        default: break;
    }
    // Promoted VOPC (0x000..0x0FF)
    if (raw < 0x100u) {
        Rdna2Opcode op = DecodeVopcOpcode(raw);
        if (op != Rdna2Opcode::Unknown) {
            out_src_count = 2;
            return op;
        }
    }
    // Promoted VOP2 (0x100..0x13F)
    if (raw >= 0x100u && raw < 0x140u) {
        Rdna2Opcode op = DecodeVop2Opcode(static_cast<uint16_t>(raw - 0x100u));
        if (op != Rdna2Opcode::Unknown) {
            out_src_count = 2;
            return op;
        }
    }
    // Promoted VOP1 (0x180..0x1FF)
    if (raw >= 0x180u && raw < 0x200u) {
        Rdna2Opcode op = DecodeVop1Opcode(static_cast<uint16_t>(raw - 0x180u));
        if (op != Rdna2Opcode::Unknown) {
            out_src_count = 1;
            return op;
        }
    }
    out_src_count = 3;
    return Rdna2Opcode::Unknown;
}

uint16_t EncodeVop3Opcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_CNDMASK_B32:      return 0x101;
        case Rdna2Opcode::V_MAD_F32:          return 0x141;
        case Rdna2Opcode::V_BFE_U32:          return 0x148;
        case Rdna2Opcode::V_BFE_I32:          return 0x149;
        case Rdna2Opcode::V_BFI_B32:          return 0x14A;
        case Rdna2Opcode::V_FMA_F32:          return 0x14B;
        case Rdna2Opcode::V_ALIGNBYTE_B32:    return 0x14C;
        case Rdna2Opcode::V_ALIGNBIT_B32:     return 0x14F;
        case Rdna2Opcode::V_DIV_FMAS_F32:     return 0x162;
        case Rdna2Opcode::V_MUL_LO_U32:       return 0x169;
        case Rdna2Opcode::V_MAD_U64_U32:      return 0x16C;
        case Rdna2Opcode::V_MAD_I64_I32:      return 0x16D;
        case Rdna2Opcode::V_ADD_NC_U16:       return 0x303;
        case Rdna2Opcode::V_SUB_NC_U16:       return 0x304;
        case Rdna2Opcode::V_MBCNT_LO_U32_B32: return 0x305;
        case Rdna2Opcode::V_MBCNT_HI_U32_B32: return 0x306;
        case Rdna2Opcode::V_MIN_U16:          return 0x309;
        case Rdna2Opcode::V_MAX_U16:          return 0x30A;
        case Rdna2Opcode::V_MIN_I16:          return 0x30B;
        case Rdna2Opcode::V_MAX_I16:          return 0x30C;
        case Rdna2Opcode::V_MUL_LO_U16:       return 0x312;
        case Rdna2Opcode::V_ASHRREV_I16:      return 0x313;
        case Rdna2Opcode::V_LSHLREV_B16:      return 0x314;
        case Rdna2Opcode::V_LSHRREV_B16:      return 0x315;
        case Rdna2Opcode::V_MAD_U16:          return 0x340;
        case Rdna2Opcode::V_FMA_F16:          return 0x34B;
        case Rdna2Opcode::V_READLANE_B32:       return 0x360;
        case Rdna2Opcode::V_CVT_PKNORM_I16_F16: return 0x366;
        case Rdna2Opcode::V_CVT_PKNORM_U16_F16: return 0x367;
        case Rdna2Opcode::V_CVT_PKNORM_I16_F32: return 0x368;
        case Rdna2Opcode::V_CVT_PKNORM_U16_F32: return 0x369;
        case Rdna2Opcode::V_ADD3_U32:           return 0x36D;
        case Rdna2Opcode::V_DOT2_F32_F16:     return 0x373;
        case Rdna2Opcode::V_PERMLANE16_B32:   return 0x377;
        case Rdna2Opcode::V_PERMLANEX16_B32:  return 0x378;
        default: break;
    }
    uint16_t v2 = EncodeVop2Opcode(op);
    if (v2 != 0xFFFFu) return static_cast<uint16_t>(0x100u + v2);
    uint16_t v1 = EncodeVop1Opcode(op);
    if (v1 != 0xFFFFu) return static_cast<uint16_t>(0x180u + v1);
    uint16_t vc = EncodeVopcOpcode(op);
    if (vc != 0xFFFFu) return vc;
    return 0xFFFFu;
}

Rdna2Opcode DecodeVop3pOpcode(uint16_t raw, uint8_t& out_src_count) {
    switch (raw) {
        case 0x04: out_src_count = 2; return Rdna2Opcode::V_PK_LSHLREV_B16;
        case 0x05: out_src_count = 2; return Rdna2Opcode::V_PK_LSHRREV_B16;
        case 0x0A: out_src_count = 2; return Rdna2Opcode::V_PK_ADD_U16;
        case 0x0E: out_src_count = 3; return Rdna2Opcode::V_PK_FMA_F16;
        case 0x0F: out_src_count = 2; return Rdna2Opcode::V_PK_ADD_F16;
        case 0x10: out_src_count = 2; return Rdna2Opcode::V_PK_MUL_F16;
        case 0x20: out_src_count = 3; return Rdna2Opcode::V_FMA_MIX_F32;
        case 0x21: out_src_count = 3; return Rdna2Opcode::V_FMA_MIXLO_F16;
        case 0x22: out_src_count = 3; return Rdna2Opcode::V_FMA_MIXHI_F16;
        default:   out_src_count = 2; return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeVop3pOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_PK_LSHLREV_B16: return 0x04;
        case Rdna2Opcode::V_PK_LSHRREV_B16: return 0x05;
        case Rdna2Opcode::V_PK_ADD_U16:     return 0x0A;
        case Rdna2Opcode::V_PK_FMA_F16:     return 0x0E;
        case Rdna2Opcode::V_PK_ADD_F16:     return 0x0F;
        case Rdna2Opcode::V_PK_MUL_F16:     return 0x10;
        case Rdna2Opcode::V_FMA_MIX_F32:    return 0x20;
        case Rdna2Opcode::V_FMA_MIXLO_F16:  return 0x21;
        case Rdna2Opcode::V_FMA_MIXHI_F16:  return 0x22;
        default:                            return 0xFFFF;
    }
}

Rdna2Opcode DecodeSmemOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::S_LOAD_DWORD;
        case 0x01: return Rdna2Opcode::S_BUFFER_LOAD_DWORDX2;  // s_load_dwordx2
        case 0x02: return Rdna2Opcode::S_BUFFER_LOAD_DWORDX4;  // s_load_dwordx4
        case 0x03: return Rdna2Opcode::S_BUFFER_LOAD_DWORDX8;  // s_load_dwordx8
        case 0x04: return Rdna2Opcode::S_BUFFER_LOAD_DWORDX16; // s_load_dwordx16
        case 0x08: return Rdna2Opcode::S_BUFFER_LOAD_DWORD;
        case 0x09: return Rdna2Opcode::S_BUFFER_LOAD_DWORDX2;
        case 0x0A: return Rdna2Opcode::S_BUFFER_LOAD_DWORDX4;
        case 0x0B: return Rdna2Opcode::S_BUFFER_LOAD_DWORDX8;
        case 0x0C: return Rdna2Opcode::S_BUFFER_LOAD_DWORDX16;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeSmemOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_LOAD_DWORD:           return 0x00;
        case Rdna2Opcode::S_BUFFER_LOAD_DWORD:    return 0x08;
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX2:  return 0x09;
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX4:  return 0x0A;
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX8:  return 0x0B;
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX16: return 0x0C;
        default:                                  return 0xFFFF;
    }
}

Rdna2Opcode DecodeMubufOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::BUFFER_LOAD_FORMAT_X;
        case 0x01: return Rdna2Opcode::BUFFER_LOAD_FORMAT_XY;
        case 0x02: return Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZ;
        case 0x03: return Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW;
        case 0x08: return Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_X;
        case 0x09: return Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XY;
        case 0x0A: return Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZ;
        case 0x0B: return Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZW;
        case 0x0C: return Rdna2Opcode::BUFFER_LOAD_DWORD;        // GFX10.3 buffer_load_dword
        case 0x0D: return Rdna2Opcode::BUFFER_LOAD_FORMAT_XY;    // GFX10.3 buffer_load_dwordx2
        case 0x0E: return Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW;  // GFX10.3 buffer_load_dwordx4 (4 dwords)
        case 0x0F: return Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZ;   // GFX10.3 buffer_load_dwordx3 (3 dwords)
        case 0x14: return Rdna2Opcode::BUFFER_LOAD_DWORD;
        case 0x18: return Rdna2Opcode::BUFFER_STORE_BYTE_D16_HI;   // GFX10.3 buffer_store_byte (low 8 bits)
        case 0x19: return Rdna2Opcode::BUFFER_STORE_BYTE_D16_HI;
        case 0x1A: return Rdna2Opcode::BUFFER_STORE_SHORT_D16_HI;  // GFX10.3 buffer_store_short (low 16 bits)
        case 0x1B: return Rdna2Opcode::BUFFER_STORE_SHORT_D16_HI;
        case 0x1C: return Rdna2Opcode::BUFFER_STORE_DWORD;
        case 0x1D: return Rdna2Opcode::BUFFER_STORE_DWORD;         // GFX10.3 buffer_store_dwordx2
        case 0x1E: return Rdna2Opcode::BUFFER_STORE_DWORD;         // GFX10.3 buffer_store_dwordx3
        case 0x1F: return Rdna2Opcode::BUFFER_STORE_DWORD;         // GFX10.3 buffer_store_dwordx4
        case 0x20: return Rdna2Opcode::BUFFER_LOAD_UBYTE_D16;
        case 0x21: return Rdna2Opcode::BUFFER_LOAD_UBYTE_D16_HI;
        case 0x22: return Rdna2Opcode::BUFFER_LOAD_SBYTE_D16;
        case 0x23: return Rdna2Opcode::BUFFER_LOAD_SBYTE_D16_HI;
        case 0x24: return Rdna2Opcode::BUFFER_LOAD_SHORT_D16;
        case 0x25: return Rdna2Opcode::BUFFER_LOAD_SHORT_D16_HI;
        // GFX10.3 MUBUF buffer_atomic_* (0x30..0x3D, 0x3F, 0x40, 0x4D; distinguished by raw_opcode)
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
        case 0x38:
        case 0x39:
        case 0x3A:
        case 0x3B:
        case 0x3C:
        case 0x3D:
        case 0x3F:
        case 0x40:
        case 0x4D: return Rdna2Opcode::BUFFER_LOAD_DWORD;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeMubufOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_X:        return 0x00;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XY:       return 0x01;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZ:      return 0x02;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW:     return 0x03;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_X:    return 0x08;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XY:   return 0x09;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZ:  return 0x0A;
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZW: return 0x0B;
        case Rdna2Opcode::BUFFER_LOAD_DWORD:           return 0x14;
        case Rdna2Opcode::BUFFER_STORE_BYTE_D16_HI:    return 0x19;
        case Rdna2Opcode::BUFFER_STORE_SHORT_D16_HI:   return 0x1B;
        case Rdna2Opcode::BUFFER_STORE_DWORD:          return 0x1C;
        case Rdna2Opcode::BUFFER_LOAD_UBYTE_D16:       return 0x20;
        case Rdna2Opcode::BUFFER_LOAD_UBYTE_D16_HI:    return 0x21;
        case Rdna2Opcode::BUFFER_LOAD_SBYTE_D16:       return 0x22;
        case Rdna2Opcode::BUFFER_LOAD_SBYTE_D16_HI:    return 0x23;
        case Rdna2Opcode::BUFFER_LOAD_SHORT_D16:       return 0x24;
        case Rdna2Opcode::BUFFER_LOAD_SHORT_D16_HI:    return 0x25;
        default:                                       return 0xFFFF;
    }
}

Rdna2Opcode DecodeMtbufOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::TBUFFER_LOAD_FORMAT_X;
        case 0x01: return Rdna2Opcode::TBUFFER_LOAD_FORMAT_XY;
        case 0x02: return Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZ;
        case 0x03: return Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZW;
        case 0x08: return Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_X;
        case 0x09: return Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY;
        case 0x0A: return Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZ;
        case 0x0B: return Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeMtbufOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_X:        return 0x00;
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XY:       return 0x01;
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZ:      return 0x02;
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZW:     return 0x03;
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_X:    return 0x08;
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY:   return 0x09;
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZ:  return 0x0A;
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW: return 0x0B;
        default:                                        return 0xFFFF;
    }
}

Rdna2Opcode DecodeVintrpOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::V_INTERP_P1_F32;
        case 0x01: return Rdna2Opcode::V_INTERP_P2_F32;
        case 0x02: return Rdna2Opcode::V_INTERP_MOV_F32;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeVintrpOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::V_INTERP_P1_F32:  return 0x00;
        case Rdna2Opcode::V_INTERP_P2_F32:  return 0x01;
        case Rdna2Opcode::V_INTERP_MOV_F32: return 0x02;
        default:                            return 0xFFFF;
    }
}

Rdna2Opcode DecodeFlatOpcode(uint16_t raw, uint8_t seg) {
    if (seg == 2u) {
        switch (raw) {
            case 0x0C: return Rdna2Opcode::GLOBAL_LOAD_DWORD;
            case 0x1C: return Rdna2Opcode::GLOBAL_STORE_DWORD;
            default:   return Rdna2Opcode::Unknown;
        }
    }
    if (seg == 1u) {
        switch (raw) {
            case 0x0C: return Rdna2Opcode::SCRATCH_LOAD_DWORD;
            case 0x1C: return Rdna2Opcode::SCRATCH_STORE_DWORD;
            default:   return Rdna2Opcode::Unknown;
        }
    }
    if (seg == 0u) {
        switch (raw) {
            case 0x0C: return Rdna2Opcode::FLAT_LOAD_DWORD;
            case 0x1C: return Rdna2Opcode::FLAT_STORE_DWORD;
            default:   return Rdna2Opcode::Unknown;
        }
    }
    return Rdna2Opcode::Unknown;
}

uint16_t EncodeFlatOpcode(Rdna2Opcode op, uint8_t& out_seg) {
    switch (op) {
        case Rdna2Opcode::GLOBAL_LOAD_DWORD:   out_seg = 2; return 0x0C;
        case Rdna2Opcode::GLOBAL_STORE_DWORD:  out_seg = 2; return 0x1C;
        case Rdna2Opcode::SCRATCH_LOAD_DWORD:  out_seg = 1; return 0x0C;
        case Rdna2Opcode::SCRATCH_STORE_DWORD: out_seg = 1; return 0x1C;
        case Rdna2Opcode::FLAT_LOAD_DWORD:     out_seg = 0; return 0x0C;
        case Rdna2Opcode::FLAT_STORE_DWORD:    out_seg = 0; return 0x1C;
        default:                               out_seg = 0; return 0xFFFF;
    }
}

Rdna2Opcode DecodeMimgOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::IMAGE_LOAD;
        case 0x01: return Rdna2Opcode::IMAGE_LOAD_MIP;
        case 0x08: return Rdna2Opcode::IMAGE_STORE;
        case 0x0E: return Rdna2Opcode::IMAGE_LOAD_MIP;  // GFX10.3 image_get_resinfo (raw_opcode == 0x0E)
        case 0x11: return Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP;
        // GFX10.3 image_sample* & image_sample*_o (0x20..0x27, 0x30..0x37)
        case 0x20:
        case 0x24:  // image_sample_l
        case 0x30:  // image_sample_o
        case 0x34:  // image_sample_l_o
            return Rdna2Opcode::IMAGE_SAMPLE;
        case 0x21:
        case 0x31:  // image_sample_cl_o
            return Rdna2Opcode::IMAGE_SAMPLE_CL;
        case 0x22:  // image_sample_d
        case 0x23:
        case 0x32:  // image_sample_d_o
        case 0x33:  // image_sample_d_cl_o
            return Rdna2Opcode::IMAGE_SAMPLE_D_CL;
        case 0x25:
        case 0x27:
        case 0x35:  // image_sample_b_o
        case 0x37:  // image_sample_lz_o
            return Rdna2Opcode::IMAGE_SAMPLE_LZ;
        case 0x26:
        case 0x36:  // image_sample_b_cl_o
            return Rdna2Opcode::IMAGE_SAMPLE_B_CL;
        // GFX10.3 image_sample_c* & image_sample_c*_o (0x28..0x2F, 0x38..0x3F)
        case 0x28:  // image_sample_c
        case 0x2C:  // image_sample_c_l
        case 0x2F:
        case 0x38:  // image_sample_c_o
        case 0x3C:  // image_sample_c_l_o
        case 0x3F:  // image_sample_c_lz_o
            return Rdna2Opcode::IMAGE_SAMPLE_C_LZ;
        case 0x29:
        case 0x39:  // image_sample_c_cl_o
            return Rdna2Opcode::IMAGE_SAMPLE_C_CL;
        case 0x2A:  // image_sample_c_d
        case 0x2B:
        case 0x3A:  // image_sample_c_d_o
        case 0x3B:  // image_sample_c_d_cl_o
            return Rdna2Opcode::IMAGE_SAMPLE_C_D_CL;
        case 0x2D:  // image_sample_c_b
        case 0x2E:
        case 0x3D:  // image_sample_c_b_o
        case 0x3E:  // image_sample_c_b_cl_o
            return Rdna2Opcode::IMAGE_SAMPLE_C_B_CL;
        // GFX10.3 image_gather4* (0x40..0x47, 0x50..0x57 non-cmp; 0x48..0x4F, 0x58..0x5F cmp)
        case 0x40: case 0x41: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x50: case 0x51: case 0x54: case 0x55: case 0x56: case 0x57:
            return Rdna2Opcode::IMAGE_SAMPLE;
        case 0x48: case 0x49: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            return Rdna2Opcode::IMAGE_SAMPLE_C_LZ;
        case 0xE6: return Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeMimgOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::IMAGE_LOAD:              return 0x00;
        case Rdna2Opcode::IMAGE_LOAD_MIP:          return 0x01;
        case Rdna2Opcode::IMAGE_STORE:             return 0x08;
        case Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP:    return 0x11;
        case Rdna2Opcode::IMAGE_SAMPLE:            return 0x20;
        case Rdna2Opcode::IMAGE_SAMPLE_CL:         return 0x21;
        case Rdna2Opcode::IMAGE_SAMPLE_D_CL:       return 0x23;
        case Rdna2Opcode::IMAGE_SAMPLE_LZ:         return 0x25;
        case Rdna2Opcode::IMAGE_SAMPLE_B_CL:       return 0x26;
        case Rdna2Opcode::IMAGE_SAMPLE_C_CL:       return 0x29;
        case Rdna2Opcode::IMAGE_SAMPLE_C_D_CL:     return 0x2B;
        case Rdna2Opcode::IMAGE_SAMPLE_C_B_CL:     return 0x2E;
        case Rdna2Opcode::IMAGE_SAMPLE_C_LZ:       return 0x2F;
        case Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY: return 0xE6;
        default:                                   return 0xFFFF;
    }
}

Rdna2Opcode DecodeDsOpcode(uint16_t raw) {
    switch (raw) {
        case 0x00: return Rdna2Opcode::DS_ADD_U32;
        case 0x0C: return Rdna2Opcode::DS_MSKOR_B32;
        case 0x0D: return Rdna2Opcode::DS_WRITE_B32;
        case 0x14: return Rdna2Opcode::DS_NOP;
        case 0x20: return Rdna2Opcode::DS_ADD_RTN_U32;
        case 0x2C: return Rdna2Opcode::DS_MSKOR_RTN_B32;
        case 0x2E: return Rdna2Opcode::DS_WRXCHG2_RTN_B32;
        case 0x2F: return Rdna2Opcode::DS_WRXCHG2ST64_RTN_B32;
        case 0x35: return Rdna2Opcode::DS_SWIZZLE_B32;
        case 0x36: return Rdna2Opcode::DS_READ_B32;
        case 0xA2: return Rdna2Opcode::DS_READ_U8_D16;
        case 0xA3: return Rdna2Opcode::DS_READ_U8_D16_HI;
        case 0xA4: return Rdna2Opcode::DS_READ_I8_D16;
        case 0xA5: return Rdna2Opcode::DS_READ_I8_D16_HI;
        case 0xA6: return Rdna2Opcode::DS_READ_U16_D16;
        case 0xA7: return Rdna2Opcode::DS_READ_U16_D16_HI;
        case 0xDE: return Rdna2Opcode::DS_WRITE_B96;
        case 0xDF: return Rdna2Opcode::DS_WRITE_B128;
        case 0xFE: return Rdna2Opcode::DS_READ_B96;
        case 0xFF: return Rdna2Opcode::DS_READ_B128;
        default:   return Rdna2Opcode::Unknown;
    }
}

uint16_t EncodeDsOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::DS_ADD_U32:             return 0x00;
        case Rdna2Opcode::DS_MSKOR_B32:           return 0x0C;
        case Rdna2Opcode::DS_WRITE_B32:           return 0x0D;
        case Rdna2Opcode::DS_NOP:                 return 0x14;
        case Rdna2Opcode::DS_ADD_RTN_U32:         return 0x20;
        case Rdna2Opcode::DS_MSKOR_RTN_B32:       return 0x2C;
        case Rdna2Opcode::DS_WRXCHG2_RTN_B32:     return 0x2E;
        case Rdna2Opcode::DS_WRXCHG2ST64_RTN_B32: return 0x2F;
        case Rdna2Opcode::DS_SWIZZLE_B32:         return 0x35;
        case Rdna2Opcode::DS_READ_B32:            return 0x36;
        case Rdna2Opcode::DS_READ_U8_D16:         return 0xA2;
        case Rdna2Opcode::DS_READ_U8_D16_HI:      return 0xA3;
        case Rdna2Opcode::DS_READ_I8_D16:         return 0xA4;
        case Rdna2Opcode::DS_READ_I8_D16_HI:      return 0xA5;
        case Rdna2Opcode::DS_READ_U16_D16:        return 0xA6;
        case Rdna2Opcode::DS_READ_U16_D16_HI:     return 0xA7;
        case Rdna2Opcode::DS_WRITE_B96:           return 0xDE;
        case Rdna2Opcode::DS_WRITE_B128:          return 0xDF;
        case Rdna2Opcode::DS_READ_B96:            return 0xFE;
        case Rdna2Opcode::DS_READ_B128:           return 0xFF;
        default:                                  return 0xFFFF;
    }
}

std::string FormatExpTarget(uint8_t target) {
    if (target <= 7u) {
        return "mrt" + std::to_string(target);
    }
    if (target == 8u) return "mrtz";
    if (target == 9u) return "null";
    if (target >= 12u && target <= 15u) {
        return "pos" + std::to_string(target - 12u);
    }
    if (target == 20u) return "prim";
    if (target >= 32u && target <= 63u) {
        return "param" + std::to_string(target - 32u);
    }
    return "tgt" + std::to_string(target);
}

}  // namespace

// ---------------------------------------------------------------------------
// XS2\0 Container Serialization & Parsing
// ---------------------------------------------------------------------------
Status ParseXs2Container(const std::vector<uint8_t>& bytes, Xs2ShaderContainer& out_container) {
    out_container = Xs2ShaderContainer{};
    if (bytes.size() < kXs2HeaderSizeBytes) {
        return Status::Error(StatusCode::ParseError, "XS2 container smaller than 40-byte header");
    }
    if (std::memcmp(bytes.data(), "XS2\0", 4) != 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Invalid XS2 container magic (expected 'XS2\\0')");
    }

    const uint32_t version = ReadLe32(bytes.data() + 4);
    const uint32_t stage_raw = ReadLe32(bytes.data() + 8);
    const uint32_t sgpr_count = ReadLe32(bytes.data() + 12);
    const uint32_t vgpr_count = ReadLe32(bytes.data() + 16);
    const uint32_t tg_x = ReadLe32(bytes.data() + 20);
    const uint32_t tg_y = ReadLe32(bytes.data() + 24);
    const uint32_t tg_z = ReadLe32(bytes.data() + 28);
    const uint32_t entry_offset = ReadLe32(bytes.data() + 32);
    const uint32_t code_size_bytes = ReadLe32(bytes.data() + 36);

    if (version == 0 || version > kXs2ContainerVersionV2) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported XS2 container version");
    }

    const auto stage = static_cast<ShaderStage>(stage_raw);
    if (stage != ShaderStage::Vertex && stage != ShaderStage::Pixel && stage != ShaderStage::Compute &&
        stage != ShaderStage::Geometry && stage != ShaderStage::Hull && stage != ShaderStage::Domain &&
        stage != ShaderStage::Mesh && stage != ShaderStage::Amplification) {
        return Status::Error(StatusCode::ParseError, "Unsupported XS2 shader stage (expected VS/PS/CS/GS/HS/DS/MS/AS)");
    }

    if (entry_offset < kXs2HeaderSizeBytes || (entry_offset & 3u) != 0u) {
        return Status::Error(StatusCode::ParseError, "Invalid XS2 entry_offset");
    }
    if (code_size_bytes == 0 || (code_size_bytes & 3u) != 0u) {
        return Status::Error(StatusCode::ParseError, "Invalid XS2 code_size_bytes (must be non-zero multiple of 4)");
    }
    const uint64_t code_end = static_cast<uint64_t>(entry_offset) + static_cast<uint64_t>(code_size_bytes);
    if (code_end > bytes.size()) {
        return Status::Error(StatusCode::ParseError, "XS2 code section extends past end of container");
    }

    uint32_t wave_size = ((sgpr_count & kXs2SgprFlagWave64) != 0u) ? 64u : 32u;
    const uint32_t decoded_sgprs = sgpr_count & ~kXs2SgprFlagWave64;
    if (version >= kXs2ContainerVersionV2 && entry_offset >= kXs2HeaderSizeBytesV2) {
        const uint32_t ws = ReadLe32(bytes.data() + kXs2HeaderSizeBytes);
        if (ws == 32u || ws == 64u) {
            wave_size = ws;
        }
    }

    out_container.version = version;
    out_container.stage = stage;
    out_container.sgpr_count = decoded_sgprs;
    out_container.vgpr_count = vgpr_count;
    out_container.threadgroup_x = tg_x == 0 ? 1u : tg_x;
    out_container.threadgroup_y = tg_y == 0 ? 1u : tg_y;
    out_container.threadgroup_z = tg_z == 0 ? 1u : tg_z;
    out_container.entry_offset = entry_offset;
    out_container.wave_size = wave_size;

    const uint32_t word_count = code_size_bytes / 4u;
    out_container.code_words.resize(word_count);
    for (uint32_t i = 0; i < word_count; ++i) {
        out_container.code_words[i] = ReadLe32(bytes.data() + entry_offset + i * 4u);
    }
    return Status::Ok();
}

std::vector<uint8_t> BuildXs2Container(const Xs2ShaderContainer& container) {
    const uint32_t ver = container.version ? container.version : kXs2ContainerVersion;
    const uint32_t min_header = (ver >= kXs2ContainerVersionV2) ? kXs2HeaderSizeBytesV2 : kXs2HeaderSizeBytes;
    const uint32_t entry_offset = container.entry_offset >= min_header
                                      ? ((container.entry_offset + 3u) & ~3u)
                                      : min_header;
    const uint32_t code_size_bytes = static_cast<uint32_t>(container.code_words.size() * 4u);
    const uint32_t wave_size = (container.wave_size == 64u) ? 64u : 32u;
    const uint32_t encoded_sgprs = (wave_size == 64u)
                                       ? (container.sgpr_count | kXs2SgprFlagWave64)
                                       : (container.sgpr_count & ~kXs2SgprFlagWave64);

    std::vector<uint8_t> out;
    out.reserve(entry_offset + code_size_bytes);
    out.push_back('X');
    out.push_back('S');
    out.push_back('2');
    out.push_back('\0');
    AppendLe32(out, ver);
    AppendLe32(out, static_cast<uint32_t>(container.stage));
    AppendLe32(out, encoded_sgprs);
    AppendLe32(out, container.vgpr_count);
    AppendLe32(out, container.threadgroup_x ? container.threadgroup_x : 1u);
    AppendLe32(out, container.threadgroup_y ? container.threadgroup_y : 1u);
    AppendLe32(out, container.threadgroup_z ? container.threadgroup_z : 1u);
    AppendLe32(out, entry_offset);
    AppendLe32(out, code_size_bytes);

    if (ver >= kXs2ContainerVersionV2) {
        AppendLe32(out, wave_size);
    }

    while (out.size() < entry_offset) {
        out.push_back(0u);
    }
    for (uint32_t w : container.code_words) {
        AppendLe32(out, w);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Xbox-Wrapped DXBC / DXIL Shader Container Parser & Builder
// ---------------------------------------------------------------------------
namespace {

bool ParseDxbcSignatureChunk(
    const uint8_t* data,
    size_t size,
    bool is_v1,
    std::vector<XboxSignatureElement>& out_elements) {
    out_elements.clear();
    if (!data || size < 8u) return false;
    const uint32_t count = ReadLe32(data);
    const uint32_t param_offset = ReadLe32(data + 4u);
    const uint32_t table_off = (param_offset == 0u) ? 8u : param_offset;
    const uint32_t elem_size = is_v1 ? 32u : 24u;
    if (count > 256u || table_off < 8u || table_off > size ||
        static_cast<uint64_t>(table_off) + static_cast<uint64_t>(count) * elem_size > size) {
        return false;
    }

    out_elements.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* rec = data + table_off + static_cast<size_t>(i) * elem_size;
        XboxSignatureElement el{};
        uint32_t name_off = 0;
        if (is_v1) {
            el.stream = ReadLe32(rec + 0);
            name_off = ReadLe32(rec + 4);
            el.semantic_index = ReadLe32(rec + 8);
            el.system_value_type = ReadLe32(rec + 12);
            el.component_type = ReadLe32(rec + 16);
            el.register_index = ReadLe32(rec + 20);
            el.mask = rec[24];
            el.rw_mask = rec[25];
        } else {
            el.stream = 0;
            name_off = ReadLe32(rec + 0);
            el.semantic_index = ReadLe32(rec + 4);
            el.system_value_type = ReadLe32(rec + 8);
            el.component_type = ReadLe32(rec + 12);
            el.register_index = ReadLe32(rec + 16);
            el.mask = rec[20];
            el.rw_mask = rec[21];
        }
        // Reject incomplete/truncated semantic string offsets or unterminated strings (Leonida #4572)
        if (name_off < 8u || name_off >= size) {
            out_elements.clear();
            return false;
        }
        size_t end = name_off;
        while (end < size && data[end] != 0) ++end;
        if (end >= size || end == name_off) {
            out_elements.clear();
            return false;
        }
        el.semantic_name.assign(reinterpret_cast<const char*>(data + name_off), end - name_off);
        out_elements.push_back(std::move(el));
    }
    return true;
}

std::vector<uint8_t> BuildDxbcSignatureV1Chunk(const std::vector<XboxSignatureElement>& elements) {
    std::vector<uint8_t> out;
    const uint32_t count = static_cast<uint32_t>(elements.size());
    const uint32_t header_and_table = 8u + count * 32u;

    std::vector<uint8_t> string_table;
    std::vector<uint32_t> name_offsets;
    name_offsets.reserve(count);
    for (const auto& el : elements) {
        name_offsets.push_back(header_and_table + static_cast<uint32_t>(string_table.size()));
        for (char c : el.semantic_name) {
            string_table.push_back(static_cast<uint8_t>(c));
        }
        string_table.push_back(0u);
    }
    while ((string_table.size() & 3u) != 0u) {
        string_table.push_back(0u);
    }

    out.reserve(header_and_table + string_table.size());
    AppendLe32(out, count);
    AppendLe32(out, 8u);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& el = elements[i];
        AppendLe32(out, el.stream);
        AppendLe32(out, name_offsets[i]);
        AppendLe32(out, el.semantic_index);
        AppendLe32(out, el.system_value_type);
        AppendLe32(out, el.component_type);
        AppendLe32(out, el.register_index);
        out.push_back(el.mask);
        out.push_back(el.rw_mask);
        out.push_back(0u);
        out.push_back(0u);
        AppendLe32(out, 0u);  // min_precision
    }
    out.insert(out.end(), string_table.begin(), string_table.end());
    return out;
}

void ReadStaticSamplerRecord(const uint8_t* p, XboxStaticSamplerInfo& out) {
    out.filter = ReadLe32(p + 0);
    out.address_u = ReadLe32(p + 4);
    out.address_v = ReadLe32(p + 8);
    out.address_w = ReadLe32(p + 12);
    out.mip_lod_bias = BitsToFloat(ReadLe32(p + 16));
    out.max_anisotropy = ReadLe32(p + 20);
    out.comparison_func = ReadLe32(p + 24);
    out.border_color = ReadLe32(p + 28);
    out.min_lod = BitsToFloat(ReadLe32(p + 32));
    out.max_lod = BitsToFloat(ReadLe32(p + 36));
    out.shader_register = ReadLe32(p + 40);
    out.register_space = ReadLe32(p + 44);
    out.shader_visibility = ReadLe32(p + 48);
}

void AppendStaticSamplerRecord(std::vector<uint8_t>& out, const XboxStaticSamplerInfo& s) {
    AppendLe32(out, s.filter);
    AppendLe32(out, s.address_u);
    AppendLe32(out, s.address_v);
    AppendLe32(out, s.address_w);
    AppendLe32(out, FloatToBits(s.mip_lod_bias));
    AppendLe32(out, s.max_anisotropy);
    AppendLe32(out, s.comparison_func);
    AppendLe32(out, s.border_color);
    AppendLe32(out, FloatToBits(s.min_lod));
    AppendLe32(out, FloatToBits(s.max_lod));
    AppendLe32(out, s.shader_register);
    AppendLe32(out, s.register_space);
    AppendLe32(out, s.shader_visibility);
}

void ParseStsmChunk(const uint8_t* data, size_t size, std::vector<XboxStaticSamplerInfo>& out_samplers) {
    if (size < 4u) return;
    const uint32_t count = ReadLe32(data);
    if (count > 256u || 4u + static_cast<size_t>(count) * 52u > size) return;
    for (uint32_t i = 0; i < count; ++i) {
        XboxStaticSamplerInfo s{};
        ReadStaticSamplerRecord(data + 4u + static_cast<size_t>(i) * 52u, s);
        out_samplers.push_back(s);
    }
}

void ParseRts0Chunk(const uint8_t* data, size_t size, std::vector<XboxStaticSamplerInfo>& out_samplers) {
    // Public D3D12 serialized root signature (RTS0) header: 24 bytes
    // [0]: version (1 or 2), [4]: num_params, [8]: params_offset,
    // [12]: num_static_samplers, [16]: static_samplers_offset, [20]: flags
    if (size < 24u) return;
    const uint32_t version = ReadLe32(data + 0);
    if (version != 1u && version != 2u) return;
    const uint32_t num_samplers = ReadLe32(data + 12);
    const uint32_t samplers_off = ReadLe32(data + 16);
    if (num_samplers == 0u || num_samplers > 256u) return;
    if (static_cast<uint64_t>(samplers_off) + static_cast<uint64_t>(num_samplers) * 52ull > size) return;
    for (uint32_t i = 0; i < num_samplers; ++i) {
        XboxStaticSamplerInfo s{};
        ReadStaticSamplerRecord(data + samplers_off + static_cast<size_t>(i) * 52u, s);
        out_samplers.push_back(s);
    }
}

void ParsePsv0Chunk(const uint8_t* data, size_t size, XboxPsv0Info& out_psv) {
    // Public DXIL PSV0 layout:
    // [0..3]: runtime_info_size (>= 24 for PSVRuntimeInfo0, >= 36 for v1, >= 48 for v2, >= 52 for v3)
    // [4..19]: stage-specific 16-byte union
    // [20..23]: MinimumExpectedWaveLaneCount
    // [24..27]: MaximumExpectedWaveLaneCount (0 or 0xFFFFFFFF = unbounded/unspecified)
    // [28]: ShaderStage (v1+)
    // [40..43]: NumThreadsX (v2+)
    // [44..47]: NumThreadsY (v2+)
    // [48..51]: NumThreadsZ (v2+)
    out_psv = XboxPsv0Info{};
    if (!data || size < 28u) return;
    const uint32_t runtime_info_size = ReadLe32(data + 0);
    if (runtime_info_size < 24u || 4u + static_cast<size_t>(runtime_info_size) > size) {
        return;
    }
    out_psv.present = true;
    out_psv.runtime_info_size = runtime_info_size;
    out_psv.min_wave_lane_count = ReadLe32(data + 20u);
    out_psv.max_wave_lane_count = ReadLe32(data + 24u);
    if (runtime_info_size >= 36u && size >= 40u) {
        out_psv.shader_stage = data[28u];
    }
    if (runtime_info_size >= 48u && size >= 52u) {
        out_psv.num_threads_x = ReadLe32(data + 40u);
        out_psv.num_threads_y = ReadLe32(data + 44u);
        out_psv.num_threads_z = ReadLe32(data + 48u);
    }
    const size_t res_pos = 4u + static_cast<size_t>(runtime_info_size);
    if (res_pos + 4u <= size) {
        const uint32_t res_count = ReadLe32(data + res_pos);
        if (res_count > 0u && res_count <= 512u && res_pos + 8u <= size) {
            const uint32_t res_bind_size = ReadLe32(data + res_pos + 4u);
            const size_t table_off = res_pos + 8u;
            if (res_bind_size >= 16u &&
                table_off + static_cast<size_t>(res_count) * static_cast<size_t>(res_bind_size) <= size) {
                out_psv.resource_bindings.reserve(res_count);
                for (uint32_t i = 0u; i < res_count; ++i) {
                    const uint8_t* e = data + table_off + static_cast<size_t>(i) * static_cast<size_t>(res_bind_size);
                    XboxPsv0ResourceBinding rb{};
                    rb.res_type = ReadLe32(e + 0u);
                    rb.space = ReadLe32(e + 4u);
                    rb.lower_bound = ReadLe32(e + 8u);
                    rb.upper_bound = ReadLe32(e + 12u);
                    if (res_bind_size >= 24u) {
                        rb.res_kind = ReadLe32(e + 16u);
                        rb.res_flags = ReadLe32(e + 20u);
                    }
                    out_psv.resource_bindings.push_back(rb);
                }
            }
        }
    }
}

std::vector<uint8_t> BuildPsv0Chunk(const XboxPsv0Info& psv) {
    // Emit PSVRuntimeInfo2 (48-byte runtime info + 4-byte size prefix + 4-byte resource count = 56 bytes)
    std::vector<uint8_t> out;
    out.reserve(56u);
    AppendLe32(out, 48u);  // runtime_info_size = sizeof(PSVRuntimeInfo2)
    // 16-byte stage-specific union (zeroed)
    AppendLe32(out, 0u);
    AppendLe32(out, 0u);
    AppendLe32(out, 0u);
    AppendLe32(out, 0u);
    AppendLe32(out, psv.min_wave_lane_count);
    AppendLe32(out, psv.max_wave_lane_count);
    // PSVRuntimeInfo1 (12 bytes): stage + flags + signature counts
    AppendLe32(out, static_cast<uint32_t>(psv.shader_stage));
    AppendLe32(out, 0u);
    AppendLe32(out, 0u);
    // PSVRuntimeInfo2 (12 bytes): NumThreadsX, NumThreadsY, NumThreadsZ
    AppendLe32(out, psv.num_threads_x);
    AppendLe32(out, psv.num_threads_y);
    AppendLe32(out, psv.num_threads_z);
    // Resource binding count + optional 24-byte PSVResourceBindInfo1 table
    const uint32_t res_count = static_cast<uint32_t>(psv.resource_bindings.size());
    AppendLe32(out, res_count);
    if (res_count > 0u) {
        AppendLe32(out, 24u);
        for (const auto& rb : psv.resource_bindings) {
            AppendLe32(out, rb.res_type);
            AppendLe32(out, rb.space);
            AppendLe32(out, rb.lower_bound);
            AppendLe32(out, rb.upper_bound);
            AppendLe32(out, rb.res_kind);
            AppendLe32(out, rb.res_flags);
        }
    }
    return out;
}

// RFC 1321 MD5 compression function used for DXBC / DXIL container digest.
void DxbcMd5Compress(uint32_t state[4], const uint8_t block[64]) noexcept {
    static constexpr uint32_t kSinTable[64] = {
        0xD76AA478u, 0xE8C7B756u, 0x242070DBu, 0xC1BDCEEEu,
        0xF57C0FAFu, 0x4787C62Au, 0xA8304613u, 0xFD469501u,
        0x698098D8u, 0x8B44F7AFu, 0xFFFF5BB1u, 0x895CD7BEu,
        0x6B901122u, 0xFD987193u, 0xA679438Eu, 0x49B40821u,
        0xF61E2562u, 0xC040B340u, 0x265E5A51u, 0xE9B6C7AAu,
        0xD62F105Du, 0x02441453u, 0xD8A1E681u, 0xE7D3FBC8u,
        0x21E1CDE6u, 0xC33707D6u, 0xF4D50D87u, 0x455A14EDu,
        0xA9E3E905u, 0xFCEFA3F8u, 0x676F02D9u, 0x8D2A4C8Au,
        0xFFFA3942u, 0x8771F681u, 0x6D9D6122u, 0xFDE5380Cu,
        0xA4BEEA44u, 0x4BDECFA9u, 0xF6BB4B60u, 0xBEBFBC70u,
        0x289B7EC6u, 0xEAA127FAu, 0xD4EF3085u, 0x04881D05u,
        0xD9D4D039u, 0xE6DB99E5u, 0x1FA27CF8u, 0xC4AC5665u,
        0xF4292244u, 0x432AFF97u, 0xAB9423A7u, 0xFC93A039u,
        0x655B59C3u, 0x8F0CCC92u, 0xFFEFF47Du, 0x85845DD1u,
        0x6FA87E4Fu, 0xFE2CE6E0u, 0xA3014314u, 0x4E0811A1u,
        0xF7537E82u, 0xBD3AF235u, 0x2AD7D2BBu, 0xEB86D391u,
    };
    static constexpr uint8_t kShiftAmt[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };

    uint32_t m[16]{};
    for (uint32_t i = 0; i < 16u; ++i) {
        m[i] = ReadLe32(block + i * 4u);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    for (uint32_t i = 0; i < 64u; ++i) {
        uint32_t f = 0u;
        uint32_t g = 0u;
        if (i < 16u) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32u) {
            f = (d & b) | (~d & c);
            g = (5u * i + 1u) & 15u;
        } else if (i < 48u) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) & 15u;
        } else {
            f = c ^ (b | ~d);
            g = (7u * i) & 15u;
        }
        const uint32_t temp = d;
        d = c;
        c = b;
        const uint32_t x = a + f + kSinTable[i] + m[g];
        const uint32_t sh = kShiftAmt[i];
        b = b + ((x << sh) | (x >> (32u - sh)));
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

bool TryExtractXs2FromXboxChunk(const std::vector<uint8_t>& chunk_data, Xs2ShaderContainer& out_xs2) {
    if (chunk_data.size() >= kXs2HeaderSizeBytes && std::memcmp(chunk_data.data(), "XS2\0", 4) == 0) {
        return ParseXs2Container(chunk_data, out_xs2).ok();
    }
    // Also handle wrapped Xbox shader chunks with a metadata prefix before "XS2\0"
    for (size_t off = 4; off + kXs2HeaderSizeBytes <= chunk_data.size(); off += 4) {
        if (std::memcmp(chunk_data.data() + off, "XS2\0", 4) == 0) {
            std::vector<uint8_t> slice(chunk_data.begin() + static_cast<std::ptrdiff_t>(off), chunk_data.end());
            if (ParseXs2Container(slice, out_xs2).ok()) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

uint32_t SelectGuestWaveSizeFromPsv0(const XboxPsv0Info& psv0, uint32_t default_wave_size) noexcept {
    const uint32_t fallback = (default_wave_size == 64u) ? 64u : 32u;
    if (!psv0.present) return fallback;

    const uint32_t min_w = psv0.min_wave_lane_count;
    const uint32_t max_w = (psv0.max_wave_lane_count == 0xFFFFFFFFu) ? 0u : psv0.max_wave_lane_count;

    // Explicit Wave64 minimum bound requires guest Wave64 mode.
    if (min_w >= 64u) {
        return 64u;
    }
    // Explicit upper bound <= 32 constrains guest execution to Wave32.
    if (max_w > 0u && max_w <= 32u) {
        return 32u;
    }
    // Explicit Wave32 min with unbounded max prefers native RDNA2 Wave32 unless caller requested Wave64 with max >= 64.
    if (min_w == 32u && max_w == 0u) {
        return 32u;
    }
    // Bounded range [min..max] where min <= 32 and max >= 64 preserves XS2 header preference.
    return fallback;
}

void ComputeDxbcContainerHash(const uint8_t* dxbc_bytes, size_t size, uint8_t out_hash[16]) noexcept {
    std::memset(out_hash, 0, 16u);
    if (!dxbc_bytes || size < 20u) return;

    // Public DXBC container MD5 digest over bytes [20 .. size):
    // Standard MD5 blocks followed by DXBC-specific length-prefixed final block.
    const uint8_t* payload = dxbc_bytes + 20u;
    const size_t payload_len = size - 20u;

    uint32_t state[4] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u};
    const size_t full_bytes = (payload_len / 64u) * 64u;
    for (size_t off = 0; off < full_bytes; off += 64u) {
        DxbcMd5Compress(state, payload + off);
    }

    const size_t rem = payload_len - full_bytes;
    const uint32_t bit_len = static_cast<uint32_t>(payload_len * 8u);
    const uint32_t tail_word = static_cast<uint32_t>((payload_len << 1) | 1u);

    uint8_t blk[64]{};
    if (rem >= 56u) {
        std::memcpy(blk, payload + full_bytes, rem);
        blk[rem] = 0x80u;
        DxbcMd5Compress(state, blk);

        std::memset(blk, 0, sizeof(blk));
        std::memcpy(blk + 0u, &bit_len, 4u);
        std::memcpy(blk + 60u, &tail_word, 4u);
        DxbcMd5Compress(state, blk);
    } else {
        std::memcpy(blk + 0u, &bit_len, 4u);
        if (rem > 0u) {
            std::memcpy(blk + 4u, payload + full_bytes, rem);
        }
        blk[4u + rem] = 0x80u;
        std::memcpy(blk + 60u, &tail_word, 4u);
        DxbcMd5Compress(state, blk);
    }

    std::memcpy(out_hash + 0u, &state[0], 4u);
    std::memcpy(out_hash + 4u, &state[1], 4u);
    std::memcpy(out_hash + 8u, &state[2], 4u);
    std::memcpy(out_hash + 12u, &state[3], 4u);
}

bool VerifyDxbcContainerHash(const uint8_t* dxbc_bytes, size_t size) noexcept {
    if (!dxbc_bytes || size < 32u || std::memcmp(dxbc_bytes, "DXBC", 4u) != 0) {
        return false;
    }
    const uint32_t total_size = ReadLe32(dxbc_bytes + 24u);
    if (total_size < 32u || total_size > size) {
        return false;
    }
    uint8_t expected[16]{};
    ComputeDxbcContainerHash(dxbc_bytes, total_size, expected);
    return std::memcmp(dxbc_bytes + 4u, expected, 16u) == 0;
}

Status ResignDxilContainer(std::vector<uint8_t>& dxbc_bytes) {
    if (dxbc_bytes.size() < 32u || std::memcmp(dxbc_bytes.data(), "DXBC", 4u) != 0) {
        return Status::Error(StatusCode::InvalidArgument, "Cannot re-sign non-DXBC/DXIL container");
    }
    const uint32_t total_size = static_cast<uint32_t>(dxbc_bytes.size());
    std::memcpy(dxbc_bytes.data() + 24u, &total_size, 4u);
    uint8_t digest[16]{};
    ComputeDxbcContainerHash(dxbc_bytes.data(), dxbc_bytes.size(), digest);
    std::memcpy(dxbc_bytes.data() + 4u, digest, 16u);
    return Status::Ok();
}

bool IsXboxShaderBytecode(const void* data, size_t size) noexcept {
    if (!data || size < 4u) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (std::memcmp(bytes, "XS2\0", 4) == 0) {
        return size >= kXs2HeaderSizeBytes;
    }
    if (size < 32u || std::memcmp(bytes, "DXBC", 4) != 0) {
        return false;
    }
    const uint32_t total_size = ReadLe32(bytes + 24);
    const uint32_t chunk_count = ReadLe32(bytes + 28);
    if (total_size < 32u || total_size > size || chunk_count == 0u || chunk_count > 128u) {
        return false;
    }
    const uint64_t dir_end = 32ull + static_cast<uint64_t>(chunk_count) * 4ull;
    if (dir_end > total_size) return false;

    for (uint32_t i = 0; i < chunk_count; ++i) {
        const uint32_t off = ReadLe32(bytes + 32u + static_cast<size_t>(i) * 4u);
        if (static_cast<uint64_t>(off) + 8ull > total_size) continue;
        if (std::memcmp(bytes + off, "XBOX", 4) == 0 || std::memcmp(bytes + off, "XS2M", 4) == 0) {
            return true;
        }
        if (std::memcmp(bytes + off, "DXIL", 4) == 0) {
            const uint32_t csz = ReadLe32(bytes + off + 4u);
            if (csz >= 48u && static_cast<uint64_t>(off) + 8ull + 48ull <= total_size &&
                std::memcmp(bytes + off + 8u + 24u, "XDAT", 4) == 0) {
                return true;
            }
        }
    }
    return false;
}

Status ParseXboxShaderContainer(const void* data, size_t size, XboxDxbcContainer& out_container) {
    out_container = XboxDxbcContainer{};
    if (!data || size < 4u) {
        return Status::Error(StatusCode::ParseError, "Shader container buffer is empty or truncated");
    }
    const auto* bytes = static_cast<const uint8_t*>(data);

    // 1. Raw XS2\0 container
    if (std::memcmp(bytes, "XS2\0", 4) == 0) {
        std::vector<uint8_t> raw(bytes, bytes + size);
        if (auto st = ParseXs2Container(raw, out_container.xs2); !st.ok()) {
            return st;
        }
        out_container.format = XboxShaderBlobFormat::RawXs2;
        out_container.has_xs2 = true;
        return Status::Ok();
    }

    // 2. DXBC container ("DXBC" magic + 16-byte hash + version + total_size + chunk_count + offsets[])
    if (size < 32u || std::memcmp(bytes, "DXBC", 4) != 0) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Unrecognized shader container magic (expected 'XS2\\0' or 'DXBC')");
    }

    const uint32_t version = ReadLe32(bytes + 20);
    const uint32_t total_size = ReadLe32(bytes + 24);
    const uint32_t chunk_count = ReadLe32(bytes + 28);
    if (version == 0u || total_size < 32u || total_size > size) {
        return Status::Error(StatusCode::ParseError, "Invalid DXBC container header size/version");
    }
    if (chunk_count == 0u || chunk_count > 128u) {
        return Status::Error(StatusCode::ParseError, "Invalid DXBC chunk count");
    }
    const uint64_t dir_end = 32ull + static_cast<uint64_t>(chunk_count) * 4ull;
    if (dir_end > total_size) {
        return Status::Error(StatusCode::ParseError, "DXBC chunk offset directory extends past container end");
    }

    out_container.chunks.reserve(chunk_count);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        const uint32_t off = ReadLe32(bytes + 32u + static_cast<size_t>(i) * 4u);
        if (static_cast<uint64_t>(off) + 8ull > total_size) {
            return Status::Error(StatusCode::ParseError, "DXBC chunk header extends past container end");
        }
        const uint32_t chunk_size = ReadLe32(bytes + off + 4u);
        if (static_cast<uint64_t>(off) + 8ull + static_cast<uint64_t>(chunk_size) > total_size) {
            return Status::Error(StatusCode::ParseError, "DXBC chunk payload extends past container end");
        }

        XboxDxbcChunk chunk{};
        std::memcpy(chunk.fourcc, bytes + off, 4u);
        chunk.fourcc[4] = '\0';
        chunk.data.assign(bytes + off + 8u, bytes + off + 8u + chunk_size);

        if (std::memcmp(chunk.fourcc, "XBOX", 4) == 0 || std::memcmp(chunk.fourcc, "XS2M", 4) == 0) {
            if (!out_container.has_xs2 && TryExtractXs2FromXboxChunk(chunk.data, out_container.xs2)) {
                out_container.has_xs2 = true;
            }
        } else if (std::memcmp(chunk.fourcc, "DXIL", 4) == 0 ||
                   std::memcmp(chunk.fourcc, "SHEX", 4) == 0 ||
                   std::memcmp(chunk.fourcc, "SHDR", 4) == 0) {
            out_container.has_dxil_or_shdr = true;
            if (out_container.host_shader_chunk_fourcc.empty()) {
                out_container.host_shader_chunk_fourcc = chunk.fourcc;
            }
            if (std::memcmp(chunk.fourcc, "DXIL", 4) == 0 && !out_container.has_xs2 &&
                chunk.data.size() >= 48u && std::memcmp(chunk.data.data() + 24u, "XDAT", 4) == 0) {
                if (ExtractXdatDxilChunkToXs2(chunk.data, out_container.xs2)) {
                    out_container.has_xs2 = true;
                }
            }
        } else if (std::memcmp(chunk.fourcc, "ISG1", 4) == 0) {
            if (!ParseDxbcSignatureChunk(chunk.data.data(), chunk.data.size(), true, out_container.input_signature)) {
                return Status::Error(StatusCode::ParseError, "Incomplete or truncated DXBC ISG1 input signature chunk");
            }
        } else if (std::memcmp(chunk.fourcc, "ISGN", 4) == 0 && out_container.input_signature.empty()) {
            if (!ParseDxbcSignatureChunk(chunk.data.data(), chunk.data.size(), false, out_container.input_signature)) {
                return Status::Error(StatusCode::ParseError, "Incomplete or truncated DXBC ISGN input signature chunk");
            }
        } else if (std::memcmp(chunk.fourcc, "OSG1", 4) == 0) {
            ParseDxbcSignatureChunk(chunk.data.data(), chunk.data.size(), true, out_container.output_signature);
        } else if (std::memcmp(chunk.fourcc, "OSGN", 4) == 0 && out_container.output_signature.empty()) {
            ParseDxbcSignatureChunk(chunk.data.data(), chunk.data.size(), false, out_container.output_signature);
        } else if (std::memcmp(chunk.fourcc, "STSM", 4) == 0) {
            ParseStsmChunk(chunk.data.data(), chunk.data.size(), out_container.static_samplers);
        } else if (std::memcmp(chunk.fourcc, "RTS0", 4) == 0) {
            out_container.raw_root_signature_chunk = chunk.data;
            ParseRts0Chunk(chunk.data.data(), chunk.data.size(), out_container.static_samplers);
        } else if (std::memcmp(chunk.fourcc, "PSV0", 4) == 0) {
            ParsePsv0Chunk(chunk.data.data(), chunk.data.size(), out_container.psv0);
        }

        out_container.chunks.push_back(std::move(chunk));
    }

    // Apply PSV0 wave lane bounds & NumThreads metadata (always honor explicit PSV0 compute dimensions).
    if (out_container.has_xs2 && out_container.psv0.present) {
        out_container.xs2.wave_size =
            SelectGuestWaveSizeFromPsv0(out_container.psv0, out_container.xs2.wave_size);
        if (out_container.psv0.num_threads_x > 0u ||
            out_container.psv0.num_threads_y > 0u ||
            out_container.psv0.num_threads_z > 0u) {
            out_container.xs2.threadgroup_x = out_container.psv0.num_threads_x ? out_container.psv0.num_threads_x : 1u;
            out_container.xs2.threadgroup_y = out_container.psv0.num_threads_y ? out_container.psv0.num_threads_y : 1u;
            out_container.xs2.threadgroup_z = out_container.psv0.num_threads_z ? out_container.psv0.num_threads_z : 1u;
        }
    }

    out_container.format = out_container.has_xs2
                               ? XboxShaderBlobFormat::XboxDxbc
                               : XboxShaderBlobFormat::StandardHostDxbc;
    return Status::Ok();
}

Status ParseXboxShaderContainer(const std::vector<uint8_t>& bytes, XboxDxbcContainer& out_container) {
    return ParseXboxShaderContainer(bytes.data(), bytes.size(), out_container);
}

std::vector<uint8_t> BuildXboxDxbcContainer(const XboxDxbcBuildDesc& desc) {
    std::vector<XboxDxbcChunk> chunks;

    if (!desc.input_signature.empty()) {
        XboxDxbcChunk c{};
        std::memcpy(c.fourcc, "ISG1", 5);
        c.data = BuildDxbcSignatureV1Chunk(desc.input_signature);
        chunks.push_back(std::move(c));
    }
    if (!desc.output_signature.empty()) {
        XboxDxbcChunk c{};
        std::memcpy(c.fourcc, "OSG1", 5);
        c.data = BuildDxbcSignatureV1Chunk(desc.output_signature);
        chunks.push_back(std::move(c));
    }
    if (!desc.static_samplers.empty()) {
        XboxDxbcChunk c{};
        if (desc.emit_rts0_for_samplers) {
            std::memcpy(c.fourcc, "RTS0", 5);
            AppendLe32(c.data, 1u);  // D3D_ROOT_SIGNATURE_VERSION_1_0
            AppendLe32(c.data, 0u);  // num_parameters
            AppendLe32(c.data, 24u); // parameters_offset
            AppendLe32(c.data, static_cast<uint32_t>(desc.static_samplers.size()));
            AppendLe32(c.data, 24u); // static_samplers_offset
            AppendLe32(c.data, 0u);  // flags
            for (const auto& s : desc.static_samplers) {
                AppendStaticSamplerRecord(c.data, s);
            }
        } else {
            std::memcpy(c.fourcc, "STSM", 5);
            AppendLe32(c.data, static_cast<uint32_t>(desc.static_samplers.size()));
            for (const auto& s : desc.static_samplers) {
                AppendStaticSamplerRecord(c.data, s);
            }
        }
        chunks.push_back(std::move(c));
    }
    if (!desc.aux_dxil_or_shdr.empty()) {
        XboxDxbcChunk c{};
        const std::string tag = (desc.aux_shader_fourcc.size() == 4) ? desc.aux_shader_fourcc : "DXIL";
        std::memcpy(c.fourcc, tag.data(), 4);
        c.fourcc[4] = '\0';
        c.data = desc.aux_dxil_or_shdr;
        chunks.push_back(std::move(c));
    }
    if (desc.psv0.present) {
        XboxDxbcChunk c{};
        std::memcpy(c.fourcc, "PSV0", 5);
        c.data = BuildPsv0Chunk(desc.psv0);
        chunks.push_back(std::move(c));
    }
    if (!desc.xs2_blob.empty()) {
        XboxDxbcChunk c{};
        const std::string tag = (desc.xbox_chunk_fourcc.size() == 4) ? desc.xbox_chunk_fourcc : "XBOX";
        std::memcpy(c.fourcc, tag.data(), 4);
        c.fourcc[4] = '\0';
        c.data = desc.xs2_blob;
        chunks.push_back(std::move(c));
    }

    const uint32_t chunk_count = static_cast<uint32_t>(chunks.size());
    uint32_t cur_offset = 32u + chunk_count * 4u;
    std::vector<uint32_t> offsets;
    offsets.reserve(chunk_count);
    for (const auto& c : chunks) {
        offsets.push_back(cur_offset);
        cur_offset += 8u + static_cast<uint32_t>(c.data.size());
    }
    const uint32_t total_size = cur_offset;

    std::vector<uint8_t> out;
    out.reserve(total_size);
    out.push_back('D');
    out.push_back('X');
    out.push_back('B');
    out.push_back('C');
    // 16-byte DXBC MD5 digest placeholder (signed below)
    for (int i = 0; i < 16; ++i) out.push_back(0u);
    AppendLe32(out, 1u);  // DXBC container version 1
    AppendLe32(out, total_size);
    AppendLe32(out, chunk_count);
    for (uint32_t off : offsets) {
        AppendLe32(out, off);
    }
    for (const auto& c : chunks) {
        out.push_back(static_cast<uint8_t>(c.fourcc[0]));
        out.push_back(static_cast<uint8_t>(c.fourcc[1]));
        out.push_back(static_cast<uint8_t>(c.fourcc[2]));
        out.push_back(static_cast<uint8_t>(c.fourcc[3]));
        AppendLe32(out, static_cast<uint32_t>(c.data.size()));
        out.insert(out.end(), c.data.begin(), c.data.end());
    }

    // Compute exact DXBC MD5 container digest over [20..total_size)
    uint8_t digest[16]{};
    ComputeDxbcContainerHash(out.data(), out.size(), digest);
    std::memcpy(out.data() + 4u, digest, 16u);
    return out;
}

// ---------------------------------------------------------------------------
// Authoritative Typed-Buffer Format Table (Leonida aa2988c) & DS Swizzle Codec
// ---------------------------------------------------------------------------
Rdna2MtbufFormatInfo GetRdna2MtbufFormatInfo(Rdna2MtbufFormat fmt) noexcept {
    auto make = [&](uint8_t cb, uint8_t cc, bool is_int, bool is_sgn, bool is_norm, bool is_flt) noexcept {
        Rdna2MtbufFormatInfo info{};
        info.format = fmt;
        info.component_bytes = cb;
        info.component_count = cc;
        info.byte_stride = static_cast<uint8_t>(cb * cc);
        info.is_integer = is_int;
        info.is_signed = is_sgn;
        info.is_normalized = is_norm;
        info.is_float = is_flt;
        info.is_valid = true;
        return info;
    };
    switch (fmt) {
        case Rdna2MtbufFormat::R8_UNORM:            return make(1u, 1u, false, false, true,  false);
        case Rdna2MtbufFormat::R8_SNORM:            return make(1u, 1u, false, true,  true,  false);
        case Rdna2MtbufFormat::R8_UINT:             return make(1u, 1u, true,  false, false, false);
        case Rdna2MtbufFormat::R8_SINT:             return make(1u, 1u, true,  true,  false, false);
        case Rdna2MtbufFormat::R16_UNORM:           return make(2u, 1u, false, false, true,  false);
        case Rdna2MtbufFormat::R16_SNORM:           return make(2u, 1u, false, true,  true,  false);
        case Rdna2MtbufFormat::R16_UINT:            return make(2u, 1u, true,  false, false, false);
        case Rdna2MtbufFormat::R16_SINT:            return make(2u, 1u, true,  true,  false, false);
        case Rdna2MtbufFormat::R16_FLOAT:           return make(2u, 1u, false, true,  false, true);
        case Rdna2MtbufFormat::R8G8_UNORM:          return make(1u, 2u, false, false, true,  false);
        case Rdna2MtbufFormat::R8G8_SNORM:          return make(1u, 2u, false, true,  true,  false);
        case Rdna2MtbufFormat::R8G8_UINT:           return make(1u, 2u, true,  false, false, false);
        case Rdna2MtbufFormat::R8G8_SINT:           return make(1u, 2u, true,  true,  false, false);
        case Rdna2MtbufFormat::R32_UINT:            return make(4u, 1u, true,  false, false, false);
        case Rdna2MtbufFormat::R32_SINT:            return make(4u, 1u, true,  true,  false, false);
        case Rdna2MtbufFormat::R32_FLOAT:           return make(4u, 1u, false, true,  false, true);
        case Rdna2MtbufFormat::R16G16_UNORM:        return make(2u, 2u, false, false, true,  false);
        case Rdna2MtbufFormat::R16G16_SNORM:        return make(2u, 2u, false, true,  true,  false);
        case Rdna2MtbufFormat::R16G16_UINT:         return make(2u, 2u, true,  false, false, false);
        case Rdna2MtbufFormat::R16G16_SINT:         return make(2u, 2u, true,  true,  false, false);
        case Rdna2MtbufFormat::R16G16_FLOAT:        return make(2u, 2u, false, true,  false, true);
        case Rdna2MtbufFormat::R8G8B8A8_UNORM:      return make(1u, 4u, false, false, true,  false);
        case Rdna2MtbufFormat::R8G8B8A8_SNORM:      return make(1u, 4u, false, true,  true,  false);
        case Rdna2MtbufFormat::R8G8B8A8_UINT:       return make(1u, 4u, true,  false, false, false);
        case Rdna2MtbufFormat::R8G8B8A8_SINT:       return make(1u, 4u, true,  true,  false, false);
        case Rdna2MtbufFormat::R32G32_FLOAT:        return make(4u, 2u, false, true,  false, true);
        case Rdna2MtbufFormat::R16G16B16A16_UNORM:  return make(2u, 4u, false, false, true,  false);
        case Rdna2MtbufFormat::R16G16B16A16_SNORM:  return make(2u, 4u, false, true,  true,  false);
        case Rdna2MtbufFormat::R16G16B16A16_UINT:   return make(2u, 4u, true,  false, false, false);
        case Rdna2MtbufFormat::R16G16B16A16_SINT:   return make(2u, 4u, true,  true,  false, false);
        case Rdna2MtbufFormat::R16G16B16A16_FLOAT:  return make(2u, 4u, false, true,  false, true);
        case Rdna2MtbufFormat::R32G32B32_FLOAT:     return make(4u, 3u, false, true,  false, true);
        case Rdna2MtbufFormat::R32G32B32A32_FLOAT:  return make(4u, 4u, false, true,  false, true);
        case Rdna2MtbufFormat::Invalid:
        default:
            return Rdna2MtbufFormatInfo{};
    }
}

Rdna2MtbufFormatInfo GetRdna2MtbufFormatInfo(uint8_t raw_fmt) noexcept {
    return GetRdna2MtbufFormatInfo(static_cast<Rdna2MtbufFormat>(raw_fmt));
}

Rdna2DsSwizzleInfo DecodeRdna2DsSwizzleOffset(uint16_t offset) noexcept {
    Rdna2DsSwizzleInfo info{};
    if ((offset & 0x8000u) != 0u) {
        // Bit-mode (offset[15] == 1): ((lane & and_mask) | or_mask) ^ xor_mask
        info.mode = Rdna2DsSwizzleMode::BitMode;
        info.and_mask = static_cast<uint8_t>(offset & 0x1Fu);
        info.or_mask = static_cast<uint8_t>((offset >> 5) & 0x1Fu);
        info.xor_mask = static_cast<uint8_t>((offset >> 10) & 0x1Fu);
        return info;
    }
    const uint32_t submode = (offset >> 13) & 0x3u;
    if (submode == 0u) {
        // Quad-permute (offset[15]==0, offset[14:13]==0): 4x2-bit lane indices within 4-lane quad
        info.mode = Rdna2DsSwizzleMode::QuadPermute;
        info.quad_sel[0] = static_cast<uint8_t>((offset >> 0) & 0x3u);
        info.quad_sel[1] = static_cast<uint8_t>((offset >> 2) & 0x3u);
        info.quad_sel[2] = static_cast<uint8_t>((offset >> 4) & 0x3u);
        info.quad_sel[3] = static_cast<uint8_t>((offset >> 6) & 0x3u);
    } else if (submode == 1u) {
        // Rotate (offset[15]==0, offset[14:13]==1): rotate-right within cluster size
        info.mode = Rdna2DsSwizzleMode::Rotate;
        info.rotate_shift = static_cast<uint8_t>(offset & 0x1Fu);
        const uint8_t size_bits = static_cast<uint8_t>((offset >> 5) & 0x1Fu);
        info.rotate_cluster_mask = (size_bits == 0u) ? 31u : size_bits;
    } else if (submode == 2u) {
        // FFT / butterfly (offset[15]==0, offset[14:13]==2): pair swap via xor mask within 32-lane half
        info.mode = Rdna2DsSwizzleMode::Fft;
        info.fft_mask = static_cast<uint8_t>(offset & 0x1Fu);
    } else {
        info.mode = Rdna2DsSwizzleMode::Reserved;
    }
    return info;
}

uint32_t ComputeRdna2DsSwizzleSrcLane(uint32_t lane, uint16_t offset, uint32_t wave_size) noexcept {
    const uint32_t ws = (wave_size == 64u) ? 64u : 32u;
    const uint32_t half_base = lane & ~31u;
    const uint32_t l32 = lane & 31u;
    const Rdna2DsSwizzleInfo info = DecodeRdna2DsSwizzleOffset(offset);
    uint32_t src_in_32 = l32;
    switch (info.mode) {
        case Rdna2DsSwizzleMode::BitMode:
            src_in_32 = (((l32 & info.and_mask) | info.or_mask) ^ info.xor_mask) & 31u;
            break;
        case Rdna2DsSwizzleMode::QuadPermute: {
            const uint32_t qbase = l32 & ~3u;
            const uint32_t qoff = l32 & 3u;
            src_in_32 = qbase | info.quad_sel[qoff];
            break;
        }
        case Rdna2DsSwizzleMode::Rotate: {
            const uint32_t cmask = info.rotate_cluster_mask;
            src_in_32 = (l32 & ~cmask) | ((l32 - info.rotate_shift) & cmask);
            break;
        }
        case Rdna2DsSwizzleMode::Fft:
            src_in_32 = (l32 ^ info.fft_mask) & 31u;
            break;
        case Rdna2DsSwizzleMode::Reserved:
        default:
            src_in_32 = l32;
            break;
    }
    return (half_base | src_in_32) % ws;
}

// ---------------------------------------------------------------------------
// Operand Factories & Codec
// ---------------------------------------------------------------------------
Rdna2Operand Rdna2Operand::Sgpr(uint8_t idx) {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Sgpr;
    op.raw_code = idx;
    op.reg_index = idx;
    return op;
}

Rdna2Operand Rdna2Operand::Vgpr(uint8_t idx) {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Vgpr;
    op.raw_code = static_cast<uint16_t>(256u + idx);
    op.reg_index = idx;
    return op;
}

Rdna2Operand Rdna2Operand::InlineInt(int32_t val) {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::InlineInt;
    op.int_value = val;
    if (val >= 0 && val <= 64) {
        op.raw_code = static_cast<uint16_t>(128 + val);
    } else if (val >= -16 && val <= -1) {
        op.raw_code = static_cast<uint16_t>(192 - val);
    } else {
        op.kind = Rdna2OperandKind::Literal32;
        op.raw_code = 255;
        op.literal_bits = static_cast<uint32_t>(val);
    }
    return op;
}

Rdna2Operand Rdna2Operand::InlineFloat(float val) {
    const uint32_t bits = FloatToBits(val);
    if (bits == 0x00000000u) {
        Rdna2Operand op{};
        op.kind = Rdna2OperandKind::InlineInt;
        op.raw_code = 128;
        op.int_value = 0;
        op.float_value = 0.0f;
        return op;
    }
    for (uint16_t i = 0; i < 9; ++i) {
        if (bits == kInlineFloatBits[i]) {
            Rdna2Operand op{};
            op.kind = Rdna2OperandKind::InlineFloat;
            op.raw_code = static_cast<uint16_t>(240u + i);
            op.float_value = BitsToFloat(bits);
            op.literal_bits = bits;
            return op;
        }
    }
    return LiteralF32(val);
}

Rdna2Operand Rdna2Operand::LiteralU32(uint32_t bits) {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Literal32;
    op.raw_code = 255;
    op.literal_bits = bits;
    op.int_value = static_cast<int32_t>(bits);
    op.float_value = BitsToFloat(bits);
    return op;
}

Rdna2Operand Rdna2Operand::LiteralF32(float val) {
    return LiteralU32(FloatToBits(val));
}

Rdna2Operand Rdna2Operand::VccLo() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::VccLo;
    op.raw_code = 106;
    return op;
}

Rdna2Operand Rdna2Operand::VccHi() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::VccHi;
    op.raw_code = 107;
    return op;
}

Rdna2Operand Rdna2Operand::ExecLo() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::ExecLo;
    op.raw_code = 126;
    return op;
}

Rdna2Operand Rdna2Operand::ExecHi() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::ExecHi;
    op.raw_code = 127;
    return op;
}

Rdna2Operand Rdna2Operand::Scc() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Scc;
    op.raw_code = 253;
    return op;
}

Rdna2Operand Rdna2Operand::M0() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::M0;
    op.raw_code = 124;
    return op;
}

Rdna2Operand Rdna2Operand::Null() {
    Rdna2Operand op{};
    op.kind = Rdna2OperandKind::Null;
    op.raw_code = 125;
    return op;
}

Rdna2Operand DecodeRdna2SourceOperand(uint16_t code, uint32_t literal_bits) {
    Rdna2Operand op{};
    op.raw_code = code & 0x1FFu;
    const uint16_t c = op.raw_code;

    if (c <= 105u) {
        op.kind = Rdna2OperandKind::Sgpr;
        op.reg_index = c;
    } else if (c == 106u) {
        op.kind = Rdna2OperandKind::VccLo;
    } else if (c == 107u) {
        op.kind = Rdna2OperandKind::VccHi;
    } else if (c == 124u) {
        op.kind = Rdna2OperandKind::M0;
    } else if (c == 125u) {
        op.kind = Rdna2OperandKind::Null;
    } else if (c == 126u) {
        op.kind = Rdna2OperandKind::ExecLo;
    } else if (c == 127u) {
        op.kind = Rdna2OperandKind::ExecHi;
    } else if (c >= 128u && c <= 192u) {
        op.kind = Rdna2OperandKind::InlineInt;
        op.int_value = static_cast<int32_t>(c) - 128;
        op.literal_bits = static_cast<uint32_t>(op.int_value);
    } else if (c >= 193u && c <= 208u) {
        op.kind = Rdna2OperandKind::InlineInt;
        op.int_value = -static_cast<int32_t>(c - 192u);
        op.literal_bits = static_cast<uint32_t>(op.int_value);
    } else if (c >= 240u && c <= 248u) {
        op.kind = Rdna2OperandKind::InlineFloat;
        op.literal_bits = kInlineFloatBits[c - 240u];
        op.float_value = BitsToFloat(op.literal_bits);
    } else if (c == 253u) {
        op.kind = Rdna2OperandKind::Scc;
    } else if (c == 255u) {
        op.kind = Rdna2OperandKind::Literal32;
        op.literal_bits = literal_bits;
        op.int_value = static_cast<int32_t>(literal_bits);
        op.float_value = BitsToFloat(literal_bits);
    } else if (c >= 256u && c <= 511u) {
        op.kind = Rdna2OperandKind::Vgpr;
        op.reg_index = static_cast<uint16_t>(c - 256u);
    } else {
        op.kind = Rdna2OperandKind::Null;
    }
    return op;
}

bool EncodeRdna2SourceOperand(
    const Rdna2Operand& op,
    uint16_t& out_code,
    uint32_t& out_literal,
    bool& out_needs_literal) {
    out_needs_literal = false;
    out_literal = 0;
    switch (op.kind) {
        case Rdna2OperandKind::Sgpr:
            if (op.reg_index > 105u) return false;
            out_code = op.reg_index;
            return true;
        case Rdna2OperandKind::Vgpr:
            if (op.reg_index > 255u) return false;
            out_code = static_cast<uint16_t>(256u + op.reg_index);
            return true;
        case Rdna2OperandKind::VccLo:
            out_code = 106u;
            return true;
        case Rdna2OperandKind::VccHi:
            out_code = 107u;
            return true;
        case Rdna2OperandKind::M0:
            out_code = 124u;
            return true;
        case Rdna2OperandKind::Null:
            out_code = 125u;
            return true;
        case Rdna2OperandKind::ExecLo:
            out_code = 126u;
            return true;
        case Rdna2OperandKind::ExecHi:
            out_code = 127u;
            return true;
        case Rdna2OperandKind::Scc:
            out_code = 253u;
            return true;
        case Rdna2OperandKind::InlineInt:
            if (op.int_value >= 0 && op.int_value <= 64) {
                out_code = static_cast<uint16_t>(128 + op.int_value);
                return true;
            }
            if (op.int_value >= -16 && op.int_value <= -1) {
                out_code = static_cast<uint16_t>(192 - op.int_value);
                return true;
            }
            out_code = 255u;
            out_literal = static_cast<uint32_t>(op.int_value);
            out_needs_literal = true;
            return true;
        case Rdna2OperandKind::InlineFloat: {
            const uint32_t bits = FloatToBits(op.float_value);
            if (bits == 0u) {
                out_code = 128u;
                return true;
            }
            for (uint16_t i = 0; i < 9; ++i) {
                if (bits == kInlineFloatBits[i]) {
                    out_code = static_cast<uint16_t>(240u + i);
                    return true;
                }
            }
            out_code = 255u;
            out_literal = bits;
            out_needs_literal = true;
            return true;
        }
        case Rdna2OperandKind::Literal32:
            out_code = 255u;
            out_literal = op.literal_bits;
            out_needs_literal = true;
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Names & Disassembly Formatting
// ---------------------------------------------------------------------------
const char* Rdna2EncodingName(Rdna2Encoding enc) noexcept {
    switch (enc) {
        case Rdna2Encoding::SOP2:  return "SOP2";
        case Rdna2Encoding::SOP1:  return "SOP1";
        case Rdna2Encoding::SOPC:  return "SOPC";
        case Rdna2Encoding::SOPK:  return "SOPK";
        case Rdna2Encoding::SOPP:  return "SOPP";
        case Rdna2Encoding::VOP2:  return "VOP2";
        case Rdna2Encoding::VOP1:  return "VOP1";
        case Rdna2Encoding::VOPC:  return "VOPC";
        case Rdna2Encoding::VOP3:  return "VOP3";
        case Rdna2Encoding::VOP3P: return "VOP3P";
        case Rdna2Encoding::SMEM:   return "SMEM";
        case Rdna2Encoding::MUBUF:  return "MUBUF";
        case Rdna2Encoding::MTBUF:  return "MTBUF";
        case Rdna2Encoding::MIMG:   return "MIMG";
        case Rdna2Encoding::DS:     return "DS";
        case Rdna2Encoding::FLAT:   return "FLAT";
        case Rdna2Encoding::EXP:    return "EXP";
        case Rdna2Encoding::VINTRP: return "VINTRP";
        default:                    return "UNKNOWN";
    }
}

const char* Rdna2OpcodeName(Rdna2Opcode op) noexcept {
    switch (op) {
        case Rdna2Opcode::S_ADD_U32:          return "s_add_u32";
        case Rdna2Opcode::S_SUB_U32:          return "s_sub_u32";
        case Rdna2Opcode::S_ADD_I32:          return "s_add_i32";
        case Rdna2Opcode::S_SUB_I32:          return "s_sub_i32";
        case Rdna2Opcode::S_ADDC_U32:         return "s_addc_u32";
        case Rdna2Opcode::S_SUBB_U32:         return "s_subb_u32";
        case Rdna2Opcode::S_ABSDIFF_I32:      return "s_absdiff_i32";
        case Rdna2Opcode::S_MIN_I32:          return "s_min_i32";
        case Rdna2Opcode::S_MIN_U32:          return "s_min_u32";
        case Rdna2Opcode::S_MAX_I32:          return "s_max_i32";
        case Rdna2Opcode::S_MAX_U32:          return "s_max_u32";
        case Rdna2Opcode::S_CSELECT_B32:      return "s_cselect_b32";
        case Rdna2Opcode::S_AND_B32:          return "s_and_b32";
        case Rdna2Opcode::S_OR_B32:           return "s_or_b32";
        case Rdna2Opcode::S_XOR_B32:          return "s_xor_b32";
        case Rdna2Opcode::S_ANDN2_B32:        return "s_andn2_b32";
        case Rdna2Opcode::S_LSHL_B32:         return "s_lshl_b32";
        case Rdna2Opcode::S_LSHR_B32:         return "s_lshr_b32";
        case Rdna2Opcode::S_ASHR_I32:         return "s_ashr_i32";
        case Rdna2Opcode::S_BFE_U32:          return "s_bfe_u32";
        case Rdna2Opcode::S_BFE_I32:          return "s_bfe_i32";
        case Rdna2Opcode::S_MUL_I32:          return "s_mul_i32";
        case Rdna2Opcode::S_MOV_B32:          return "s_mov_b32";
        case Rdna2Opcode::S_NOT_B32:          return "s_not_b32";
        case Rdna2Opcode::S_BREV_B32:         return "s_brev_b32";
        case Rdna2Opcode::S_BCNT1_I32_B32:    return "s_bcnt1_i32_b32";
        case Rdna2Opcode::S_BCNT1_I32_B64:    return "s_bcnt1_i32_b64";
        case Rdna2Opcode::S_FLBIT_I32_B32:    return "s_flbit_i32_b32";
        case Rdna2Opcode::S_FLBIT_I32_B64:    return "s_flbit_i32_b64";
        case Rdna2Opcode::S_FLBIT_I32:        return "s_flbit_i32";
        case Rdna2Opcode::S_FLBIT_I32_I64:    return "s_flbit_i32_i64";
        case Rdna2Opcode::S_SETPC_B64:        return "s_setpc_b64";
        case Rdna2Opcode::S_SWAPPC_B64:       return "s_swappc_b64";
        case Rdna2Opcode::S_GETPC_B64:        return "s_getpc_b64";
        case Rdna2Opcode::S_CMP_EQ_I32:       return "s_cmp_eq_i32";
        case Rdna2Opcode::S_CMP_LG_I32:       return "s_cmp_lg_i32";
        case Rdna2Opcode::S_CMP_GT_I32:       return "s_cmp_gt_i32";
        case Rdna2Opcode::S_CMP_GE_I32:       return "s_cmp_ge_i32";
        case Rdna2Opcode::S_CMP_LT_I32:       return "s_cmp_lt_i32";
        case Rdna2Opcode::S_CMP_LE_I32:       return "s_cmp_le_i32";
        case Rdna2Opcode::S_CMP_EQ_U32:       return "s_cmp_eq_u32";
        case Rdna2Opcode::S_CMP_LG_U32:       return "s_cmp_lg_u32";
        case Rdna2Opcode::S_CMP_GT_U32:       return "s_cmp_gt_u32";
        case Rdna2Opcode::S_CMP_GE_U32:       return "s_cmp_ge_u32";
        case Rdna2Opcode::S_CMP_LT_U32:       return "s_cmp_lt_u32";
        case Rdna2Opcode::S_CMP_LE_U32:       return "s_cmp_le_u32";
        case Rdna2Opcode::S_CMP_EQ_U64:       return "s_cmp_eq_u64";
        case Rdna2Opcode::S_CMP_LG_U64:       return "s_cmp_lg_u64";
        case Rdna2Opcode::S_MOVK_I32:         return "s_movk_i32";
        case Rdna2Opcode::S_VERSION:          return "s_version";
        case Rdna2Opcode::S_ADDK_I32:         return "s_addk_i32";
        case Rdna2Opcode::S_MULK_I32:         return "s_mulk_i32";
        case Rdna2Opcode::S_WAITCNT_VSCNT:    return "s_waitcnt_vscnt";
        case Rdna2Opcode::S_WAITCNT_VMCNT:    return "s_waitcnt_vmcnt";
        case Rdna2Opcode::S_WAITCNT_EXPCNT:   return "s_waitcnt_expcnt";
        case Rdna2Opcode::S_WAITCNT_LGKMCNT:  return "s_waitcnt_lgkmcnt";
        case Rdna2Opcode::S_NOP:              return "s_nop";
        case Rdna2Opcode::S_ENDPGM:           return "s_endpgm";
        case Rdna2Opcode::S_BRANCH:           return "s_branch";
        case Rdna2Opcode::S_CBRANCH_SCC0:     return "s_cbranch_scc0";
        case Rdna2Opcode::S_CBRANCH_SCC1:     return "s_cbranch_scc1";
        case Rdna2Opcode::S_CBRANCH_VCCZ:     return "s_cbranch_vccz";
        case Rdna2Opcode::S_CBRANCH_VCCNZ:    return "s_cbranch_vccnz";
        case Rdna2Opcode::S_CBRANCH_EXECZ:    return "s_cbranch_execz";
        case Rdna2Opcode::S_CBRANCH_EXECNZ:   return "s_cbranch_execnz";
        case Rdna2Opcode::S_WAITCNT:          return "s_waitcnt";
        case Rdna2Opcode::S_BARRIER:          return "s_barrier";
        case Rdna2Opcode::S_SENDMSG:          return "s_sendmsg";
        case Rdna2Opcode::S_ICACHE_INV:       return "s_icache_inv";
        case Rdna2Opcode::S_TTRACEDATA:       return "s_ttracedata";
        case Rdna2Opcode::S_CODE_END:         return "s_code_end";
        case Rdna2Opcode::S_TTRACEDATA_IMM:   return "s_ttracedata_imm";
        case Rdna2Opcode::V_CNDMASK_B32:      return "v_cndmask_b32";
        case Rdna2Opcode::V_ADD_F32:          return "v_add_f32";
        case Rdna2Opcode::V_SUB_F32:          return "v_sub_f32";
        case Rdna2Opcode::V_SUBREV_F32:       return "v_subrev_f32";
        case Rdna2Opcode::V_MUL_F32:          return "v_mul_f32";
        case Rdna2Opcode::V_MIN_F32:          return "v_min_f32";
        case Rdna2Opcode::V_MAX_F32:          return "v_max_f32";
        case Rdna2Opcode::V_MIN_I32:          return "v_min_i32";
        case Rdna2Opcode::V_MAX_I32:          return "v_max_i32";
        case Rdna2Opcode::V_MIN_U32:          return "v_min_u32";
        case Rdna2Opcode::V_MAX_U32:          return "v_max_u32";
        case Rdna2Opcode::V_MAC_F32:          return "v_mac_f32";
        case Rdna2Opcode::V_MADMK_F32:        return "v_madmk_f32";
        case Rdna2Opcode::V_MADAK_F32:        return "v_madak_f32";
        case Rdna2Opcode::V_FMAC_F32:         return "v_fmac_f32";
        case Rdna2Opcode::V_FMAC_F16:         return "v_fmac_f16";
        case Rdna2Opcode::V_FMAMK_F16:        return "v_fmamk_f16";
        case Rdna2Opcode::V_FMAAK_F16:        return "v_fmaak_f16";
        case Rdna2Opcode::V_AND_B32:          return "v_and_b32";
        case Rdna2Opcode::V_OR_B32:           return "v_or_b32";
        case Rdna2Opcode::V_XOR_B32:          return "v_xor_b32";
        case Rdna2Opcode::V_LSHLREV_B32:      return "v_lshlrev_b32";
        case Rdna2Opcode::V_LSHRREV_B32:      return "v_lshrrev_b32";
        case Rdna2Opcode::V_ASHRREV_I32:      return "v_ashrrev_i32";
        case Rdna2Opcode::V_ADD_NC_U32:       return "v_add_nc_u32";
        case Rdna2Opcode::V_SUB_NC_U32:       return "v_sub_nc_u32";
        case Rdna2Opcode::V_NOP:              return "v_nop";
        case Rdna2Opcode::V_MOV_B32:          return "v_mov_b32";
        case Rdna2Opcode::V_READFIRSTLANE_B32:return "v_readfirstlane_b32";
        case Rdna2Opcode::V_CVT_F32_I32:      return "v_cvt_f32_i32";
        case Rdna2Opcode::V_CVT_I32_F32:      return "v_cvt_i32_f32";
        case Rdna2Opcode::V_CVT_F32_U32:      return "v_cvt_f32_u32";
        case Rdna2Opcode::V_CVT_U32_F32:      return "v_cvt_u32_f32";
        case Rdna2Opcode::V_RCP_F32:          return "v_rcp_f32";
        case Rdna2Opcode::V_RSQ_F32:          return "v_rsq_f32";
        case Rdna2Opcode::V_SQRT_F32:         return "v_sqrt_f32";
        case Rdna2Opcode::V_FLOOR_F32:        return "v_floor_f32";
        case Rdna2Opcode::V_CEIL_F32:         return "v_ceil_f32";
        case Rdna2Opcode::V_FRACT_F32:        return "v_fract_f32";
        case Rdna2Opcode::V_TRUNC_F32:        return "v_trunc_f32";
        case Rdna2Opcode::V_RNDNE_F32:        return "v_rndne_f32";
        case Rdna2Opcode::V_EXP_F32:          return "v_exp_f32";
        case Rdna2Opcode::V_LOG_F32:          return "v_log_f32";
        case Rdna2Opcode::V_SIN_F32:          return "v_sin_f32";
        case Rdna2Opcode::V_COS_F32:          return "v_cos_f32";
        case Rdna2Opcode::V_CVT_NORM_I16_F16: return "v_cvt_norm_i16_f16";
        case Rdna2Opcode::V_CVT_NORM_U16_F16: return "v_cvt_norm_u16_f16";
        case Rdna2Opcode::V_SWAP_B32:         return "v_swap_b32";
        case Rdna2Opcode::V_CMP_F_F32:        return "v_cmp_f_f32";
        case Rdna2Opcode::V_CMP_LT_F32:       return "v_cmp_lt_f32";
        case Rdna2Opcode::V_CMP_EQ_F32:       return "v_cmp_eq_f32";
        case Rdna2Opcode::V_CMP_LE_F32:       return "v_cmp_le_f32";
        case Rdna2Opcode::V_CMP_GT_F32:       return "v_cmp_gt_f32";
        case Rdna2Opcode::V_CMP_LG_F32:       return "v_cmp_lg_f32";
        case Rdna2Opcode::V_CMP_GE_F32:       return "v_cmp_ge_f32";
        case Rdna2Opcode::V_CMP_O_F32:        return "v_cmp_o_f32";
        case Rdna2Opcode::V_CMP_U_F32:        return "v_cmp_u_f32";
        case Rdna2Opcode::V_CMP_NGE_F32:      return "v_cmp_nge_f32";
        case Rdna2Opcode::V_CMP_NLG_F32:      return "v_cmp_nlg_f32";
        case Rdna2Opcode::V_CMP_NGT_F32:      return "v_cmp_ngt_f32";
        case Rdna2Opcode::V_CMP_NLE_F32:      return "v_cmp_nle_f32";
        case Rdna2Opcode::V_CMP_NEQ_F32:      return "v_cmp_neq_f32";
        case Rdna2Opcode::V_CMP_NLT_F32:      return "v_cmp_nlt_f32";
        case Rdna2Opcode::V_CMP_TRU_F32:      return "v_cmp_tru_f32";
        case Rdna2Opcode::V_CMPX_LT_F32:      return "v_cmpx_lt_f32";
        case Rdna2Opcode::V_CMPX_EQ_F32:      return "v_cmpx_eq_f32";
        case Rdna2Opcode::V_CMPX_LE_F32:      return "v_cmpx_le_f32";
        case Rdna2Opcode::V_CMPX_GT_F32:      return "v_cmpx_gt_f32";
        case Rdna2Opcode::V_CMPX_LG_F32:      return "v_cmpx_lg_f32";
        case Rdna2Opcode::V_CMPX_GE_F32:      return "v_cmpx_ge_f32";
        case Rdna2Opcode::V_CMP_LT_I32:       return "v_cmp_lt_i32";
        case Rdna2Opcode::V_CMP_EQ_I32:       return "v_cmp_eq_i32";
        case Rdna2Opcode::V_CMP_GT_I32:       return "v_cmp_gt_i32";
        case Rdna2Opcode::V_CMP_GE_I32:       return "v_cmp_ge_i32";
        case Rdna2Opcode::V_CMPX_LT_I32:      return "v_cmpx_lt_i32";
        case Rdna2Opcode::V_CMPX_EQ_I32:      return "v_cmpx_eq_i32";
        case Rdna2Opcode::V_CMPX_GT_I32:      return "v_cmpx_gt_i32";
        case Rdna2Opcode::V_CMPX_GE_I32:      return "v_cmpx_ge_i32";
        case Rdna2Opcode::V_CMP_LT_U32:       return "v_cmp_lt_u32";
        case Rdna2Opcode::V_CMP_EQ_U32:       return "v_cmp_eq_u32";
        case Rdna2Opcode::V_CMP_GT_U32:       return "v_cmp_gt_u32";
        case Rdna2Opcode::V_CMP_GE_U32:       return "v_cmp_ge_u32";
        case Rdna2Opcode::V_CMPX_LT_U32:      return "v_cmpx_lt_u32";
        case Rdna2Opcode::V_CMPX_EQ_U32:      return "v_cmpx_eq_u32";
        case Rdna2Opcode::V_CMPX_GT_U32:      return "v_cmpx_gt_u32";
        case Rdna2Opcode::V_CMPX_GE_U32:      return "v_cmpx_ge_u32";
        case Rdna2Opcode::V_CMP_LT_F16:       return "v_cmp_lt_f16";
        case Rdna2Opcode::V_CMP_EQ_F16:       return "v_cmp_eq_f16";
        case Rdna2Opcode::V_CMP_LE_F16:       return "v_cmp_le_f16";
        case Rdna2Opcode::V_CMP_GT_F16:       return "v_cmp_gt_f16";
        case Rdna2Opcode::V_CMP_LG_F16:       return "v_cmp_lg_f16";
        case Rdna2Opcode::V_CMP_GE_F16:       return "v_cmp_ge_f16";
        case Rdna2Opcode::V_CMP_U_F16:        return "v_cmp_u_f16";
        case Rdna2Opcode::V_CMP_NGE_F16:      return "v_cmp_nge_f16";
        case Rdna2Opcode::V_CMP_NLG_F16:      return "v_cmp_nlg_f16";
        case Rdna2Opcode::V_CMP_NGT_F16:      return "v_cmp_ngt_f16";
        case Rdna2Opcode::V_CMP_NLE_F16:      return "v_cmp_nle_f16";
        case Rdna2Opcode::V_CMP_NEQ_F16:      return "v_cmp_neq_f16";
        case Rdna2Opcode::V_CMP_NLT_F16:      return "v_cmp_nlt_f16";
        case Rdna2Opcode::V_CMP_TRU_F16:      return "v_cmp_tru_f16";
        case Rdna2Opcode::V_CMPX_LT_F16:      return "v_cmpx_lt_f16";
        case Rdna2Opcode::V_CMPX_EQ_F16:      return "v_cmpx_eq_f16";
        case Rdna2Opcode::V_CMPX_LE_F16:      return "v_cmpx_le_f16";
        case Rdna2Opcode::V_CMPX_GT_F16:      return "v_cmpx_gt_f16";
        case Rdna2Opcode::V_CMPX_LG_F16:      return "v_cmpx_lg_f16";
        case Rdna2Opcode::V_CMPX_GE_F16:      return "v_cmpx_ge_f16";
        case Rdna2Opcode::V_CMPX_NEQ_F16:     return "v_cmpx_neq_f16";
        case Rdna2Opcode::V_CMPX_NLT_F16:     return "v_cmpx_nlt_f16";
        case Rdna2Opcode::V_CMP_LT_I64:       return "v_cmp_lt_i64";
        case Rdna2Opcode::V_CMP_EQ_I64:       return "v_cmp_eq_i64";
        case Rdna2Opcode::V_CMP_LE_I64:       return "v_cmp_le_i64";
        case Rdna2Opcode::V_CMP_GT_I64:       return "v_cmp_gt_i64";
        case Rdna2Opcode::V_CMP_LG_I64:       return "v_cmp_lg_i64";
        case Rdna2Opcode::V_CMP_GE_I64:       return "v_cmp_ge_i64";
        case Rdna2Opcode::V_CMP_LT_U64:       return "v_cmp_lt_u64";
        case Rdna2Opcode::V_CMP_EQ_U64:       return "v_cmp_eq_u64";
        case Rdna2Opcode::V_CMP_LE_U64:       return "v_cmp_le_u64";
        case Rdna2Opcode::V_CMP_GT_U64:       return "v_cmp_gt_u64";
        case Rdna2Opcode::V_CMP_LG_U64:       return "v_cmp_lg_u64";
        case Rdna2Opcode::V_CMP_GE_U64:       return "v_cmp_ge_u64";
        case Rdna2Opcode::V_FMA_F32:          return "v_fma_f32";
        case Rdna2Opcode::V_LSHL_ADD_U32:     return "v_lshl_add_u32";
        case Rdna2Opcode::V_LSHL_OR_B32:      return "v_lshl_or_b32";
        case Rdna2Opcode::V_MIN3_F32:         return "v_min3_f32";
        case Rdna2Opcode::V_MIN3_I32:         return "v_min3_i32";
        case Rdna2Opcode::V_MIN3_U32:         return "v_min3_u32";
        case Rdna2Opcode::V_MAX3_F32:         return "v_max3_f32";
        case Rdna2Opcode::V_MAX3_I32:         return "v_max3_i32";
        case Rdna2Opcode::V_MAX3_U32:         return "v_max3_u32";
        case Rdna2Opcode::V_MED3_F32:         return "v_med3_f32";
        case Rdna2Opcode::V_MED3_I32:         return "v_med3_i32";
        case Rdna2Opcode::V_MED3_U32:         return "v_med3_u32";
        case Rdna2Opcode::V_FMA_F16:          return "v_fma_f16";
        case Rdna2Opcode::V_MAD_F32:          return "v_mad_f32";
        case Rdna2Opcode::V_BFE_U32:          return "v_bfe_u32";
        case Rdna2Opcode::V_BFE_I32:          return "v_bfe_i32";
        case Rdna2Opcode::V_BFI_B32:          return "v_bfi_b32";
        case Rdna2Opcode::V_ALIGNBYTE_B32:    return "v_alignbyte_b32";
        case Rdna2Opcode::V_ALIGNBIT_B32:     return "v_alignbit_b32";
        case Rdna2Opcode::V_DIV_FMAS_F32:     return "v_div_fmas_f32";
        case Rdna2Opcode::V_MUL_LO_U32:       return "v_mul_lo_u32";
        case Rdna2Opcode::V_MAD_U64_U32:      return "v_mad_u64_u32";
        case Rdna2Opcode::V_MAD_I64_I32:      return "v_mad_i64_i32";
        case Rdna2Opcode::V_ADD_NC_U16:       return "v_add_nc_u16";
        case Rdna2Opcode::V_SUB_NC_U16:       return "v_sub_nc_u16";
        case Rdna2Opcode::V_MIN_U16:          return "v_min_u16";
        case Rdna2Opcode::V_MAX_U16:          return "v_max_u16";
        case Rdna2Opcode::V_MIN_I16:          return "v_min_i16";
        case Rdna2Opcode::V_MAX_I16:          return "v_max_i16";
        case Rdna2Opcode::V_MUL_LO_U16:       return "v_mul_lo_u16";
        case Rdna2Opcode::V_ASHRREV_I16:      return "v_ashrrev_i16";
        case Rdna2Opcode::V_LSHLREV_B16:      return "v_lshlrev_b16";
        case Rdna2Opcode::V_LSHRREV_B16:      return "v_lshrrev_b16";
        case Rdna2Opcode::V_MAD_U16:          return "v_mad_u16";
        case Rdna2Opcode::V_MBCNT_LO_U32_B32: return "v_mbcnt_lo_u32_b32";
        case Rdna2Opcode::V_MBCNT_HI_U32_B32: return "v_mbcnt_hi_u32_b32";
        case Rdna2Opcode::V_READLANE_B32:       return "v_readlane_b32";
        case Rdna2Opcode::V_CVT_PKNORM_I16_F16: return "v_cvt_pknorm_i16_f16";
        case Rdna2Opcode::V_CVT_PKNORM_U16_F16: return "v_cvt_pknorm_u16_f16";
        case Rdna2Opcode::V_CVT_PKRTZ_F16_F32:  return "v_cvt_pkrtz_f16_f32";
        case Rdna2Opcode::V_CVT_PKNORM_I16_F32: return "v_cvt_pknorm_i16_f32";
        case Rdna2Opcode::V_CVT_PKNORM_U16_F32: return "v_cvt_pknorm_u16_f32";
        case Rdna2Opcode::V_ADD3_U32:           return "v_add3_u32";
        case Rdna2Opcode::V_DOT2_F32_F16:     return "v_dot2_f32_f16";
        case Rdna2Opcode::V_PERMLANE16_B32:   return "v_permlane16_b32";
        case Rdna2Opcode::V_PERMLANEX16_B32:  return "v_permlanex16_b32";
        case Rdna2Opcode::V_PK_LSHLREV_B16:   return "v_pk_lshlrev_b16";
        case Rdna2Opcode::V_PK_LSHRREV_B16:   return "v_pk_lshrrev_b16";
        case Rdna2Opcode::V_PK_ADD_U16:       return "v_pk_add_u16";
        case Rdna2Opcode::V_PK_FMA_F16:       return "v_pk_fma_f16";
        case Rdna2Opcode::V_PK_ADD_F16:       return "v_pk_add_f16";
        case Rdna2Opcode::V_PK_MUL_F16:       return "v_pk_mul_f16";
        case Rdna2Opcode::V_FMA_MIX_F32:      return "v_fma_mix_f32";
        case Rdna2Opcode::V_FMA_MIXLO_F16:    return "v_fma_mixlo_f16";
        case Rdna2Opcode::V_FMA_MIXHI_F16:    return "v_fma_mixhi_f16";
        case Rdna2Opcode::S_LOAD_DWORD:           return "s_load_dword";
        case Rdna2Opcode::S_BUFFER_LOAD_DWORD:    return "s_buffer_load_dword";
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX2:  return "s_buffer_load_dwordx2";
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX4:  return "s_buffer_load_dwordx4";
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX8:  return "s_buffer_load_dwordx8";
        case Rdna2Opcode::S_BUFFER_LOAD_DWORDX16: return "s_buffer_load_dwordx16";
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_X:        return "buffer_load_format_x";
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XY:       return "buffer_load_format_xy";
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZ:      return "buffer_load_format_xyz";
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW:     return "buffer_load_format_xyzw";
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_X:    return "buffer_load_format_d16_x";
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XY:   return "buffer_load_format_d16_xy";
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZ:  return "buffer_load_format_d16_xyz";
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZW: return "buffer_load_format_d16_xyzw";
        case Rdna2Opcode::BUFFER_LOAD_DWORD:           return "buffer_load_dword";
        case Rdna2Opcode::BUFFER_STORE_BYTE_D16_HI:    return "buffer_store_byte_d16_hi";
        case Rdna2Opcode::BUFFER_STORE_SHORT_D16_HI:   return "buffer_store_short_d16_hi";
        case Rdna2Opcode::BUFFER_STORE_DWORD:          return "buffer_store_dword";
        case Rdna2Opcode::BUFFER_LOAD_UBYTE_D16:       return "buffer_load_ubyte_d16";
        case Rdna2Opcode::BUFFER_LOAD_UBYTE_D16_HI:    return "buffer_load_ubyte_d16_hi";
        case Rdna2Opcode::BUFFER_LOAD_SBYTE_D16:       return "buffer_load_sbyte_d16";
        case Rdna2Opcode::BUFFER_LOAD_SBYTE_D16_HI:    return "buffer_load_sbyte_d16_hi";
        case Rdna2Opcode::BUFFER_LOAD_SHORT_D16:       return "buffer_load_short_d16";
        case Rdna2Opcode::BUFFER_LOAD_SHORT_D16_HI:    return "buffer_load_short_d16_hi";
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_X:       return "tbuffer_load_format_x";
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XY:      return "tbuffer_load_format_xy";
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZ:     return "tbuffer_load_format_xyz";
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZW:    return "tbuffer_load_format_xyzw";
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_X:   return "tbuffer_load_format_d16_x";
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY:  return "tbuffer_load_format_d16_xy";
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZ: return "tbuffer_load_format_d16_xyz";
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW:return "tbuffer_load_format_d16_xyzw";
        case Rdna2Opcode::IMAGE_LOAD:                  return "image_load";
        case Rdna2Opcode::IMAGE_LOAD_MIP:              return "image_load_mip";
        case Rdna2Opcode::IMAGE_STORE:                 return "image_store";
        case Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP:        return "image_atomic_cmpswap";
        case Rdna2Opcode::IMAGE_SAMPLE:                return "image_sample";
        case Rdna2Opcode::IMAGE_SAMPLE_CL:             return "image_sample_cl";
        case Rdna2Opcode::IMAGE_SAMPLE_D_CL:           return "image_sample_d_cl";
        case Rdna2Opcode::IMAGE_SAMPLE_LZ:             return "image_sample_lz";
        case Rdna2Opcode::IMAGE_SAMPLE_B_CL:           return "image_sample_b_cl";
        case Rdna2Opcode::IMAGE_SAMPLE_C_CL:           return "image_sample_c_cl";
        case Rdna2Opcode::IMAGE_SAMPLE_C_D_CL:         return "image_sample_c_d_cl";
        case Rdna2Opcode::IMAGE_SAMPLE_C_B_CL:         return "image_sample_c_b_cl";
        case Rdna2Opcode::IMAGE_SAMPLE_C_LZ:           return "image_sample_c_lz";
        case Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY:     return "image_bvh_intersect_ray";
        case Rdna2Opcode::DS_ADD_U32:                  return "ds_add_u32";
        case Rdna2Opcode::DS_MSKOR_B32:                return "ds_mskor_b32";
        case Rdna2Opcode::DS_WRITE_B32:                return "ds_write_b32";
        case Rdna2Opcode::DS_NOP:                      return "ds_nop";
        case Rdna2Opcode::DS_ADD_RTN_U32:              return "ds_add_rtn_u32";
        case Rdna2Opcode::DS_MSKOR_RTN_B32:            return "ds_mskor_rtn_b32";
        case Rdna2Opcode::DS_WRXCHG2_RTN_B32:          return "ds_wrxchg2_rtn_b32";
        case Rdna2Opcode::DS_WRXCHG2ST64_RTN_B32:      return "ds_wrxchg2st64_rtn_b32";
        case Rdna2Opcode::DS_SWIZZLE_B32:              return "ds_swizzle_b32";
        case Rdna2Opcode::DS_READ_B32:                 return "ds_read_b32";
        case Rdna2Opcode::DS_READ_U8_D16:              return "ds_read_u8_d16";
        case Rdna2Opcode::DS_READ_U8_D16_HI:           return "ds_read_u8_d16_hi";
        case Rdna2Opcode::DS_READ_I8_D16:              return "ds_read_i8_d16";
        case Rdna2Opcode::DS_READ_I8_D16_HI:           return "ds_read_i8_d16_hi";
        case Rdna2Opcode::DS_READ_U16_D16:             return "ds_read_u16_d16";
        case Rdna2Opcode::DS_READ_U16_D16_HI:          return "ds_read_u16_d16_hi";
        case Rdna2Opcode::DS_WRITE_B96:                return "ds_write_b96";
        case Rdna2Opcode::DS_WRITE_B128:               return "ds_write_b128";
        case Rdna2Opcode::DS_READ_B96:                 return "ds_read_b96";
        case Rdna2Opcode::DS_READ_B128:                return "ds_read_b128";
        case Rdna2Opcode::GLOBAL_LOAD_DWORD:           return "global_load_dword";
        case Rdna2Opcode::GLOBAL_STORE_DWORD:          return "global_store_dword";
        case Rdna2Opcode::FLAT_LOAD_DWORD:             return "flat_load_dword";
        case Rdna2Opcode::FLAT_STORE_DWORD:            return "flat_store_dword";
        case Rdna2Opcode::SCRATCH_LOAD_DWORD:          return "scratch_load_dword";
        case Rdna2Opcode::SCRATCH_STORE_DWORD:         return "scratch_store_dword";
        case Rdna2Opcode::EXP:                         return "exp";
        case Rdna2Opcode::V_INTERP_P1_F32:             return "v_interp_p1_f32";
        case Rdna2Opcode::V_INTERP_P2_F32:             return "v_interp_p2_f32";
        case Rdna2Opcode::V_INTERP_MOV_F32:            return "v_interp_mov_f32";
        default:                                       return "unknown";
    }
}

bool IsVcmpxOpcode(Rdna2Opcode op) noexcept {
    switch (op) {
        case Rdna2Opcode::V_CMPX_LT_F32:
        case Rdna2Opcode::V_CMPX_EQ_F32:
        case Rdna2Opcode::V_CMPX_LE_F32:
        case Rdna2Opcode::V_CMPX_GT_F32:
        case Rdna2Opcode::V_CMPX_LG_F32:
        case Rdna2Opcode::V_CMPX_GE_F32:
        case Rdna2Opcode::V_CMPX_LT_I32:
        case Rdna2Opcode::V_CMPX_EQ_I32:
        case Rdna2Opcode::V_CMPX_GT_I32:
        case Rdna2Opcode::V_CMPX_GE_I32:
        case Rdna2Opcode::V_CMPX_LT_U32:
        case Rdna2Opcode::V_CMPX_EQ_U32:
        case Rdna2Opcode::V_CMPX_GT_U32:
        case Rdna2Opcode::V_CMPX_GE_U32:
        case Rdna2Opcode::V_CMPX_LT_F16:
        case Rdna2Opcode::V_CMPX_EQ_F16:
        case Rdna2Opcode::V_CMPX_LE_F16:
        case Rdna2Opcode::V_CMPX_GT_F16:
        case Rdna2Opcode::V_CMPX_LG_F16:
        case Rdna2Opcode::V_CMPX_GE_F16:
        case Rdna2Opcode::V_CMPX_NEQ_F16:
        case Rdna2Opcode::V_CMPX_NLT_F16:
            return true;
        default:
            return false;
    }
}

bool IsF16CompareOpcode(Rdna2Opcode op) noexcept {
    switch (op) {
        case Rdna2Opcode::V_CMP_LT_F16:
        case Rdna2Opcode::V_CMP_EQ_F16:
        case Rdna2Opcode::V_CMP_LE_F16:
        case Rdna2Opcode::V_CMP_GT_F16:
        case Rdna2Opcode::V_CMP_LG_F16:
        case Rdna2Opcode::V_CMP_GE_F16:
        case Rdna2Opcode::V_CMP_U_F16:
        case Rdna2Opcode::V_CMP_NGE_F16:
        case Rdna2Opcode::V_CMP_NLG_F16:
        case Rdna2Opcode::V_CMP_NGT_F16:
        case Rdna2Opcode::V_CMP_NLE_F16:
        case Rdna2Opcode::V_CMP_NEQ_F16:
        case Rdna2Opcode::V_CMP_NLT_F16:
        case Rdna2Opcode::V_CMP_TRU_F16:
        case Rdna2Opcode::V_CMPX_LT_F16:
        case Rdna2Opcode::V_CMPX_EQ_F16:
        case Rdna2Opcode::V_CMPX_LE_F16:
        case Rdna2Opcode::V_CMPX_GT_F16:
        case Rdna2Opcode::V_CMPX_LG_F16:
        case Rdna2Opcode::V_CMPX_GE_F16:
        case Rdna2Opcode::V_CMPX_NEQ_F16:
        case Rdna2Opcode::V_CMPX_NLT_F16:
            return true;
        default:
            return false;
    }
}

bool IsVop3EncodingWord(uint32_t word0) noexcept {
    // GFX10.3 / RDNA2 64-bit VOP3 prefix [31:26] == 0b110101 (top byte 0xD4..0xD7)
    const uint32_t top_byte = (word0 >> 24) & 0xFFu;
    return top_byte >= 0xD4u && top_byte <= 0xD7u;
}

bool IsVop3Int16Instruction(const Rdna2Instruction& inst) noexcept {
    return inst.encoding == Rdna2Encoding::VOP3 &&
           inst.word_count >= 2u &&
           IsVop3EncodingWord(inst.raw_words[0]) &&
           inst.operand_width == Rdna2OperandWidth::Bits16 &&
           (inst.data_kind == Rdna2DataKind::UnsignedInt || inst.data_kind == Rdna2DataKind::SignedInt);
}

bool IsVop3Float16Instruction(const Rdna2Instruction& inst) noexcept {
    return inst.encoding == Rdna2Encoding::VOP3 &&
           inst.word_count >= 2u &&
           IsVop3EncodingWord(inst.raw_words[0]) &&
           inst.operand_width == Rdna2OperandWidth::Bits16 &&
           inst.data_kind == Rdna2DataKind::Float;
}

namespace {

void PopulateInstructionMetadata(Rdna2Instruction& inst) noexcept {
    inst.operand_width = Rdna2OperandWidth::Bits32;
    inst.data_kind = Rdna2DataKind::Untyped;
    switch (inst.opcode) {
        case Rdna2Opcode::S_BCNT1_I32_B64:
        case Rdna2Opcode::S_GETPC_B64:
            inst.operand_width = Rdna2OperandWidth::Bits64;
            inst.data_kind = Rdna2DataKind::UnsignedInt;
            break;
        case Rdna2Opcode::S_BCNT1_I32_B32:
            inst.operand_width = Rdna2OperandWidth::Bits32;
            inst.data_kind = Rdna2DataKind::UnsignedInt;
            break;
        case Rdna2Opcode::V_FMAC_F16:
        case Rdna2Opcode::V_FMAMK_F16:
        case Rdna2Opcode::V_FMAAK_F16:
        case Rdna2Opcode::V_FMA_F16:
        case Rdna2Opcode::V_CMP_LT_F16:
        case Rdna2Opcode::V_CMP_EQ_F16:
        case Rdna2Opcode::V_CMP_LE_F16:
        case Rdna2Opcode::V_CMP_GT_F16:
        case Rdna2Opcode::V_CMP_LG_F16:
        case Rdna2Opcode::V_CMP_GE_F16:
        case Rdna2Opcode::V_CMP_U_F16:
        case Rdna2Opcode::V_CMP_NGE_F16:
        case Rdna2Opcode::V_CMP_NLG_F16:
        case Rdna2Opcode::V_CMP_NGT_F16:
        case Rdna2Opcode::V_CMP_NLE_F16:
        case Rdna2Opcode::V_CMP_NEQ_F16:
        case Rdna2Opcode::V_CMP_NLT_F16:
        case Rdna2Opcode::V_CMP_TRU_F16:
        case Rdna2Opcode::V_CMPX_LT_F16:
        case Rdna2Opcode::V_CMPX_EQ_F16:
        case Rdna2Opcode::V_CMPX_LE_F16:
        case Rdna2Opcode::V_CMPX_GT_F16:
        case Rdna2Opcode::V_CMPX_LG_F16:
        case Rdna2Opcode::V_CMPX_GE_F16:
        case Rdna2Opcode::V_CMPX_NEQ_F16:
        case Rdna2Opcode::V_CMPX_NLT_F16:
            inst.operand_width = Rdna2OperandWidth::Bits16;
            inst.data_kind = Rdna2DataKind::Float;
            break;
        case Rdna2Opcode::V_ADD_NC_U16:
        case Rdna2Opcode::V_SUB_NC_U16:
        case Rdna2Opcode::V_MIN_U16:
        case Rdna2Opcode::V_MAX_U16:
        case Rdna2Opcode::V_MUL_LO_U16:
        case Rdna2Opcode::V_LSHLREV_B16:
        case Rdna2Opcode::V_LSHRREV_B16:
        case Rdna2Opcode::V_MAD_U16:
        case Rdna2Opcode::V_CVT_NORM_U16_F16:
            inst.operand_width = Rdna2OperandWidth::Bits16;
            inst.data_kind = Rdna2DataKind::UnsignedInt;
            break;
        case Rdna2Opcode::V_MIN_I16:
        case Rdna2Opcode::V_MAX_I16:
        case Rdna2Opcode::V_ASHRREV_I16:
        case Rdna2Opcode::V_CVT_NORM_I16_F16:
            inst.operand_width = Rdna2OperandWidth::Bits16;
            inst.data_kind = Rdna2DataKind::SignedInt;
            break;
        case Rdna2Opcode::V_PK_LSHLREV_B16:
        case Rdna2Opcode::V_PK_LSHRREV_B16:
        case Rdna2Opcode::V_PK_ADD_U16:
        case Rdna2Opcode::V_PK_FMA_F16:
        case Rdna2Opcode::V_PK_ADD_F16:
        case Rdna2Opcode::V_PK_MUL_F16:
        case Rdna2Opcode::V_CVT_PKNORM_I16_F16:
        case Rdna2Opcode::V_CVT_PKNORM_U16_F16:
        case Rdna2Opcode::V_CVT_PKNORM_I16_F32:
        case Rdna2Opcode::V_CVT_PKNORM_U16_F32:
            inst.operand_width = Rdna2OperandWidth::Bits16;
            inst.data_kind = Rdna2DataKind::Packed16;
            break;
        case Rdna2Opcode::V_ADD_F32:
        case Rdna2Opcode::V_SUB_F32:
        case Rdna2Opcode::V_SUBREV_F32:
        case Rdna2Opcode::V_MUL_F32:
        case Rdna2Opcode::V_MIN_F32:
        case Rdna2Opcode::V_MAX_F32:
        case Rdna2Opcode::V_MAC_F32:
        case Rdna2Opcode::V_MADMK_F32:
        case Rdna2Opcode::V_MADAK_F32:
        case Rdna2Opcode::V_FMAC_F32:
        case Rdna2Opcode::V_FMA_F32:
        case Rdna2Opcode::V_MAD_F32:
            inst.operand_width = Rdna2OperandWidth::Bits32;
            inst.data_kind = Rdna2DataKind::Float;
            break;
        default:
            break;
    }
}

bool Vop2OpcodeHasMandatoryLiteral(Rdna2Opcode op) noexcept {
    return op == Rdna2Opcode::V_FMAMK_F16 ||
           op == Rdna2Opcode::V_FMAAK_F16 ||
           op == Rdna2Opcode::V_MADMK_F32 ||
           op == Rdna2Opcode::V_MADAK_F32;
}

}  // namespace

std::string FormatRdna2Operand(const Rdna2Operand& op) {
    std::string base;
    char buf[64]{};
    switch (op.kind) {
        case Rdna2OperandKind::Sgpr:
            base = "s" + std::to_string(op.reg_index);
            break;
        case Rdna2OperandKind::Vgpr:
            base = "v" + std::to_string(op.reg_index);
            break;
        case Rdna2OperandKind::InlineInt:
            base = std::to_string(op.int_value);
            break;
        case Rdna2OperandKind::InlineFloat:
            std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(op.float_value));
            base = buf;
            break;
        case Rdna2OperandKind::Literal32:
            std::snprintf(buf, sizeof(buf), "0x%08x", op.literal_bits);
            base = buf;
            break;
        case Rdna2OperandKind::VccLo:  base = "vcc_lo"; break;
        case Rdna2OperandKind::VccHi:  base = "vcc_hi"; break;
        case Rdna2OperandKind::ExecLo: base = "exec_lo"; break;
        case Rdna2OperandKind::ExecHi: base = "exec_hi"; break;
        case Rdna2OperandKind::Scc:    base = "scc"; break;
        case Rdna2OperandKind::M0:     base = "m0"; break;
        case Rdna2OperandKind::Null:   base = "null"; break;
        default:                       base = "<none>"; break;
    }
    if (op.abs) base = "|" + base + "|";
    if (op.neg) base = "-" + base;
    return base;
}

std::string FormatRdna2Instruction(const Rdna2Instruction& inst) {
    std::ostringstream os;
    os << inst.mnemonic;

    switch (inst.encoding) {
        case Rdna2Encoding::SOPP: {
            if (inst.opcode == Rdna2Opcode::S_ENDPGM ||
                inst.opcode == Rdna2Opcode::S_BARRIER ||
                inst.opcode == Rdna2Opcode::S_ICACHE_INV ||
                inst.opcode == Rdna2Opcode::S_TTRACEDATA ||
                inst.opcode == Rdna2Opcode::S_CODE_END) {
                break;
            }
            os << " " << inst.simm16;
            break;
        }
        case Rdna2Encoding::SOPK: {
            if (inst.opcode == Rdna2Opcode::S_VERSION) {
                os << " " << inst.simm16;
            } else {
                os << " " << FormatRdna2Operand(inst.dst) << ", " << inst.simm16;
            }
            break;
        }
        case Rdna2Encoding::SOPC: {
            os << " " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1]);
            break;
        }
        case Rdna2Encoding::VOPC: {
            if (IsVcmpxOpcode(inst.opcode)) {
                os << " exec_lo, " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1]);
            } else {
                os << " vcc_lo, " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1]);
            }
            break;
        }
        case Rdna2Encoding::VINTRP: {
            static constexpr char kChanChar[4] = {'x', 'y', 'z', 'w'};
            os << " " << FormatRdna2Operand(inst.dst) << ", " << FormatRdna2Operand(inst.src[0])
               << ", attr" << static_cast<uint32_t>(inst.vintrp_attr) << "." << kChanChar[inst.vintrp_chan & 3u];
            break;
        }
        case Rdna2Encoding::MTBUF: {
            const uint32_t base_s = static_cast<uint32_t>(inst.srsrc) * 4u;
            os << " " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1])
               << ", s[" << base_s << ":" << (base_s + 3u) << "], "
               << FormatRdna2Operand(DecodeRdna2SourceOperand(inst.soffset))
               << " format:" << static_cast<uint32_t>(inst.mtbuf_format);
            if (inst.idxen) os << " idxen";
            if (inst.offen) os << " offen";
            if (inst.mem_offset != 0) os << " offset:" << inst.mem_offset;
            break;
        }
        case Rdna2Encoding::SMEM: {
            const uint32_t base_s = static_cast<uint32_t>(inst.sbase) * 2u;
            const uint32_t end_s = (inst.opcode == Rdna2Opcode::S_BUFFER_LOAD_DWORD) ? (base_s + 3u) : (base_s + 1u);
            os << " " << FormatRdna2Operand(inst.dst) << ", s[" << base_s << ":" << end_s << "], ";
            if (inst.mem_offset != 0 || inst.soffset == 0x7Du || inst.soffset == 0x7Cu) {
                os << inst.mem_offset;
            } else {
                os << "s" << static_cast<uint32_t>(inst.soffset);
            }
            break;
        }
        case Rdna2Encoding::MUBUF: {
            const uint32_t base_s = static_cast<uint32_t>(inst.srsrc) * 4u;
            os << " " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1])
               << ", s[" << base_s << ":" << (base_s + 3u) << "], "
               << FormatRdna2Operand(DecodeRdna2SourceOperand(inst.soffset));
            if (inst.idxen) os << " idxen";
            if (inst.offen) os << " offen";
            if (inst.mem_offset != 0) os << " offset:" << inst.mem_offset;
            break;
        }
        case Rdna2Encoding::MIMG: {
            const uint32_t base_rsrc = static_cast<uint32_t>(inst.srsrc) * 4u;
            const uint32_t base_samp = static_cast<uint32_t>(inst.ssamp) * 4u;
            if (inst.opcode == Rdna2Opcode::IMAGE_STORE ||
                inst.opcode == Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP) {
                os << " " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1]);
            } else {
                os << " " << FormatRdna2Operand(inst.dst) << ", " << FormatRdna2Operand(inst.src[0]);
            }
            os << ", s[" << base_rsrc << ":" << (base_rsrc + 7u) << "]";
            if (inst.opcode == Rdna2Opcode::IMAGE_SAMPLE ||
                inst.opcode == Rdna2Opcode::IMAGE_SAMPLE_LZ ||
                inst.opcode == Rdna2Opcode::IMAGE_SAMPLE_C_LZ) {
                os << ", s[" << base_samp << ":" << (base_samp + 3u) << "]";
            }
            os << " dmask:0x" << std::hex << static_cast<uint32_t>(inst.mimg_dmask) << std::dec
               << " dim:" << static_cast<uint32_t>(inst.mimg_dim);
            if (inst.mimg_unrm) os << " unrm";
            if (inst.glc) os << " glc";
            break;
        }
        case Rdna2Encoding::DS: {
            bool first = true;
            if (inst.dst.kind != Rdna2OperandKind::None) {
                os << " " << FormatRdna2Operand(inst.dst);
                first = false;
            }
            for (uint8_t i = 0; i < inst.src_count; ++i) {
                os << (first ? " " : ", ") << FormatRdna2Operand(inst.src[i]);
                first = false;
            }
            if (inst.mem_offset != 0) os << " offset:" << inst.mem_offset;
            if (inst.ds_gds) os << " gds";
            break;
        }
        case Rdna2Encoding::FLAT: {
            if (inst.opcode == Rdna2Opcode::GLOBAL_STORE_DWORD ||
                inst.opcode == Rdna2Opcode::FLAT_STORE_DWORD ||
                inst.opcode == Rdna2Opcode::SCRATCH_STORE_DWORD) {
                os << " " << FormatRdna2Operand(inst.src[0]) << ", " << FormatRdna2Operand(inst.src[1]);
            } else {
                os << " " << FormatRdna2Operand(inst.dst) << ", " << FormatRdna2Operand(inst.src[0]);
            }
            if (inst.saddr != 0x7Fu) {
                if (inst.flat_seg == 1u) {
                    os << ", s" << static_cast<uint32_t>(inst.saddr);
                } else {
                    os << ", s[" << static_cast<uint32_t>(inst.saddr) << ":" << (static_cast<uint32_t>(inst.saddr) + 1u) << "]";
                }
            } else {
                os << ", off";
            }
            if (inst.mem_offset != 0) os << " offset:" << inst.mem_offset;
            if (inst.dlc) os << " dlc";
            if (inst.glc) os << " glc";
            if (inst.slc) os << " slc";
            break;
        }
        case Rdna2Encoding::EXP: {
            os << " " << FormatExpTarget(inst.exp_target);
            for (uint8_t i = 0; i < 4; ++i) {
                os << ", ";
                if ((inst.exp_en & (1u << i)) != 0u) {
                    os << FormatRdna2Operand(inst.src[i]);
                } else {
                    os << "off";
                }
            }
            if (inst.exp_done) os << " done";
            if (inst.exp_compr) os << " compr";
            if (inst.exp_vm) os << " vm";
            break;
        }
        default: {
            bool first = true;
            if (inst.dst.kind != Rdna2OperandKind::None) {
                os << " " << FormatRdna2Operand(inst.dst);
                first = false;
            }
            for (uint8_t i = 0; i < inst.src_count; ++i) {
                os << (first ? " " : ", ") << FormatRdna2Operand(inst.src[i]);
                first = false;
            }
            if (inst.clamp) os << " clamp";
            break;
        }
    }
    return os.str();
}

// ---------------------------------------------------------------------------
// RDNA2 / GFX10.3 Binary Disassembler
// ---------------------------------------------------------------------------
Status DisassembleRdna2(
    const std::vector<uint32_t>& words,
    std::vector<Rdna2Instruction>& out_instructions) {
    out_instructions.clear();
    if (words.empty()) {
        return Status::Error(StatusCode::ParseError, "Empty RDNA2 instruction stream");
    }

    size_t pc = 0;
    while (pc < words.size()) {
        const uint32_t w0 = words[pc];
        Rdna2Instruction inst{};
        inst.word_offset = static_cast<uint32_t>(pc);
        inst.raw_words[0] = w0;
        inst.word_count = 1;

        auto consume_word = [&](uint32_t& out_w) -> Status {
            if (pc + inst.word_count >= words.size()) {
                return Status::Error(StatusCode::ParseError, "Truncated multi-word RDNA2 instruction");
            }
            out_w = words[pc + inst.word_count];
            inst.raw_words[inst.word_count] = out_w;
            ++inst.word_count;
            return Status::Ok();
        };

        auto decode_scalar_dst = [](uint8_t sdst) -> Rdna2Operand {
            if (sdst == 106u) return Rdna2Operand::VccLo();
            if (sdst == 107u) return Rdna2Operand::VccHi();
            if (sdst == 124u) return Rdna2Operand::M0();
            if (sdst == 125u) return Rdna2Operand::Null();
            if (sdst == 126u) return Rdna2Operand::ExecLo();
            if (sdst == 127u) return Rdna2Operand::ExecHi();
            return Rdna2Operand::Sgpr(sdst);
        };

        if ((w0 >> 23) == 0x17Du) {
            // SOP1: [31:23] == 101111101
            inst.encoding = Rdna2Encoding::SOP1;
            const uint8_t sdst = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
            uint32_t lit = 0;
            if (ssrc0 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSop1Opcode(raw_op);
            inst.dst = decode_scalar_dst(sdst);
            if (inst.opcode != Rdna2Opcode::S_GETPC_B64) {
                inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
                inst.src_count = 1;
            }
        } else if ((w0 >> 23) == 0x17Eu) {
            // SOPC: [31:23] == 101111110
            inst.encoding = Rdna2Encoding::SOPC;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            const uint16_t ssrc1 = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
            uint32_t lit = 0;
            if (ssrc0 == 255u || ssrc1 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSopcOpcode(raw_op);
            inst.dst = Rdna2Operand::Scc();
            inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
            inst.src[1] = DecodeRdna2SourceOperand(ssrc1, lit);
            inst.src_count = 2;
        } else if ((w0 >> 23) == 0x17Fu) {
            // SOPP: [31:23] == 101111111
            inst.encoding = Rdna2Encoding::SOPP;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSoppOpcode(raw_op);
            inst.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
        } else if ((w0 >> 28) == 0xBu) {
            // SOPK: [31:28] == 1011
            inst.encoding = Rdna2Encoding::SOPK;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 23) & 0x1Fu);
            const uint8_t sdst = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
            if (raw_op == 0x14u || raw_op == 0x15u) {
                uint32_t imm32 = 0;
                if (auto st = consume_word(imm32); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSopkOpcode(raw_op);
            if (inst.opcode != Rdna2Opcode::S_VERSION) {
                inst.dst = decode_scalar_dst(sdst);
            }
            inst.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
        } else if ((w0 >> 30) == 0x2u) {
            // SOP2: [31:30] == 10
            inst.encoding = Rdna2Encoding::SOP2;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 23) & 0x7Fu);
            const uint8_t sdst = static_cast<uint8_t>((w0 >> 16) & 0x7Fu);
            const uint16_t ssrc1 = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            const uint16_t ssrc0 = static_cast<uint16_t>(w0 & 0xFFu);
            uint32_t lit = 0;
            if (ssrc0 == 255u || ssrc1 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSop2Opcode(raw_op);
            inst.dst = decode_scalar_dst(sdst);
            inst.src[0] = DecodeRdna2SourceOperand(ssrc0, lit);
            inst.src[1] = DecodeRdna2SourceOperand(ssrc1, lit);
            inst.src_count = 2;
        } else if ((w0 >> 25) == 0x3Fu) {
            // VOP1: [31:25] == 0111111
            inst.encoding = Rdna2Encoding::VOP1;
            const uint8_t vdst = static_cast<uint8_t>((w0 >> 17) & 0xFFu);
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 9) & 0xFFu);
            const uint16_t src0 = static_cast<uint16_t>(w0 & 0x1FFu);
            uint32_t lit = 0;
            uint32_t dpp8_raw = 0;
            bool has_sdwa_dpp16 = false;
            if (src0 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            } else if (src0 == 0xE9u || src0 == 0xEAu) {
                if (auto st = consume_word(dpp8_raw); !st.ok()) return st;
                inst.has_dpp8 = true;
                inst.dpp8_fi = (src0 == 0xEAu);
                inst.dpp8_word = (dpp8_raw >> 8) & 0xFFFFFFu;
            } else if (src0 == 0xF9u || src0 == 0xFAu) {
                if (auto st = consume_word(dpp8_raw); !st.ok()) return st;
                has_sdwa_dpp16 = true;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVop1Opcode(raw_op);
            if (inst.opcode != Rdna2Opcode::V_NOP) {
                if (inst.opcode == Rdna2Opcode::V_READFIRSTLANE_B32) {
                    inst.dst = Rdna2Operand::Sgpr(static_cast<uint8_t>(vdst & 0x7Fu));
                } else {
                    inst.dst = Rdna2Operand::Vgpr(vdst);
                }
                inst.src[0] = (inst.has_dpp8 || has_sdwa_dpp16)
                                  ? Rdna2Operand::Vgpr(static_cast<uint8_t>(dpp8_raw & 0xFFu))
                                  : DecodeRdna2SourceOperand(src0, lit);
                inst.src_count = 1;
            }
        } else if ((w0 >> 25) == 0x3Eu) {
            // VOPC: [31:25] == 0111110
            inst.encoding = Rdna2Encoding::VOPC;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 17) & 0xFFu);
            const uint8_t vsrc1 = static_cast<uint8_t>((w0 >> 9) & 0xFFu);
            const uint16_t src0 = static_cast<uint16_t>(w0 & 0x1FFu);
            uint32_t lit = 0;
            uint32_t dpp8_raw = 0;
            bool has_sdwa_dpp16 = false;
            if (src0 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            } else if (src0 == 0xE9u || src0 == 0xEAu) {
                if (auto st = consume_word(dpp8_raw); !st.ok()) return st;
                inst.has_dpp8 = true;
                inst.dpp8_fi = (src0 == 0xEAu);
                inst.dpp8_word = (dpp8_raw >> 8) & 0xFFFFFFu;
            } else if (src0 == 0xF9u || src0 == 0xFAu) {
                if (auto st = consume_word(dpp8_raw); !st.ok()) return st;
                has_sdwa_dpp16 = true;
            }
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVopcOpcode(raw_op);
            inst.dst = IsVcmpxOpcode(inst.opcode) ? Rdna2Operand::ExecLo() : Rdna2Operand::VccLo();
            inst.src[0] = (inst.has_dpp8 || has_sdwa_dpp16)
                              ? Rdna2Operand::Vgpr(static_cast<uint8_t>(dpp8_raw & 0xFFu))
                              : DecodeRdna2SourceOperand(src0, lit);
            inst.src[1] = Rdna2Operand::Vgpr(vsrc1);
            inst.src_count = 2;
        } else if ((w0 >> 31) == 0x0u) {
            // VOP2: [31] == 0
            inst.encoding = Rdna2Encoding::VOP2;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 25) & 0x3Fu);
            const uint8_t vdst = static_cast<uint8_t>((w0 >> 17) & 0xFFu);
            const uint8_t vsrc1 = static_cast<uint8_t>((w0 >> 9) & 0xFFu);
            const uint16_t src0 = static_cast<uint16_t>(w0 & 0x1FFu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVop2Opcode(raw_op);
            uint32_t lit = 0;
            uint32_t dpp8_raw = 0;
            bool has_sdwa_dpp16 = false;
            const bool mandatory_lit = Vop2OpcodeHasMandatoryLiteral(inst.opcode);
            if (src0 == 0xE9u || src0 == 0xEAu) {
                if (auto st = consume_word(dpp8_raw); !st.ok()) return st;
                inst.has_dpp8 = true;
                inst.dpp8_fi = (src0 == 0xEAu);
                inst.dpp8_word = (dpp8_raw >> 8) & 0xFFFFFFu;
                if (mandatory_lit) {
                    if (auto st = consume_word(lit); !st.ok()) return st;
                }
            } else if (src0 == 0xF9u || src0 == 0xFAu) {
                if (auto st = consume_word(dpp8_raw); !st.ok()) return st;
                has_sdwa_dpp16 = true;
                if (mandatory_lit) {
                    if (auto st = consume_word(lit); !st.ok()) return st;
                }
            } else if (src0 == 255u || mandatory_lit) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }
            const Rdna2Operand decoded_src0 = (inst.has_dpp8 || has_sdwa_dpp16)
                                                  ? Rdna2Operand::Vgpr(static_cast<uint8_t>(dpp8_raw & 0xFFu))
                                                  : DecodeRdna2SourceOperand(src0, lit);
            inst.dst = Rdna2Operand::Vgpr(vdst);
            if (inst.opcode == Rdna2Opcode::V_FMAMK_F16 || inst.opcode == Rdna2Opcode::V_MADMK_F32) {
                // D = S0 * K + VSRC1 (share mandatory literal K if src0 == 255)
                inst.src[0] = decoded_src0;
                inst.src[1] = Rdna2Operand::LiteralU32(lit);
                inst.src[2] = Rdna2Operand::Vgpr(vsrc1);
                inst.src_count = 3;
            } else if (inst.opcode == Rdna2Opcode::V_FMAAK_F16 || inst.opcode == Rdna2Opcode::V_MADAK_F32) {
                // D = S0 * VSRC1 + K (share mandatory literal K if src0 == 255)
                inst.src[0] = decoded_src0;
                inst.src[1] = Rdna2Operand::Vgpr(vsrc1);
                inst.src[2] = Rdna2Operand::LiteralU32(lit);
                inst.src_count = 3;
            } else {
                inst.src[0] = decoded_src0;
                inst.src[1] = Rdna2Operand::Vgpr(vsrc1);
                inst.src_count = 2;
                if (inst.opcode == Rdna2Opcode::V_CNDMASK_B32) {
                    inst.src[2] = Rdna2Operand::VccLo();
                    inst.src_count = 3;
                } else if (inst.opcode == Rdna2Opcode::V_DOT2_F32_F16) {
                    inst.src[2] = Rdna2Operand::Vgpr(vdst);
                    inst.src_count = 3;
                }
            }
        } else if ((w0 >> 26) == 0x32u) {
            // VINTRP (32-bit): [31:26] == 110010 (FSSM pixel interpolation)
            inst.encoding = Rdna2Encoding::VINTRP;
            const uint8_t vdst = static_cast<uint8_t>((w0 >> 18) & 0xFFu);
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16) & 0x3u);
            inst.vintrp_attr = static_cast<uint8_t>((w0 >> 10) & 0x3Fu);
            inst.vintrp_chan = static_cast<uint8_t>((w0 >> 8) & 0x3u);
            const uint8_t vsrc = static_cast<uint8_t>(w0 & 0xFFu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVintrpOpcode(raw_op);
            inst.dst = Rdna2Operand::Vgpr(vdst);
            inst.src[0] = Rdna2Operand::Vgpr(vsrc);
            inst.src_count = 1;
        } else if ((w0 >> 26) == 0x33u) {
            // VOP3P (64-bit, or 96-bit with literal32 in GFX10.3): [31:26] == 110011
            inst.encoding = Rdna2Encoding::VOP3P;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            inst.clamp = ((w0 >> 15) & 1u) != 0u;
            const uint8_t op_sel_hi_2 = static_cast<uint8_t>((w0 >> 14) & 1u);
            inst.vop3p_op_sel = static_cast<uint8_t>((w0 >> 11) & 0x7u);
            inst.vop3p_neg_hi = static_cast<uint8_t>((w0 >> 8) & 0x7u);
            const uint8_t vdst = static_cast<uint8_t>(w0 & 0xFFu);

            inst.vop3p_neg_lo = static_cast<uint8_t>((w1 >> 29) & 0x7u);
            const uint8_t op_sel_hi_10 = static_cast<uint8_t>((w1 >> 27) & 0x3u);
            inst.vop3p_op_sel_hi = static_cast<uint8_t>(op_sel_hi_10 | (op_sel_hi_2 << 2));
            const uint16_t src2 = static_cast<uint16_t>((w1 >> 18) & 0x1FFu);
            const uint16_t src1 = static_cast<uint16_t>((w1 >> 9) & 0x1FFu);
            const uint16_t src0 = static_cast<uint16_t>(w1 & 0x1FFu);

            uint32_t lit = 0;
            if (src0 == 255u || src1 == 255u || src2 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }

            uint8_t sc = 2;
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVop3pOpcode(raw_op, sc);
            inst.src_count = sc;
            inst.dst = Rdna2Operand::Vgpr(vdst);
            const uint16_t raw_srcs[3] = {src0, src1, src2};
            for (uint8_t i = 0; i < sc; ++i) {
                inst.src[i] = DecodeRdna2SourceOperand(raw_srcs[i], lit);
            }
        } else if (IsVop3EncodingWord(w0)) {
            // VOP3 (64-bit, or 96-bit with literal32 in GFX10.3): [31:26] == 110101 (top byte 0xD4..0xD7)
            inst.encoding = Rdna2Encoding::VOP3;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 16) & 0x3FFu);
            inst.clamp = ((w0 >> 15) & 1u) != 0u;
            inst.vop3_op_sel = static_cast<uint8_t>((w0 >> 11) & 0xFu);
            const uint8_t abs_bits = static_cast<uint8_t>((w0 >> 8) & 0x7u);
            const uint8_t vdst = static_cast<uint8_t>(w0 & 0xFFu);

            const uint8_t neg_bits = static_cast<uint8_t>((w1 >> 29) & 0x7u);
            inst.omod = static_cast<uint8_t>((w1 >> 27) & 0x3u);
            const uint16_t src2 = static_cast<uint16_t>((w1 >> 18) & 0x1FFu);
            const uint16_t src1 = static_cast<uint16_t>((w1 >> 9) & 0x1FFu);
            const uint16_t src0 = static_cast<uint16_t>(w1 & 0x1FFu);

            uint32_t lit = 0;
            if (src0 == 255u || src1 == 255u || src2 == 255u) {
                if (auto st = consume_word(lit); !st.ok()) return st;
            }

            uint8_t sc = 3;
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeVop3Opcode(raw_op, sc);
            inst.src_count = sc;
            if (IsVcmpxOpcode(inst.opcode)) {
                // GFX10 V_CMPX writes EXEC directly (VOP3 sdst is ignored / null)
                inst.dst = Rdna2Operand::ExecLo();
            } else if (raw_op < 0x100u ||
                inst.opcode == Rdna2Opcode::V_READLANE_B32 ||
                inst.opcode == Rdna2Opcode::V_READFIRSTLANE_B32) {
                if (vdst == 106u) {
                    inst.dst = Rdna2Operand::VccLo();
                } else {
                    inst.dst = Rdna2Operand::Sgpr(static_cast<uint8_t>(vdst & 0x7Fu));
                }
            } else {
                inst.dst = Rdna2Operand::Vgpr(vdst);
            }
            const bool is_vop3b =
                (inst.opcode == Rdna2Opcode::V_MAD_U64_U32 ||
                 inst.opcode == Rdna2Opcode::V_MAD_I64_I32 ||
                 raw_op == 0x128u || raw_op == 0x129u || raw_op == 0x12Au ||
                 raw_op == 0x30Du || raw_op == 0x30Eu || raw_op == 0x30Fu);
            if (is_vop3b) {
                // VOP3B format: bits [14:8] encode SDST carry-out (not abs/op_sel)
                const uint8_t vop3b_sdst = static_cast<uint8_t>((w0 >> 8) & 0x7Fu);
                inst.sdst = decode_scalar_dst(vop3b_sdst);
                inst.vop3_op_sel = 0u;
                const uint16_t raw_srcs[3] = {src0, src1, src2};
                for (uint8_t i = 0; i < sc; ++i) {
                    inst.src[i] = DecodeRdna2SourceOperand(raw_srcs[i], lit);
                    inst.src[i].neg = ((neg_bits >> i) & 1u) != 0u;
                }
            } else {
                const uint16_t raw_srcs[3] = {src0, src1, src2};
                for (uint8_t i = 0; i < sc; ++i) {
                    inst.src[i] = DecodeRdna2SourceOperand(raw_srcs[i], lit);
                    inst.src[i].abs = ((abs_bits >> i) & 1u) != 0u;
                    inst.src[i].neg = ((neg_bits >> i) & 1u) != 0u;
                }
            }
        } else if ((w0 >> 26) == 0x36u) {
            // DS (64-bit): [31:26] == 110110
            inst.encoding = Rdna2Encoding::DS;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0xFFu);
            inst.ds_gds = ((w0 >> 17) & 1u) != 0u;
            inst.ds_offset1 = static_cast<uint8_t>((w0 >> 8) & 0xFFu);
            inst.ds_offset0 = static_cast<uint8_t>(w0 & 0xFFu);
            inst.mem_offset = static_cast<int32_t>((static_cast<uint32_t>(inst.ds_offset1) << 8) | inst.ds_offset0);
            const uint8_t vdst = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
            const uint8_t data1 = static_cast<uint8_t>((w1 >> 16) & 0xFFu);
            const uint8_t data0 = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            const uint8_t addr = static_cast<uint8_t>(w1 & 0xFFu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeDsOpcode(raw_op);
            switch (inst.opcode) {
                case Rdna2Opcode::DS_NOP:
                    inst.src_count = 0;
                    break;
                case Rdna2Opcode::DS_READ_B32:
                case Rdna2Opcode::DS_READ_B96:
                case Rdna2Opcode::DS_READ_B128:
                case Rdna2Opcode::DS_READ_U8_D16:
                case Rdna2Opcode::DS_READ_U8_D16_HI:
                case Rdna2Opcode::DS_READ_I8_D16:
                case Rdna2Opcode::DS_READ_I8_D16_HI:
                case Rdna2Opcode::DS_READ_U16_D16:
                case Rdna2Opcode::DS_READ_U16_D16_HI:
                case Rdna2Opcode::DS_SWIZZLE_B32:
                    inst.dst = Rdna2Operand::Vgpr(vdst);
                    inst.src[0] = Rdna2Operand::Vgpr(addr);
                    inst.src_count = 1;
                    break;
                case Rdna2Opcode::DS_WRITE_B32:
                case Rdna2Opcode::DS_WRITE_B96:
                case Rdna2Opcode::DS_WRITE_B128:
                case Rdna2Opcode::DS_ADD_U32:
                    inst.src[0] = Rdna2Operand::Vgpr(addr);
                    inst.src[1] = Rdna2Operand::Vgpr(data0);
                    inst.src_count = 2;
                    break;
                case Rdna2Opcode::DS_MSKOR_B32:
                    inst.src[0] = Rdna2Operand::Vgpr(addr);
                    inst.src[1] = Rdna2Operand::Vgpr(data0);
                    inst.src[2] = Rdna2Operand::Vgpr(data1);
                    inst.src_count = 3;
                    break;
                case Rdna2Opcode::DS_ADD_RTN_U32:
                    inst.dst = Rdna2Operand::Vgpr(vdst);
                    inst.src[0] = Rdna2Operand::Vgpr(addr);
                    inst.src[1] = Rdna2Operand::Vgpr(data0);
                    inst.src_count = 2;
                    break;
                case Rdna2Opcode::DS_MSKOR_RTN_B32:
                case Rdna2Opcode::DS_WRXCHG2_RTN_B32:
                case Rdna2Opcode::DS_WRXCHG2ST64_RTN_B32:
                    inst.dst = Rdna2Operand::Vgpr(vdst);
                    inst.src[0] = Rdna2Operand::Vgpr(addr);
                    inst.src[1] = Rdna2Operand::Vgpr(data0);
                    inst.src[2] = Rdna2Operand::Vgpr(data1);
                    inst.src_count = 3;
                    break;
                default:
                    (void)data1;
                    break;
            }
        } else if ((w0 >> 26) == 0x3Cu) {
            // MIMG (64-bit): [31:26] == 111100
            inst.encoding = Rdna2Encoding::MIMG;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            inst.slc = ((w0 >> 25) & 1u) != 0u;
            const uint16_t raw_op = static_cast<uint16_t>(((w0 >> 18) & 0x7Fu) | ((w0 & 1u) << 7));
            inst.glc = ((w0 >> 13) & 1u) != 0u;
            inst.mimg_unrm = ((w0 >> 12) & 1u) != 0u;
            inst.mimg_dmask = static_cast<uint8_t>((w0 >> 8) & 0xFu);
            inst.dlc = ((w0 >> 7) & 1u) != 0u;
            inst.mimg_dim = static_cast<uint8_t>((w0 >> 3) & 0x7u);
            inst.mimg_nsa_dwords = static_cast<uint8_t>((w0 >> 1) & 0x3u);
            for (uint8_t nd = 0u; nd < inst.mimg_nsa_dwords; ++nd) {
                uint32_t nw = 0u;
                if (auto st = consume_word(nw); !st.ok()) return st;
                inst.mimg_nsa_vaddr[nd * 4u + 0u] = static_cast<uint8_t>(nw & 0xFFu);
                inst.mimg_nsa_vaddr[nd * 4u + 1u] = static_cast<uint8_t>((nw >> 8) & 0xFFu);
                inst.mimg_nsa_vaddr[nd * 4u + 2u] = static_cast<uint8_t>((nw >> 16) & 0xFFu);
                inst.mimg_nsa_vaddr[nd * 4u + 3u] = static_cast<uint8_t>((nw >> 24) & 0xFFu);
            }
            inst.ssamp = static_cast<uint8_t>((w1 >> 21) & 0x1Fu);
            inst.srsrc = static_cast<uint8_t>((w1 >> 16) & 0x1Fu);
            const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeMimgOpcode(raw_op);
            if (inst.opcode == Rdna2Opcode::IMAGE_STORE) {
                inst.src[0] = Rdna2Operand::Vgpr(vdata);
                inst.src[1] = Rdna2Operand::Vgpr(vaddr);
                inst.src_count = 2;
            } else if (inst.opcode == Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP) {
                inst.dst = Rdna2Operand::Vgpr(vdata);
                inst.src[0] = Rdna2Operand::Vgpr(vdata);
                inst.src[1] = Rdna2Operand::Vgpr(vaddr);
                inst.src_count = 2;
            } else {
                inst.dst = Rdna2Operand::Vgpr(vdata);
                inst.src[0] = Rdna2Operand::Vgpr(vaddr);
                inst.src_count = 1;
            }
        } else if ((w0 >> 26) == 0x3Du) {
            // SMEM (64-bit): [31:26] == 111101
            inst.encoding = Rdna2Encoding::SMEM;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0xFFu);
            inst.dlc = ((w0 >> 17) & 1u) != 0u;
            inst.glc = ((w0 >> 16) & 1u) != 0u;
            const uint8_t sdata = static_cast<uint8_t>((w0 >> 6) & 0x7Fu);
            inst.sbase = static_cast<uint8_t>(w0 & 0x3Fu);
            inst.soffset = static_cast<uint8_t>((w1 >> 25) & 0x7Fu);
            inst.mem_offset = SignExtend(w1 & 0x1FFFFFu, 21);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeSmemOpcode(raw_op);
            inst.dst = Rdna2Operand::Sgpr(sdata);
            inst.src[0] = Rdna2Operand::Sgpr(static_cast<uint8_t>(inst.sbase * 2u));
            inst.src_count = 1;
        } else if ((w0 >> 26) == 0x38u) {
            // MUBUF (64-bit): [31:26] == 111000
            inst.encoding = Rdna2Encoding::MUBUF;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
            inst.slc = ((w0 >> 16) & 1u) != 0u;
            inst.dlc = ((w0 >> 15) & 1u) != 0u;
            inst.glc = ((w0 >> 14) & 1u) != 0u;
            inst.idxen = ((w0 >> 13) & 1u) != 0u;
            inst.offen = ((w0 >> 12) & 1u) != 0u;
            inst.mem_offset = static_cast<int32_t>(w0 & 0xFFFu);
            inst.soffset = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
            inst.srsrc = static_cast<uint8_t>((w1 >> 16) & 0x3Fu);
            const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeMubufOpcode(raw_op);
            const bool is_mubuf_atomic = (raw_op >= 0x30u && raw_op <= 0x4Du);
            if (is_mubuf_atomic) {
                if (inst.glc) {
                    inst.dst = Rdna2Operand::Vgpr(vdata);
                }
            } else if (inst.opcode != Rdna2Opcode::BUFFER_STORE_DWORD &&
                       inst.opcode != Rdna2Opcode::BUFFER_STORE_BYTE_D16_HI &&
                       inst.opcode != Rdna2Opcode::BUFFER_STORE_SHORT_D16_HI &&
                       inst.opcode != Rdna2Opcode::Unknown) {
                inst.dst = Rdna2Operand::Vgpr(vdata);
            }
            inst.src[0] = Rdna2Operand::Vgpr(vdata);
            inst.src[1] = Rdna2Operand::Vgpr(vaddr);
            inst.src_count = 2;
        } else if ((w0 >> 26) == 0x3Au) {
            // MTBUF (64-bit): [31:26] == 111010 (GFX10 unified 7-bit format)
            inst.encoding = Rdna2Encoding::MTBUF;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            inst.mtbuf_format = static_cast<uint8_t>((w0 >> 19) & 0x7Fu);
            const uint16_t raw_op = static_cast<uint16_t>(((w0 >> 16) & 0x7u) | (((w1 >> 23) & 1u) << 3));
            inst.dlc = ((w0 >> 15) & 1u) != 0u;
            inst.glc = ((w0 >> 14) & 1u) != 0u;
            inst.idxen = ((w0 >> 13) & 1u) != 0u;
            inst.offen = ((w0 >> 12) & 1u) != 0u;
            inst.mem_offset = static_cast<int32_t>(w0 & 0xFFFu);
            inst.soffset = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
            inst.slc = ((w1 >> 22) & 1u) != 0u;
            inst.srsrc = static_cast<uint8_t>((w1 >> 16) & 0x3Fu);
            const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeMtbufOpcode(raw_op);
            if (inst.opcode != Rdna2Opcode::Unknown) {
                inst.dst = Rdna2Operand::Vgpr(vdata);
            }
            inst.src[0] = Rdna2Operand::Vgpr(vdata);
            inst.src[1] = Rdna2Operand::Vgpr(vaddr);
            inst.src_count = 2;
        } else if ((w0 >> 26) == 0x37u) {
            // FLAT / GLOBAL (64-bit): [31:26] == 110111
            inst.encoding = Rdna2Encoding::FLAT;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            const uint16_t raw_op = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
            inst.slc = ((w0 >> 17) & 1u) != 0u;
            inst.glc = ((w0 >> 16) & 1u) != 0u;
            inst.flat_seg = static_cast<uint8_t>((w0 >> 14) & 0x3u);
            inst.dlc = ((w0 >> 12) & 1u) != 0u;
            inst.mem_offset = (inst.flat_seg == 0u)
                                  ? static_cast<int32_t>(w0 & 0xFFFu)
                                  : SignExtend(w0 & 0xFFFu, 12);
            const uint8_t vdst = static_cast<uint8_t>((w1 >> 24) & 0xFFu);
            inst.saddr = static_cast<uint8_t>((w1 >> 16) & 0x7Fu);
            const uint8_t vdata = static_cast<uint8_t>((w1 >> 8) & 0xFFu);
            const uint8_t vaddr = static_cast<uint8_t>(w1 & 0xFFu);
            inst.raw_opcode = raw_op;
            inst.opcode = DecodeFlatOpcode(raw_op, inst.flat_seg);
            if (inst.opcode == Rdna2Opcode::GLOBAL_LOAD_DWORD ||
                inst.opcode == Rdna2Opcode::FLAT_LOAD_DWORD ||
                inst.opcode == Rdna2Opcode::SCRATCH_LOAD_DWORD) {
                inst.dst = Rdna2Operand::Vgpr(vdst);
                inst.src[0] = Rdna2Operand::Vgpr(vaddr);
                inst.src_count = 1;
            } else {
                inst.src[0] = Rdna2Operand::Vgpr(vaddr);
                inst.src[1] = Rdna2Operand::Vgpr(vdata);
                inst.src_count = 2;
            }
        } else if ((w0 >> 26) == 0x3Eu) {
            // EXP (64-bit): [31:26] == 111110
            inst.encoding = Rdna2Encoding::EXP;
            uint32_t w1 = 0;
            if (auto st = consume_word(w1); !st.ok()) return st;
            inst.opcode = Rdna2Opcode::EXP;
            inst.exp_vm = ((w0 >> 12) & 1u) != 0u;
            inst.exp_done = ((w0 >> 11) & 1u) != 0u;
            inst.exp_compr = ((w0 >> 10) & 1u) != 0u;
            inst.exp_target = static_cast<uint8_t>((w0 >> 4) & 0x3Fu);
            inst.exp_en = static_cast<uint8_t>(w0 & 0xFu);
            inst.src[0] = Rdna2Operand::Vgpr(static_cast<uint8_t>(w1 & 0xFFu));
            inst.src[1] = Rdna2Operand::Vgpr(static_cast<uint8_t>((w1 >> 8) & 0xFFu));
            inst.src[2] = Rdna2Operand::Vgpr(static_cast<uint8_t>((w1 >> 16) & 0xFFu));
            inst.src[3] = Rdna2Operand::Vgpr(static_cast<uint8_t>((w1 >> 24) & 0xFFu));
            inst.src_count = 4;
        } else {
            inst.encoding = Rdna2Encoding::Unknown;
            inst.opcode = Rdna2Opcode::Unknown;
            inst.word_count = 1;
        }

        if (inst.opcode == Rdna2Opcode::Unknown) {
            inst.mnemonic = "unknown";
            out_instructions.push_back(std::move(inst));
            pc += inst.word_count;
            continue;
        }

        PopulateInstructionMetadata(inst);
        inst.mnemonic = Rdna2OpcodeName(inst.opcode);
        inst.disasm = FormatRdna2Instruction(inst);
        out_instructions.push_back(std::move(inst));
        pc += inst.word_count;
    }

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// RDNA2 / GFX10.3 Binary Assembler
// ---------------------------------------------------------------------------
void Rdna2Assembler::EmitSop2(
    Rdna2Opcode op,
    uint8_t sdst,
    const Rdna2Operand& ssrc0,
    const Rdna2Operand& ssrc1) {
    const uint16_t raw_op = EncodeSop2Opcode(op);
    uint16_t c0 = 0, c1 = 0;
    uint32_t lit0 = 0, lit1 = 0;
    bool need0 = false, need1 = false;
    EncodeRdna2SourceOperand(ssrc0, c0, lit0, need0);
    EncodeRdna2SourceOperand(ssrc1, c1, lit1, need1);
    const uint32_t w0 = (0x2u << 30) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 23) |
                        (static_cast<uint32_t>(sdst & 0x7Fu) << 16) |
                        (static_cast<uint32_t>(c1 & 0xFFu) << 8) |
                        static_cast<uint32_t>(c0 & 0xFFu);
    words_.push_back(w0);
    if (need0) {
        words_.push_back(lit0);
    } else if (need1) {
        words_.push_back(lit1);
    }
}

void Rdna2Assembler::EmitSop1(Rdna2Opcode op, uint8_t sdst, const Rdna2Operand& ssrc0) {
    const uint16_t raw_op = EncodeSop1Opcode(op);
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(ssrc0, c0, lit0, need0);
    const uint32_t w0 = (0x17Du << 23) | (static_cast<uint32_t>(sdst & 0x7Fu) << 16) |
                        (static_cast<uint32_t>(raw_op & 0xFFu) << 8) |
                        static_cast<uint32_t>(c0 & 0xFFu);
    words_.push_back(w0);
    if (need0) words_.push_back(lit0);
}

void Rdna2Assembler::EmitSopc(Rdna2Opcode op, const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1) {
    const uint16_t raw_op = EncodeSopcOpcode(op);
    uint16_t c0 = 0, c1 = 0;
    uint32_t lit0 = 0, lit1 = 0;
    bool need0 = false, need1 = false;
    EncodeRdna2SourceOperand(ssrc0, c0, lit0, need0);
    EncodeRdna2SourceOperand(ssrc1, c1, lit1, need1);
    const uint32_t w0 = (0x17Eu << 23) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 16) |
                        (static_cast<uint32_t>(c1 & 0xFFu) << 8) |
                        static_cast<uint32_t>(c0 & 0xFFu);
    words_.push_back(w0);
    if (need0) {
        words_.push_back(lit0);
    } else if (need1) {
        words_.push_back(lit1);
    }
}

void Rdna2Assembler::EmitSopk(Rdna2Opcode op, uint8_t sdst, int16_t simm16) {
    const uint16_t raw_op = EncodeSopkOpcode(op);
    const uint32_t w0 = (0xBu << 28) | (static_cast<uint32_t>(raw_op & 0x1Fu) << 23) |
                        (static_cast<uint32_t>(sdst & 0x7Fu) << 16) |
                        static_cast<uint32_t>(static_cast<uint16_t>(simm16));
    words_.push_back(w0);
}

uint32_t Rdna2Assembler::EmitSopp(Rdna2Opcode op, int16_t simm16) {
    const uint32_t pos = CurrentWordOffset();
    const uint16_t raw_op = EncodeSoppOpcode(op);
    const uint32_t w0 = (0x17Fu << 23) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 16) |
                        static_cast<uint32_t>(static_cast<uint16_t>(simm16));
    words_.push_back(w0);
    return pos;
}

void Rdna2Assembler::PatchBranch(uint32_t branch_word_offset, uint32_t target_word_offset) {
    if (branch_word_offset >= words_.size()) return;
    const int32_t rel = static_cast<int32_t>(target_word_offset) - static_cast<int32_t>(branch_word_offset + 1u);
    const uint16_t imm = static_cast<uint16_t>(static_cast<int16_t>(rel));
    words_[branch_word_offset] = (words_[branch_word_offset] & 0xFFFF0000u) | static_cast<uint32_t>(imm);
}

void Rdna2Assembler::EmitVop2(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0, uint8_t vsrc1) {
    const uint16_t raw_op = EncodeVop2Opcode(op);
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    const uint32_t w0 = (static_cast<uint32_t>(raw_op & 0x3Fu) << 25) |
                        (static_cast<uint32_t>(vdst) << 17) |
                        (static_cast<uint32_t>(vsrc1) << 9) |
                        static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    if (need0) words_.push_back(lit0);
}

void Rdna2Assembler::EmitVop2Literal(
    Rdna2Opcode op,
    uint8_t vdst,
    const Rdna2Operand& src0,
    uint8_t vsrc1,
    uint32_t literal_k) {
    const uint16_t raw_op = EncodeVop2Opcode(op);
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    const uint32_t w0 = (static_cast<uint32_t>(raw_op & 0x3Fu) << 25) |
                        (static_cast<uint32_t>(vdst) << 17) |
                        (static_cast<uint32_t>(vsrc1) << 9) |
                        static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    words_.push_back(literal_k);
}

void Rdna2Assembler::EmitVop1(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0) {
    const uint16_t raw_op = EncodeVop1Opcode(op);
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    const uint32_t w0 = (0x3Fu << 25) | (static_cast<uint32_t>(vdst) << 17) |
                        (static_cast<uint32_t>(raw_op & 0xFFu) << 9) |
                        static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    if (need0) words_.push_back(lit0);
}

void Rdna2Assembler::EmitVop1Dpp8(Rdna2Opcode op, uint8_t vdst, uint8_t vsrc0, uint32_t dpp8_sel24, bool fi) {
    const uint16_t raw_op = EncodeVop1Opcode(op);
    const uint32_t dpp_code = fi ? 0xEAu : 0xE9u;
    const uint32_t w0 = (0x3Fu << 25) | (static_cast<uint32_t>(vdst) << 17) |
                        (static_cast<uint32_t>(raw_op & 0xFFu) << 9) | dpp_code;
    const uint32_t w1 = ((dpp8_sel24 & 0xFFFFFFu) << 8) | static_cast<uint32_t>(vsrc0);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitVop2Dpp8(Rdna2Opcode op, uint8_t vdst, uint8_t vsrc0, uint8_t vsrc1, uint32_t dpp8_sel24, bool fi) {
    const uint16_t raw_op = EncodeVop2Opcode(op);
    const uint32_t dpp_code = fi ? 0xEAu : 0xE9u;
    const uint32_t w0 = (static_cast<uint32_t>(raw_op & 0x3Fu) << 25) |
                        (static_cast<uint32_t>(vdst) << 17) |
                        (static_cast<uint32_t>(vsrc1) << 9) | dpp_code;
    const uint32_t w1 = ((dpp8_sel24 & 0xFFFFFFu) << 8) | static_cast<uint32_t>(vsrc0);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitVop3b(
    Rdna2Opcode op,
    uint8_t vdst,
    uint8_t sdst,
    const Rdna2Operand& src0,
    const Rdna2Operand& src1,
    const Rdna2Operand& src2) {
    const uint16_t raw_op = EncodeVop3Opcode(op);
    uint16_t c0 = 0, c1 = 0, c2 = 0;
    uint32_t lit0 = 0, lit1 = 0, lit2 = 0;
    bool need0 = false, need1 = false, need2 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    EncodeRdna2SourceOperand(src1, c1, lit1, need1);
    EncodeRdna2SourceOperand(src2, c2, lit2, need2);
    const uint32_t neg_bits = (src0.neg ? 1u : 0u) | (src1.neg ? 2u : 0u) | (src2.neg ? 4u : 0u);
    const uint32_t w0 = (0x35u << 26) | (static_cast<uint32_t>(raw_op & 0x3FFu) << 16) |
                        (static_cast<uint32_t>(sdst & 0x7Fu) << 8) | static_cast<uint32_t>(vdst);
    const uint32_t w1 = (neg_bits << 29) | (static_cast<uint32_t>(c2 & 0x1FFu) << 18) |
                        (static_cast<uint32_t>(c1 & 0x1FFu) << 9) | static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    words_.push_back(w1);
    if (need0) words_.push_back(lit0);
    else if (need1) words_.push_back(lit1);
    else if (need2) words_.push_back(lit2);
}

void Rdna2Assembler::EmitVopc(Rdna2Opcode op, const Rdna2Operand& src0, uint8_t vsrc1) {
    const uint16_t raw_op = EncodeVopcOpcode(op);
    uint16_t c0 = 0;
    uint32_t lit0 = 0;
    bool need0 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    const uint32_t w0 = (0x3Eu << 25) | (static_cast<uint32_t>(raw_op & 0xFFu) << 17) |
                        (static_cast<uint32_t>(vsrc1) << 9) |
                        static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    if (need0) words_.push_back(lit0);
}

void Rdna2Assembler::EmitVop3(
    Rdna2Opcode op,
    uint8_t vdst,
    const Rdna2Operand& src0,
    const Rdna2Operand& src1,
    const Rdna2Operand& src2,
    bool clamp,
    uint8_t op_sel) {
    const uint16_t raw_op = EncodeVop3Opcode(op);
    uint16_t c0 = 0, c1 = 0, c2 = 0;
    uint32_t lit0 = 0, lit1 = 0, lit2 = 0;
    bool need0 = false, need1 = false, need2 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    EncodeRdna2SourceOperand(src1, c1, lit1, need1);
    EncodeRdna2SourceOperand(src2, c2, lit2, need2);

    const uint32_t abs_bits = (src0.abs ? 1u : 0u) | (src1.abs ? 2u : 0u) | (src2.abs ? 4u : 0u);
    const uint32_t neg_bits = (src0.neg ? 1u : 0u) | (src1.neg ? 2u : 0u) | (src2.neg ? 4u : 0u);

    const uint32_t w0 = (0x35u << 26) | (static_cast<uint32_t>(raw_op & 0x3FFu) << 16) |
                        (clamp ? (1u << 15) : 0u) | (static_cast<uint32_t>(op_sel & 0xFu) << 11) |
                        (abs_bits << 8) | static_cast<uint32_t>(vdst);
    const uint32_t w1 = (neg_bits << 29) | (static_cast<uint32_t>(c2 & 0x1FFu) << 18) |
                        (static_cast<uint32_t>(c1 & 0x1FFu) << 9) | static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    words_.push_back(w1);
    if (need0) {
        words_.push_back(lit0);
    } else if (need1) {
        words_.push_back(lit1);
    } else if (need2) {
        words_.push_back(lit2);
    }
}

void Rdna2Assembler::EmitVop3p(
    Rdna2Opcode op,
    uint8_t vdst,
    const Rdna2Operand& src0,
    const Rdna2Operand& src1,
    const Rdna2Operand& src2,
    uint8_t op_sel,
    uint8_t op_sel_hi,
    uint8_t neg_lo,
    uint8_t neg_hi,
    bool clamp) {
    const uint16_t raw_op = EncodeVop3pOpcode(op);
    uint16_t c0 = 0, c1 = 0, c2 = 0;
    uint32_t lit0 = 0, lit1 = 0, lit2 = 0;
    bool need0 = false, need1 = false, need2 = false;
    EncodeRdna2SourceOperand(src0, c0, lit0, need0);
    EncodeRdna2SourceOperand(src1, c1, lit1, need1);
    EncodeRdna2SourceOperand(src2, c2, lit2, need2);

    const uint32_t op_sel_hi_2 = (static_cast<uint32_t>(op_sel_hi) >> 2) & 1u;
    const uint32_t op_sel_hi_10 = static_cast<uint32_t>(op_sel_hi) & 0x3u;
    const uint32_t w0 = (0x33u << 26) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 16) |
                        (clamp ? (1u << 15) : 0u) | (op_sel_hi_2 << 14) |
                        (static_cast<uint32_t>(op_sel & 0x7u) << 11) |
                        (static_cast<uint32_t>(neg_hi & 0x7u) << 8) |
                        static_cast<uint32_t>(vdst);
    const uint32_t w1 = (static_cast<uint32_t>(neg_lo & 0x7u) << 29) |
                        (op_sel_hi_10 << 27) |
                        (static_cast<uint32_t>(c2 & 0x1FFu) << 18) |
                        (static_cast<uint32_t>(c1 & 0x1FFu) << 9) |
                        static_cast<uint32_t>(c0 & 0x1FFu);
    words_.push_back(w0);
    words_.push_back(w1);
    if (need0) {
        words_.push_back(lit0);
    } else if (need1) {
        words_.push_back(lit1);
    } else if (need2) {
        words_.push_back(lit2);
    }
}

void Rdna2Assembler::EmitSmem(
    Rdna2Opcode op,
    uint8_t sdata,
    uint8_t sbase_sgpr,
    int32_t offset,
    uint8_t soffset) {
    const uint16_t raw_op = EncodeSmemOpcode(op);
    const uint32_t sbase = static_cast<uint32_t>(sbase_sgpr >> 1) & 0x3Fu;
    const uint32_t w0 = (0x3Du << 26) | (static_cast<uint32_t>(raw_op & 0xFFu) << 18) |
                        (static_cast<uint32_t>(sdata & 0x7Fu) << 6) | sbase;
    const uint32_t w1 = (static_cast<uint32_t>(soffset & 0x7Fu) << 25) |
                        (static_cast<uint32_t>(offset) & 0x1FFFFFu);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitMubuf(
    Rdna2Opcode op,
    uint8_t vdata,
    uint8_t vaddr,
    uint8_t srsrc_sgpr,
    uint16_t offset,
    bool offen,
    bool idxen,
    uint8_t soffset) {
    const uint16_t raw_op = EncodeMubufOpcode(op);
    const uint32_t srsrc = static_cast<uint32_t>(srsrc_sgpr >> 2) & 0x3Fu;
    const uint32_t w0 = (0x38u << 26) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 18) |
                        (idxen ? (1u << 13) : 0u) | (offen ? (1u << 12) : 0u) |
                        static_cast<uint32_t>(offset & 0xFFFu);
    const uint32_t w1 = (static_cast<uint32_t>(soffset) << 24) | (srsrc << 16) |
                        (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitMtbuf(
    Rdna2Opcode op,
    uint8_t vdata,
    uint8_t vaddr,
    uint8_t srsrc_sgpr,
    Rdna2MtbufFormat format,
    uint16_t offset,
    bool offen,
    bool idxen,
    uint8_t soffset) {
    const uint16_t raw_op = EncodeMtbufOpcode(op);
    const uint32_t srsrc = static_cast<uint32_t>(srsrc_sgpr >> 2) & 0x3Fu;
    const uint32_t w0 = (0x3Au << 26) | (static_cast<uint32_t>(static_cast<uint8_t>(format) & 0x7Fu) << 19) |
                        (static_cast<uint32_t>(raw_op & 0x7u) << 16) |
                        (idxen ? (1u << 13) : 0u) | (offen ? (1u << 12) : 0u) |
                        static_cast<uint32_t>(offset & 0xFFFu);
    const uint32_t w1 = (static_cast<uint32_t>(soffset) << 24) |
                        (static_cast<uint32_t>((raw_op >> 3) & 1u) << 23) |
                        (srsrc << 16) |
                        (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitVintrp(
    Rdna2Opcode op,
    uint8_t vdst,
    uint8_t vsrc,
    uint8_t attr,
    uint8_t chan) {
    const uint16_t raw_op = EncodeVintrpOpcode(op);
    const uint32_t w0 = (0x32u << 26) | (static_cast<uint32_t>(vdst) << 18) |
                        (static_cast<uint32_t>(raw_op & 0x3u) << 16) |
                        (static_cast<uint32_t>(attr & 0x3Fu) << 10) |
                        (static_cast<uint32_t>(chan & 0x3u) << 8) |
                        static_cast<uint32_t>(vsrc);
    words_.push_back(w0);
}

void Rdna2Assembler::EmitMimg(
    Rdna2Opcode op,
    uint8_t vdata,
    uint8_t vaddr,
    uint8_t srsrc_sgpr,
    uint8_t ssamp_sgpr,
    uint8_t dmask,
    uint8_t dim,
    bool unrm,
    bool glc) {
    const uint16_t raw_op = EncodeMimgOpcode(op);
    const uint32_t srsrc = static_cast<uint32_t>(srsrc_sgpr >> 2) & 0x1Fu;
    const uint32_t ssamp = static_cast<uint32_t>(ssamp_sgpr >> 2) & 0x1Fu;
    const uint32_t w0 = (0x3Cu << 26) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 18) |
                        (glc ? (1u << 13) : 0u) |
                        (unrm ? (1u << 12) : 0u) | (static_cast<uint32_t>(dmask & 0xFu) << 8) |
                        (static_cast<uint32_t>(dim & 0x7u) << 3) |
                        static_cast<uint32_t>((raw_op >> 7) & 1u);
    const uint32_t w1 = (ssamp << 21) | (srsrc << 16) |
                        (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitDs(
    Rdna2Opcode op,
    uint8_t vdst,
    uint8_t addr,
    uint8_t data0,
    uint8_t data1,
    uint16_t offset,
    bool gds) {
    const uint16_t raw_op = EncodeDsOpcode(op);
    const uint32_t w0 = (0x36u << 26) | (static_cast<uint32_t>(raw_op & 0xFFu) << 18) |
                        (gds ? (1u << 17) : 0u) | static_cast<uint32_t>(offset);
    const uint32_t w1 = (static_cast<uint32_t>(vdst) << 24) |
                        (static_cast<uint32_t>(data1) << 16) |
                        (static_cast<uint32_t>(data0) << 8) |
                        static_cast<uint32_t>(addr);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitGlobal(
    Rdna2Opcode op,
    uint8_t vdst_or_data,
    uint8_t vaddr,
    int16_t offset,
    uint8_t saddr,
    bool dlc,
    bool glc,
    bool slc) {
    uint8_t seg = 2;
    const uint16_t raw_op = EncodeFlatOpcode(op, seg);
    const bool is_load = (op == Rdna2Opcode::GLOBAL_LOAD_DWORD ||
                          op == Rdna2Opcode::FLAT_LOAD_DWORD ||
                          op == Rdna2Opcode::SCRATCH_LOAD_DWORD);
    // Pin FLAT/GLOBAL/SCRATCH immediate offset strictly to word0[11:0] so dlc(12)/glc(16)/slc(17) never bleed
    const uint32_t w0 = (0x37u << 26) | (static_cast<uint32_t>(raw_op & 0x7Fu) << 18) |
                        (slc ? (1u << 17) : 0u) | (glc ? (1u << 16) : 0u) |
                        (static_cast<uint32_t>(seg & 0x3u) << 14) |
                        (dlc ? (1u << 12) : 0u) |
                        (static_cast<uint32_t>(offset) & 0xFFFu);
    const uint32_t w1 = (is_load ? (static_cast<uint32_t>(vdst_or_data) << 24) : 0u) |
                        (static_cast<uint32_t>(saddr & 0x7Fu) << 16) |
                        (!is_load ? (static_cast<uint32_t>(vdst_or_data) << 8) : 0u) |
                        static_cast<uint32_t>(vaddr);
    words_.push_back(w0);
    words_.push_back(w1);
}

void Rdna2Assembler::EmitScratch(
    Rdna2Opcode op,
    uint8_t vdst_or_data,
    uint8_t vaddr,
    int16_t offset,
    uint8_t saddr,
    bool dlc,
    bool glc,
    bool slc) {
    EmitGlobal(op, vdst_or_data, vaddr, offset, saddr, dlc, glc, slc);
}

void Rdna2Assembler::EmitExp(
    uint8_t target,
    uint8_t vsrc0,
    uint8_t vsrc1,
    uint8_t vsrc2,
    uint8_t vsrc3,
    uint8_t en,
    bool done,
    bool compr,
    bool vm) {
    const uint32_t w0 = (0x3Eu << 26) | (vm ? (1u << 12) : 0u) | (done ? (1u << 11) : 0u) |
                        (compr ? (1u << 10) : 0u) | (static_cast<uint32_t>(target & 0x3Fu) << 4) |
                        static_cast<uint32_t>(en & 0xFu);
    const uint32_t w1 = static_cast<uint32_t>(vsrc0) | (static_cast<uint32_t>(vsrc1) << 8) |
                        (static_cast<uint32_t>(vsrc2) << 16) | (static_cast<uint32_t>(vsrc3) << 24);
    words_.push_back(w0);
    words_.push_back(w1);
}

std::vector<uint8_t> Rdna2Assembler::BuildXs2(
    ShaderStage stage,
    uint32_t tg_x,
    uint32_t tg_y,
    uint32_t tg_z,
    uint32_t sgpr_count,
    uint32_t vgpr_count,
    uint32_t wave_size) const {
    Xs2ShaderContainer c{};
    c.version = kXs2ContainerVersion;
    c.stage = stage;
    c.sgpr_count = sgpr_count;
    c.vgpr_count = vgpr_count;
    c.threadgroup_x = tg_x;
    c.threadgroup_y = tg_y;
    c.threadgroup_z = tg_z;
    c.entry_offset = kXs2HeaderSizeBytes;
    c.wave_size = (wave_size == 64u) ? 64u : 32u;
    c.code_words = words_;
    return BuildXs2Container(c);
}

}  // namespace openscarlett
