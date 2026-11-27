#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/shader_ir.h"

namespace openscarlett {

// ---------------------------------------------------------------------------
// Scarlett RDNA2 / GFX10.3 ('XS2\0') Shader Container
// ---------------------------------------------------------------------------
constexpr uint32_t kXs2ContainerVersion = 1u;
constexpr uint32_t kXs2ContainerVersionV2 = 2u;
constexpr uint32_t kXs2HeaderSizeBytes = 40u;
constexpr uint32_t kXs2HeaderSizeBytesV2 = 44u;
constexpr uint32_t kXs2SgprFlagWave64 = 0x80000000u;

// Public RDNA2 / GFX10.3 S_SENDMSG message IDs & ISA-exact GS_ALLOC_REQ field widths
constexpr uint16_t kRdna2SendMsgGsAllocReq = 9u;
constexpr uint16_t kRdna2SendMsgDispatchMesh = 10u;
constexpr uint32_t kGsAllocReqVertMask = 0x7FFu;   // M0[10:0]  (11 bits, 0..2047)
constexpr uint32_t kGsAllocReqPrimShift = 12u;     // M0[22:12] (11 bits, 0..2047)
constexpr uint32_t kGsAllocReqPrimMask = 0x7FFu;

inline constexpr uint32_t PackGsAllocReqM0(uint32_t vert_count, uint32_t prim_count) noexcept {
    return (vert_count & kGsAllocReqVertMask) | ((prim_count & kGsAllocReqPrimMask) << kGsAllocReqPrimShift);
}

// GFX10.3 / RDNA2 merged_wave_info packed in s3 for merged ES/GS & Mesh stages
struct Rdna2MergedWaveInfo {
    uint8_t es_thread_count = 0;    // [7:0]   vertex / ES thread count (0..255)
    uint8_t gs_thread_count = 0;    // [15:8]  primitive / GS thread count (0..255)
    uint8_t gs_wave_id = 0;         // [23:16] wave ID in subgroup / workgroup
    uint8_t wave_in_group_idx = 0;  // [27:24] wave index within workgroup (0..15)
    uint8_t waves_in_group = 1;     // [31:28] total waves in workgroup (0..15)
};

inline constexpr uint32_t PackMergedWaveInfo(const Rdna2MergedWaveInfo& info) noexcept {
    return static_cast<uint32_t>(info.es_thread_count) |
           (static_cast<uint32_t>(info.gs_thread_count) << 8) |
           (static_cast<uint32_t>(info.gs_wave_id) << 16) |
           (static_cast<uint32_t>(info.wave_in_group_idx & 0xFu) << 24) |
           (static_cast<uint32_t>(info.waves_in_group & 0xFu) << 28);
}

inline constexpr Rdna2MergedWaveInfo UnpackMergedWaveInfo(uint32_t s3) noexcept {
    Rdna2MergedWaveInfo info{};
    info.es_thread_count = static_cast<uint8_t>(s3 & 0xFFu);
    info.gs_thread_count = static_cast<uint8_t>((s3 >> 8) & 0xFFu);
    info.gs_wave_id = static_cast<uint8_t>((s3 >> 16) & 0xFFu);
    info.wave_in_group_idx = static_cast<uint8_t>((s3 >> 24) & 0xFu);
    info.waves_in_group = static_cast<uint8_t>((s3 >> 28) & 0xFu);
    return info;
}

struct Xs2ShaderContainer {
    uint32_t version = kXs2ContainerVersion;
    ShaderStage stage = ShaderStage::Compute;
    uint32_t sgpr_count = 16;
    uint32_t vgpr_count = 16;
    uint32_t threadgroup_x = 1;
    uint32_t threadgroup_y = 1;
    uint32_t threadgroup_z = 1;
    uint32_t entry_offset = kXs2HeaderSizeBytes;
    uint32_t wave_size = 32;  // 32 (native RDNA2 wave32) or 64 (wave64)
    std::vector<uint32_t> code_words;
};

Status ParseXs2Container(const std::vector<uint8_t>& bytes, Xs2ShaderContainer& out_container);
std::vector<uint8_t> BuildXs2Container(const Xs2ShaderContainer& container);

// ---------------------------------------------------------------------------
// Xbox-Wrapped DXBC / DXIL Shader Container Parser & Builder
// ---------------------------------------------------------------------------
enum class XboxShaderBlobFormat : uint8_t {
    Unknown = 0,
    RawXs2,            // Raw "XS2\0" RDNA2 shader blob
    XboxDxbc,          // "DXBC" container carrying Xbox RDNA2 chunks ("XBOX" / "XS2M")
    StandardHostDxbc,  // Standard host PC DXBC / DXIL container (no Xbox RDNA2 chunks)
};

struct XboxDxbcChunk {
    char fourcc[5] = {};  // Null-terminated 4-char chunk ID (e.g. "XBOX", "XS2M", "ISG1")
    std::vector<uint8_t> data;
};

struct XboxSignatureElement {
    std::string semantic_name;
    uint32_t semantic_index = 0;
    uint32_t system_value_type = 0;
    uint32_t component_type = 0;  // 1=uint, 2=sint, 3=float
    uint32_t register_index = 0;
    uint8_t mask = 0xFu;
    uint8_t rw_mask = 0xFu;
    uint32_t stream = 0;
};

struct XboxStaticSamplerInfo {
    uint32_t filter = 0;
    uint32_t address_u = 0;
    uint32_t address_v = 0;
    uint32_t address_w = 0;
    float mip_lod_bias = 0.0f;
    uint32_t max_anisotropy = 1;
    uint32_t comparison_func = 0;
    uint32_t border_color = 0;
    float min_lod = 0.0f;
    float max_lod = 3.402823466e+38f;
    uint32_t shader_register = 0;
    uint32_t register_space = 0;
    uint32_t shader_visibility = 0;
};

struct XboxPsv0ResourceBinding {
    uint32_t res_type = 0u;     // PSVResourceType: 1=CBV, 2=Sampler, 3=SRV_Typed, 4=SRV_Raw, 5=SRV_Struct, 6..9=UAV
    uint32_t space = 0u;
    uint32_t lower_bound = 0u;
    uint32_t upper_bound = 0u;
    uint32_t res_kind = 0u;     // PSVResourceKind: 2=Texture2D, 11=RawBuffer, 12=StructuredBuffer, 13=CBuffer
    uint32_t res_flags = 0u;
};

struct XboxPsv0Info {
    bool present = false;
    uint32_t runtime_info_size = 0;
    uint32_t min_wave_lane_count = 0;  // 0 = unspecified, 32, 64
    uint32_t max_wave_lane_count = 0;  // 0 or 0xFFFFFFFF = unspecified/unbounded, 32, 64
    uint8_t shader_stage = 0;          // DXIL PSV shader stage ID
    uint32_t num_threads_x = 0;
    uint32_t num_threads_y = 0;
    uint32_t num_threads_z = 0;
    std::vector<XboxPsv0ResourceBinding> resource_bindings;
};

struct XboxDxbcContainer {
    XboxShaderBlobFormat format = XboxShaderBlobFormat::Unknown;
    bool has_xs2 = false;
    Xs2ShaderContainer xs2{};
    bool has_dxil_or_shdr = false;
    std::string host_shader_chunk_fourcc;  // "DXIL", "SHEX", or "SHDR" if present
    std::vector<XboxDxbcChunk> chunks;
    std::vector<XboxSignatureElement> input_signature;   // Parsed from ISG1 / ISGN
    std::vector<XboxSignatureElement> output_signature;  // Parsed from OSG1 / OSGN
    std::vector<XboxStaticSamplerInfo> static_samplers;  // Parsed from RTS0 / STSM
    std::vector<uint8_t> raw_root_signature_chunk;       // Raw RTS0 chunk payload if present
    XboxPsv0Info psv0{};                                 // Parsed from PSV0 runtime info chunk
};

struct XboxDxbcBuildDesc {
    std::vector<uint8_t> xs2_blob;          // Embedded into "XBOX" or "XS2M" chunk
    std::string xbox_chunk_fourcc = "XBOX"; // "XBOX" or "XS2M"
    std::vector<XboxSignatureElement> input_signature;
    std::vector<XboxSignatureElement> output_signature;
    std::vector<XboxStaticSamplerInfo> static_samplers;
    bool emit_rts0_for_samplers = false;    // Emit RTS0 root signature chunk vs STSM chunk
    std::vector<uint8_t> aux_dxil_or_shdr;  // Optional "DXIL"/"SHEX"/"SHDR" chunk payload
    std::string aux_shader_fourcc = "DXIL";
    XboxPsv0Info psv0{};                    // Optional PSV0 chunk (wave lane bounds & NumThreads)
};

// Selects guest Wave32 vs Wave64 mode from parsed PSV0 wave lane bounds.
uint32_t SelectGuestWaveSizeFromPsv0(const XboxPsv0Info& psv0, uint32_t default_wave_size = 32u) noexcept;

// DXBC / DXIL container MD5 hash computation, verification, and in-place re-signing.
void ComputeDxbcContainerHash(const uint8_t* dxbc_bytes, size_t size, uint8_t out_hash[16]) noexcept;
bool VerifyDxbcContainerHash(const uint8_t* dxbc_bytes, size_t size) noexcept;
Status ResignDxilContainer(std::vector<uint8_t>& dxbc_bytes);

// Fast, zero-allocation check: returns true iff blob is raw "XS2\0" or "DXBC" with "XBOX"/"XS2M" chunk.
bool IsXboxShaderBytecode(const void* data, size_t size) noexcept;

// Parses raw "XS2\0" OR "DXBC" container carrying Xbox chunks ("XBOX", "XS2M", "SHDR"/"SHEX"/"DXIL",
// "ISG1"/"ISGN", "OSG1"/"OSGN", "RTS0"/"STSM").
Status ParseXboxShaderContainer(const void* data, size_t size, XboxDxbcContainer& out_container);
Status ParseXboxShaderContainer(const std::vector<uint8_t>& bytes, XboxDxbcContainer& out_container);

// Builder for Xbox-wrapped DXBC containers (for testing and tooling).
std::vector<uint8_t> BuildXboxDxbcContainer(const XboxDxbcBuildDesc& desc);

// ---------------------------------------------------------------------------
// AMD RDNA2 / GFX10.3 Binary Instruction Model (Public ISA Reference Guide)
// ---------------------------------------------------------------------------
// Public GFX10.3 unified 7-bit MTBUF buffer format IDs (dfmt + nfmt combined)
enum class Rdna2MtbufFormat : uint8_t {
    Invalid = 0,
    R8_UNORM = 1,
    R8_SNORM = 2,
    R8_UINT = 3,
    R8_SINT = 4,
    R16_UNORM = 5,
    R16_SNORM = 6,
    R16_UINT = 7,
    R16_SINT = 8,
    R16_FLOAT = 9,
    R8G8_UNORM = 10,
    R8G8_SNORM = 11,
    R8G8_UINT = 12,
    R8G8_SINT = 13,
    R32_UINT = 14,
    R32_SINT = 15,
    R32_FLOAT = 16,
    R16G16_UNORM = 17,
    R16G16_SNORM = 18,
    R16G16_UINT = 19,
    R16G16_SINT = 20,
    R16G16_FLOAT = 21,
    R8G8B8A8_UNORM = 28,
    R8G8B8A8_SNORM = 29,
    R8G8B8A8_UINT = 30,
    R8G8B8A8_SINT = 31,
    R32G32_FLOAT = 46,
    R16G16B16A16_UNORM = 47,
    R16G16B16A16_SNORM = 48,
    R16G16B16A16_UINT = 49,
    R16G16B16A16_SINT = 50,
    R16G16B16A16_FLOAT = 51,
    R32G32B32_FLOAT = 60,
    R32G32B32A32_FLOAT = 74,
};

// Single authoritative GFX10.3 typed-buffer (MTBUF/MUBUF) format byte-size & kind descriptor (Leonida aa2988c)
struct Rdna2MtbufFormatInfo {
    Rdna2MtbufFormat format = Rdna2MtbufFormat::Invalid;
    uint8_t component_bytes = 0;   // 1, 2, or 4 bytes per component (0 if invalid)
    uint8_t component_count = 0;   // 1, 2, 3, or 4 components
    uint8_t byte_stride = 0;       // total element byte stride
    bool is_integer = false;       // UINT / SINT
    bool is_signed = false;        // SNORM / SINT
    bool is_normalized = false;    // UNORM / SNORM
    bool is_float = false;         // FLOAT
    bool is_valid = false;
};

Rdna2MtbufFormatInfo GetRdna2MtbufFormatInfo(Rdna2MtbufFormat fmt) noexcept;
Rdna2MtbufFormatInfo GetRdna2MtbufFormatInfo(uint8_t raw_fmt) noexcept;

// Full RDNA2 / GFX10.3 ds_swizzle_b32 offset mode decoding & source-lane evaluation (xbox-emulator #184/#186)
enum class Rdna2DsSwizzleMode : uint8_t {
    BitMode = 0,      // offset[15]==1: ((lane & and_mask) | or_mask) ^ xor_mask
    QuadPermute = 1,  // offset[15]==0, offset[14:13]==0: 4x2-bit lane indices in 4-lane quad
    Rotate = 2,       // offset[15]==0, offset[14:13]==1: rotate-right within cluster size
    Fft = 3,          // offset[15]==0, offset[14:13]==2: butterfly pair swap
    Reserved = 4,     // offset[15]==0, offset[14:13]==3
};

struct Rdna2DsSwizzleInfo {
    Rdna2DsSwizzleMode mode = Rdna2DsSwizzleMode::BitMode;
    uint8_t and_mask = 0x1Fu;
    uint8_t or_mask = 0u;
    uint8_t xor_mask = 0u;
    uint8_t quad_sel[4] = {0u, 1u, 2u, 3u};
    uint8_t rotate_shift = 0u;
    uint8_t rotate_cluster_mask = 31u;  // cluster_size - 1
    uint8_t fft_mask = 0u;
};

Rdna2DsSwizzleInfo DecodeRdna2DsSwizzleOffset(uint16_t offset) noexcept;
uint32_t ComputeRdna2DsSwizzleSrcLane(uint32_t lane, uint16_t offset, uint32_t wave_size = 32u) noexcept;

enum class Rdna2Encoding : uint8_t {
    Unknown = 0,
    SOP2,
    SOP1,
    SOPC,
    SOPK,
    SOPP,
    VOP2,
    VOP1,
    VOPC,
    VOP3,
    VOP3P,
    SMEM,
    MUBUF,
    MTBUF,
    MIMG,
    DS,
    FLAT,
    EXP,
    VINTRP,
};

enum class Rdna2Opcode : uint16_t {
    Unknown = 0,

