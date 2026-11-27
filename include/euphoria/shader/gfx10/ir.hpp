#pragma once

#include <euphoria/shader/gfx10/decode.hpp>
#include <euphoria/shader/gfx10/error.hpp>
#include <euphoria/shader/gfx10/fields.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::ir {

using Fields = ::euphoria::shader::gfx10::Fields;

struct Reg {
    enum class Kind : std::uint8_t {
        S,
        Scc,
        V,
    };

    Kind kind{Kind::S};
    std::uint8_t index{};

    static constexpr Reg S(std::uint8_t code) noexcept { return {Kind::S, code}; }
    static constexpr Reg Scc() noexcept { return {Kind::Scc, 0}; }
    static constexpr Reg V(std::uint8_t reg) noexcept { return {Kind::V, reg}; }

    constexpr bool is_s() const noexcept { return kind == Kind::S; }
    constexpr bool is_scc() const noexcept { return kind == Kind::Scc; }
    constexpr bool is_v() const noexcept { return kind == Kind::V; }

    friend constexpr auto operator<=>(const Reg&, const Reg&) = default;
    friend constexpr bool operator==(const Reg&, const Reg&) = default;
};

inline constexpr std::uint8_t VCC = 106;
inline constexpr std::uint8_t M0 = 124;
inline constexpr std::uint8_t EXEC = 126;
inline constexpr std::uint8_t PARKED = 128;

struct Value {
    std::uint32_t id{};

    friend constexpr auto operator<=>(const Value&, const Value&) = default;
    friend constexpr bool operator==(const Value&, const Value&) = default;
};

inline constexpr Value NONE{0xFFFFFFFFu};

struct Exit {
    enum class Kind : std::uint8_t {
        Jump,
        Branch,
        End,
        Indirect,
    };

    Kind kind{Kind::End};
    std::size_t target{};
    std::size_t taken{};
    std::size_t not_taken{};

    static constexpr Exit Jump(std::size_t block) noexcept { return {Kind::Jump, block, 0, 0}; }
    static constexpr Exit Branch(std::size_t taken, std::size_t not_taken) noexcept
    {
        return {Kind::Branch, 0, taken, not_taken};
    }
    static constexpr Exit End() noexcept { return {Kind::End, 0, 0, 0}; }
    static constexpr Exit Indirect() noexcept { return {Kind::Indirect, 0, 0, 0}; }

    std::vector<std::size_t> successors() const;

    friend constexpr bool operator==(const Exit&, const Exit&) = default;
};

struct Phi {
    Reg reg;
    Value value;
    std::vector<Value> args;

    friend bool operator==(const Phi&, const Phi&) = default;
};

struct Inst {
    Instruction machine;
    Fields fields;
    std::vector<std::pair<Reg, Value>> reads;
    std::vector<std::pair<Reg, Value>> writes;

    std::string_view name() const noexcept { return machine.name().value_or(""); }
    std::optional<Value> read(Reg reg) const noexcept;
    std::optional<Value> write(Reg reg) const noexcept;

    friend bool operator==(const Inst&, const Inst&) = default;
};

struct Block {
    std::size_t offset{};
    std::vector<std::size_t> preds;
    std::vector<Phi> phis;
    std::vector<Inst> insts;
    Exit exit;

    friend bool operator==(const Block&, const Block&) = default;
};

struct Program {
    bool wave64{};
    std::uint16_t vgprs{};
    std::vector<Block> blocks;
    std::vector<std::pair<Reg, Value>> inputs;

    friend bool operator==(const Program&, const Program&) = default;
};

std::expected<Program, ShaderError> build(std::span<const std::uint8_t> code, bool wave64, std::uint16_t vgprs);

std::expected<Program, ShaderError> thread_edges(const Program& p,
                                                 std::span<const std::tuple<std::size_t, std::size_t, std::size_t>> edges);

std::optional<Program> without_early_outs(const Program& p);

std::optional<Program> without_skips(const Program& p, bool deep);

std::vector<Reg> operand_reads(const Program& p, const Inst& inst);

std::vector<std::size_t> reverse_postorder(std::span<const Block> blocks);

std::optional<std::pair<std::size_t, std::size_t>> irreducible_edge(std::span<const Block> blocks,
                                                                    std::span<const std::size_t> order,
                                                                    std::span<const std::size_t> index);

std::vector<std::size_t> idoms(std::span<const Block> blocks, std::span<const std::size_t> order,
                               std::span<const std::size_t> index);

}

template <>
struct std::hash<euphoria::shader::gfx10::ir::Reg> {
    std::size_t operator()(const euphoria::shader::gfx10::ir::Reg& reg) const noexcept
    {
        return (static_cast<std::size_t>(reg.kind) << 8) | reg.index;
    }
};

template <>
struct std::hash<euphoria::shader::gfx10::ir::Value> {
    std::size_t operator()(const euphoria::shader::gfx10::ir::Value& value) const noexcept
    {
        return std::hash<std::uint32_t>{}(value.id);
    }
};
