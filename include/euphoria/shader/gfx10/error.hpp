#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace euphoria::shader::gfx10 {

struct ShaderError {
    enum class Kind : std::uint8_t {
        NotDxbc,
        Malformed,
        Isa,
        Unsupported,
    };

    Kind kind{Kind::NotDxbc};
    std::size_t offset{0};
    const char* detail{""};

    static constexpr ShaderError NotDxbc() noexcept { return {Kind::NotDxbc, 0, ""}; }
    static constexpr ShaderError Malformed(const char* what) noexcept { return {Kind::Malformed, 0, what}; }
    static constexpr ShaderError Isa(std::size_t offset, const char* reason) noexcept
    {
        return {Kind::Isa, offset, reason};
    }
    static constexpr ShaderError Unsupported(std::size_t offset, const char* what) noexcept
    {
        return {Kind::Unsupported, offset, what};
    }

    constexpr bool is(Kind other) const noexcept { return kind == other; }

    std::string message() const
    {
        switch (kind) {
        case Kind::NotDxbc:
            return "not a DXBC container";
        case Kind::Malformed:
            return std::format("malformed shader: {}", detail);
        case Kind::Isa:
            return std::format("GFX10 code at {:#x}: {}", offset, detail);
        case Kind::Unsupported:
            return std::format("GFX10 code at {:#x}: {} is not supported", offset, detail);
        }
        return {};
    }

    std::string what() const { return message(); }

    friend bool operator==(const ShaderError& left, const ShaderError& right) noexcept
    {
        return left.kind == right.kind && left.offset == right.offset
            && std::string_view{left.detail} == std::string_view{right.detail};
    }
};

}
