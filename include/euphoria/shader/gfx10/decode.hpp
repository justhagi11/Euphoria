#pragma once

#include <euphoria/shader/gfx10/error.hpp>
#include <euphoria/shader/gfx10/fields.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace euphoria::shader::gfx10 {

enum class Encoding : std::uint8_t {
    Sop2,
    Sopk,
    Sop1,
    Sopc,
    Sopp,
    Smem,
    Vop2,
    Vop1,
    Vopc,
    Vop3,
    Vop3p,
    Vintrp,
    Ds,
    Mubuf,
    Mtbuf,
    Mimg,
    Exp,
    Flat,
    Global,
    Scratch,
};

const char* encoding_name(Encoding encoding) noexcept;

std::optional<std::string_view> name(Encoding encoding, std::uint16_t opcode) noexcept;

struct Instruction {
    std::size_t offset{};
    Encoding encoding{};
    std::uint16_t opcode{};
    std::vector<std::uint32_t> words;

    std::optional<std::string_view> name() const noexcept { return gfx10::name(encoding, opcode); }
    std::size_t end() const noexcept { return offset + 4 * words.size(); }
    Fields fields(bool wave64) const;
    std::string text(bool wave64) const;

    friend bool operator==(const Instruction&, const Instruction&) = default;
};

std::expected<std::vector<Instruction>, ShaderError> decode(std::span<const std::uint8_t> code);

struct Flow {
    enum class Kind : std::uint8_t {
        Next,
        Stop,
        Jump,
        Branch,
    };

    Kind kind{Kind::Next};
    std::size_t target{};

    static constexpr Flow Next() noexcept { return {Kind::Next, 0}; }
    static constexpr Flow Stop() noexcept { return {Kind::Stop, 0}; }
    static constexpr Flow Jump(std::size_t target) noexcept { return {Kind::Jump, target}; }
    static constexpr Flow Branch(std::size_t target) noexcept { return {Kind::Branch, target}; }

    constexpr bool has_target() const noexcept { return kind == Kind::Jump || kind == Kind::Branch; }

    friend constexpr bool operator==(const Flow&, const Flow&) = default;
};

using Before = std::function<const Instruction*(std::size_t)>;

std::expected<Flow, ShaderError> control(const Instruction& inst, std::size_t len, const Before& before);

}
