#pragma once

#include <euphoria/shader/gfx10/emit/bitcode.hpp>
#include <euphoria/shader/gfx10/emit/container.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::emit {

inline constexpr std::uint32_t USER_DATA_SPACE = 0;
inline constexpr std::uint32_t SAMPLER_SPACE = 0;
inline constexpr std::uint32_t ROOT_CBV_SPACE = 1;
inline constexpr std::uint8_t SLOT_STRIDES = 255;
inline constexpr std::uint32_t SLOT_STRIDES_COUNT = 1u << 21;
inline constexpr std::uint8_t SLOT_MEMORY = 254;
inline constexpr std::uint8_t SLOT_FAULTS = 253;
inline constexpr std::uint32_t MEMORY_PAGES = 1u << 21;
inline constexpr std::uint32_t MEMORY_FAULTS = 4096;
inline constexpr std::uint32_t USER_MEMORY_BASE = 0x100;
inline constexpr std::uint32_t USER_VIEW_ADDRESS = 0x101;
inline constexpr std::uint32_t USER_VIEW_BIAS = 0x103;

namespace flags {
inline constexpr std::uint64_t RAW_BUFFERS = 1ull << 4;
inline constexpr std::uint64_t DOUBLES = 1ull << 2;
inline constexpr std::uint64_t STENCIL_REF = 1ull << 11;
inline constexpr std::uint64_t TYPED_UAV_LOADS = 1ull << 13;
inline constexpr std::uint64_t WAVE_OPS = 1ull << 19;
inline constexpr std::uint64_t BARYCENTRICS = 1ull << 22;
inline constexpr std::uint64_t INT64 = 1ull << 20;
inline constexpr std::uint64_t ATOMIC_TYPED64 = 1ull << 27;
inline constexpr std::uint64_t ATOMIC_HEAP64 = 1ull << 32;
inline constexpr std::uint64_t RESOURCE_HEAP = 1ull << 30;
inline constexpr std::uint64_t SAMPLER_HEAP = 1ull << 31;
}

struct Types {
    Ty void_;
    Ty i1;
    Ty i32;
    Ty f32;
    Ty handle;
    Ty props;
    Ty bind;
    Ty cbuf_ret;
    Ty res_ret;
};

void set_view_heap_limit(std::uint32_t null_slot);
std::uint32_t view_heap_limit();

class Shader {
public:
    explicit Shader(bool pixel);

    Module m;
    Types t;
    bool pixel{false};
    std::vector<SigElement> inputs;
    std::vector<SigElement> outputs;
    std::vector<SigElement> prims;
    std::optional<Mesh> mesh;
    std::optional<Geometry> geometry;
    std::uint32_t user_data{0};
    std::uint32_t samplers{0};
    std::uint64_t cmp_samplers{0};
    std::vector<std::pair<std::uint8_t, V>> root_cbvs;
    std::vector<std::pair<std::uint8_t, V>> root_srvs;
    std::vector<std::pair<std::uint8_t, V>> root_uavs;
    std::uint64_t flags{0};
    std::uint32_t nonuniform_handles{0};
    std::optional<std::array<std::uint32_t, 3>> threads;
    std::optional<std::uint32_t> wave_size;
    std::uint32_t lds_dwords{0};
    std::size_t entry{0};

    V lds();
    V op(std::size_t block, std::string_view name, Attr attr, Ty ret, std::uint32_t opcode, std::span<const V> args);
    V op(std::size_t block, std::string_view name, Attr attr, Ty ret, std::uint32_t opcode, std::initializer_list<V> args);
    V user_data_handle();
    V root_cbv_handle(std::uint8_t slot);
    V root_srv_handle(std::uint8_t slot);
    V root_uav_handle(std::uint8_t slot);
    V sampler_handle(std::size_t block, std::uint32_t n);
    V annotate(std::size_t block, V h, std::uint32_t props0, std::uint32_t props1);
    V heap_handle(std::size_t block, V index, bool sampler, bool nonuniform, std::pair<std::uint32_t, std::uint32_t> props);

    std::vector<std::uint8_t> finish();

private:
    std::optional<V> lds_global_;
    V main_;

    void fix_user_data_size(std::uint32_t size);
    std::optional<Md> signature_md(const std::vector<SigElement>& elements);
};

}
