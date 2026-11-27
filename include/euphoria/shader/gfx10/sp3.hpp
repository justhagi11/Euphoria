#pragma once

#include <euphoria/shader/gfx10/decode.hpp>
#include <euphoria/shader/gfx10/fields.hpp>

#include <format>
#include <string>

namespace euphoria::shader::gfx10 {

std::string to_string(const Operand& operand);
std::string scalar_name(std::uint8_t code);
std::string const_name(std::uint8_t code);

}

template <>
struct std::formatter<euphoria::shader::gfx10::Operand> : std::formatter<std::string> {
    auto format(const euphoria::shader::gfx10::Operand& operand, std::format_context& context) const
    {
        return std::formatter<std::string>::format(euphoria::shader::gfx10::to_string(operand), context);
    }
};
