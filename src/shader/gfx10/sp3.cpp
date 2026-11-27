#include <euphoria/shader/gfx10/sp3.hpp>

#include <array>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace euphoria::shader::gfx10 {
namespace {

std::string join(const std::vector<std::string>& parts, std::string_view separator = ", ")
{
    std::string out;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) {
            out += separator;
        }
        out += parts[index];
    }
    return out;
}

void flags(std::string& out, const Cache& cache, bool dlc_first)
{
    if (dlc_first && cache.dlc) {
        out += " dlc";
    }
    if (cache.glc) {
        out += " glc";
    }
    if (cache.slc) {
        out += " slc";
    }
    if (!dlc_first && cache.dlc) {
        out += " dlc";
    }
    if (out.starts_with(' ')) {
        out.erase(0, 1);
    }
}

std::string label(const Instruction& inst, std::uint16_t imm)
{
    const std::int64_t target = static_cast<std::int64_t>(inst.offset)
                              + 4 * static_cast<std::int64_t>(inst.words.size())
                              + 4 * static_cast<std::int64_t>(static_cast<std::int16_t>(imm));
    return std::format("label_{:04X}", static_cast<std::uint64_t>(target));
}

std::string hwreg(std::uint16_t imm)
{
    const unsigned id = imm & 0x3F;
    const unsigned offset = (imm >> 6) & 0x1F;
    const unsigned size = ((imm >> 11) & 0x1F) + 1;
    std::string name;
    switch (id) {
    case 1:
        name = "HW_REG_MODE";
        break;
    case 20:
        name = "HW_REG_SHADER_FLAT_SCRATCH_LO";
        break;
    case 21:
        name = "HW_REG_SHADER_FLAT_SCRATCH_HI";
        break;
    case 25:
        name = "HW_REG_POPS_PACKER";
        break;
    default:
        name = std::to_string(id);
        break;
    }
    if (offset == 0 && size == 32) {
        return std::format("hwreg({})", name);
    }
    return std::format("hwreg({}, {}, {})", name, offset, size);
}

std::string waitcnt(std::uint16_t imm)
{
    const unsigned vm = (imm & 0xF) | ((imm >> 14) << 4);
    const unsigned exp = (imm >> 4) & 7;
    const unsigned lgkm = (imm >> 8) & 0x3F;
    std::vector<std::string> parts;
    if (vm != 0x3F) {
        parts.push_back(std::format("vmcnt({})", vm));
    }
    if (exp != 7) {
        parts.push_back(std::format("expcnt({})", exp));
    }
    if (lgkm != 0x3F) {
        parts.push_back(std::format("lgkmcnt({})", lgkm));
    }
    if (parts.empty() || parts.size() == 3) {
        return std::format("0x{:04x}", imm);
    }
    return join(parts, " & ");
}

std::string sendmsg(std::uint16_t imm)
{
    const unsigned msg = imm & 0xF;
    const unsigned op = (imm >> 4) & 7;
    const unsigned stream = (imm >> 8) & 3;
    std::string msg_name;
    switch (msg) {
    case 2:
        msg_name = "MSG_GS";
        break;
    case 3:
        msg_name = "MSG_GS_DONE";
        break;
    case 7:
        msg_name = "MSG_ORDERED_PS_DONE";
        break;
    case 9:
        msg_name = "MSG_GS_ALLOC_REQ";
        break;
    default:
        msg_name = std::to_string(msg);
        break;
    }
    constexpr std::array<const char*, 4> kOps{"GS_OP_NOP", "GS_OP_CUT", "GS_OP_EMIT", "GS_OP_EMIT_CUT"};
    const std::string op_name = op < kOps.size() ? std::string{kOps[op]} : std::to_string(op);
    if (msg == 3 && op == 0) {
        return std::format("sendmsg({}, {})", msg_name, op_name);
    }
    return std::format("sendmsg({}, {}, {})", msg_name, op_name, stream);
}