    // SOP2
    S_ADD_U32,
    S_SUB_U32,
    S_ADD_I32,
    S_SUB_I32,
    S_ADDC_U32,
    S_SUBB_U32,
    S_ABSDIFF_I32,
    S_MIN_I32,
    S_MIN_U32,
    S_MAX_I32,
    S_MAX_U32,
    S_CSELECT_B32,
    S_AND_B32,
    S_OR_B32,
    S_XOR_B32,
    S_ANDN2_B32,
    S_LSHL_B32,
    S_LSHR_B32,
    S_ASHR_I32,
    S_BFE_U32,
    S_BFE_I32,
    S_MUL_I32,

    // SOP1
    S_MOV_B32,
    S_NOT_B32,
    S_BREV_B32,
    S_BCNT1_I32_B32,
    S_BCNT1_I32_B64,
    S_FLBIT_I32_B32,
    S_FLBIT_I32_B64,
    S_FLBIT_I32,
    S_FLBIT_I32_I64,
    S_SETPC_B64,
    S_SWAPPC_B64,
    S_GETPC_B64,

    // SOPC
    S_CMP_EQ_I32,
    S_CMP_LG_I32,
    S_CMP_GT_I32,
    S_CMP_GE_I32,
    S_CMP_LT_I32,
    S_CMP_LE_I32,
    S_CMP_EQ_U32,
    S_CMP_LG_U32,
    S_CMP_GT_U32,
    S_CMP_GE_U32,
    S_CMP_LT_U32,
    S_CMP_LE_U32,
    S_CMP_EQ_U64,
    S_CMP_LG_U64,

