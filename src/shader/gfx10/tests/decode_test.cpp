#include <euphoria/shader/gfx10/decode.hpp>
#include <euphoria/shader/gfx10/opcodes.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <format>
#include <string>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

constexpr std::uint32_t kEndpgm = 0xBF810000u;

constexpr std::uint32_t sopp(std::uint32_t op, std::int16_t simm16)
{
    return 0xBF800000u | (op << 16) | static_cast<std::uint16_t>(simm16);
}

std::string summary(std::vector<std::uint32_t> words)
{
    const auto decoded = decode(bytes(words));
    if (!decoded) {
        return "error: " + decoded.error().message();
    }
    std::string out;
    for (const Instruction& inst : *decoded) {
        out += std::format("({},{},{})", inst.offset, inst.words.size(), inst.name().value_or("?"));
    }
    return out;
}

std::string offsets(std::vector<std::uint32_t> words, bool names)
{
    const auto decoded = decode(bytes(words));
    if (!decoded) {
        return "error: " + decoded.error().message();
    }
    std::string out;
    for (const Instruction& inst : *decoded) {
        if (names) {
            out += std::format("({},{})", inst.offset, inst.name().value_or("?"));
        } else {
            out += std::format("{} ", inst.offset);
        }
    }
    return out;
}

void tables_are_sorted(Context& context)
{
    const std::pair<const char*, OpcodeTable> tables[]{
        {"SOP2", opcodes::SOP2},   {"SOPK", opcodes::SOPK},     {"SOP1", opcodes::SOP1},
        {"SOPC", opcodes::SOPC},   {"SOPP", opcodes::SOPP},     {"SMEM", opcodes::SMEM},
        {"VOP2", opcodes::VOP2},   {"VOP1", opcodes::VOP1},     {"VOPC", opcodes::VOPC},
        {"VOP3", opcodes::VOP3},   {"VOP3P", opcodes::VOP3P},   {"VINTRP", opcodes::VINTRP},
        {"DS", opcodes::DS},       {"MUBUF", opcodes::MUBUF},   {"MTBUF", opcodes::MTBUF},
        {"MIMG", opcodes::MIMG},   {"FLAT", opcodes::FLAT},     {"GLOBAL", opcodes::GLOBAL},
        {"SCRATCH", opcodes::SCRATCH},
    };
    for (const auto& [label, table] : tables) {
        bool sorted = !table.empty();
        for (std::size_t index = 1; index < table.size(); ++index) {
            sorted = sorted && table[index - 1].opcode < table[index].opcode;
        }
        context.expect(sorted, std::format("table {} is sorted", label));
    }
}

void lengths_and_names(Context& context)
{
    context.expect_eq(summary({
                          0xBE8003FF, 0x3F800000,
                          0x06000000,
                          0xD54B0000, 0x03FE0000, 0x40000000,
                          0xF0800F0A, 0, 0,
                          0xF800080F, 0,
                          kEndpgm,
                      }),
                      "(0,2,s_mov_b32)(8,1,v_add_f32)(12,3,v_fma_f32)(24,3,image_sample)(36,2,exp)(44,1,s_endpgm)");
}

void follows_branches_and_skips_data(Context& context)
{
    context.expect_eq(offsets({sopp(0x04, 1), sopp(0x02, 1), kEndpgm, kEndpgm, kEndpgm, 0xFFFFFFFFu}, false),
                      "0 4 8 12 ");
}

void follows_far_jumps(Context& context)
{
    context.expect_eq(offsets({0xBE881F00, 0x8008FF08, 20, 0x82098009, 0xBE802008, 0xFFFFFFFFu, kEndpgm}, true),
                      "(0,s_getpc_b64)(4,s_add_u32)(12,s_addc_u32)(16,s_setpc_b64)(24,s_endpgm)");
    const auto handover = decode(bytes(std::vector<std::uint32_t>{0xBE802018, kEndpgm}));
    context.expect(handover && handover->size() == 1, "a handover through other registers ends the path");
}

void follows_far_jumps_backward(Context& context)
{
    context.expect_eq(offsets({sopp(0x02, 1), kEndpgm, 0xBE881F00, 0x8008FF08, 0xFFFFFFF8u, 0x8209C109, 0xBE802008},
                              true),
                      "(0,s_branch)(4,s_endpgm)(8,s_getpc_b64)(12,s_add_u32)(20,s_addc_u32)(24,s_setpc_b64)");
}

void rejects_bad_code(Context& context)
{
    const std::vector<std::uint8_t> three(3, 0);
    context.expect(!decode(three), "size not a multiple of 4");
    context.expect(!decode(bytes(std::vector<std::uint32_t>{sopp(0x02, 100)})), "branch out of range");
    context.expect(!decode(bytes(std::vector<std::uint32_t>{0xBE8003FF})), "literal past the end");
    context.expect(!decode(bytes(std::vector<std::uint32_t>{0xE4000000, 0})), "unknown encoding");
    context.expect(!decode(bytes(std::vector<std::uint32_t>{sopp(0x04, 1), 0xBE8003FF, kEndpgm, kEndpgm})),
                   "branch into a literal");
}

}

void register_decode_tests()
{
    register_case("decode", "tables_are_sorted", tables_are_sorted);
    register_case("decode", "lengths_and_names", lengths_and_names);
    register_case("decode", "follows_branches_and_skips_data", follows_branches_and_skips_data);
    register_case("decode", "follows_far_jumps", follows_far_jumps);
    register_case("decode", "follows_far_jumps_backward", follows_far_jumps_backward);
    register_case("decode", "rejects_bad_code", rejects_bad_code);
}

}
