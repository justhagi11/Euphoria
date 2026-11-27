#include <euphoria/shader/gfx10/bvh.hpp>

#include <cmath>
#include <cstring>
#include <limits>

namespace euphoria::shader::gfx10::bvh {
namespace {

constexpr float kMax = std::numeric_limits<float>::max();

float fmax_rust(float a, float b)
{
    if (std::isnan(a)) {
        return b;
    }
    if (std::isnan(b)) {
        return a;
    }
    return a > b ? a : b;
}

float fmin_rust(float a, float b)
{
    if (std::isnan(a)) {
        return b;
    }
    if (std::isnan(b)) {
        return a;
    }
    return a < b ? a : b;
}

std::uint32_t bits(float value)
{
    std::uint32_t result = 0;
    static_assert(sizeof(result) == sizeof(value));
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

float from_bits(std::uint32_t value)
{
    float result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

}

Ray Ray::make(const std::array<float, 3>& origin, const std::array<float, 3>& direction,
              float extent)
{
    Ray ray{};
    ray.extent = extent;
    ray.origin = origin;
    ray.direction = direction;
    for (std::size_t i = 0; i < 3; ++i) {
        ray.inverse[i] = 1.0f / direction[i];
    }
    return ray;
}

std::uint32_t kind(std::uint64_t pointer)
{
    return static_cast<std::uint32_t>(pointer & 7);
}

std::uint64_t node_address(std::uint32_t word0, std::uint32_t word1, std::uint64_t pointer)
{
    const std::uint64_t base =
        ((static_cast<std::uint64_t>(word1 & 0xFF) << 32) | static_cast<std::uint64_t>(word0))
        << 8;
    return base + ((pointer >> 3) << 6);
}

std::array<std::uint32_t, 4> intersect(const std::array<std::uint32_t, 28>& words,
                                       std::uint32_t kind_value, const Ray& ray, bool sort)
{
    switch (kind_value & 7) {
    case 4:
        return boxes(children(words), boxes16(words), ray, sort);
    case 5:
        return boxes(children(words), boxes32(words), ray, sort);
    case 0:
    case 1:
    case 2:
    case 3: {
        const std::array<float, 4> hit = triangle(triangle_vertices(words, kind_value & 7), ray);
        return {bits(hit[0]), bits(hit[1]), bits(hit[2]), bits(hit[3])};
    }
    default:
        return {bits(MISS[0]), bits(MISS[1]), bits(MISS[2]), bits(MISS[3])};
    }
}

std::array<std::uint32_t, 4> children(const std::array<std::uint32_t, 28>& words)
{
    return {words[0], words[1], words[2], words[3]};
}

std::array<std::array<float, 6>, 4> boxes16(const std::array<std::uint32_t, 28>& words)
{
    std::array<std::array<float, 6>, 4> result{};
    for (std::uint32_t b = 0; b < 4; ++b) {
        for (std::uint32_t k = 0; k < 6; ++k) {
            const std::uint32_t half = 6 * b + k;
            const std::uint16_t value =
                static_cast<std::uint16_t>(words[4 + half / 2] >> (16 * (half % 2)));
            result[b][k] = half_to_f32(value);
        }
    }
    return result;
}

std::array<std::array<float, 6>, 4> boxes32(const std::array<std::uint32_t, 28>& words)
{
    std::array<std::array<float, 6>, 4> result{};
    for (std::uint32_t b = 0; b < 4; ++b) {
        for (std::uint32_t k = 0; k < 6; ++k) {
            result[b][k] = from_bits(words[4 + 6 * b + k]);
        }
    }
    return result;
}

std::uint32_t vertex_map(std::uint32_t k)
{
    const std::array<std::uint32_t, 3>& v = TRIANGLE_VERTICES[k & 3];
    return v[0] | (v[1] << 4) | (v[2] << 8);
}

std::array<std::array<float, 3>, 3> triangle_vertices(const std::array<std::uint32_t, 28>& words,
                                                      std::uint32_t k)
{
    const std::uint32_t id = words[15] >> (8 * k);
    const std::uint32_t i = id & 3;
    const std::uint32_t j = (id >> 2) & 3;
    const std::uint32_t first = (3u - i - j) & 3;
    const std::uint32_t order[3] = {first, i, j};
    std::array<std::array<float, 3>, 3> result{};
    for (std::size_t n = 0; n < 3; ++n) {
        const std::uint32_t vertex = (vertex_map(k) >> (4 * order[n])) & 0xF;
        for (std::size_t c = 0; c < 3; ++c) {
            result[n][c] = from_bits(words[3 * vertex + c]);
        }
    }
    return result;
}

std::array<std::uint32_t, 4> boxes(const std::array<std::uint32_t, 4>& child_pointer,
                                   const std::array<std::array<float, 6>, 4>& box_list,
                                   const Ray& ray, bool sort)
{
    std::array<float, 3> inverse{};
    for (std::size_t i = 0; i < 3; ++i) {
        inverse[i] = fmin_rust(fmax_rust(ray.inverse[i], -kMax), kMax);
    }
    std::array<std::uint32_t, 4> ids{NONE, NONE, NONE, NONE};
    std::array<float, 4> distances{std::numeric_limits<float>::infinity(),
                                   std::numeric_limits<float>::infinity(),
                                   std::numeric_limits<float>::infinity(),
                                   std::numeric_limits<float>::infinity()};
    for (std::size_t n = 0; n < 4; ++n) {
        const std::array<float, 6>& b = box_list[n];
        std::array<float, 3> near_values{};
        std::array<float, 3> far_values{};
        for (std::size_t a = 0; a < 3; ++a) {
            const float low = (b[a] - ray.origin[a]) * inverse[a];
            const float high = (b[3 + a] - ray.origin[a]) * inverse[a];
            near_values[a] = fmin_rust(low, high);
            far_values[a] = fmax_rust(low, high);
        }
        const float enter = fmax_rust(fmax_rust(near_values[0], near_values[1]), near_values[2]);
        const float leave = fmin_rust(fmin_rust(far_values[0], far_values[1]), far_values[2]);
        if (!std::isnan(b[0]) && leave >= fmax_rust(enter, 0.0f) && enter < ray.extent) {
            ids[n] = child_pointer[n];
            distances[n] = enter;
        }
    }
    if (sort) {
        const std::pair<std::size_t, std::size_t> network[5] = {
            {0, 1}, {2, 3}, {0, 2}, {1, 3}, {1, 2}};
        for (const auto& [a, b] : network) {
            if (distances[b] < distances[a]) {
                std::swap(distances[a], distances[b]);
                std::swap(ids[a], ids[b]);
            }
        }
    }
    return ids;
}

std::array<float, 4> triangle(const std::array<std::array<float, 3>, 3>& p, const Ray& ray)
{
    const std::array<float, 3>& d = ray.direction;
    std::array<float, 3> size{std::fabs(d[0]), std::fabs(d[1]), std::fabs(d[2])};
    std::size_t kz = 0;
    if (size[0] >= size[1]) {
        kz = size[0] >= size[2] ? 0 : 2;
    } else if (size[1] >= size[2]) {
        kz = 1;
    } else {
        kz = 2;
    }
    std::size_t kx = (kz + 1) % 3;
    std::size_t ky = (kz + 2) % 3;
    if (d[kz] < 0.0f) {
        std::swap(kx, ky);
    }
    const float sz = ray.inverse[kz];
    const float sx = d[kx] * sz;
    const float sy = d[ky] * sz;
    const auto sheared = [&](const std::array<float, 3>& v) {
        const std::array<float, 3> r{v[0] - ray.origin[0], v[1] - ray.origin[1],
                                     v[2] - ray.origin[2]};
        return std::array<float, 3>{r[kx] - sx * r[kz], r[ky] - sy * r[kz], sz * r[kz]};
    };
    const auto a = sheared(p[0]);
    const auto b = sheared(p[1]);
    const auto c = sheared(p[2]);
    const float u = c[0] * b[1] - c[1] * b[0];
    const float v = a[0] * c[1] - a[1] * c[0];
    const float w = b[0] * a[1] - b[1] * a[0];
    const bool behind = u < 0.0f || v < 0.0f || w < 0.0f;
    const bool before = u > 0.0f || v > 0.0f || w > 0.0f;
    const float det = u + (v + w);
    const float t = (u * a[2] + v * b[2]) + w * c[2];
    const bool ahead = (det > 0.0f && t >= 0.0f) || (det < 0.0f && t <= 0.0f);
    if (!(behind && before) && ahead) {
        return {t, det, v, w};
    }
    return MISS;
}

float half_to_f32(std::uint16_t h)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(h >> 15) << 31;
    const std::uint32_t exponent = static_cast<std::uint32_t>((h >> 10) & 0x1F);
    const std::uint32_t mantissa = static_cast<std::uint32_t>(h & 0x3FF);
    std::uint32_t value = 0;
    if (exponent == 0 && mantissa == 0) {
        value = sign;
    } else if (exponent == 0) {
        std::uint32_t leading = 0;
        for (std::uint32_t m = mantissa; (m & 0x80000000u) == 0; m <<= 1) {
            ++leading;
        }
        const std::uint32_t shift = leading - 21;
        value = sign | ((113u - shift) << 23) | (((mantissa << shift) & 0x3FFu) << 13);
    } else if (exponent == 0x1F) {
        value = sign | (0xFFu << 23) | (mantissa << 13);
    } else {
        value = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    return from_bits(value);
}

}