    // SOPK
    S_MOVK_I32,
    S_VERSION,
    S_ADDK_I32,
    S_MULK_I32,
    S_WAITCNT_VSCNT,
    S_WAITCNT_VMCNT,
    S_WAITCNT_EXPCNT,
    S_WAITCNT_LGKMCNT,

    // SOPP
    S_NOP,
    S_ENDPGM,
    S_BRANCH,
    S_CBRANCH_SCC0,
    S_CBRANCH_SCC1,
    S_CBRANCH_VCCZ,
    S_CBRANCH_VCCNZ,
    S_CBRANCH_EXECZ,
    S_CBRANCH_EXECNZ,
    S_WAITCNT,
    S_BARRIER,
    S_SENDMSG,
    S_ICACHE_INV,
    S_TTRACEDATA,
    S_CODE_END,
    S_TTRACEDATA_IMM,

    // VOP2
    V_CNDMASK_B32,
    V_ADD_F32,
    V_SUB_F32,
    V_SUBREV_F32,
    V_MUL_F32,
    V_MIN_F32,
    V_MAX_F32,
    V_MIN_I32,
    V_MAX_I32,
    V_MIN_U32,
    V_MAX_U32,
    V_MAC_F32,
    V_MADMK_F32,
    V_MADAK_F32,
    V_FMAC_F32,
    V_FMAC_F16,
    V_FMAMK_F16,
    V_FMAAK_F16,
    V_AND_B32,
    V_OR_B32,
    V_XOR_B32,
    V_LSHLREV_B32,
    V_LSHRREV_B32,
    V_ASHRREV_I32,
    V_ADD_NC_U32,
    V_SUB_NC_U32,