std::string buffer_format(std::uint8_t format)
{
    constexpr std::array<std::string_view, 78> kNames{
        "INVALID",
        "8_UNORM",
        "8_SNORM",
        "8_USCALED",
        "8_SSCALED",
        "8_UINT",
        "8_SINT",
        "16_UNORM",
        "16_SNORM",
        "16_USCALED",
        "16_SSCALED",
        "16_UINT",
        "16_SINT",
        "16_FLOAT",
        "8_8_UNORM",
        "8_8_SNORM",
        "8_8_USCALED",
        "8_8_SSCALED",
        "8_8_UINT",
        "8_8_SINT",
        "32_UINT",
        "32_SINT",
        "32_FLOAT",
        "16_16_UNORM",
        "16_16_SNORM",
        "16_16_USCALED",
        "16_16_SSCALED",
        "16_16_UINT",
        "16_16_SINT",
        "16_16_FLOAT",
        "10_11_11_UNORM",
        "10_11_11_SNORM",
        "10_11_11_USCALED",
        "10_11_11_SSCALED",
        "10_11_11_UINT",
        "10_11_11_SINT",
        "10_11_11_FLOAT",
        "11_11_10_UNORM",
        "11_11_10_SNORM",
        "11_11_10_USCALED",
        "11_11_10_SSCALED",
        "11_11_10_UINT",
        "11_11_10_SINT",
        "11_11_10_FLOAT",
        "10_10_10_2_UNORM",
        "10_10_10_2_SNORM",
        "10_10_10_2_USCALED",
        "10_10_10_2_SSCALED",
        "10_10_10_2_UINT",
        "10_10_10_2_SINT",
        "2_10_10_10_UNORM",
        "2_10_10_10_SNORM",
        "2_10_10_10_USCALED",
        "2_10_10_10_SSCALED",
        "2_10_10_10_UINT",
        "2_10_10_10_SINT",
        "8_8_8_8_UNORM",
        "8_8_8_8_SNORM",
        "8_8_8_8_USCALED",
        "8_8_8_8_SSCALED",
        "8_8_8_8_UINT",
        "8_8_8_8_SINT",
        "32_32_UINT",
        "32_32_SINT",
        "32_32_FLOAT",
        "16_16_16_16_UNORM",
        "16_16_16_16_SNORM",
        "16_16_16_16_USCALED",
        "16_16_16_16_SSCALED",
        "16_16_16_16_UINT",
        "16_16_16_16_SINT",
        "16_16_16_16_FLOAT",
        "32_32_32_UINT",
        "32_32_32_SINT",
        "32_32_32_FLOAT",
        "32_32_32_32_UINT",
        "32_32_32_32_SINT",
        "32_32_32_32_FLOAT",
    };
    if (format < kNames.size()) {
        return std::format("BUF_FMT_{}", kNames[format]);
    }
    return std::to_string(format);
}

bool bit(std::uint8_t mask, std::size_t index) noexcept
{
    return ((mask >> index) & 1) != 0;
}

std::string dpp_ctrl(std::uint16_t ctrl)
{
    if (ctrl <= 0x0FF) {
        return std::format("quad_perm:[{},{},{},{}]", ctrl & 3, (ctrl >> 2) & 3, (ctrl >> 4) & 3, (ctrl >> 6) & 3);
    }
    if (ctrl >= 0x101 && ctrl <= 0x10F) {
        return std::format("row_shl:{}", ctrl & 0xF);
    }
    if (ctrl >= 0x111 && ctrl <= 0x11F) {
        return std::format("row_shr:{}", ctrl & 0xF);
    }
    if (ctrl >= 0x121 && ctrl <= 0x12F) {
        return std::format("row_ror:{}", ctrl & 0xF);
    }
    if (ctrl == 0x140) {
        return "row_mirror";
    }
    if (ctrl == 0x141) {
        return "row_half_mirror";
    }
    if (ctrl >= 0x150 && ctrl <= 0x15F) {
        return std::format("row_share:{}", ctrl & 0xF);
    }
    if (ctrl >= 0x160 && ctrl <= 0x16F) {
        return std::format("row_xmask:{}", ctrl & 0xF);
    }
    return std::format("dpp_ctrl:0x{:x}", ctrl);
}

