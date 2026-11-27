#pragma once

#include <euphoria/shader/gfx10/error.hpp>
#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/native.hpp>
#include <euphoria/shader/gfx10/resource.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::io {

using Site = resource::Site;

enum class Barycentric : std::uint8_t {
    PerspectiveSample,
    PerspectiveCenter,
    PerspectiveCentroid,
    PerspectivePullModel,
    LinearSample,
    LinearCenter,
    LinearCentroid,
};

struct Input {
    enum class Kind : std::uint8_t {
        VertexId,
        PrimitiveId,
        InstanceId,
        Attribute,
        BarycentricValue,
        Position,
        FrontFace,
        Ancillary,
        SampleCoverage,
        FixedPosition,
        LineStipple,
        GroupId,
        ThreadIdInGroup,
        GroupInfo,
        MeshGroup,
        MergedWaveInfo,
    };

    Kind kind{Kind::VertexId};
    Barycentric barycentric{Barycentric::PerspectiveCenter};
    std::uint8_t index{};
    std::uint8_t row{};
    std::uint8_t channel{};

    static constexpr Input Attribute(std::uint8_t row, std::uint8_t channel) noexcept
    {
        return {Kind::Attribute, Barycentric::PerspectiveCenter, 0, row, channel};
    }
    static constexpr Input BarycentricInput(Barycentric kind, std::uint8_t index) noexcept
    {
        return {Kind::BarycentricValue, kind, index, 0, 0};
    }
    static constexpr Input Position(std::uint8_t channel) noexcept
    {
        return {Kind::Position, Barycentric::PerspectiveCenter, channel, 0, 0};
    }
    static constexpr Input GroupId(std::uint8_t index) noexcept
    {
        return {Kind::GroupId, Barycentric::PerspectiveCenter, index, 0, 0};
    }
    static constexpr Input ThreadIdInGroup(std::uint8_t index) noexcept
    {
        return {Kind::ThreadIdInGroup, Barycentric::PerspectiveCenter, index, 0, 0};
    }
    static constexpr Input Simple(Kind kind) noexcept
    {
        return {kind, Barycentric::PerspectiveCenter, 0, 0, 0};
    }

    friend constexpr bool operator==(const Input&, const Input&) = default;
};

struct Attribute {
    native::Usage usage{native::Usage::Generic};
    std::uint8_t row{};
    std::uint8_t channel{};
    bool high{false};

    friend bool operator==(const Attribute&, const Attribute&) = default;
};

struct Interp {
    enum class Kind : std::uint8_t {
        Interpolated,
        Vertex,
        Weighted,
    };

    Kind kind{Kind::Weighted};
    Attribute attribute;
    Barycentric barycentric{Barycentric::PerspectiveCenter};
    std::vector<Site> first;
    std::uint8_t vertex{};

    friend bool operator==(const Interp&, const Interp&) = default;
};

struct Target {
    enum class Kind : std::uint8_t {
        Color,
        Depth,
        Null,
        Position,
        Param,
        Unread,
        Primitive,
        PrimitiveParam,
    };

    Kind kind{Kind::Null};
    std::uint8_t index{};
    native::Usage usage{native::Usage::Generic};
    std::uint8_t row{};

    friend bool operator==(const Target&, const Target&) = default;
};

struct Export {
    Site site;
    Target target;
    std::array<std::optional<ir::Value>, 4> values;
    bool compressed{false};
    bool done{false};

    friend bool operator==(const Export&, const Export&) = default;
};

struct Io {
    std::vector<std::pair<ir::Value, Input>> inputs;
    std::vector<std::pair<Site, Interp>> interps;
    std::vector<Export> exports;
    std::optional<std::uint8_t> initialized_sgprs;

    friend bool operator==(const Io&, const Io&) = default;
};

std::expected<Io, ShaderError> raise(const ir::Program& program,
                                     const native::NativeProgram& source);

}