    // VOP1
    V_NOP,
    V_MOV_B32,
    V_READFIRSTLANE_B32,
    V_CVT_F32_I32,
    V_CVT_I32_F32,
    V_CVT_F32_U32,
    V_CVT_U32_F32,
    V_RCP_F32,
    V_RSQ_F32,
    V_SQRT_F32,
    V_FLOOR_F32,
    V_CEIL_F32,
    V_FRACT_F32,
    V_TRUNC_F32,
    V_RNDNE_F32,
    V_EXP_F32,
    V_LOG_F32,
    V_SIN_F32,
    V_COS_F32,
    V_CVT_NORM_I16_F16,
    V_CVT_NORM_U16_F16,
    V_SWAP_B32,

    // VOPC (and GFX10 V_CMPX execution-mask compares + full ordered/unordered F32 & split-range F16 compares)
    V_CMP_F_F32,
    V_CMP_LT_F32,
    V_CMP_EQ_F32,
    V_CMP_LE_F32,
    V_CMP_GT_F32,
    V_CMP_LG_F32,
    V_CMP_GE_F32,
    V_CMP_O_F32,
    V_CMP_U_F32,
    V_CMP_NGE_F32,
    V_CMP_NLG_F32,
    V_CMP_NGT_F32,
    V_CMP_NLE_F32,
    V_CMP_NEQ_F32,
    V_CMP_NLT_F32,
    V_CMP_TRU_F32,
    V_CMPX_LT_F32,
    V_CMPX_EQ_F32,
    V_CMPX_LE_F32,
    V_CMPX_GT_F32,
    V_CMPX_LG_F32,
    V_CMPX_GE_F32,
    V_CMP_LT_I32,
    V_CMP_EQ_I32,
    V_CMP_GT_I32,
    V_CMP_GE_I32,
    V_CMPX_LT_I32,
    V_CMPX_EQ_I32,
    V_CMPX_GT_I32,
    V_CMPX_GE_I32,
    V_CMP_LT_U32,
    V_CMP_EQ_U32,
    V_CMP_GT_U32,
    V_CMP_GE_U32,
    V_CMPX_LT_U32,
    V_CMPX_EQ_U32,
    V_CMPX_GT_U32,
    V_CMPX_GE_U32,
    // GFX10 split F16 compare range: [0xC8..0xCF] (ordered) and [0xD8..0xDF] (unordered/negated)
    V_CMP_LT_F16,
    V_CMP_EQ_F16,
    V_CMP_LE_F16,
    V_CMP_GT_F16,
    V_CMP_LG_F16,
    V_CMP_GE_F16,
    V_CMP_U_F16,
    V_CMP_NGE_F16,
    V_CMP_NLG_F16,
    V_CMP_NGT_F16,
    V_CMP_NLE_F16,
    V_CMP_NEQ_F16,
    V_CMP_NLT_F16,
    V_CMP_TRU_F16,
    // GFX10 split F16 V_CMPX range: [0xE8..0xEF] and [0xF8..0xFF]
    V_CMPX_LT_F16,
    V_CMPX_EQ_F16,
    V_CMPX_LE_F16,
    V_CMPX_GT_F16,
    V_CMPX_LG_F16,
    V_CMPX_GE_F16,
    V_CMPX_NEQ_F16,
    V_CMPX_NLT_F16,
    // 64-bit integer compares (CS / VS / PS)
    V_CMP_LT_I64,
    V_CMP_EQ_I64,
    V_CMP_LE_I64,
    V_CMP_GT_I64,
    V_CMP_LG_I64,
    V_CMP_GE_I64,
    V_CMP_LT_U64,
    V_CMP_EQ_U64,
    V_CMP_LE_U64,
    V_CMP_GT_U64,
    V_CMP_LG_U64,
    V_CMP_GE_U64,

