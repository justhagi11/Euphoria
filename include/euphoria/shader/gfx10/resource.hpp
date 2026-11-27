#pragma once

#include <euphoria/shader/gfx10/error.hpp>
#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace euphoria::shader::gfx10::structure {
struct Structured;
}

namespace euphoria::shader::gfx10::resource {

using Site = std::pair<std::size_t, std::size_t>;

struct Address {
    std::size_t param{};
    std::uint32_t offset{};
    std::optional<ir::Value> dynamic;

    friend bool operator==(const Address&, const Address&) = default;
};

inline constexpr std::size_t CODE = static_cast<std::size_t>(-1);
inline constexpr std::size_t MEMORY = static_cast<std::size_t>(-1) - 1;

struct Source {
    struct Loaded {
        Site load;
        std::uint8_t first{};
        friend bool operator==(const Loaded&, const Loaded&) = default;
    };
    struct Built {
        Address address;
        std::uint32_t bits{};
        std::array<std::uint32_t, 2> words{};
        friend bool operator==(const Built&, const Built&) = default;
    };
    struct Constant {
        std::vector<std::uint32_t> words;
        friend bool operator==(const Constant&, const Constant&) = default;
    };
    struct Phi {
        std::size_t block{};
        std::vector<Source> args;
        friend bool operator==(const Phi& a, const Phi& b);
    };
    struct Select {
        ir::Value scc;
        std::vector<Source> args;
        friend bool operator==(const Select& a, const Select& b);
    };
    struct Fetched {
        Site load;
        std::uint8_t first{};
        friend bool operator==(const Fetched&, const Fetched&) = default;
    };
    struct Lanes {
        ir::Value mask;
        std::vector<Source> args;
        friend bool operator==(const Lanes& a, const Lanes& b);
    };
    struct LanePhi {
        std::size_t block{};
        std::vector<std::optional<Source>> args;
        friend bool operator==(const LanePhi& a, const LanePhi& b);
    };

    using Variant = std::variant<Loaded, Built, Constant, Phi, Select, Fetched, Lanes, LanePhi>;
    Variant value;

    Source() : value(Constant{}) {}
    Source(Loaded source) : value(source) {}
    Source(Built source) : value(source) {}
    Source(Constant source) : value(std::move(source)) {}
    Source(Phi source) : value(std::move(source)) {}
    Source(Select source) : value(std::move(source)) {}
    Source(Fetched source) : value(source) {}
    Source(Lanes source) : value(std::move(source)) {}
    Source(LanePhi source) : value(std::move(source)) {}

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

    std::vector<const Source*> leaves() const;

    friend bool operator==(const Source& a, const Source& b);
};

std::string to_string(const Source& source);

enum class Role : std::uint8_t {
    Resource,
    Sampler,
};

struct Descriptor {
    Site site;
    Role role{Role::Resource};
    Source source;

    friend bool operator==(const Descriptor&, const Descriptor&) = default;
};

struct RootIndexedView {
    std::uint8_t table_slot{};
    std::uint8_t constant_slot{};
    std::uint32_t constant_dword{};
    std::uint32_t byte_offset{};
    std::uint8_t shift{};

    friend constexpr bool operator==(const RootIndexedView&, const RootIndexedView&) = default;
};

struct BvhNodes {
    enum class Kind : std::uint8_t {
        Resource,
        Memory,
    };

    Kind kind{Kind::Resource};
    std::uint32_t word1{};
    std::uint32_t word3{};

    static constexpr BvhNodes Resource() noexcept { return {Kind::Resource, 0, 0}; }
    static constexpr BvhNodes Memory(std::uint32_t word1, std::uint32_t word3) noexcept
    {
        return {Kind::Memory, word1, word3};
    }

    constexpr bool is(Kind other) const noexcept { return kind == other; }

    friend constexpr bool operator==(const BvhNodes&, const BvhNodes&) = default;
};

struct Waterfall {
    std::vector<Site> compares;
    std::vector<ir::Value> reads;
    std::vector<Site> loads;

    friend bool operator==(const Waterfall&, const Waterfall&) = default;
};

struct Resources {
    std::vector<std::pair<Site, Address>> scalar;
    std::vector<Descriptor> descriptors;
    std::vector<RootIndexedView> root_indexed_views;
    std::vector<std::tuple<ir::Value, std::size_t, std::uint32_t>> params;
    std::vector<Site> lane_copied;
    Waterfall waterfall;
    std::vector<std::pair<Site, BvhNodes>> bvh;

    friend bool operator==(const Resources&, const Resources&) = default;
};

std::expected<Resources, ShaderError> track(const ir::Program& p, std::span<const RootParam> layout, HwStage stage,
                                            const structure::Structured* lanes);

std::optional<std::uint32_t> inline_const(std::uint8_t code) noexcept;

namespace tests {

extern const std::span<const std::uint32_t> WATERFALL_FETCHED_WAVE32;
extern const std::span<const std::uint32_t> WATERFALL_FETCHED_WAVE64;
extern const std::span<const std::uint32_t> WATERFALL_PICKED_WAVE32;
extern const std::span<const std::uint32_t> WATERFALL_JOINED_WAVE32;
extern const std::span<const std::uint32_t> WATERFALL_INDEXED_WAVE32;
extern const std::span<const std::uint32_t> WATERFALL_JOINED_MIXED_WAVE32;
extern const std::span<const std::uint32_t> WATERFALL_LOOPED_WAVE32;
extern const std::span<const std::uint32_t> WATERFALL_JOINED_BUFFER_WAVE32;

std::vector<std::uint32_t> cmpx_waterfall(bool wave64, bool folded);

}

}
