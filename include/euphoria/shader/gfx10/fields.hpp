#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace euphoria::shader::gfx10 {

struct Operand {
    struct Scalar {
        std::uint8_t code{};
        std::uint8_t dwords{};
        friend bool operator==(const Scalar&, const Scalar&) = default;
    };
    struct Vector {
        std::uint8_t reg{};
        std::uint8_t dwords{};
        friend bool operator==(const Vector&, const Vector&) = default;
    };
    struct Const {
        std::uint8_t code{};
        friend bool operator==(const Const&, const Const&) = default;
    };
    struct Literal {
        std::uint32_t value{};
        friend bool operator==(const Literal&, const Literal&) = default;
    };
    struct Attr {
        std::uint8_t index{};
        std::uint8_t chan{};
        bool high{};
        friend bool operator==(const Attr&, const Attr&) = default;
    };

    using Variant = std::variant<Scalar, Vector, Const, Literal, Attr>;
    Variant value;

    constexpr Operand() noexcept : value(Scalar{}) {}
    constexpr Operand(Scalar operand) noexcept : value(operand) {}
    constexpr Operand(Vector operand) noexcept : value(operand) {}
    constexpr Operand(Const operand) noexcept : value(operand) {}
    constexpr Operand(Literal operand) noexcept : value(operand) {}
    constexpr Operand(Attr operand) noexcept : value(operand) {}

    template <typename T>
    constexpr bool is() const noexcept
    {
        return std::holds_alternative<T>(value);
    }

    template <typename T>
    constexpr const T* as() const noexcept
    {
        return std::get_if<T>(&value);
    }

    template <typename T>
    constexpr T* as() noexcept
    {
        return std::get_if<T>(&value);
    }

    constexpr bool is_scalar(std::uint8_t code) const noexcept
    {
        const Scalar* scalar = as<Scalar>();
        return scalar != nullptr && scalar->code == code;
    }

    friend bool operator==(const Operand&, const Operand&) = default;
};

inline constexpr std::uint8_t NULL_REG = 125;

struct Cache {
    bool glc{};
    bool slc{};
    bool dlc{};
    friend bool operator==(const Cache&, const Cache&) = default;
};

struct Sdwa {
    std::uint8_t dst_sel{};
    std::uint8_t dst_unused{};
    std::array<std::uint8_t, 2> src_sel{};
    std::uint8_t sext{};
    friend bool operator==(const Sdwa&, const Sdwa&) = default;
};

struct Dpp {
    struct Row {
        std::uint16_t ctrl{};
        std::uint8_t row_mask{};
        std::uint8_t bank_mask{};
        bool bound_ctrl{};
        bool fi{};
        friend bool operator==(const Row&, const Row&) = default;
    };
    struct Lanes {
        std::uint32_t sel{};
        bool fi{};
        friend bool operator==(const Lanes&, const Lanes&) = default;
    };

    using Variant = std::variant<Row, Lanes>;
    Variant value;

    constexpr Dpp(Row dpp) noexcept : value(dpp) {}
    constexpr Dpp(Lanes dpp) noexcept : value(dpp) {}

    template <typename T>
    constexpr bool is() const noexcept
    {
        return std::holds_alternative<T>(value);
    }

    template <typename T>
    constexpr const T* as() const noexcept
    {
        return std::get_if<T>(&value);
    }

    friend bool operator==(const Dpp&, const Dpp&) = default;
};

struct Valu {
    std::optional<Operand> dst;
    std::optional<Operand> carry;
    std::vector<Operand> src;
    std::uint8_t neg{};
    std::uint8_t abs{};
    std::uint8_t neg_hi{};
    std::uint8_t opsel{};
    std::uint8_t opsel_hi{};
    bool clamp{};
    std::uint8_t omod{};
    std::optional<Sdwa> sdwa;
    std::optional<Dpp> dpp;
    friend bool operator==(const Valu&, const Valu&) = default;
};

struct Fields {
    using Valu = ::euphoria::shader::gfx10::Valu;

    struct Sop {
        std::optional<Operand> dst;
        std::vector<Operand> src;
        std::optional<std::uint16_t> imm;
        friend bool operator==(const Sop&, const Sop&) = default;
    };
    struct Sopp {
        std::uint16_t imm{};
        friend bool operator==(const Sopp&, const Sopp&) = default;
    };
    struct Smem {
        std::optional<Operand> data;
        Operand base;
        std::int32_t offset{};
        Operand soffset;
        Cache cache;
        friend bool operator==(const Smem&, const Smem&) = default;
    };
    struct Vintrp {
        Operand dst;
        Operand src;
        Operand attr;
        friend bool operator==(const Vintrp&, const Vintrp&) = default;
    };
    struct Ds {
        std::optional<Operand> dst;
        std::optional<Operand> addr;
        std::vector<Operand> data;
        std::uint8_t offset0{};
        std::uint8_t offset1{};
        bool gds{};
        friend bool operator==(const Ds&, const Ds&) = default;
    };
    struct Buffer {
        std::optional<Operand> data;
        Operand addr;
        Operand rsrc;
        Operand soffset;
        std::uint16_t offset{};
        bool offen{};
        bool idxen{};
        std::optional<std::uint8_t> format;
        bool lds{};
        bool tfe{};
        Cache cache;
        friend bool operator==(const Buffer&, const Buffer&) = default;
    };
    struct Image {
        Operand data;
        std::vector<Operand> addr;
        Operand rsrc;
        std::optional<Operand> sampler;
        std::uint8_t dmask{};
        std::uint8_t dim{};
        bool unorm{};
        bool r128{};
        bool a16{};
        bool d16{};
        bool tfe{};
        bool lwe{};
        Cache cache;
        friend bool operator==(const Image&, const Image&) = default;
    };
    struct Flat {
        std::optional<Operand> dst;
        std::optional<Operand> addr;
        std::optional<Operand> data;
        std::optional<Operand> saddr;
        std::int16_t offset{};
        bool lds{};
        Cache cache;
        friend bool operator==(const Flat&, const Flat&) = default;
    };
    struct Exp {
        std::uint8_t target{};
        std::array<std::optional<Operand>, 4> src;
        bool compr{};
        bool done{};
        bool vm{};
        friend bool operator==(const Exp&, const Exp&) = default;
    };

    using Variant = std::variant<Sop, Sopp, Smem, Valu, Vintrp, Ds, Buffer, Image, Flat, Exp>;
    Variant value;

    Fields() : value(Sopp{}) {}
    Fields(Sop fields) : value(std::move(fields)) {}
    Fields(Sopp fields) : value(fields) {}
    Fields(Smem fields) : value(std::move(fields)) {}
    Fields(Valu fields) : value(std::move(fields)) {}
    Fields(Vintrp fields) : value(std::move(fields)) {}
    Fields(Ds fields) : value(std::move(fields)) {}
    Fields(Buffer fields) : value(std::move(fields)) {}
    Fields(Image fields) : value(std::move(fields)) {}
    Fields(Flat fields) : value(std::move(fields)) {}
    Fields(Exp fields) : value(std::move(fields)) {}

    template <typename T>
    bool is() const noexcept
    {
        return std::holds_alternative<T>(value);
    }

    template <typename T>
    const T* as() const noexcept
    {
        return std::get_if<T>(&value);
    }

    template <typename T>
    T* as() noexcept
    {
        return std::get_if<T>(&value);
    }

    friend bool operator==(const Fields&, const Fields&) = default;
};

bool ds_pair(std::string_view name);

}