    // VOP3 (native 3-operand / wave / bitfield / 16-bit ops)
    V_FMA_F32,
    V_FMA_F16,
    V_MAD_F32,
    V_BFE_U32,
    V_BFE_I32,
    V_BFI_B32,
    V_ALIGNBIT_B32,
    V_ALIGNBYTE_B32,
    V_DIV_FMAS_F32,
    V_MUL_LO_U32,
    V_MAD_U64_U32,
    V_MAD_I64_I32,
    V_ADD_NC_U16,
    V_SUB_NC_U16,
    V_MIN_U16,
    V_MAX_U16,
    V_MIN_I16,
    V_MAX_I16,
    V_MUL_LO_U16,
    V_ASHRREV_I16,
    V_LSHLREV_B16,
    V_LSHRREV_B16,
    V_MAD_U16,
    V_MBCNT_LO_U32_B32,
    V_MBCNT_HI_U32_B32,
    V_READLANE_B32,
    V_CVT_PKNORM_I16_F16,
    V_CVT_PKNORM_U16_F16,
    V_CVT_PKRTZ_F16_F32,
    V_CVT_PKNORM_I16_F32,
    V_CVT_PKNORM_U16_F32,
    V_ADD3_U32,
    V_LSHL_ADD_U32,
    V_LSHL_OR_B32,
    V_MIN3_F32,
    V_MIN3_I32,
    V_MIN3_U32,
    V_MAX3_F32,
    V_MAX3_I32,
    V_MAX3_U32,
    V_MED3_F32,
    V_MED3_I32,
    V_MED3_U32,
    V_DOT2_F32_F16,
    V_PERMLANE16_B32,
    V_PERMLANEX16_B32,

    // VOP3P (packed 16-bit f16 / u16 / b16 & mixed-precision ops, encoding 0x33)
    V_PK_LSHLREV_B16,
    V_PK_LSHRREV_B16,
    V_PK_ADD_U16,
    V_PK_FMA_F16,
    V_PK_ADD_F16,
    V_PK_MUL_F16,
    V_FMA_MIX_F32,
    V_FMA_MIXLO_F16,
    V_FMA_MIXHI_F16,

    // SMEM
    S_LOAD_DWORD,
    S_BUFFER_LOAD_DWORD,
    S_BUFFER_LOAD_DWORDX2,
    S_BUFFER_LOAD_DWORDX4,
    S_BUFFER_LOAD_DWORDX8,
    S_BUFFER_LOAD_DWORDX16,

    // MUBUF
    BUFFER_LOAD_FORMAT_X,
    BUFFER_LOAD_FORMAT_XY,
    BUFFER_LOAD_FORMAT_XYZ,
    BUFFER_LOAD_FORMAT_XYZW,
    BUFFER_LOAD_FORMAT_D16_X,
    BUFFER_LOAD_FORMAT_D16_XY,
    BUFFER_LOAD_FORMAT_D16_XYZ,
    BUFFER_LOAD_FORMAT_D16_XYZW,
    BUFFER_LOAD_DWORD,
    BUFFER_STORE_BYTE_D16_HI,
    BUFFER_STORE_SHORT_D16_HI,
    BUFFER_STORE_DWORD,
    BUFFER_LOAD_UBYTE_D16,
    BUFFER_LOAD_UBYTE_D16_HI,
    BUFFER_LOAD_SBYTE_D16,
    BUFFER_LOAD_SBYTE_D16_HI,
    BUFFER_LOAD_SHORT_D16,
    BUFFER_LOAD_SHORT_D16_HI,

    // MTBUF (typed format-converting buffer loads, including D16)
    TBUFFER_LOAD_FORMAT_X,
    TBUFFER_LOAD_FORMAT_XY,
    TBUFFER_LOAD_FORMAT_XYZ,
    TBUFFER_LOAD_FORMAT_XYZW,
    TBUFFER_LOAD_FORMAT_D16_X,
    TBUFFER_LOAD_FORMAT_D16_XY,
    TBUFFER_LOAD_FORMAT_D16_XYZ,
    TBUFFER_LOAD_FORMAT_D16_XYZW,

    // MIMG
    IMAGE_LOAD,
    IMAGE_LOAD_MIP,
    IMAGE_STORE,
    IMAGE_ATOMIC_CMPSWAP,
    IMAGE_SAMPLE,
    IMAGE_SAMPLE_CL,
    IMAGE_SAMPLE_D_CL,
    IMAGE_SAMPLE_LZ,
    IMAGE_SAMPLE_B_CL,
    IMAGE_SAMPLE_C_CL,
    IMAGE_SAMPLE_C_D_CL,
    IMAGE_SAMPLE_C_B_CL,
    IMAGE_SAMPLE_C_LZ,
    IMAGE_BVH_INTERSECT_RAY,

