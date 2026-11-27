#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <vector>
#include <cstdint>

namespace euphoria::shader::gfx10::bvh {

inline constexpr std::uint32_t NONE = 0xFFFFFFFFu;

inline constexpr std::array<std::array<std::uint32_t, 3>, 4> TRIANGLE_VERTICES{{
    {{0, 1, 2}},
    {{1, 3, 2}},
    {{2, 3, 4}},
    {{2, 4, 0}},
}};

inline constexpr std::uint32_t BOX_SORT = 1u << 31;
inline constexpr std::uint32_t RETURN_BARYCENTRICS = 1u << 24;
inline constexpr std::uint32_t DESCRIPTOR_TYPE = 8;

struct Ray {
    float extent{0.0f};
    std::array<float, 3> origin{};
    std::array<float, 3> direction{};
    std::array<float, 3> inverse{};

    static Ray make(const std::array<float, 3>& origin, const std::array<float, 3>& direction,
                    float extent);
};

std::uint32_t kind(std::uint64_t pointer);
std::uint64_t node_address(std::uint32_t word0, std::uint32_t word1, std::uint64_t pointer);
std::array<std::uint32_t, 4> intersect(const std::array<std::uint32_t, 28>& words,
                                       std::uint32_t kind, const Ray& ray, bool sort);
std::array<std::uint32_t, 4> children(const std::array<std::uint32_t, 28>& words);
std::array<std::array<float, 6>, 4> boxes16(const std::array<std::uint32_t, 28>& words);
std::array<std::array<float, 6>, 4> boxes32(const std::array<std::uint32_t, 28>& words);
std::uint32_t vertex_map(std::uint32_t k);
std::array<std::array<float, 3>, 3> triangle_vertices(const std::array<std::uint32_t, 28>& words,
                                                      std::uint32_t k);
std::array<std::uint32_t, 4> boxes(const std::array<std::uint32_t, 4>& children,
                                   const std::array<std::array<float, 6>, 4>& boxes,
                                   const Ray& ray, bool sort);

inline constexpr std::array<float, 4> MISS{std::numeric_limits<float>::infinity(), 1.0f, 0.0f, 0.0f};

std::array<float, 4> triangle(const std::array<std::array<float, 3>, 3>& p, const Ray& ray);
float half_to_f32(std::uint16_t h);

using Vec3 = std::array<float, 3>;
using Box = std::array<float, 6>;

struct Node {
    std::array<std::uint32_t, 28> words{};
};

inline std::array<std::uint32_t, 4> intersect(const Node& node, std::uint32_t kind_value,
                                              const Ray& ray, bool sort)
{
    return intersect(node.words, kind_value, ray, sort);
}

namespace tests {

inline std::uint32_t float_bits(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

inline std::uint16_t f32_to_half(float value, bool up)
{
    std::uint16_t best = up ? 0x7BFF : 0x0000;
    float best_value = up ? 65504.0f : 0.0f;
    bool found = false;
    for (std::uint32_t h = 0; h < 0x8000; ++h) {
        const auto candidate = static_cast<std::uint16_t>(h);
        const float converted = half_to_f32(candidate);
        const bool fits = up ? converted >= value : converted <= value;
        if (!fits) {
            continue;
        }
        if (!found || (up ? converted < best_value : converted > best_value)) {
            best = candidate;
            best_value = converted;
            found = true;
        }
    }
    if (!found) {
        return best;
    }
    const std::uint32_t negative = static_cast<std::uint32_t>(best) | 0x8000u;
    const float negated = half_to_f32(static_cast<std::uint16_t>(negative));
    const bool fits = up ? negated >= value : negated <= value;
    if (value < 0.0f && fits
        && (up ? negated < best_value : negated > best_value)) {
        return static_cast<std::uint16_t>(negative);
    }
    return best;
}

inline Node box32(const std::array<std::uint32_t, 4>& kids, const std::array<Box, 4>& boxes)
{
    Node node{};
    for (std::size_t i = 0; i < 4; ++i) {
        node.words[i] = kids[i];
        for (std::size_t k = 0; k < 6; ++k) {
            node.words[4 + 6 * i + k] = float_bits(boxes[i][k]);
        }
    }
    return node;
}

inline Node box16(const std::array<std::uint32_t, 4>& kids, const std::array<Box, 4>& boxes)
{
    Node node{};
    for (std::size_t i = 0; i < 4; ++i) {
        node.words[i] = kids[i];
        for (std::size_t k = 0; k < 6; ++k) {
            const std::size_t half = 6 * i + k;
            const std::uint16_t value = f32_to_half(boxes[i][k], false);
            node.words[4 + half / 2] |= static_cast<std::uint32_t>(value) << (16 * (half % 2));
        }
    }
    return node;
}

inline Node triangles(const std::vector<Vec3>& vertices, std::uint32_t id)
{
    Node node{};
    const std::size_t count = std::min<std::size_t>(vertices.size(), 5);
    for (std::size_t v = 0; v < count; ++v) {
        for (std::size_t c = 0; c < 3; ++c) {
            node.words[3 * v + c] = float_bits(vertices[v][c]);
        }
    }
    node.words[15] = id;
    return node;
}

inline Node triangles(const std::array<Vec3, 3>& vertices, std::uint32_t id)
{
    return triangles(std::vector<Vec3>(vertices.begin(), vertices.end()), id);
}

}

}
