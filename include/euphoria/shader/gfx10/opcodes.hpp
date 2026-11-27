#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace euphoria::shader::gfx10 {

struct OpcodeName {
    std::uint16_t opcode;
    std::string_view name;
};

using OpcodeTable = std::span<const OpcodeName>;

namespace opcodes {

extern const OpcodeTable SOP2;
extern const OpcodeTable SOPK;
extern const OpcodeTable SOP1;
extern const OpcodeTable SOPC;
extern const OpcodeTable SOPP;
extern const OpcodeTable SMEM;
extern const OpcodeTable VOP2;
extern const OpcodeTable VOP1;
extern const OpcodeTable VOPC;
extern const OpcodeTable VOP3;
extern const OpcodeTable VOP3P;
extern const OpcodeTable VINTRP;
extern const OpcodeTable DS;
extern const OpcodeTable MUBUF;
extern const OpcodeTable MTBUF;
extern const OpcodeTable MIMG;
extern const OpcodeTable FLAT;
extern const OpcodeTable GLOBAL;
extern const OpcodeTable SCRATCH;

std::optional<std::string_view> lookup(OpcodeTable table, std::uint16_t opcode) noexcept;

}

}