    // DS (LDS / Shared Memory & Swizzle)
    DS_ADD_U32,
    DS_MSKOR_B32,
    DS_WRITE_B32,
    DS_NOP,
    DS_ADD_RTN_U32,
    DS_MSKOR_RTN_B32,
    DS_WRXCHG2_RTN_B32,
    DS_WRXCHG2ST64_RTN_B32,
    DS_SWIZZLE_B32,
    DS_READ_B32,
    DS_READ_U8_D16,
    DS_READ_U8_D16_HI,
    DS_READ_I8_D16,
    DS_READ_I8_D16_HI,
    DS_READ_U16_D16,
    DS_READ_U16_D16_HI,
    DS_WRITE_B96,
    DS_WRITE_B128,
    DS_READ_B96,
    DS_READ_B128,

    // FLAT / GLOBAL / SCRATCH
    GLOBAL_LOAD_DWORD,
    GLOBAL_STORE_DWORD,
    FLAT_LOAD_DWORD,
    FLAT_STORE_DWORD,
    SCRATCH_LOAD_DWORD,
    SCRATCH_STORE_DWORD,

    // EXP
    EXP,

    // VINTRP (FSSM pixel shader parameter interpolation)
    V_INTERP_P1_F32,
    V_INTERP_P2_F32,
    V_INTERP_MOV_F32,
};

enum class Rdna2OperandKind : uint8_t {
    None = 0,
    Sgpr,
    Vgpr,
    InlineInt,
    InlineFloat,
    Literal32,
    VccLo,
    VccHi,
    ExecLo,
    ExecHi,
    Scc,
    M0,
    Null,
};

struct Rdna2Operand {
    Rdna2OperandKind kind = Rdna2OperandKind::None;
    uint16_t raw_code = 0;   // 9-bit RDNA2 source code or register index
    uint16_t reg_index = 0;  // SGPR (0..105) or VGPR (0..255)
    int32_t int_value = 0;
    float float_value = 0.0f;
    uint32_t literal_bits = 0;
    bool neg = false;
    bool abs = false;

    static Rdna2Operand Sgpr(uint8_t idx);
    static Rdna2Operand Vgpr(uint8_t idx);
    static Rdna2Operand InlineInt(int32_t val);
    static Rdna2Operand InlineFloat(float val);
    static Rdna2Operand LiteralU32(uint32_t bits);
    static Rdna2Operand LiteralF32(float val);
    static Rdna2Operand VccLo();
    static Rdna2Operand VccHi();
    static Rdna2Operand ExecLo();
    static Rdna2Operand ExecHi();
    static Rdna2Operand Scc();
    static Rdna2Operand M0();
    static Rdna2Operand Null();
};

enum class Rdna2ExportTarget : uint8_t {
    Mrt0 = 0,
    Mrt1 = 1,
    Mrt2 = 2,
    Mrt3 = 3,
    Mrt4 = 4,
    Mrt5 = 5,
    Mrt6 = 6,
    Mrt7 = 7,
    MrtZ = 8,
    Null = 9,
    Pos0 = 12,
    Pos1 = 13,
    Pos2 = 14,
    Pos3 = 15,
    Prim = 20,  // GFX10.3 NGG / Mesh primitive connectivity export
    Param0 = 32,
    Param1 = 33,
    Param2 = 34,
    Param3 = 35,
};

enum class Rdna2OperandWidth : uint8_t {
    Bits32 = 32,
    Bits16 = 16,
    Bits64 = 64,
};

enum class Rdna2DataKind : uint8_t {
    Untyped = 0,
    Float,
    UnsignedInt,
    SignedInt,
    Packed16,
};

struct Rdna2Instruction {
    uint32_t word_offset = 0;   // Index of word0 in code_words
    uint32_t word_count = 1;    // 1 (32-bit), 2 (64-bit), or 3 (VOP3 + literal32)
    uint32_t raw_words[3] = {0, 0, 0};

    Rdna2Encoding encoding = Rdna2Encoding::Unknown;
    Rdna2Opcode opcode = Rdna2Opcode::Unknown;
    uint16_t raw_opcode = 0;
    Rdna2OperandWidth operand_width = Rdna2OperandWidth::Bits32;
    Rdna2DataKind data_kind = Rdna2DataKind::Untyped;
    uint8_t vop3_op_sel = 0;    // VOP3 bits [14:11] for 16-bit operand/dst half selection

    Rdna2Operand dst{};
    Rdna2Operand src[4]{};
    uint8_t src_count = 0;

    // Immediate / modifier / memory / export metadata
    int16_t simm16 = 0;       // SOPK / SOPP signed 16-bit immediate
    int32_t mem_offset = 0;   // SMEM (21-bit signed), MUBUF (12-bit unsigned), FLAT/GLOBAL (12-bit signed), DS (16-bit)
    uint8_t sbase = 0;        // SMEM base SGPR index (s[sbase*2])
    uint8_t srsrc = 0;        // MUBUF/MIMG descriptor SGPR base (s[srsrc*4])
    uint8_t ssamp = 0;        // MIMG sampler descriptor SGPR base (s[ssamp*4])
    uint8_t soffset = 0;      // SMEM / MUBUF scalar offset code
    uint8_t saddr = 0x7Fu;    // GLOBAL/FLAT scalar base SGPR (0x7F = off)
    uint8_t flat_seg = 0;     // 0=FLAT, 1=SCRATCH, 2=GLOBAL
    bool offen = false;
    bool idxen = false;
    bool glc = false;
    bool slc = false;
    bool dlc = false;
    bool clamp = false;
    uint8_t omod = 0;

