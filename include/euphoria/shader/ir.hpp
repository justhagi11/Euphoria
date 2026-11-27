#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace euphoria::shader {

enum class Format : std::uint8_t {
    Unknown,
    Sop1,
    Sop2,
    Sopk,
    Sopp,
    Vop1,
    Vop2,
    Vop3,
    Smem,
    Mubuf,
    Mtbuf,
    Exp,
};

enum class Stage : std::uint8_t {
    Unknown,
    Vertex,
    Pixel,
    Compute,
    Mesh,
    Hull,
    Domain,
};

enum class Opcode : std::uint32_t {
    Unknown,
    EndProgram,
    Nop,
    Branch,
    ConditionalBranch,
    Move,
    Add,
    Subtract,
    Multiply,
    MultiplyAdd,
    Min,
    Max,
    And,
    Or,
    Xor,
    Not,
    ShiftLeft,
    ShiftRight,
    Compare,
    Export,
    Load,
    Store,
    Sample,
    Atom,
};

enum class OperandKind : std::uint8_t {
    None,
    ScalarRegister,
    VectorRegister,
    Literal,
    Immediate,
    Special,
};

struct Operand {
    OperandKind kind{OperandKind::None};
    std::uint32_t value{};
    std::int32_t offset{};
};

struct Instruction {
    std::uint64_t encoding{};
    std::uint32_t size{};
    std::uint16_t raw_opcode{};
    Format format{Format::Unknown};
    Opcode opcode{Opcode::Unknown};
    std::array<Operand, 4> operands{};
};

struct Block {
    std::vector<Instruction> instructions;
    std::uint32_t target{};
};

struct Program {
    Stage stage{Stage::Unknown};
    std::vector<Block> blocks;
    std::vector<std::uint32_t> inputs;
    std::vector<std::uint32_t> outputs;
};

}