std::string valu_text(std::string_view name, const Valu& v, Encoding encoding)
{
    const bool mix = name.starts_with("v_fma_mix");
    const bool permlane = name.starts_with("v_permlane");
    const auto src = [&](std::size_t index) {
        std::string s = to_string(v.src[index]);
        if (v.sdwa && bit(v.sdwa->sext, index)) {
            s = std::format("sext({})", s);
        }
        if (encoding == Encoding::Vop3p) {
            if (mix) {
                if (bit(v.opsel_hi, index)) {
                    s = std::format("{}({})", bit(v.opsel, index) ? "sel_hi" : "sel_lo", s);
                }
                if (bit(v.neg_hi, index)) {
                    s = std::format("abs({})", s);
                }
                if (bit(v.neg, index)) {
                    s = std::format("-{}", s);
                }
            } else {
                if (bit(v.neg, index) && bit(v.neg_hi, index)) {
                    s = std::format("-{}", s);
                }
                const bool lo = bit(v.opsel, index);
                const bool hi = bit(v.opsel_hi, index);
                if (!lo && !hi) {
                    s = std::format("sel_lo({})", s);
                } else if (lo && hi) {
                    s = std::format("sel_hi({})", s);
                } else if (lo && !hi) {
                    s = std::format("sel_hi_lo({})", s);
                }
            }
        } else {
            if (bit(v.abs, index)) {
                s = std::format("abs({})", s);
            }
            if (bit(v.neg, index)) {
                s = std::format("-{}", s);
            }
            if (bit(v.opsel, index) && !permlane) {
                s = std::format("sel_hi({})", s);
            }
        }
        return s;
    };

    std::vector<std::string> ops;
    if (v.dst) {
        ops.push_back(to_string(*v.dst));
    }
    if (v.carry) {
        ops.push_back(to_string(*v.carry));
    }
    std::vector<std::size_t> order(v.src.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    if (name.starts_with("v_interp_") && order.size() > 1) {
        std::swap(order[0], order[1]);
    }
    for (const std::size_t index : order) {
        ops.push_back(src(index));
    }
    std::string out = join(ops);
    const auto suffix = [&out](std::string_view text) {
        out += ' ';
        out += text;
    };
    if (!v.src.empty()) {
        if (const Operand::Attr* attr = v.src.front().as<Operand::Attr>(); attr != nullptr && attr->high) {
            suffix("high");
        }
    }
    constexpr std::array<std::string_view, 4> kOmod{"", "mul:2", "mul:4", "div:2"};
    const std::string_view omod = kOmod[v.omod & 3];
    if (permlane) {
        if ((v.opsel & 1) != 0) {
            suffix("fi:1");
        }
        if ((v.opsel & 2) != 0) {
            suffix("bound_ctrl:1");
        }
    }
    if (v.sdwa) {
        const Sdwa& sdwa = *v.sdwa;
        if (v.clamp) {
            suffix("clamp");
        }
        if (!omod.empty()) {
            suffix(omod);
        }
        constexpr std::array<std::string_view, 8> kSel{
            "BYTE_0", "BYTE_1", "BYTE_2", "BYTE_3", "WORD_0", "WORD_1", "DWORD", "?",
        };
        if (sdwa.dst_sel != 6) {
            suffix(std::format("dst_sel:{}", kSel[sdwa.dst_sel & 7]));
        }
        if (sdwa.dst_unused != 0) {
            constexpr std::array<std::string_view, 4> kUnused{"UNUSED_PAD", "UNUSED_SEXT", "UNUSED_PRESERVE", "?"};
            suffix(std::format("dst_unused:{}", kUnused[sdwa.dst_unused & 3]));
        }
        for (std::size_t index = 0; index < sdwa.src_sel.size(); ++index) {
            if (sdwa.src_sel[index] != 6) {
                suffix(std::format("src{}_sel:{}", index, kSel[sdwa.src_sel[index] & 7]));
            }
        }
    } else {
        if (!omod.empty()) {
            suffix(omod);
        }
        if (v.clamp) {
            suffix("clamp");
        }
    }
    if (v.dpp) {
        if (const Dpp::Row* row = v.dpp->as<Dpp::Row>(); row != nullptr) {
            suffix(dpp_ctrl(row->ctrl));
            if (row->row_mask != 0xF) {
                suffix(std::format("row_mask:0x{:x}", row->row_mask));
            }
            if (row->bank_mask != 0xF) {
                suffix(std::format("bank_mask:0x{:x}", row->bank_mask));
            }
            if (row->bound_ctrl) {
                suffix("bound_ctrl:1");
            }
            if (row->fi) {
                suffix("fi:1");
            }
        } else if (const Dpp::Lanes* lanes = v.dpp->as<Dpp::Lanes>(); lanes != nullptr) {
            std::vector<std::string> selects;
            for (unsigned index = 0; index < 8; ++index) {
                selects.push_back(std::to_string((lanes->sel >> (3 * index)) & 7));
            }
            suffix(std::format("dpp8:[{}]", join(selects, ",")));
            if (lanes->fi) {
                suffix("fi:1");
            }
        }
    }
    return out;
}

std::string operands(const Instruction& inst, std::string_view name, const Fields& fields)
{
    std::string out;
    if (const auto* sop = fields.as<Fields::Sop>()) {
        std::vector<std::string> regs;
        if (sop->dst) {
            regs.push_back(to_string(*sop->dst));
        }
        for (const Operand& operand : sop->src) {
            regs.push_back(to_string(operand));
        }
        const std::string text = join(regs);
        if (!sop->imm) {
            return text;
        }
        const std::uint16_t imm = *sop->imm;
        if (name == "s_getreg_b32") {
            return std::format("{}, {}", text, hwreg(imm));
        }
        if (name == "s_setreg_b32" || name == "s_setreg_imm32_b32") {
            return std::format("{}, {}", hwreg(imm), text);
        }
        if (name == "s_call_b64") {
            return std::format("{}, {}", text, label(inst, imm));
        }
        if (text.empty()) {
            return std::format("0x{:04x}", imm);
        }
        return std::format("{}, 0x{:04x}", text, imm);
    }
    if (const auto* sopp = fields.as<Fields::Sopp>()) {
        if (name == "s_endpgm" || name == "s_barrier" || name == "s_code_end" || name == "s_endpgm_saved"
            || name == "s_endpgm_ordered_ps_done" || name == "s_wakeup" || name == "s_icache_inv"
            || name == "s_ttracedata") {
            return {};
        }
        if (name == "s_branch" || name.starts_with("s_cbranch_")) {
            return label(inst, sopp->imm);
        }
        if (name == "s_waitcnt") {
            return waitcnt(sopp->imm);
        }
        if (name == "s_sendmsg" || name == "s_sendmsghalt") {
            return sendmsg(sopp->imm);
        }
        return std::format("0x{:04x}", sopp->imm);
    }
    if (const auto* smem = fields.as<Fields::Smem>()) {
        if (smem->data) {
            const bool null_offset = smem->soffset.is_scalar(NULL_REG);
            std::string off;
            if (null_offset && smem->offset == 0) {
                off = to_string(smem->soffset);
            } else if (null_offset) {
                off = std::format("0x{:06x}", static_cast<std::uint32_t>(smem->offset) & 0x1FFFFF);
            } else if (smem->offset == 0) {
                off = to_string(smem->soffset);
            } else {
                off = std::format("{} offset:0x{:06x}", to_string(smem->soffset),
                                  static_cast<std::uint32_t>(smem->offset) & 0x1FFFFF);
            }
            out = std::format("{}, {}, {}", to_string(*smem->data), to_string(smem->base), off);
        }
        flags(out, smem->cache, false);
        return out;
    }
    if (const auto* valu = fields.as<Valu>()) {
        return valu_text(name, *valu, inst.encoding);
    }
    if (const auto* vintrp = fields.as<Fields::Vintrp>()) {
        std::string src;
        const Operand::Const* parameter = vintrp->src.as<Operand::Const>();
        if (name == "v_interp_mov_f32" && parameter != nullptr) {
            constexpr std::array<const char*, 3> kParameters{"p10", "p20", "p0"};
            src = parameter->code < kParameters.size() ? kParameters[parameter->code] : "p?";
        } else {
            src = to_string(vintrp->src);
        }
        return std::format("{}, {}, {}", to_string(vintrp->dst), src, to_string(vintrp->attr));
    }
    if (const auto* ds = fields.as<Fields::Ds>()) {
        std::vector<std::string> parts;
        if (ds->dst) {
            parts.push_back(to_string(*ds->dst));
        }
        if (ds->addr) {
            parts.push_back(to_string(*ds->addr));
        }
        for (const Operand& operand : ds->data) {
            parts.push_back(to_string(operand));
        }
        out = join(parts);
        if (ds_pair(name)) {
            if (ds->offset0 != 0) {
                out += std::format(" offset0:{}", ds->offset0);
            }
            if (ds->offset1 != 0) {
                out += std::format(" offset1:{}", ds->offset1);
            }
        } else {
            const unsigned offset = (static_cast<unsigned>(ds->offset1) << 8) | ds->offset0;
            if (offset != 0) {
                out += std::format(" offset:{}", offset);
            }
        }
        if (ds->gds) {
            out += " gds";
        }
        return out;
    }
    if (const auto* buffer = fields.as<Fields::Buffer>()) {
        if (buffer->data) {
            out = std::format("{}, {}, {}, {}", to_string(*buffer->data), to_string(buffer->addr),
                              to_string(buffer->rsrc), to_string(buffer->soffset));
            if (buffer->idxen) {
                out += " idxen";
            }
            if (buffer->offen) {
                out += " offen";
            }
            if (buffer->offset != 0) {
                out += std::format(" offset:{}", buffer->offset);
            }
            if (buffer->format) {
                out += std::format(" format:[{}]", buffer_format(*buffer->format));
            }
        }
        flags(out, buffer->cache, false);
        if (buffer->lds) {
            out += " lds";
        }
        if (buffer->tfe) {
            out += " tfe";
        }
        return out;
    }
    if (const auto* image = fields.as<Fields::Image>()) {
        std::string addr;
        if (image->addr.size() == 1) {
            addr = to_string(image->addr[0]);
        } else {
            std::vector<std::string> parts;
            for (const Operand& operand : image->addr) {
                parts.push_back(to_string(operand));
            }
            addr = std::format("[{}]", join(parts, ","));
        }
        std::vector<std::string> parts{to_string(image->data), addr, to_string(image->rsrc)};
        if (image->sampler) {
            parts.push_back(to_string(*image->sampler));
        }
        out = join(parts);
        if (image->dmask != 1) {
            out += std::format(" dmask:0x{:x}", image->dmask);
        }
        constexpr std::array<std::string_view, 8> kDims{
            "1D", "2D", "3D", "CUBE", "1D_ARRAY", "2D_ARRAY", "2D_MSAA", "2D_MSAA_ARRAY",
        };
        out += std::format(" dim:SQ_RSRC_IMG_{}", kDims[image->dim & 7]);
        if (image->r128) {
            out += " r128";
        }
        if (image->unorm) {
            out += " unorm";
        }
        flags(out, image->cache, true);
        if (image->a16) {
            out += " a16";
        }
        if (image->d16) {
            out += " d16";
        }
        if (image->tfe) {
            out += " tfe";
        }
        if (image->lwe) {
            out += " lwe";
        }
        return out;
    }
    if (const auto* flat = fields.as<Fields::Flat>()) {
        std::vector<std::string> parts;
        if (flat->dst) {
            parts.push_back(to_string(*flat->dst));
        }
        parts.push_back(flat->addr ? to_string(*flat->addr) : std::string{"off"});
        if (flat->data) {
            parts.push_back(to_string(*flat->data));
        }
        if (flat->saddr) {
            parts.push_back(to_string(*flat->saddr));
        }
        out = join(parts);
        if (flat->offset != 0) {
            out += std::format(" inst_offset:{}", flat->offset);
        }
        flags(out, flat->cache, false);
        if (flat->lds) {
            out += " lds";
        }
        return out;
    }
    if (const auto* exp = fields.as<Fields::Exp>()) {
        std::string target;
        if (exp->target <= 7) {
            target = std::format("mrt{}", exp->target);
        } else if (exp->target == 8) {
            target = "mrtz";
        } else if (exp->target == 9) {
            target = "null";
        } else if (exp->target >= 12 && exp->target <= 15) {
            target = std::format("pos{}", exp->target - 12);
        } else if (exp->target == 20) {
            target = "prim";
        } else if (exp->target >= 32 && exp->target <= 63) {
            target = std::format("param{}", exp->target - 32);
        } else {
            target = std::format("<target {}>", exp->target);
        }
        std::vector<std::string> parts{target};
        for (const auto& source : exp->src) {
            parts.push_back(source ? to_string(*source) : std::string{"off"});
        }
        out = join(parts);
        if (exp->done) {
            out += " done";
        }
        if (exp->compr) {
            out += " compr";
        }
        if (exp->vm) {
            out += " vm";
        }
        return out;
    }
    return out;
}

}