    // VOP3P metadata (packed 2x16 ops, encoding 0x33)
    uint8_t vop3p_op_sel = 0;     // bits [2:0]: lo-half src0/1/2 select (0=lo16, 1=hi16)
    uint8_t vop3p_op_sel_hi = 7;  // bits [2:0]: hi-half src0/1/2 select (default 0b111 = hi16)
    uint8_t vop3p_neg_lo = 0;     // bits [2:0]: lo-half negate
    uint8_t vop3p_neg_hi = 0;     // bits [2:0]: hi-half negate

    // MIMG metadata
    uint8_t mimg_dmask = 0;
    uint8_t mimg_dim = 0;
    bool mimg_unrm = false;
    uint8_t mimg_nsa_dwords = 0;
    uint8_t mimg_nsa_vaddr[12]{};

    // DS metadata
    uint8_t ds_offset0 = 0;
    uint8_t ds_offset1 = 0;
    bool ds_gds = false;

    // EXP metadata
    uint8_t exp_target = 0;
    uint8_t exp_en = 0;
    bool exp_done = false;
    bool exp_compr = false;
    bool exp_vm = false;

    // MTBUF & VINTRP metadata
    uint8_t mtbuf_format = 0;  // 7-bit GFX10 unified buffer format (Rdna2MtbufFormat)
    uint8_t vintrp_attr = 0;   // 6-bit parameter attribute slot (0..31)
    uint8_t vintrp_chan = 0;   // 2-bit attribute channel (0..3)

    // DPP8 / DPP8FI (0xE9 / 0xEA) 8-lane quad-local permutation metadata
    bool has_dpp8 = false;
    bool dpp8_fi = false;
    uint32_t dpp8_word = 0;    // 8 x 3-bit lane selectors [23:0]

    // VOP3 SDST carry-out register for 64-bit integer MAD (v_mad_u64_u32 / v_mad_i64_i32)
    Rdna2Operand sdst{};

    std::string mnemonic;
    std::string disasm;
};

// Decodes a 9-bit RDNA2 source operand code (0..511), optionally consuming a 32-bit literal.
Rdna2Operand DecodeRdna2SourceOperand(uint16_t code, uint32_t literal_bits = 0);

// Encodes an operand into a 9-bit RDNA2 source code; sets out_needs_literal if code == 255.
bool EncodeRdna2SourceOperand(
    const Rdna2Operand& op,
    uint16_t& out_code,
    uint32_t& out_literal,
    bool& out_needs_literal);

const char* Rdna2OpcodeName(Rdna2Opcode op) noexcept;
const char* Rdna2EncodingName(Rdna2Encoding enc) noexcept;
// Structural VOP3 (0xD4..0xD7 / 64-bit) & 16-bit instruction classification by encoding/metadata (no _e64 string checks).
bool IsVop3EncodingWord(uint32_t word0) noexcept;
bool IsVop3Int16Instruction(const Rdna2Instruction& inst) noexcept;
bool IsVop3Float16Instruction(const Rdna2Instruction& inst) noexcept;
bool IsVcmpxOpcode(Rdna2Opcode op) noexcept;
bool IsF16CompareOpcode(Rdna2Opcode op) noexcept;
std::string FormatRdna2Operand(const Rdna2Operand& op);
std::string FormatRdna2Instruction(const Rdna2Instruction& inst);

// Disassembles a raw 32-bit RDNA2 / GFX10.3 instruction word stream.
Status DisassembleRdna2(
    const std::vector<uint32_t>& words,
    std::vector<Rdna2Instruction>& out_instructions);

// ---------------------------------------------------------------------------
// RDNA2 / GFX10.3 Binary Instruction Assembler
// ---------------------------------------------------------------------------
class Rdna2Assembler {
public:
    void Clear() { words_.clear(); }
    const std::vector<uint32_t>& Words() const noexcept { return words_; }
    uint32_t CurrentWordOffset() const noexcept { return static_cast<uint32_t>(words_.size()); }

    // SOP2
    void EmitSop2(Rdna2Opcode op, uint8_t sdst, const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1);
    // SOP1
    void EmitSop1(Rdna2Opcode op, uint8_t sdst, const Rdna2Operand& ssrc0);
    // SOPC
    void EmitSopc(Rdna2Opcode op, const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1);
    // SOPK
    void EmitSopk(Rdna2Opcode op, uint8_t sdst, int16_t simm16);
    // SOPP (returns word offset of emitted instruction so branch offsets can be patched)
    uint32_t EmitSopp(Rdna2Opcode op, int16_t simm16 = 0);
    void PatchBranch(uint32_t branch_word_offset, uint32_t target_word_offset);

