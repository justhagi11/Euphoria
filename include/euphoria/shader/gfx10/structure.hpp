#pragma once

#include <euphoria/shader/gfx10/error.hpp>
#include <euphoria/shader/gfx10/fields.hpp>
#include <euphoria/shader/gfx10/ir.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::structure {

inline constexpr std::uint32_t TERMINAL = 0xFFFFFFFFu;
inline constexpr std::size_t MAX_NODES = std::size_t{1} << 21;
inline constexpr std::size_t MAX_MEET = std::size_t{1} << 16;
inline constexpr std::size_t MAX_MEETS = std::size_t{1} << 18;

struct Bool {
    std::uint32_t id{};

    static const Bool False;
    static const Bool True;

    friend constexpr auto operator<=>(const Bool&, const Bool&) = default;
    friend constexpr bool operator==(const Bool&, const Bool&) = default;
};

inline const Bool Bool::False{0};
inline const Bool Bool::True{1};

struct Atom {
    enum class Kind : std::uint8_t {
        Lane,
        Uniform,
        Phi,
        Quad,
        Opaque,
        Constant,
    };

    Kind kind{Kind::Lane};
    ir::Value value{};
    std::uint64_t constant{};

    static constexpr Atom Lane(ir::Value v) noexcept { return {Kind::Lane, v, 0}; }
    static constexpr Atom Uniform(ir::Value v) noexcept { return {Kind::Uniform, v, 0}; }
    static constexpr Atom Phi(ir::Value v) noexcept { return {Kind::Phi, v, 0}; }
    static constexpr Atom Quad(ir::Value v) noexcept { return {Kind::Quad, v, 0}; }
    static constexpr Atom Opaque(ir::Value v) noexcept { return {Kind::Opaque, v, 0}; }
    static constexpr Atom Constant(std::uint64_t v) noexcept { return {Kind::Constant, {}, v}; }

    friend constexpr bool operator==(const Atom&, const Atom&) = default;
};

struct AtomHash {
    std::size_t operator()(const Atom& atom) const noexcept
    {
        const std::size_t kind = static_cast<std::size_t>(atom.kind);
        const std::size_t payload = atom.kind == Atom::Kind::Constant
                                        ? static_cast<std::size_t>(atom.constant)
                                        : static_cast<std::size_t>(atom.value.id);
        return (kind * 1000003u) ^ payload;
    }
};

struct MaskKey {
    ir::Value low{};
    ir::Value high{};

    friend constexpr bool operator==(const MaskKey&, const MaskKey&) = default;
};

struct MaskKeyHash {
    std::size_t operator()(const MaskKey& key) const noexcept
    {
        return (static_cast<std::size_t>(key.low.id) * 1000003u) ^ key.high.id;
    }
};

struct VectorHash {
    std::size_t operator()(const std::vector<std::uint32_t>& values) const noexcept
    {
        std::size_t hash = 1469598103934665603ull;
        for (const std::uint32_t value : values) {
            hash = (hash ^ value) * 1099511628211ull;
        }
        return hash;
    }
};

struct Meets {
    std::unordered_map<std::vector<std::uint32_t>, std::size_t, VectorHash> none;
};

class Bools {
public:
    Bools();

    std::optional<std::tuple<Atom, Bool, Bool>> node(Bool b) const;
    std::vector<Atom> support(Bool b) const;
    std::uint32_t top(Bool b) const;
    std::pair<Bool, Bool> split(Bool b, std::uint32_t v) const;

    bool meet(std::span<const std::pair<Bool, bool>> all) const;
    bool meet_seen(std::span<const std::pair<Bool, bool>> all, Meets& seen) const;
    bool meet_within(std::span<const std::pair<Bool, bool>> all, std::size_t budget,
                     Meets& seen) const;
    bool meet_in(std::size_t at, std::vector<std::uint32_t>& work, std::size_t& budget,
                 Meets& seen) const;
    bool overflowed() const noexcept { return overflow_; }
    std::size_t size() const noexcept { return nodes_.size(); }

    Bool atom(Atom atom, std::size_t born);
    Bool ite(Bool f, Bool g, Bool h);
    Bool and_(Bool a, Bool b);
    Bool or_(Bool a, Bool b);
    Bool not_(Bool a);
    Bool xor_(Bool a, Bool b);
    Bool land_not(Bool a, Bool b);
    Bool lor_not(Bool a, Bool b);
    bool implies(Bool a, Bool b);
    Bool forget(Bool b, std::size_t from, std::unordered_map<std::uint32_t, Bool>& memo);
    Bool restrict(Bool b, std::uint32_t v, bool value,
                  std::unordered_map<std::uint32_t, Bool>& memo);
    Bool given(Bool b, Bool known);

private:
    struct Node {
        std::uint32_t var{0};
        Bool low{};
        Bool high{};
    };

    std::vector<std::uint32_t> vars(Bool b) const;
    Bool mk(std::uint32_t v, Bool lo, Bool hi);

    std::vector<Node> nodes_;
    std::unordered_map<std::uint64_t, Bool> unique_;
    std::unordered_map<std::uint64_t, Bool> memo_;
    std::vector<Atom> atoms_;
    std::vector<std::size_t> born_;
    std::unordered_map<Atom, std::uint32_t, AtomHash> ids_;
    bool overflow_{false};
};

struct LaneExit {
    enum class Kind : std::uint8_t {
        Jump,
        Branch,
        End,
        Indirect,
    };

    Kind kind{Kind::End};
    Bool when{};
    std::size_t target{};
    std::size_t taken{};
    std::size_t not_taken{};
};

struct Structured {
    Bools bools;
    std::vector<LaneExit> exits;
    std::vector<std::vector<std::optional<Bool>>> guards;
    std::vector<Bool> reach;
    std::unordered_map<ir::Value, Bool> masks;
    std::unordered_map<MaskKey, Bool, MaskKeyHash> paired_masks;
    std::unordered_map<ir::Value, ir::Value> highs;
    std::unordered_set<ir::Value> active;
    std::unordered_map<ir::Value, std::vector<Bool>> phi_masks;
    std::unordered_set<std::size_t> wide;
    std::vector<std::optional<bool>> wave_any;
    std::unordered_map<ir::Value, Bool> scc_some;

    std::optional<Bool> mask_bits(ir::Value low, std::optional<ir::Value> high) const;
};

std::expected<Structured, ShaderError> structure(const ir::Program& program);
std::expected<Structured, ShaderError> structure_wide(
    const ir::Program& program, const std::unordered_set<std::size_t>& wide);

}
