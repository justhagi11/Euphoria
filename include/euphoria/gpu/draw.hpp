#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <d3d12.h>

#include <euphoria/gpu/command.hpp>

namespace euphoria::gpu {

inline constexpr std::size_t draw_root_parameter_limit = 64;
inline constexpr std::size_t draw_root_constant_limit = 64;
inline constexpr std::size_t draw_vertex_buffer_limit = 32;
inline constexpr std::size_t draw_viewport_limit = 16;
inline constexpr std::size_t draw_scissor_limit = 16;

enum class RootArgumentKind : std::uint8_t {
    None,
    Table,
    Constants,
    View,
};

struct RootArgument {
    RootArgumentKind kind = RootArgumentKind::None;
    D3D12_GPU_DESCRIPTOR_HANDLE table{};
    D3D12_GPU_VIRTUAL_ADDRESS view = 0;
    std::array<std::uint32_t, draw_root_constant_limit> constants{};
    std::uint32_t constant_count = 0;
};

enum class ValueWriteKind : std::uint8_t {
    Value,
    GpuTimestamp,
    SystemTimestamp,
};

struct ValueWrite {
    std::uint64_t address = 0;
    std::uint64_t value = 0;
    std::uint32_t bytes = 4;
    ValueWriteKind kind = ValueWriteKind::Value;
};

struct ValueWait {
    std::uint64_t address = 0;
    std::uint64_t reference = 0;
    std::uint64_t mask = 0;
    std::uint32_t comparison = 0;
    std::uint32_t bytes = 4;
};

struct DrawState {
    CommandLayout layout = default_command_layout;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12RootSignature* graphics_root_signature = nullptr;
    ID3D12RootSignature* compute_root_signature = nullptr;
    std::array<ID3D12DescriptorHeap*, 2> descriptor_heaps{};
    UINT descriptor_heap_count = 0;
    std::array<RootArgument, draw_root_parameter_limit> graphics_roots{};
    std::array<RootArgument, draw_root_parameter_limit> compute_roots{};
    std::array<D3D12_VERTEX_BUFFER_VIEW, draw_vertex_buffer_limit> vertex_buffers{};
    std::array<bool, draw_vertex_buffer_limit> vertex_bound{};
    UINT vertex_buffer_count = 0;
    D3D12_INDEX_BUFFER_VIEW index_buffer{};
    bool index_bound = false;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT>
        render_targets{};
    UINT render_target_count = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil{};
    bool depth_stencil_bound = false;
    std::array<D3D12_VIEWPORT, draw_viewport_limit> viewports{};
    UINT viewport_count = 0;
    std::array<D3D12_RECT, draw_scissor_limit> scissors{};
    UINT scissor_count = 0;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::uint32_t console_topology = 0;
    std::vector<ValueWrite> writes;
    std::vector<ValueWait> waits;

