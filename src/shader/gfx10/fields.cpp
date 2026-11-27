#include <euphoria/shader/gfx10/decode.hpp>
#include <euphoria/shader/gfx10/fields.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <ranges>
#include <string_view>

namespace euphoria::shader::gfx10 {
namespace {

constexpr std::uint8_t kVcc = 106;
constexpr std::uint8_t kExec = 126;

constexpr std::array<std::string_view, 45> kVop3TwoSources{
    "v_add_f64",
    "v_mul_f64",
    "v_min_f64",
    "v_max_f64",
    "v_ldexp_f64",
    "v_mul_lo_u32",
    "v_mul_hi_u32",
    "v_mul_lo_i32",
    "v_mul_hi_i32",
    "v_trig_preop_f64",
    "v_lshlrev_b64",
    "v_lshrrev_b64",
    "v_ashrrev_i64",
    "v_add_nc_u16",
    "v_sub_nc_u16",
    "v_mul_lo_u16",
    "v_lshrrev_b16",
    "v_ashrrev_i16",
    "v_max_u16",
    "v_max_i16",
    "v_min_u16",
    "v_min_i16",
    "v_add_nc_i16",
    "v_sub_nc_i16",
    "v_add_co_u32",
    "v_sub_co_u32",
    "v_subrev_co_u32",
    "v_pack_b32_f16",
    "v_cvt_pknorm_i16_f16",
    "v_cvt_pknorm_u16_f16",
    "v_lshlrev_b16",
    "v_interp_p1ll_f16",
    "v_readlane_b32",
    "v_writelane_b32",
    "v_ldexp_f32",
    "v_bfm_b32",
    "v_bcnt_u32_b32",
    "v_mbcnt_lo_u32_b32",
    "v_mbcnt_hi_u32_b32",
    "v_cvt_pknorm_i16_f32",
    "v_cvt_pknorm_u16_f32",
    "v_cvt_pk_u16_u32",
    "v_cvt_pk_i16_i32",
    "v_sub_nc_i32",
    "v_add_nc_i32",
};

constexpr std::array<std::string_view, 10> kVop3b{
    "v_add_co_u32",
    "v_sub_co_u32",
    "v_subrev_co_u32",
    "v_add_co_ci_u32",
    "v_sub_co_ci_u32",
    "v_subrev_co_ci_u32",
    "v_div_scale_f32",
    "v_div_scale_f64",
    "v_mad_u64_u32",
    "v_mad_i64_i32",
};

template <std::size_t N>
bool listed(const std::array<std::string_view, N>& list, std::string_view name) noexcept
{
    return std::ranges::find(list, name) != list.end();
}

bool contains(std::string_view text, std::string_view part) noexcept
{
    return text.find(part) != std::string_view::npos;
}

bool bit(std::uint32_t word, std::uint32_t n) noexcept
{
    return ((word >> n) & 1) != 0;
}

Operand scalar(std::uint8_t code, std::uint8_t dwords) noexcept
{
    return Operand::Scalar{code, dwords};
}

Operand vector(std::uint32_t reg, std::uint8_t dwords) noexcept
{
    return Operand::Vector{static_cast<std::uint8_t>(reg), dwords};
}

Operand source(std::uint32_t code, std::uint8_t dwords, std::uint32_t literal) noexcept
{
    if (code <= 127) {
        return scalar(static_cast<std::uint8_t>(code), dwords);
    }
    if (code == 255) {
        return Operand::Literal{literal};
    }
    if (code >= 256) {
        return vector(code - 256, dwords);
    }
    return Operand::Const{static_cast<std::uint8_t>(code)};
}

Operand valu_dst(std::string_view name, std::uint32_t field, std::uint8_t dwords) noexcept
{
    if (name == "v_readlane_b32" || name == "v_readfirstlane_b32") {
        return scalar(static_cast<std::uint8_t>(field), 1);
    }
    return vector(field, dwords);
}

std::vector<std::string_view> split(std::string_view text)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find('_', start);
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

std::string_view last_token(std::string_view text) noexcept
{
    const std::size_t at = text.rfind('_');
    return at == std::string_view::npos ? text : text.substr(at + 1);
}

std::uint8_t type_dwords(std::string_view token) noexcept
{
    return token == "b64" || token == "u64" || token == "i64" || token == "f64" ? 2 : 1;
}

struct SaluDwords {
    std::uint8_t dst;
    std::uint8_t src0;
    std::uint8_t src1;
};

SaluDwords salu_dwords(std::string_view name) noexcept
{
    if (name == "s_lshl_b64" || name == "s_lshr_b64" || name == "s_ashr_i64" || name == "s_bfe_u64"
        || name == "s_bfe_i64" || name == "s_bitcmp0_b64" || name == "s_bitcmp1_b64") {
        return {2, 2, 1};
    }
    if (name == "s_bfm_b64" || name == "s_bitset0_b64" || name == "s_bitset1_b64"
        || name == "s_bitreplicate_b64_b32") {
        return {2, 1, 1};
    }
    if (name == "s_bcnt0_i32_b64" || name == "s_bcnt1_i32_b64" || name == "s_ff0_i32_b64"
        || name == "s_ff1_i32_b64" || name == "s_flbit_i32_b64" || name == "s_flbit_i32_i64") {
        return {1, 2, 2};
    }
    const std::uint8_t n = type_dwords(last_token(name));
    return {n, n, n};
}

struct ValuDwords {
    std::uint8_t dst;
    std::array<std::uint8_t, 3> src;
};

ValuDwords valu_dwords(std::string_view name)
{
    if (name == "v_lshlrev_b64" || name == "v_lshrrev_b64" || name == "v_ashrrev_i64") {
        return {2, {1, 2, 2}};
    }
    if (name == "v_ldexp_f64" || name == "v_trig_preop_f64" || name == "v_cmp_class_f64"
        || name == "v_cmpx_class_f64") {
        return {2, {2, 1, 2}};
    }
    if (name == "v_mad_u64_u32" || name == "v_mad_i64_i32") {
        return {2, {1, 1, 2}};
    }
    if (name == "v_qsad_pk_u16_u8" || name == "v_mqsad_pk_u16_u8") {
        return {2, {2, 1, 2}};
    }
    if (name == "v_mqsad_u32_u8") {
        return {4, {2, 1, 4}};
    }
    if (name == "v_frexp_exp_i32_f64") {
        return {1, {2, 2, 2}};
    }
    const std::vector<std::string_view> parts = split(name);
    if (parts.size() == 4 && parts[1] == "cvt") {
        const std::uint8_t n = type_dwords(parts[3]);
        return {type_dwords(parts[2]), {n, n, n}};
    }
    const std::uint8_t n = type_dwords(parts.empty() ? std::string_view{} : parts.back());
    return {n, {n, n, n}};
}

std::uint8_t div_ceil2(std::uint8_t value) noexcept
{
    return static_cast<std::uint8_t>((value + 1) / 2);
}

std::uint8_t data_dwords(std::string_view name)
{
    std::uint8_t n = 1;
    for (const std::string_view token : split(name)) {
        if (token == "dwordx2" || token == "x2" || token == "xy" || token == "b64" || token == "u64"
            || token == "i64" || token == "f64") {
            n = 2;
        } else if (token == "dwordx3" || token == "xyz" || token == "b96") {
            n = 3;
        } else if (token == "dwordx4" || token == "xyzw" || token == "b128") {
            n = 4;
        } else if (token == "dwordx8") {
            n = 8;
        } else if (token == "dwordx16") {
            n = 16;
        }
    }
    if (contains(name, "cmpswap")) {
        n = static_cast<std::uint8_t>(n * 2);
    }
    if (contains(name, "format_d16")) {
        n = div_ceil2(n);
    }
    return n;
}

std::uint8_t image_addr_dwords(std::string_view name, std::uint8_t dim, bool a16)
{
    constexpr std::array<std::array<std::uint8_t, 2>, 8> kDims{{
        {1, 1},
        {2, 2},
        {3, 3},
        {3, 2},
        {2, 1},
        {3, 2},
        {3, 2},
        {4, 2},
    }};
    const auto [coords, grads] = kDims[dim & 7];
    if (name == "image_bvh_intersect_ray") {
        return a16 ? 8 : 11;
    }
    if (name == "image_bvh64_intersect_ray") {
        return a16 ? 9 : 12;
    }
    if (name == "image_get_resinfo") {
        return 1;
    }
    std::uint8_t whole = 0;
    std::uint8_t derivs = 0;
    std::uint8_t packed = coords;
    std::optional<std::string_view> rest;
    if (name.starts_with("image_sample")) {
        rest = name.substr(std::string_view{"image_sample"}.size());
    } else if (name.starts_with("image_gather4")) {
        rest = name.substr(std::string_view{"image_gather4"}.size());
    }
    if (rest) {
        for (const std::string_view token : split(*rest)) {
            if (token == "o" || token == "b" || token == "c") {
                ++whole;
            } else if (token == "d" || token == "cd") {
                derivs = grads;
            } else if (token == "l" || token == "cl") {
                ++packed;
            }
        }
    } else if (contains(name, "_mip")) {
        ++packed;
    }
    if (a16) {
        return static_cast<std::uint8_t>(whole + 2 * div_ceil2(derivs) + div_ceil2(packed));
    }
    return static_cast<std::uint8_t>(whole + 2 * derivs + packed);
}

Valu valu_fields(const Instruction& inst, std::string_view name, std::uint8_t lanes)
{
    if (name == "v_nop" || name == "v_pipeflush" || name == "v_clrexcp") {
        return {};
    }
    const auto w = [&inst](std::size_t index) -> std::uint32_t {
        return index < inst.words.size() ? inst.words[index] : 0;
    };
    const std::uint32_t w0 = w(0);
    const ValuDwords sizes = valu_dwords(name);
    bool compare = false;
    if (inst.encoding == Encoding::Vopc) {
        compare = true;
    } else if (inst.encoding == Encoding::Vop3) {
        compare = inst.opcode < 0x100;
    }
    const bool vop3b = listed(kVop3b, name);
    const bool carry_in = name.ends_with("_co_ci_u32") || name == "v_cndmask_b32";
    Valu v;

    switch (inst.encoding) {
    case Encoding::Vop1:
    case Encoding::Vop2:
    case Encoding::Vopc: {
        const std::uint32_t ext = w(1);
        std::uint32_t src0 = w0 & 0x1FF;
        const std::uint32_t vsrc1 = 256 + ((w0 >> 9) & 0xFF);
        std::uint32_t src1 = vsrc1;
        if (src0 == 0xF9) {
            Sdwa sdwa{
                static_cast<std::uint8_t>((ext >> 8) & 7),
                static_cast<std::uint8_t>((ext >> 11) & 3),
                {static_cast<std::uint8_t>((ext >> 16) & 7), static_cast<std::uint8_t>((ext >> 24) & 7)},
                static_cast<std::uint8_t>(((ext >> 19) & 1) | ((ext >> 26) & 2)),
            };
            src0 = (ext & 0xFF) | (((ext >> 23) & 1) == 0 ? 256u : 0u);
            if ((ext >> 31) == 1) {
                src1 = vsrc1 - 256;
            }
            v.neg = static_cast<std::uint8_t>(((ext >> 20) & 1) | ((ext >> 27) & 2));
            v.abs = static_cast<std::uint8_t>(((ext >> 21) & 1) | ((ext >> 28) & 2));
            if (compare) {
                if (((ext >> 15) & 1) == 1) {
                    v.dst = scalar(static_cast<std::uint8_t>((ext >> 8) & 0x7F), lanes);
                }
                sdwa.dst_sel = 6;
                sdwa.dst_unused = 0;
                v.sdwa = sdwa;
            } else {
                v.clamp = ((ext >> 13) & 1) == 1;
                v.omod = static_cast<std::uint8_t>((ext >> 14) & 3);
                v.sdwa = sdwa;
            }
        } else if (src0 == 0xFA) {
            src0 = 256 + (ext & 0xFF);
            v.neg = static_cast<std::uint8_t>(((ext >> 20) & 1) | ((ext >> 21) & 2));
            v.abs = static_cast<std::uint8_t>(((ext >> 21) & 1) | ((ext >> 22) & 2));
            v.dpp = Dpp{Dpp::Row{
                static_cast<std::uint16_t>((ext >> 8) & 0x1FF),
                static_cast<std::uint8_t>(ext >> 28),
                static_cast<std::uint8_t>((ext >> 24) & 0xF),
                ((ext >> 19) & 1) == 1,
                ((ext >> 18) & 1) == 1,
            }};
        } else if (src0 == 0xE9 || src0 == 0xEA) {
            v.dpp = Dpp{Dpp::Lanes{ext >> 8, src0 == 0xEA}};
            src0 = 256 + (ext & 0xFF);
        }
        const Operand op0 = source(src0, sizes.src[0], ext);
        const Operand op1 = source(src1, sizes.src[1], ext);
        if (inst.encoding == Encoding::Vopc) {
            const std::uint8_t mask = name.starts_with("v_cmpx") ? kExec : kVcc;
            if (!v.dst) {
                v.dst = scalar(mask, lanes);
            }
            v.src = {op0, op1};
        } else if (inst.encoding == Encoding::Vop1) {
            v.dst = valu_dst(name, (w0 >> 17) & 0xFF, sizes.dst);
            v.src = {op0};
        } else {
            v.dst = vector((w0 >> 17) & 0xFF, sizes.dst);
            if (name.ends_with("mk_f32") || name.ends_with("mk_f16")) {
                v.src = {op0, Operand::Literal{ext}, op1};
            } else if (name.ends_with("ak_f32") || name.ends_with("ak_f16")) {
                v.src = {op0, op1, Operand::Literal{ext}};
            } else {
                v.src = {op0, op1};
            }
            if (carry_in) {
                v.src.push_back(scalar(kVcc, lanes));
            }
            if (name.ends_with("_co_ci_u32")) {
                v.carry = scalar(kVcc, lanes);
            }
        }
        break;
    }
    case Encoding::Vop3:
    case Encoding::Vop3p: {
        const std::uint32_t w1 = w(1);
        const auto field = [w1](std::uint32_t index) { return (w1 >> (9 * index)) & 0x1FF; };
        std::size_t count = 3;
        if (inst.encoding == Encoding::Vop3p) {
            count = name.starts_with("v_pk_") && !contains(name, "_fma_") && !contains(name, "_mad_") ? 2 : 3;
        } else if (inst.opcode < 0x100) {
            count = 2;
        } else if (inst.opcode < 0x140) {
            count = 2 + (carry_in ? 1 : 0);
        } else if (inst.opcode >= 0x180 && inst.opcode < 0x200) {
            count = 1;
        } else if (listed(kVop3TwoSources, name)) {
            count = 2;
        }
        v.src.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint32_t code = field(static_cast<std::uint32_t>(index));
            if (index == 0 && name.starts_with("v_interp_")) {
                v.src.push_back(Operand::Attr{
                    static_cast<std::uint8_t>(code & 0x3F),
                    static_cast<std::uint8_t>((code >> 6) & 3),
                    ((code >> 8) & 1) == 1,
                });
            } else if (index == 2 && carry_in) {
                v.src.push_back(source(code, lanes, w(2)));
            } else {
                v.src.push_back(source(code, sizes.src[index], w(2)));
            }
        }
        v.clamp = ((w0 >> 15) & 1) == 1;
        v.neg = static_cast<std::uint8_t>(w1 >> 29);
        if (inst.encoding == Encoding::Vop3p) {
            v.neg_hi = static_cast<std::uint8_t>((w0 >> 8) & 7);
            v.opsel = static_cast<std::uint8_t>((w0 >> 11) & 7);
            v.opsel_hi = static_cast<std::uint8_t>(((w1 >> 27) & 3) | ((w0 >> 12) & 4));
        } else {
            v.omod = static_cast<std::uint8_t>((w1 >> 27) & 3);
            if (vop3b) {
                v.carry = scalar(static_cast<std::uint8_t>((w0 >> 8) & 0x7F), lanes);
            } else {
                v.abs = static_cast<std::uint8_t>((w0 >> 8) & 7);
                v.opsel = static_cast<std::uint8_t>((w0 >> 11) & 0xF);
            }
        }
        const std::uint32_t vdst = w0 & 0xFF;
        v.dst = compare ? scalar(static_cast<std::uint8_t>(vdst), lanes) : valu_dst(name, vdst, sizes.dst);
        break;
    }
    default:
        break;
    }
    return v;
}

}