    // VOP2 (automatically emits 64-bit word when src0 is a 32-bit literal)
    void EmitVop2(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0, uint8_t vsrc1);
    // VOP2 with mandatory 32-bit literal K (V_FMAMK_F16, V_FMAAK_F16, V_MADMK_F32, V_MADAK_F32)
    void EmitVop2Literal(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0, uint8_t vsrc1, uint32_t literal_k);
    // VOP1 (automatically emits 64-bit word when src0 is a 32-bit literal)
    void EmitVop1(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0);
    // VOP1 / VOP2 with GFX10.3 DPP8 (0xE9) or DPP8FI (0xEA) arbitrary 8-lane selector word
    void EmitVop1Dpp8(Rdna2Opcode op, uint8_t vdst, uint8_t vsrc0, uint32_t dpp8_sel24, bool fi = false);
    void EmitVop2Dpp8(Rdna2Opcode op, uint8_t vdst, uint8_t vsrc0, uint8_t vsrc1, uint32_t dpp8_sel24, bool fi = false);
    // VOP3B (64-bit VOP3 with explicit sdst carry-out, e.g., V_MAD_U64_U32 / V_MAD_I64_I32)
    void EmitVop3b(
        Rdna2Opcode op,
        uint8_t vdst,
        uint8_t sdst,
        const Rdna2Operand& src0,
        const Rdna2Operand& src1,
        const Rdna2Operand& src2);
    // VOPC (automatically emits 64-bit word when src0 is a 32-bit literal)
    void EmitVopc(Rdna2Opcode op, const Rdna2Operand& src0, uint8_t vsrc1);
    // VOP3 (64-bit, or 96-bit if one source is a 32-bit literal)
    void EmitVop3(
        Rdna2Opcode op,
        uint8_t vdst,
        const Rdna2Operand& src0,
        const Rdna2Operand& src1,
        const Rdna2Operand& src2 = Rdna2Operand::InlineInt(0),
        bool clamp = false,
        uint8_t op_sel = 0u);

    // VOP3P (64-bit, or 96-bit with literal; encoding 0x33 packed 16-bit ops)
    void EmitVop3p(
        Rdna2Opcode op,
        uint8_t vdst,
        const Rdna2Operand& src0,
        const Rdna2Operand& src1,
        const Rdna2Operand& src2 = Rdna2Operand::InlineInt(0),
        uint8_t op_sel = 0u,
        uint8_t op_sel_hi = 7u,
        uint8_t neg_lo = 0u,
        uint8_t neg_hi = 0u,
        bool clamp = false);

    // SMEM (64-bit)
    void EmitSmem(Rdna2Opcode op, uint8_t sdata, uint8_t sbase_sgpr, int32_t offset, uint8_t soffset = 0x7Du);

    // MUBUF (64-bit)
    void EmitMubuf(
        Rdna2Opcode op,
        uint8_t vdata,
        uint8_t vaddr,
        uint8_t srsrc_sgpr,
        uint16_t offset = 0,
        bool offen = true,
        bool idxen = false,
        uint8_t soffset = 128);

    // MTBUF (64-bit: TBUFFER_LOAD_FORMAT_* / TBUFFER_LOAD_FORMAT_D16_*)
    void EmitMtbuf(
        Rdna2Opcode op,
        uint8_t vdata,
        uint8_t vaddr,
        uint8_t srsrc_sgpr,
        Rdna2MtbufFormat format,
        uint16_t offset = 0,
        bool offen = true,
        bool idxen = false,
        uint8_t soffset = 128);

    // VINTRP (32-bit: V_INTERP_P1_F32, V_INTERP_P2_F32, V_INTERP_MOV_F32)
    void EmitVintrp(
        Rdna2Opcode op,
        uint8_t vdst,
        uint8_t vsrc,
        uint8_t attr,
        uint8_t chan);

    // MIMG (64-bit: IMAGE_LOAD, IMAGE_LOAD_MIP, IMAGE_STORE, IMAGE_ATOMIC_CMPSWAP,
    // IMAGE_SAMPLE, IMAGE_SAMPLE_LZ, IMAGE_BVH_INTERSECT_RAY)
    void EmitMimg(
        Rdna2Opcode op,
        uint8_t vdata,
        uint8_t vaddr,
        uint8_t srsrc_sgpr,
        uint8_t ssamp_sgpr = 0,
        uint8_t dmask = 0xFu,
        uint8_t dim = 1u,
        bool unrm = false,
        bool glc = false);

    // DS (64-bit: DS_WRITE_B32, DS_READ_B32, DS_ADD_U32, DS_ADD_RTN_U32, DS_SWIZZLE_B32)
    void EmitDs(
        Rdna2Opcode op,
        uint8_t vdst,
        uint8_t addr,
        uint8_t data0 = 0,
        uint8_t data1 = 0,
        uint16_t offset = 0,
        bool gds = false);

    // GLOBAL / FLAT / SCRATCH (64-bit) — 12-bit offset strictly pinned to word0[11:0] against dlc/glc/slc
    void EmitGlobal(
        Rdna2Opcode op,
        uint8_t vdst_or_data,
        uint8_t vaddr,
        int16_t offset = 0,
        uint8_t saddr = 0x7Fu,
        bool dlc = false,
        bool glc = false,
        bool slc = false);

    // SCRATCH (64-bit FLAT encoding with flat_seg == 1)
    void EmitScratch(
        Rdna2Opcode op,
        uint8_t vdst_or_data,
        uint8_t vaddr,
        int16_t offset = 0,
        uint8_t saddr = 0x7Fu,
        bool dlc = false,
        bool glc = false,
        bool slc = false);

    // EXP (64-bit)
    void EmitExp(
        uint8_t target,
        uint8_t vsrc0,
        uint8_t vsrc1,
        uint8_t vsrc2,
        uint8_t vsrc3,
        uint8_t en = 0xFu,
        bool done = true,
        bool compr = false,
        bool vm = false);

    // Helper: wrap assembled words into an XS2\0 container
    std::vector<uint8_t> BuildXs2(
        ShaderStage stage,
        uint32_t tg_x = 1,
        uint32_t tg_y = 1,
        uint32_t tg_z = 1,
        uint32_t sgpr_count = 16,
        uint32_t vgpr_count = 16,
        uint32_t wave_size = 32) const;

private:
    std::vector<uint32_t> words_;
};

}  // namespace openscarlett
