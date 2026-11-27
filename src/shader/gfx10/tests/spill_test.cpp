#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/native.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>
#include <euphoria/shader/gfx10/spill.hpp>

#include <algorithm>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

using ir::Reg;

RootParam param(RootType root, std::uint8_t user_data, std::uint32_t sgprs, std::uint8_t slot)
{
    return RootParam{root, 0, sgprs, 0, user_data, slot};
}

NativeProgram native(const std::vector<std::uint32_t>& words, std::uint32_t registers, std::vector<RootParam> layout)
{
    NativeProgram n;
    n.stage = HwStage::Cs;
    n.wave64 = false;
    n.vgprs = 256;
    n.layout = std::move(layout);
    n.io = ComputeIo{registers << 1 | 1u << 7, {1, 1, 1}};
    n.code = bytes(words);
    return n;
}

const std::vector<std::uint32_t> kTable{
    0x8703FF01, 0x000000FF, 0xBE820300, 0xF4000141, 0xFA000010, 0xF4040001, 0xFA000000, 0xBF810000,
};

std::size_t loads(const ir::Program& p)
{
    std::size_t count = 0;
    for (const auto& block : p.blocks) {
        for (const auto& inst : block.insts) {
            if (inst.name().starts_with("s_load_")) {
                ++count;
            }
        }
    }
    return count;
}

void loads_of_the_spilled_table_become_entry_registers(Context& context)
{
    const NativeProgram n = native(kTable, 4,
                                   {
                                       param(RootType::Descriptor, 16, 2, 0),
                                       param(RootType::Descriptor, 18, 2, 1),
                                       param(RootType::Constants, 20, 16, 2),
                                   });
    auto p = ir::build(n.code, false, 256);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    context.expect(spill::is_spilled(n), "the program is spilled");
    context.expect(spill::user_data(*p, n) == 2, "two loads replaced");
    context.expect(loads(*p) == 0, "no loads left");
    std::vector<Reg> regs;
    for (const auto& [reg, v] : p->inputs) {
        regs.push_back(reg);
    }
    std::ranges::sort(regs);
    context.expect(regs == std::vector<Reg>{Reg::S(16), Reg::S(17), Reg::S(20)}, "entry registers s16, s17, s20");
}

void parameters_in_registers_are_left_alone(Context& context)
{
    const NativeProgram n = native(kTable, 6, {param(RootType::Descriptor, 0, 2, 0)});
    auto p = ir::build(n.code, false, 256);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    const ir::Program before = *p;
    context.expect(!spill::is_spilled(n), "not spilled");
    context.expect(spill::user_data(*p, n) == 0, "nothing replaced");
    context.expect(*p == before, "program unchanged");
}

void a_used_dword_no_parameter_declares_keeps_its_load(Context& context)
{
    std::vector<std::uint32_t> words = kTable;
    words.insert(words.begin() + 7, 0xBE860305);
    const NativeProgram n = native(words, 4, {param(RootType::Descriptor, 16, 2, 0)});
    auto p = ir::build(n.code, false, 256);
    if (!context.expect(p.has_value(), "build")) {
        return;
    }
    context.expect(spill::user_data(*p, n) == 1, "one load replaced");
    context.expect(loads(*p) == 1, "one load stays");
    context.expect(std::ranges::any_of(p->inputs, [](const auto& i) { return i.first == Reg::S(0); }),
                   "the pointer is still read");
}

void build_rewrites_the_spilled_table(Context& context)
{
    const NativeProgram n = native(kTable, 4,
                                   {
                                       param(RootType::Descriptor, 16, 2, 0),
                                       param(RootType::Descriptor, 18, 2, 1),
                                       param(RootType::Constants, 20, 16, 2),
                                   });
    const auto p = spill::build(n);
    context.expect(p.has_value() && loads(*p) == 0, "spill::build drops the table loads");
}

}

void register_spill_tests()
{
    register_case("spill", "loads_of_the_spilled_table_become_entry_registers",
                  loads_of_the_spilled_table_become_entry_registers);
    register_case("spill", "parameters_in_registers_are_left_alone", parameters_in_registers_are_left_alone);
    register_case("spill", "a_used_dword_no_parameter_declares_keeps_its_load",
                  a_used_dword_no_parameter_declares_keeps_its_load);
    register_case("spill", "build_rewrites_the_spilled_table", build_rewrites_the_spilled_table);
}

}
