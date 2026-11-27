#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::emit {

struct SigElement {
    std::string name;
    std::uint32_t index{0};
    std::uint8_t kind{0};
    std::uint8_t comp{0};
    std::uint8_t interp{0};
    std::optional<std::uint32_t> row;
    std::uint8_t col{0};
    std::uint8_t cols{0};
    std::uint8_t usage{0};
    bool half{false};

    std::uint32_t system_value() const noexcept;
    std::uint32_t sig_comp() const noexcept;
    std::uint8_t mask() const noexcept;

    friend bool operator==(const SigElement&, const SigElement&) = default;
};

struct Mesh {
    std::uint32_t max_vertices{0};
    std::uint32_t max_primitives{0};
    std::uint8_t topology{0};
};

struct Geometry {
    std::uint32_t max_vertices{0};
};

std::vector<std::uint8_t> signature(std::span<const SigElement> elements, bool input);

inline constexpr std::uint64_t MESH_OUTPUT_LIMIT = 32 * 1024;

std::uint64_t mesh_output_bytes(
    std::span<const SigElement> vertex,
    std::span<const SigElement> primitive,
    std::uint32_t max_vertices,
    std::uint32_t max_primitives);

struct Binding {
    std::uint32_t kind{0};
    std::uint32_t space{0};
    std::uint32_t register_{0};
    std::uint32_t dxil_kind{0};
};

struct Psv {
    std::uint8_t stage{0};
    bool flag{false};
    std::span<const SigElement> inputs;
    std::span<const SigElement> outputs;
    std::span<const SigElement> prims;
    std::optional<std::pair<Mesh, std::uint32_t>> mesh;
    std::optional<std::uint32_t> geometry;
    std::span<const Binding> bindings;
    std::array<std::uint32_t, 3> threads{};
    std::optional<std::uint32_t> wave_size;
};

std::vector<std::uint8_t> psv(const Psv& p);

struct ContainerPart {
    std::array<char, 4> fourcc{};
    std::span<const std::uint8_t> data;
};

ContainerPart part(std::string_view fourcc, std::span<const std::uint8_t> data);

std::vector<std::uint8_t> build(std::span<const ContainerPart> parts);

std::array<std::uint8_t, 16> hash(std::span<const std::uint8_t> data);

void md5_block(std::array<std::uint32_t, 4>& state, const std::uint8_t* block);

}