    void reset() noexcept;
};

inline RootArgument* root_argument(DrawState& state, bool compute, std::uint32_t index) noexcept
{
    if (index >= draw_root_parameter_limit) {
        return nullptr;
    }
    return compute ? &state.compute_roots[index] : &state.graphics_roots[index];
}

inline const RootArgument* root_argument(const DrawState& state,
                                         bool compute,
                                         std::uint32_t index) noexcept
{
    if (index >= draw_root_parameter_limit) {
        return nullptr;
    }
    return compute ? &state.compute_roots[index] : &state.graphics_roots[index];
}

void draw_set_pipeline_state(DrawState& state,
                             CommandListProxy* proxy,
                             ID3D12GraphicsCommandList* host,
                             ID3D12PipelineState* pipeline);

void draw_set_descriptor_heaps(DrawState& state,
                               CommandListProxy* proxy,
                               ID3D12GraphicsCommandList* host,
                               UINT count,
                               void* const* heaps);

void draw_set_graphics_root_signature(DrawState& state,
                                      CommandListProxy* proxy,
                                      ID3D12GraphicsCommandList* host,
                                      ID3D12RootSignature* root_signature);

void draw_set_compute_root_signature(DrawState& state,
                                     CommandListProxy* proxy,
                                     ID3D12GraphicsCommandList* host,
                                     ID3D12RootSignature* root_signature);

void draw_set_graphics_root_descriptor_table(DrawState& state,
                                             CommandListProxy* proxy,
                                             ID3D12GraphicsCommandList* host,
                                             UINT root_parameter_index,
                                             D3D12_GPU_DESCRIPTOR_HANDLE handle);

void draw_set_compute_root_descriptor_table(DrawState& state,
                                            CommandListProxy* proxy,
                                            ID3D12GraphicsCommandList* host,
                                            UINT root_parameter_index,
                                            D3D12_GPU_DESCRIPTOR_HANDLE handle);

void draw_set_graphics_root_constant(DrawState& state,
                                     CommandListProxy* proxy,
                                     ID3D12GraphicsCommandList* host,
                                     UINT root_parameter_index,
                                     UINT value,
                                     UINT offset);

void draw_set_compute_root_constant(DrawState& state,
                                    CommandListProxy* proxy,
                                    ID3D12GraphicsCommandList* host,
                                    UINT root_parameter_index,
                                    UINT value,
                                    UINT offset);

void draw_set_graphics_root_constants(DrawState& state,
                                      CommandListProxy* proxy,
                                      ID3D12GraphicsCommandList* host,
                                      UINT root_parameter_index,
                                      UINT count,
                                      const void* values,
                                      UINT offset);

void draw_set_compute_root_constants(DrawState& state,
                                     CommandListProxy* proxy,
                                     ID3D12GraphicsCommandList* host,
                                     UINT root_parameter_index,
                                     UINT count,
                                     const void* values,
                                     UINT offset);

void draw_set_graphics_root_constant_buffer_view(DrawState& state,
                                                 CommandListProxy* proxy,
                                                 ID3D12GraphicsCommandList* host,
                                                 UINT root_parameter_index,
                                                 D3D12_GPU_VIRTUAL_ADDRESS address);

void draw_set_compute_root_constant_buffer_view(DrawState& state,
                                                CommandListProxy* proxy,
                                                ID3D12GraphicsCommandList* host,
                                                UINT root_parameter_index,
                                                D3D12_GPU_VIRTUAL_ADDRESS address);

void draw_set_graphics_root_shader_resource_view(DrawState& state,
                                                 CommandListProxy* proxy,
                                                 ID3D12GraphicsCommandList* host,
                                                 UINT root_parameter_index,
                                                 D3D12_GPU_VIRTUAL_ADDRESS address);

void draw_set_compute_root_shader_resource_view(DrawState& state,
                                                CommandListProxy* proxy,
                                                ID3D12GraphicsCommandList* host,
                                                UINT root_parameter_index,
                                                D3D12_GPU_VIRTUAL_ADDRESS address);

void draw_set_graphics_root_unordered_access_view(DrawState& state,
                                                  CommandListProxy* proxy,
                                                  ID3D12GraphicsCommandList* host,
                                                  UINT root_parameter_index,
                                                  D3D12_GPU_VIRTUAL_ADDRESS address);

void draw_set_compute_root_unordered_access_view(DrawState& state,
                                                 CommandListProxy* proxy,
                                                 ID3D12GraphicsCommandList* host,
                                                 UINT root_parameter_index,
                                                 D3D12_GPU_VIRTUAL_ADDRESS address);

void draw_set_vertex_buffers(DrawState& state,
                             CommandListProxy* proxy,
                             ID3D12GraphicsCommandList* host,
                             UINT start_slot,
                             UINT count,
                             const D3D12_VERTEX_BUFFER_VIEW* views);

void draw_set_index_buffer(DrawState& state,
                           CommandListProxy* proxy,
                           ID3D12GraphicsCommandList* host,
                           const D3D12_INDEX_BUFFER_VIEW* view);

void draw_set_primitive_topology(DrawState& state,
                                 CommandListProxy* proxy,
                                 ID3D12GraphicsCommandList* host,
                                 UINT topology);

void draw_set_viewports(DrawState& state,
                        CommandListProxy* proxy,
                        ID3D12GraphicsCommandList* host,
                        UINT count,
                        const D3D12_VIEWPORT* viewports);

void draw_set_scissor_rects(DrawState& state,
                            CommandListProxy* proxy,
                            ID3D12GraphicsCommandList* host,
                            UINT count,
                            const D3D12_RECT* rects);

void draw_set_render_targets(DrawState& state,
                             CommandListProxy* proxy,
                             ID3D12GraphicsCommandList* host,
                             UINT count,
                             const UINT64* handles,
                             INT one_range,
                             const UINT64* depth);

void draw_clear_render_target_view(DrawState& state,
                                   CommandListProxy* proxy,
                                   ID3D12GraphicsCommandList* host,
                                   UINT64 handle,
                                   const FLOAT* color,
                                   UINT rect_count,
                                   const D3D12_RECT* rects);

void draw_clear_depth_stencil_view(DrawState& state,
                                   CommandListProxy* proxy,
                                   ID3D12GraphicsCommandList* host,
                                   UINT64 handle,
                                   D3D12_CLEAR_FLAGS flags,
                                   FLOAT depth,
                                   UINT8 stencil,
                                   UINT rect_count,
                                   const D3D12_RECT* rects);

HRESULT draw_clear_unordered_access_view_uint(DrawState& state,
                                              CommandListProxy* proxy,
                                              ID3D12GraphicsCommandList* host,
                                              UINT64 gpu_handle,
                                              UINT64 cpu_handle,
                                              void* resource,
                                              const UINT* values,
                                              UINT rect_count,
                                              const D3D12_RECT* rects);

HRESULT draw_clear_unordered_access_view_float(DrawState& state,
                                               CommandListProxy* proxy,
                                               ID3D12GraphicsCommandList* host,
                                               UINT64 gpu_handle,
                                               UINT64 cpu_handle,
                                               void* resource,
                                               const UINT* values,
                                               UINT rect_count,
                                               const D3D12_RECT* rects);

void draw_resource_barrier(DrawState& state,
                           CommandListProxy* proxy,
                           ID3D12GraphicsCommandList* host,
                           UINT count,
                           const D3D12_RESOURCE_BARRIER* barriers);

void draw_draw_instanced(DrawState& state,
                         CommandListProxy* proxy,
                         ID3D12GraphicsCommandList* host,
                         UINT vertex_count,
                         UINT instance_count,
                         UINT start_vertex,
                         UINT start_instance);

void draw_draw_indexed_instanced(DrawState& state,
                                 CommandListProxy* proxy,
                                 ID3D12GraphicsCommandList* host,
                                 UINT index_count,
                                 UINT instance_count,
                                 UINT start_index,
                                 INT base_vertex,
                                 UINT start_instance);

void draw_dispatch(DrawState& state,
                   CommandListProxy* proxy,
                   ID3D12GraphicsCommandList* host,
                   UINT thread_group_count_x,
                   UINT thread_group_count_y,
                   UINT thread_group_count_z);

HRESULT draw_dispatch_mesh(DrawState& state,
                           CommandListProxy* proxy,
                           ID3D12GraphicsCommandList* host,
                           UINT thread_group_count_x,
                           UINT thread_group_count_y,
                           UINT thread_group_count_z);

HRESULT draw_execute_indirect(DrawState& state,
                              CommandListProxy* proxy,
                              ID3D12GraphicsCommandList* host,
                              ID3D12CommandSignature* command_signature,
                              UINT max_command_count,
                              void* argument_buffer,
                              UINT64 argument_buffer_offset,
                              void* count_buffer,
                              UINT64 count_buffer_offset);

void draw_copy_buffer_region(DrawState& state,
                             CommandListProxy* proxy,
                             ID3D12GraphicsCommandList* host,
                             void* destination,
                             UINT64 destination_offset,
                             void* source,
                             UINT64 source_offset,
                             UINT64 bytes);

void draw_copy_texture_region(DrawState& state,
                              CommandListProxy* proxy,
                              ID3D12GraphicsCommandList* host,
                              const D3D12_TEXTURE_COPY_LOCATION* destination,
                              UINT destination_x,
                              UINT destination_y,
                              UINT destination_z,
                              const D3D12_TEXTURE_COPY_LOCATION* source,
                              const D3D12_BOX* source_box);

void draw_copy_resource(DrawState& state,
                        CommandListProxy* proxy,
                        ID3D12GraphicsCommandList* host,
                        void* destination,
                        void* source);

void draw_resolve_subresource(DrawState& state,
                              CommandListProxy* proxy,
                              ID3D12GraphicsCommandList* host,
                              void* destination,
                              UINT destination_subresource,
                              void* source,
                              UINT source_subresource,
                              DXGI_FORMAT format);

HRESULT draw_write32_bit_value_bottom_of_pipe_x(DrawState& state,
                                                CommandListProxy* proxy,
                                                ID3D12GraphicsCommandList* host,
                                                UINT64 address,
                                                UINT32 value,
                                                UINT32 flush,
                                                UINT32 flags);

HRESULT draw_write64_bit_value_bottom_of_pipe_x(DrawState& state,
                                                CommandListProxy* proxy,
                                                ID3D12GraphicsCommandList* host,
                                                UINT64 address,
                                                UINT64 value,
                                                UINT32 flush,
                                                UINT32 flags);

HRESULT draw_wait32_bit_value_x(DrawState& state,
                                CommandListProxy* proxy,
                                ID3D12GraphicsCommandList* host,
                                UINT64 address,
                                UINT32 comparison,
                                UINT32 reference,
                                UINT32 mask,
                                UINT32 flags);

HRESULT draw_wait64_bit_value_x(DrawState& state,
                                CommandListProxy* proxy,
                                ID3D12GraphicsCommandList* host,
                                UINT64 address,
                                UINT32 comparison,
                                UINT64 reference,
                                UINT32 flags);

HRESULT draw_unimplemented(DrawState& state,
                           CommandListProxy* proxy,
                           std::size_t legacy_slot,
                           const char* operation);

class DrawTranslator {
public:
    DrawTranslator(CommandListProxy* proxy, ID3D12GraphicsCommandList* host) noexcept;

    DrawState& state() noexcept;
    const DrawState& state() const noexcept;
    CommandListProxy* proxy() const noexcept;
    ID3D12GraphicsCommandList* host() const noexcept;
    void reset() noexcept;
    HRESULT unimplemented(std::size_t legacy_slot, const char* operation);

private:
    CommandListProxy* proxy_;
    ID3D12GraphicsCommandList* host_;
    DrawState state_{};
};

}