std::string scalar_name(std::uint8_t code)
{
    if (code <= 105) {
        return std::format("s{}", code);
    }
    switch (code) {
    case 106:
        return "vcc_lo";
    case 107:
        return "vcc_hi";
    case 124:
        return "m0";
    case 125:
        return "null";
    case 126:
        return "exec_lo";
    default:
        break;
    }
    if (code >= 108 && code <= 123) {
        return std::format("ttmp{}", code - 108);
    }
    return "exec_hi";
}

std::string const_name(std::uint8_t code)
{
    if (code >= 128 && code <= 192) {
        return std::to_string(static_cast<int>(code) - 128);
    }
    if (code >= 193 && code <= 208) {
        return std::to_string(192 - static_cast<int>(code));
    }
    switch (code) {
    case 240:
        return "0.5";
    case 241:
        return "-0.5";
    case 242:
        return "1.0";
    case 243:
        return "-1.0";
    case 244:
        return "2.0";
    case 245:
        return "-2.0";
    case 246:
        return "4.0";
    case 247:
        return "-4.0";
    case 248:
        return "0.15915494";
    case 235:
        return "src_shared_base";
    case 236:
        return "src_shared_limit";
    case 237:
        return "src_private_base";
    case 238:
        return "src_private_limit";
    case 239:
        return "src_pops_exiting_wave_id";
    case 251:
        return "src_vccz";
    case 252:
        return "src_execz";
    case 253:
        return "src_scc";
    case 254:
        return "src_lds_direct";
    default:
        return std::format("<src {}>", code);
    }
}

