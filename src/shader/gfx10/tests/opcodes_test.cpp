#include <euphoria/shader/gfx10/decode.hpp>
#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

void s_ttracedata_imm_is_sopp_0x28(Context& context)
{
    const std::vector<std::uint8_t> code = bytes(std::vector<std::uint32_t>{0xBFA81234, 0xBF810000});
    const auto decoded = decode(code);
    if (!context.expect(decoded.has_value(), "decodes")) {
        return;
    }
    context.expect((*decoded)[0].name() == "s_ttracedata_imm", "named s_ttracedata_imm");
    context.expect(ir::build(code, false, 1).has_value(), "the IR drops it");
}

}

void register_opcodes_tests()
{
    register_case("opcodes", "s_ttracedata_imm_is_sopp_0x28", s_ttracedata_imm_is_sopp_0x28);
}

}
