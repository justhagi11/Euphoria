#include <euphoria/shader/gfx10/bvh.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

using namespace bvh;
using Result4 = std::array<std::uint32_t, 4>;

Result4 to_bits(const std::array<float, 4>& values)
{
    Result4 out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = std::bit_cast<std::uint32_t>(values[i]);
    }
    return out;
}

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr Box UNIT{-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
constexpr Box OFF{kNan, kNan, kNan, kNan, kNan, kNan};

Box moved(Box b, Vec3 by)
{
    Box out{};
    for (std::size_t k = 0; k < 6; ++k) {
        out[k] = b[k] + by[k % 3];
    }
    return out;
}

Ray with_extent(Ray ray, float extent)
{
    ray.extent = extent;
    return ray;
}

std::string text(const Result4& r)
{
    return std::format("[{:#x}, {:#x}, {:#x}, {:#x}]", r[0], r[1], r[2], r[3]);
}

bool same(Context& c, const Result4& actual, const Result4& expected)
{
    return c.expect_eq(text(actual), text(expected));
}

struct Hit {
    float t;
    float i;
    float j;
    bool front;
};

std::optional<Hit> hit(const Result4& r)
{
    const float t = std::bit_cast<float>(r[0]);
    const float det = std::bit_cast<float>(r[1]);
    const float i = std::bit_cast<float>(r[2]);
    const float j = std::bit_cast<float>(r[3]);
    if (t == kInf) {
        return std::nullopt;
    }
    return Hit{t / det, i / det, j / det, det > 0.0f};
}

constexpr std::array<Vec3, 3> TRIANGLE{{{0.0f, 2.0f, 5.0f}, {2.0f, -2.0f, 5.0f}, {-2.0f, -2.0f, 5.0f}}};

Node tri(std::initializer_list<Vec3> vertices, std::uint32_t id)
{
    const std::vector<Vec3> list(vertices);
    return tests::triangles(list, id);
}

Node tri3(const std::array<Vec3, 3>& vertices, std::uint32_t id)
{
    return tests::triangles(vertices, id);
}

void node_pointers(Context& c)
{
    const std::uint64_t structure = 0x12'3456'7800ull;
    const std::uint64_t pointer = ((structure >> 8) << 5) + ((0x40 >> 3) | 5);
    c.expect(kind(pointer) == 5, "kind 5");
    c.expect(node_address(0, BOX_SORT, pointer) == structure + 0x40, "64-bit form");
    const auto word0 = static_cast<std::uint32_t>(structure >> 8);
    const auto word1 = static_cast<std::uint32_t>(structure >> 40) | BOX_SORT;
    c.expect(node_address(word0, word1, (0x1000 >> 3) | 4) == structure + 0x1000, "32-bit form");
    c.expect(node_address(0, 0x8000'0001u, 8) == (1ull << 40) + 64, "base past 4 GiB");
}

void halves(Context& c)
{
    c.expect(half_to_f32(0x3C00) == 1.0f, "1");
    c.expect(half_to_f32(0xC000) == -2.0f, "-2");
    c.expect(half_to_f32(0x7C00) == kInf, "inf");
    c.expect(half_to_f32(0x0001) == std::ldexp(1.0f, -24), "smallest subnormal");
    c.expect(half_to_f32(0x03FF) == 1023.0f * std::ldexp(1.0f, -24), "largest subnormal");
    c.expect(half_to_f32(0x7BFF) == 65504.0f, "max");
    c.expect(std::isnan(half_to_f32(0x7E00)), "nan");
    c.expect(std::bit_cast<std::uint32_t>(half_to_f32(0x8000)) == 0x8000'0000u, "-0");
    c.expect(half_to_f32(tests::f32_to_half(0.1f, true)) >= 0.1f, "rounded up");
    c.expect(half_to_f32(tests::f32_to_half(0.1f, false)) <= 0.1f, "rounded down");
}

void boxes_nearest_first(Context& c)
{
    const std::array<std::uint32_t, 4> kids{0x105, 0x20C, 0x304, 0x400};
    const Node node = tests::box32(kids, {moved(UNIT, {30, 0, 0}), moved(UNIT, {10, 0, 0}), moved(UNIT, {10, 5, 0}),
                                          moved(UNIT, {20, 0, 0})});
    const Ray ray = Ray::make({0, 0, 0}, {1, 0, 0}, 1000.0f);
    same(c, intersect(node, 5, ray, true), {0x20C, 0x400, 0x105, NONE});
    same(c, intersect(node, 5, ray, false), {0x105, 0x20C, NONE, 0x400});
    same(c, intersect(node, 5, with_extent(ray, 19.5f), true), {0x20C, 0x400, NONE, NONE});
    same(c, intersect(node, 5, with_extent(ray, 19.0f), true), {0x20C, NONE, NONE, NONE});
    const Ray back = Ray::make({0, 0, 0}, {-1, 0, 0}, 1000.0f);
    same(c, intersect(node, 5, back, true), {NONE, NONE, NONE, NONE});
}

void inside_a_box(Context& c)
{
    const Node node = tests::box32({8, 16, NONE, NONE}, {UNIT, moved(UNIT, {3, 0, 0}), OFF, OFF});
    const Ray ray = Ray::make({0, 0, 0}, {1, 0, 0}, 100.0f);
    same(c, intersect(node, 5, ray, true), {8, 16, NONE, NONE});
    same(c, intersect(node, 5, with_extent(ray, 1e-6f), true), {8, NONE, NONE, NONE});
}

void axis_aligned(Context& c)
{
    const Node node = tests::box32({8, 16, 24, 32}, {UNIT, OFF, OFF, OFF});
    const Ray ray = Ray::make({-5, -1, 0}, {1, 0, 0}, 100.0f);
    c.expect(std::isinf(ray.inverse[1]), "infinite inverse");
    same(c, intersect(node, 5, ray, true), {8, NONE, NONE, NONE});
    same(c, intersect(node, 5, Ray::make({-5, 1, 0}, {1, 0, 0}, 100.0f), true), {NONE, NONE, NONE, NONE});
    same(c, intersect(node, 5, Ray::make({-5, 1.5f, 0}, {1, 0, 0}, 100.0f), true), {NONE, NONE, NONE, NONE});
    const Ray minus = Ray::make({-5, 0.5f, 0}, {1, -0.0f, 0}, 100.0f);
    c.expect(minus.inverse[1] == -kInf, "-0 gives -inf");
    same(c, intersect(node, 5, minus, true), {8, NONE, NONE, NONE});
}

void switched_off(Context& c)
{
    const Node node = tests::box32({8, NONE, 24, 32}, {UNIT, UNIT, Box{kNan, -1, -1, 1, 1, 1}, OFF});
    const Ray ray = Ray::make({-5, 0, 0}, {1, 0, 0}, 100.0f);
    same(c, intersect(node, 5, ray, true), {8, NONE, NONE, NONE});
}

void equal_distances(Context& c)
{
    const Node node = tests::box32({8, 16, 24, 32}, {UNIT, UNIT, UNIT, UNIT});
    const Ray ray = Ray::make({-5, 0, 0}, {1, 0, 0}, 100.0f);
    same(c, intersect(node, 5, ray, true), {8, 16, 24, 32});
}

void half_boxes(Context& c)
{
    const std::array<Box, 4> boxes{moved(UNIT, {30, 0.1f, 0}), moved(UNIT, {10, 0, 0.3f}), moved(UNIT, {10, 5, 0}),
                                   moved(UNIT, {20.7f, 0, 0})};
    const std::array<std::uint32_t, 4> kids{0x105, 0x20C, 0x304, 0x400};
    const Node node = tests::box16(kids, boxes);
    const auto wide = boxes16(node.words);
    for (std::size_t b = 0; b < 4; ++b) {
        for (std::size_t k = 0; k < 3; ++k) {
            c.expect(wide[b][k] <= boxes[b][k] && wide[b][3 + k] >= boxes[b][3 + k], "rounded outward");
        }
    }
    const Ray ray = Ray::make({0, 0, 0}, {1, 0, 0}, 1000.0f);
    same(c, intersect(node, 4, ray, true), {0x20C, 0x400, 0x105, NONE});
    same(c, intersect(node, 4, ray, true), intersect(tests::box32(kids, wide), 5, ray, true));
}

void triangle_hit(Context& c)
{
    const Node node = tri3(TRIANGLE, 9);
    auto h = hit(intersect(node, 0, Ray::make({0, 0, 0}, {0, 0, 1}, 100.0f), true));
    if (c.expect(h.has_value(), "a hit")) {
        c.expect(h->t == 5.0f, "t 5");
        c.expect(h->i == 0.25f && h->j == 0.25f, "weights 0.25");
        c.expect(h->front, "front");
    }
    h = hit(intersect(node, 0, Ray::make({1.75f, -1.75f, 0}, {0, 0, 1}, 100.0f), true));
    if (c.expect(h.has_value(), "a hit near vertex 1")) {
        c.expect(h->i > 0.85f && h->j < 0.1f, std::format("{} {}", h->i, h->j));
    }
    h = hit(intersect(node, 0, Ray::make({0, 0, 10}, {0, 0, -1}, 100.0f), true));
    if (c.expect(h.has_value(), "a hit from behind")) {
        c.expect(h->t == 5.0f && h->i == 0.25f && h->j == 0.25f && !h->front, "back face");
    }
    h = hit(intersect(node, 0, Ray::make({0, 0, 0}, {0, 0, 4}, 100.0f), true));
    c.expect(h.has_value() && h->t == 1.25f, "unnormalised direction");
}

void triangle_miss(Context& c)
{
    const Node node = tri3(TRIANGLE, 9);
    const Result4 miss = to_bits(MISS);
    same(c, intersect(node, 0, Ray::make({3, 0, 0}, {0, 0, 1}, 100.0f), true), miss);
    same(c, intersect(node, 0, Ray::make({0, 0, 6}, {0, 0, 1}, 100.0f), true), miss);
    same(c, intersect(node, 0, Ray::make({-5, 0, 5}, {1, 0, 0}, 100.0f), true), miss);
    const Node flat = tri({{0, 0, 5}, {1, 1, 5}, {2, 2, 5}}, 9);
    same(c, intersect(flat, 0, Ray::make({1, 1, 0}, {0, 0, 1}, 100.0f), true), miss);
    const Node nan = tri({{kNan, 2, 5}, TRIANGLE[1], TRIANGLE[2]}, 9);
    same(c, intersect(nan, 0, Ray::make({0, 0, 0}, {0, 0, 1}, 100.0f), true), miss);
    same(c, intersect(node, 0, Ray::make({0, 0, 0}, {0, 0, 0}, 100.0f), true), miss);
    c.expect(hit(intersect(node, 0, Ray::make({0, 0, 0}, {0, 0, 1}, 1.0f), true)).has_value(), "far triangle hit");
}

void shared_edge(Context& c)
{
    const std::array<Vec3, 4> quad{{{-1, -1, 4}, {-1, 1, 4}, {1, 1, 4}, {1, -1, 4}}};
    const Node first = tri({quad[0], quad[1], quad[2]}, 9);
    const Node second = tri({quad[0], quad[2], quad[3]}, 9);
    for (int n = -40; n <= 40; ++n) {
        const float x = static_cast<float>(n) / 64.0f;
        const std::array<std::pair<float, float>, 3> origins{
            {{x, x}, {x, x + 1.0f / 1024.0f}, {x + 1.0f / 1024.0f, x}}};
        for (const auto& [ox, oy] : origins) {
            const std::array<Vec3, 3> dirs{{{0, 0, 1}, {0.125f, -0.0625f, 1}, {0.25f, 0.5f, 2}}};
            for (const Vec3& d : dirs) {
                const float s = 4.0f / d[2];
                const Ray ray = Ray::make({ox - d[0] * s, oy - d[1] * s, 0}, d, 100.0f);
                const auto a = hit(intersect(first, 0, ray, true));
                const auto b = hit(intersect(second, 0, ray, true));
                c.expect(a.has_value() || b.has_value(), std::format("a hole at {} {}", ox, oy));
                for (const auto& h : {a, b}) {
                    if (h) {
                        c.expect(std::fabs(h->t - s) < 1e-5f && h->front, std::format("{} {}", h->t, h->front));
                    }
                }
            }
        }
    }
}

void id_dword(Context& c)
{
    const auto [a, b, cc] = TRIANGLE;
    const Ray ray = Ray::make({0.5f, -0.25f, 0}, {0, 0, 1}, 100.0f);
    const Result4 plain = intersect(tri({a, b, cc}, 9), 0, ray, true);
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 6> pairs{{{1, 2}, {2, 0}, {0, 1}, {2, 1}, {0, 2}, {1, 0}}};
    for (const auto& [i, j] : pairs) {
        std::array<Vec3, 3> stored{};
        stored[3 - i - j] = a;
        stored[i] = b;
        stored[j] = cc;
        const Node node = tri3(stored, i | (j << 2));
        same(c, intersect(node, 0, ray, true), plain);
    }
    same(c, intersect(tri({a, b, cc}, 9 | 0x80), 0, ray, true), plain);
}

void triangles_one_to_three(Context& c)
{
    const auto [a, b, cc] = TRIANGLE;
    const Vec3 far{50, 50, 50};
    const Ray ray = Ray::make({0.5f, -0.25f, 0}, {0, 0, 1}, 100.0f);
    const Result4 plain = intersect(tri({a, b, cc}, 9), 0, ray, true);
    for (std::uint32_t k = 1; k < 4; ++k) {
        std::array<Vec3, 5> vertices{far, far, far, far, far};
        const auto& at = TRIANGLE_VERTICES[k];
        vertices[at[0]] = a;
        vertices[at[1]] = b;
        vertices[at[2]] = cc;
        const Node node = tests::triangles(std::vector<Vec3>(vertices.begin(), vertices.end()), 9u << (8 * k));
        same(c, intersect(node, k, ray, true), plain);
    }
    const Vec3 d{2, 2, 5};
    const Node node = tri({a, b, cc, d}, 9 | ((1 | (2 << 2)) << 8));
    const auto got = triangle_vertices(node.words, 1);
    c.expect(got[0] == b && got[1] == d && got[2] == cc, "triangle 1 is (b, d, c)");
}

void instances(Context& c)
{
    const Node node = tri3(TRIANGLE, 9);
    const Ray ray = Ray::make({0, 0, 0}, {0, 0, 1}, 100.0f);
    for (std::uint32_t k : {6u, 7u}) {
        same(c, intersect(node, k, ray, true), to_bits(MISS));
    }
}

void largest_axis(Context& c)
{
    const auto turn = [](Vec3 v, std::size_t axis, bool flip) {
        Vec3 r = v;
        for (std::size_t n = 0; n < axis; ++n) {
            r = {r[2], r[0], r[1]};
        }
        if (flip) {
            const std::size_t largest = (2 + axis) % 3;
            r[largest] = -r[largest];
            r[(largest + 1) % 3] = -r[(largest + 1) % 3];
        }
        return r;
    };
    const Ray ray = Ray::make({0.25f, -0.5f, 0}, {0.0625f, 0.125f, 1}, 100.0f);
    const auto base = hit(to_bits(triangle(TRIANGLE, ray)));
    if (!c.expect(base.has_value(), "a hit")) {
        return;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (bool flip : {false, true}) {
            std::array<Vec3, 3> p{};
            for (std::size_t n = 0; n < 3; ++n) {
                p[n] = turn(TRIANGLE[n], axis, flip);
            }
            const Ray r = Ray::make(turn(ray.origin, axis, flip), turn(ray.direction, axis, flip), 100.0f);
            const auto h = hit(to_bits(triangle(p, r)));
            if (!c.expect(h.has_value(), std::format("a hit, axis {} flip {}", axis, flip))) {
                continue;
            }
            c.expect(std::fabs(h->t - base->t) < 1e-5f, std::format("{} {}: distance", axis, flip));
            c.expect(std::fabs(h->i - base->i) < 1e-5f && std::fabs(h->j - base->j) < 1e-5f, "weights");
            c.expect(h->front == base->front, std::format("facing, axis {} flip {}", axis, flip));
        }
    }
}

}

void register_bvh_tests()
{
    register_case("bvh", "node_pointers_name_64_byte_nodes_past_the_descriptors_base", node_pointers);
    register_case("bvh", "halves_convert_exactly", halves);
    register_case("bvh", "boxes_are_returned_nearest_first_and_misses_last", boxes_nearest_first);
    register_case("bvh", "a_ray_starting_inside_a_box_enters_it_at_its_origin", inside_a_box);
    register_case("bvh", "axis_aligned_rays_and_origins_on_a_face_make_no_nan", axis_aligned);
    register_case("bvh", "a_box_switched_off_and_a_missing_child_return_none", switched_off);
    register_case("bvh", "equal_distances_keep_child_order", equal_distances);
    register_case("bvh", "half_float_boxes_test_like_their_widened_values", half_boxes);
    register_case("bvh", "a_triangle_returns_distance_and_weights_over_one_denominator", triangle_hit);
    register_case("bvh", "a_triangle_is_missed_beside_behind_and_edge_on", triangle_miss);
    register_case("bvh", "a_ray_through_a_shared_edge_misses_neither_triangle", shared_edge);
    register_case("bvh", "the_id_dword_says_which_vertices_the_weights_belong_to", id_dword);
    register_case("bvh", "triangles_one_to_three_take_their_vertices_and_their_id_byte", triangles_one_to_three);
    register_case("bvh", "instances_and_procedural_nodes_return_a_triangle_miss", instances);
    register_case("bvh", "the_largest_axis_and_its_sign_do_not_change_the_answer", largest_axis);
}

}
