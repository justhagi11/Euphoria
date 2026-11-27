#include <euphoria/shader/gfx10/decode.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>
#include <euphoria/shader/gfx10/sp3.hpp>

#include <string>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

std::vector<std::string> texts(std::vector<std::uint32_t> words, bool wave64)
{
    words.push_back(0xBF810000u);
    const auto decoded = decode(bytes(words));
    if (!decoded) {
        return {"error: " + decoded.error().message()};
    }
    std::vector<std::string> out;
    for (std::size_t index = 0; index + 1 < decoded->size(); ++index) {
        out.push_back((*decoded)[index].text(wave64));
    }
    return out;
}

std::string one(std::vector<std::uint32_t> words, bool wave64)
{
    const std::vector<std::string> all = texts(std::move(words), wave64);
    if (all.size() != 1) {
        std::string joined = "not one instruction:";
        for (const std::string& text : all) {
            joined += " [" + text + "]";
        }
        return joined;
    }
    return all[0];
}

void scalar(Context& context)
{
    const auto all = texts({0xBF880001, 0xBF8C0070, 0xBE8003FF, 0x12345678}, true);
    if (context.expect(all.size() == 3, "three instructions")) {
        context.expect_eq(all[0], "s_cbranch_execz label_0008");
        context.expect_eq(all[1], "s_waitcnt vmcnt(0) & lgkmcnt(0)");
        context.expect_eq(all[2], "s_mov_b32 s0, lit(0x12345678)");
    }
    context.expect_eq(one({0x8782027E}, true), "s_and_b64 s[2:3], exec, s[2:3]");
    context.expect_eq(one({0xF4080501, 0xFA000040}, true), "s_load_dwordx4 s[20:23], s[2:3], 0x000040");
    context.expect_eq(one({0xF4200144, 0x04000000}, true), "s_buffer_load_dword s5, s[8:11], s2");
}

void lane_masks_follow_the_wave_size(Context& context)
{
    context.expect_eq(one({0x7C0806F0}, false), "v_cmp_gt_f32 vcc_lo, 0.5, v3");
    context.expect_eq(one({0x7C0806F0}, true), "v_cmp_gt_f32 vcc, 0.5, v3");
    context.expect_eq(one({0xD4010106, 0x0001E501}, false), "v_cmp_lt_f32 s6, abs(v1), 1.0");
    context.expect_eq(one({0xD4010106, 0x0001E501}, true), "v_cmp_lt_f32 s[6:7], abs(v1), 1.0");
    context.expect_eq(one({0x02020480}, false), "v_cndmask_b32 v1, 0, v2, vcc_lo");
    context.expect_eq(one({0x50060801}, true), "v_add_co_ci_u32 v3, vcc, s1, v4, vcc");
}

void vector_modifiers(Context& context)
{
    context.expect_eq(one({0x10040AFF, 0x3F000001}, true), "v_mul_f32 v2, lit(0x3f000001), v5");
    context.expect_eq(one({0xD54B8004, 0x2BC00501}, true), "v_fma_f32 v4, -v1, s2, 0.5 mul:2 clamp");
    context.expect_eq(one({0x7E0216F9, 0x06050602}, true), "v_cvt_f32_f16 v1, v2 src0_sel:WORD_1");
    context.expect_eq(one({0x7E0602E9, 0x05397701}, true), "v_mov_b32 v3, v1 dpp8:[7,6,5,4,3,2,1,0]");
    context.expect_eq(one({0x7E0A02FA, 0xFF08B106}, true), "v_mov_b32 v5, v6 quad_perm:[1,0,3,2] bound_ctrl:1");
    context.expect_eq(one({0xCC200C01, 0x2C100702}, true), "v_fma_mix_f32 v1, -sel_hi(v2), s3, abs(v4)");
}

void memory_and_export(Context& context)
{
    context.expect_eq(one({0xE0347010, 0x01020402}, true),
                      "buffer_load_dwordx2 v[4:5], v[2:3], s[8:11], s1 idxen offen offset:16 glc");
    context.expect_eq(one({0xD8DC0302, 0x06000001}, true), "ds_read2_b32 v[6:7], v1 offset0:2 offset1:3");
    context.expect_eq(one({0xF8001C0F, 0x00000302}, true), "exp mrt0, v2, v2, v3, v3 done compr vm");
}

void image_d16_data_is_packed(Context& context)
{
    const std::vector<std::uint32_t> words{0xF09C070A, 0x80240801, 0x00000005};
    context.expect_eq(one(words, true),
                      "image_sample_lz v[8:9], [v1,v5], s[16:23], s[4:7] dmask:0x7 dim:SQ_RSRC_IMG_2D d16");
    std::vector<std::uint32_t> program = words;
    program.push_back(0xBF810000u);
    const auto decoded = decode(bytes(program));
    if (!context.expect(decoded.has_value(), "decodes")) {
        return;
    }
    const Fields fields = (*decoded)[0].fields(true);
    const auto* image = fields.as<Fields::Image>();
    if (!context.expect(image != nullptr, "an image instruction")) {
        return;
    }
    context.expect(image->data == Operand{Operand::Vector{8, 2}}, "data is v[8:9]");
    context.expect(image->addr.size() == 2, "two NSA addresses");
}

}

void register_sp3_tests()
{
    register_case("sp3", "scalar", scalar);
    register_case("sp3", "lane_masks_follow_the_wave_size", lane_masks_follow_the_wave_size);
    register_case("sp3", "vector_modifiers", vector_modifiers);
    register_case("sp3", "memory_and_export", memory_and_export);
    register_case("sp3", "image_d16_data_is_packed", image_d16_data_is_packed);
}

}