std::string to_string(const Operand& operand)
{
    if (const auto* s = operand.as<Operand::Scalar>()) {
        if (s->dwords == 1) {
            return scalar_name(s->code);
        }
        if (s->code == 106 && s->dwords == 2) {
            return "vcc";
        }
        if (s->code == 126 && s->dwords == 2) {
            return "exec";
        }
        if (s->code < 106) {
            return std::format("s[{}:{}]", s->code, static_cast<int>(s->code) + s->dwords - 1);
        }
        if (s->code >= 108 && s->code < 124) {
            return std::format("ttmp[{}:{}]", s->code - 108, s->code - 108 + s->dwords - 1);
        }
        return scalar_name(s->code);
    }
    if (const auto* v = operand.as<Operand::Vector>()) {
        if (v->dwords == 1) {
            return std::format("v{}", v->reg);
        }
        return std::format("v[{}:{}]", v->reg, static_cast<unsigned>(v->reg) + v->dwords - 1);
    }
    if (const auto* c = operand.as<Operand::Const>()) {
        return const_name(c->code);
    }
    if (const auto* l = operand.as<Operand::Literal>()) {
        return std::format("lit(0x{:08x})", l->value);
    }
    if (const auto* a = operand.as<Operand::Attr>()) {
        constexpr std::array<char, 4> kChannels{'x', 'y', 'z', 'w'};
        return std::format("attr{}.{}", a->index, kChannels[a->chan & 3]);
    }
    return {};
}

std::string Instruction::text(bool wave64) const
{
    const std::string_view op = name().value_or("?");
    const std::string ops = operands(*this, op, fields(wave64));
    if (ops.empty()) {
        return std::string{op};
    }
    return std::format("{} {}", op, ops);
}

}