bool ds_pair(std::string_view name)
{
    for (const std::string_view token : split(name)) {
        if (token.starts_with("read2") || token.starts_with("write2") || token.starts_with("wrxchg2")) {
            return true;
        }
    }
    return false;
}

Fields Instruction::fields(bool wave64) const
{
    const std::string_view op = name().value_or("");
    const auto w = [this](std::size_t index) -> std::uint32_t {
        return index < words.size() ? words[index] : 0;
    };
    const std::uint32_t w0 = w(0);
    const std::uint32_t w1 = w(1);
    const auto cache = [](bool glc, bool slc, bool dlc) { return Cache{glc, slc, dlc}; };

    switch (encoding) {
    case Encoding::Sop2:
    case Encoding::Sop1:
    case Encoding::Sopc: {
        const SaluDwords sizes = salu_dwords(op);
        const Operand sdst = scalar(static_cast<std::uint8_t>((w0 >> 16) & 0x7F), sizes.dst);
        const auto ssrc = [&](std::uint32_t shift, std::uint8_t dwords) {
            return source((w0 >> shift) & 0xFF, dwords, w1);
        };
        Fields::Sop sop;
        if (encoding == Encoding::Sop2) {
            sop.dst = sdst;
            sop.src = {ssrc(0, sizes.src0), ssrc(8, sizes.src1)};
        } else if (encoding == Encoding::Sopc) {
            sop.src = {ssrc(0, sizes.src0), ssrc(8, sizes.src1)};
        } else if (op == "s_getpc_b64") {
            sop.dst = sdst;
        } else if (op == "s_setpc_b64" || op == "s_rfe_b64") {
            sop.src = {ssrc(0, sizes.src0)};
        } else {
            sop.dst = sdst;
            sop.src = {ssrc(0, sizes.src0)};
        }
        return sop;
    }
    case Encoding::Sopk: {
        const Operand sdst =
            scalar(static_cast<std::uint8_t>((w0 >> 16) & 0x7F), static_cast<std::uint8_t>(op == "s_call_b64" ? 2 : 1));
        Fields::Sop sop;
        sop.imm = static_cast<std::uint16_t>(w0);
        if (op == "s_setreg_imm32_b32") {
            sop.src = {Operand::Literal{w1}};
        } else if (op == "s_version") {
        } else if (op.starts_with("s_cmpk_") || op.starts_with("s_waitcnt_") || op == "s_setreg_b32") {
            sop.src = {sdst};
        } else {
            sop.dst = sdst;
        }
        return sop;
    }
    case Encoding::Sopp:
        return Fields::Sopp{static_cast<std::uint16_t>(w0)};
    case Encoding::Smem: {
        std::optional<std::uint8_t> data;
        if (op == "s_gl1_inv" || op == "s_dcache_inv" || op == "s_dcache_wb" || op.starts_with("s_dcache_discard")) {
        } else if (op == "s_memtime" || op == "s_memrealtime") {
            data = std::uint8_t{2};
        } else {
            data = data_dwords(op);
        }
        const std::uint8_t base = op.starts_with("s_buffer_") ? 4 : 2;
        Fields::Smem smem;
        if (data) {
            smem.data = scalar(static_cast<std::uint8_t>((w0 >> 6) & 0x7F), *data);
        }
        smem.base = scalar(static_cast<std::uint8_t>((w0 & 0x3F) << 1), base);
        smem.offset = static_cast<std::int32_t>(w1 << 11) >> 11;
        smem.soffset = scalar(static_cast<std::uint8_t>(w1 >> 25), 1);
        smem.cache = cache(bit(w0, 16), false, bit(w0, 14));
        return smem;
    }
    case Encoding::Vop1:
    case Encoding::Vop2:
    case Encoding::Vopc:
    case Encoding::Vop3:
    case Encoding::Vop3p:
        return valu_fields(*this, op, wave64 ? 2 : 1);
    case Encoding::Vintrp: {
        const auto vsrc = static_cast<std::uint8_t>(w0 & 0xFF);
        Fields::Vintrp vintrp;
        vintrp.dst = vector(w0 >> 18, 1);
        vintrp.src = op == "v_interp_mov_f32" ? Operand{Operand::Const{vsrc}} : vector(vsrc, 1);
        vintrp.attr = Operand::Attr{
            static_cast<std::uint8_t>((w0 >> 10) & 0x3F),
            static_cast<std::uint8_t>((w0 >> 8) & 3),
            false,
        };
        return vintrp;
    }
    case Encoding::Ds: {
        const std::uint8_t ty = data_dwords(op);
        const bool pair = ds_pair(op);
        const bool returns = contains(op, "read") || contains(op, "_rtn") || op == "ds_swizzle_b32"
                          || op == "ds_permute_b32" || op == "ds_bpermute_b32" || op == "ds_append"
                          || op == "ds_consume" || op == "ds_ordered_count";
        const bool no_addr = op.starts_with("ds_gws") || contains(op, "addtid") || op == "ds_nop"
                          || op == "ds_append" || op == "ds_consume";
        std::uint32_t data = 1;
        if (contains(op, "read") || contains(op, "src2") || op == "ds_swizzle_b32" || op == "ds_nop"
            || op == "ds_append" || op == "ds_consume" || op == "ds_ordered_count" || op == "ds_gws_sema_v"
            || op == "ds_gws_sema_p" || op == "ds_gws_sema_release_all") {
            data = 0;
        } else if (pair || contains(op, "cmpst") || contains(op, "mskor")) {
            data = 2;
        }
        Fields::Ds ds;
        if (returns) {
            ds.dst = vector(w1 >> 24, static_cast<std::uint8_t>(ty * (pair ? 2 : 1)));
        }
        if (!no_addr) {
            ds.addr = vector(w1 & 0xFF, 1);
        }
        for (std::uint32_t index = 0; index < data; ++index) {
            ds.data.push_back(vector((w1 >> (8 + 8 * index)) & 0xFF, ty));
        }
        ds.offset0 = static_cast<std::uint8_t>(w0);
        ds.offset1 = static_cast<std::uint8_t>(w0 >> 8);
        ds.gds = bit(w0, 17);
        return ds;
    }
    case Encoding::Mubuf:
    case Encoding::Mtbuf: {
        const bool offen = bit(w0, 12);
        const bool idxen = bit(w0, 13);
        const bool tfe = bit(w1, 23);
        const bool mtbuf = encoding == Encoding::Mtbuf;
        Fields::Buffer buffer;
        if (!op.ends_with("_inv")) {
            buffer.data = vector((w1 >> 8) & 0xFF, static_cast<std::uint8_t>(data_dwords(op) + (tfe ? 1 : 0)));
        }
        buffer.addr = vector(w1 & 0xFF, static_cast<std::uint8_t>(1 + (offen && idxen ? 1 : 0)));
        buffer.rsrc = scalar(static_cast<std::uint8_t>(((w1 >> 16) & 0x1F) << 2), 4);
        buffer.soffset = source(w1 >> 24, 1, 0);
        buffer.offset = static_cast<std::uint16_t>(w0 & 0xFFF);
        buffer.offen = offen;
        buffer.idxen = idxen;
        if (mtbuf) {
            buffer.format = static_cast<std::uint8_t>((w0 >> 19) & 0x7F);
        }
        buffer.lds = !mtbuf && bit(w0, 16);
        buffer.tfe = tfe;
        buffer.cache = cache(bit(w0, 14), bit(w1, 22), bit(w0, 15));
        return buffer;
    }
    case Encoding::Mimg: {
        const auto dim = static_cast<std::uint8_t>((w0 >> 3) & 7);
        const auto dmask = static_cast<std::uint8_t>((w0 >> 8) & 0xF);
        const bool a16 = bit(w1, 30);
        const bool d16 = bit(w1, 31);
        const bool r128 = bit(w0, 15);
        const bool tfe = bit(w0, 16);
        const bool lwe = bit(w0, 17);
        std::uint8_t data = op.starts_with("image_gather4")
                              ? 4
                              : static_cast<std::uint8_t>(std::max(std::popcount(dmask), 1));
        if (d16) {
            data = div_ceil2(data);
        }
        const std::uint8_t count = image_addr_dwords(op, dim, a16);
        Fields::Image image;
        if (((w0 >> 1) & 3) == 0) {
            image.addr = {vector(w1 & 0xFF, count)};
        } else {
            for (std::size_t index = 0; index < count; ++index) {
                if (index == 0) {
                    image.addr.push_back(vector(w1 & 0xFF, 1));
                } else {
                    image.addr.push_back(vector((w(2 + (index - 1) / 4) >> (8 * ((index - 1) % 4))) & 0xFF, 1));
                }
            }
        }
        const bool sampled = op.starts_with("image_sample") || op.starts_with("image_gather4")
                          || op.starts_with("image_get_lod");
        image.data = vector((w1 >> 8) & 0xFF, static_cast<std::uint8_t>(data + (tfe || lwe ? 1 : 0)));
        image.rsrc = scalar(static_cast<std::uint8_t>(((w1 >> 16) & 0x1F) << 2), r128 ? 4 : 8);
        if (sampled) {
            image.sampler = scalar(static_cast<std::uint8_t>(((w1 >> 21) & 0x1F) << 2), 4);
        }
        image.dmask = dmask;
        image.dim = dim;
        image.unorm = bit(w0, 12);
        image.r128 = r128;
        image.a16 = a16;
        image.d16 = d16;
        image.tfe = tfe;
        image.lwe = lwe;
        image.cache = cache(bit(w0, 13), bit(w0, 25), bit(w0, 7));
        return image;
    }
    case Encoding::Flat:
    case Encoding::Global:
    case Encoding::Scratch: {
        const auto saddr = static_cast<std::uint8_t>((w1 >> 16) & 0x7F);
        const bool glc = bit(w0, 16);
        const std::uint8_t n = data_dwords(op);
        const bool load = contains(op, "_load_");
        const bool atomic = contains(op, "_atomic_");
        Fields::Flat flat;
        if (encoding == Encoding::Flat) {
            flat.addr = vector(w1 & 0xFF, 2);
        } else if (encoding == Encoding::Global) {
            flat.addr = vector(w1 & 0xFF, static_cast<std::uint8_t>(1 + (saddr == NULL_REG ? 1 : 0)));
        } else if (saddr == NULL_REG) {
            flat.addr = vector(w1 & 0xFF, 1);
        }
        const std::uint8_t ret = atomic && contains(op, "cmpswap") ? static_cast<std::uint8_t>(n / 2) : n;
        if (load || (atomic && glc)) {
            flat.dst = vector(w1 >> 24, ret);
        }
        if (!load) {
            flat.data = vector((w1 >> 8) & 0xFF, n);
        }
        if (encoding != Encoding::Flat) {
            flat.saddr = scalar(saddr, static_cast<std::uint8_t>(encoding == Encoding::Global ? 2 : 1));
        }
        if (encoding == Encoding::Flat) {
            flat.offset = static_cast<std::int16_t>(w0 & 0x7FF);
        } else {
            flat.offset = static_cast<std::int16_t>(static_cast<std::int16_t>(static_cast<std::uint16_t>(w0 << 4)) >> 4);
        }
        flat.lds = bit(w0, 13);
        flat.cache = cache(glc, bit(w0, 17), bit(w0, 12));
        return flat;
    }
    case Encoding::Exp: {
        const bool compr = bit(w0, 10);
        Fields::Exp exp;
        exp.target = static_cast<std::uint8_t>((w0 >> 4) & 0x3F);
        for (std::uint32_t index = 0; index < 4; ++index) {
            const std::uint32_t slot = compr ? index / 2 : index;
            if (bit(w0, index)) {
                exp.src[index] = vector((w1 >> (8 * slot)) & 0xFF, 1);
            }
        }
        exp.compr = compr;
        exp.done = bit(w0, 11);
        exp.vm = bit(w0, 12);
        return exp;
    }
    }
    return Fields{};
}

}
