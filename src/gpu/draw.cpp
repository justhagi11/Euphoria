#include <euphoria/gpu/draw.hpp>

#include <euphoria/gpu/resources.hpp>
#include <euphoria/log/log.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace euphoria::gpu {
namespace {

constexpr HRESULT k_not_implemented = static_cast<HRESULT>(0x80004001u);
constexpr UINT k_render_target_limit = static_cast<UINT>(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
constexpr UINT64 k_render_target_stride = 128;

constexpr UINT k_console_rectangles = 17;
constexpr UINT k_console_line_loop = 18;
constexpr UINT k_console_quads = 19;
constexpr UINT k_console_quad_strip = 20;

constexpr std::size_t slot_draw_instanced = 12;
constexpr std::size_t slot_draw_indexed_instanced = 13;
constexpr std::size_t slot_dispatch = 14;
constexpr std::size_t slot_copy_buffer_region = 15;
constexpr std::size_t slot_copy_texture_region = 16;
constexpr std::size_t slot_copy_resource = 17;
constexpr std::size_t slot_resolve_subresource = 19;
constexpr std::size_t slot_primitive_topology = 20;
constexpr std::size_t slot_viewports = 21;
constexpr std::size_t slot_scissor_rects = 22;
constexpr std::size_t slot_pipeline_state = 25;
constexpr std::size_t slot_resource_barrier = 26;
constexpr std::size_t slot_descriptor_heaps = 32;
constexpr std::size_t slot_compute_root_signature = 33;
constexpr std::size_t slot_graphics_root_signature = 34;
constexpr std::size_t slot_compute_root_descriptor_table = 35;
constexpr std::size_t slot_graphics_root_descriptor_table = 36;
constexpr std::size_t slot_compute_root_constant = 37;
constexpr std::size_t slot_graphics_root_constant = 38;
constexpr std::size_t slot_compute_root_constants = 39;
constexpr std::size_t slot_graphics_root_constants = 40;
constexpr std::size_t slot_compute_root_cbv = 41;
constexpr std::size_t slot_graphics_root_cbv = 42;
constexpr std::size_t slot_compute_root_srv = 43;
constexpr std::size_t slot_graphics_root_srv = 44;
constexpr std::size_t slot_compute_root_uav = 45;
constexpr std::size_t slot_graphics_root_uav = 46;
constexpr std::size_t slot_index_buffer = 47;
constexpr std::size_t slot_vertex_buffers = 48;
constexpr std::size_t slot_render_targets = 50;
constexpr std::size_t slot_clear_depth_stencil = 51;
constexpr std::size_t slot_clear_render_target = 52;
constexpr std::size_t slot_clear_uav_uint = 53;
constexpr std::size_t slot_clear_uav_float = 54;
constexpr std::size_t slot_execute_indirect = 59;
constexpr std::size_t slot_write32 = 69;
constexpr std::size_t slot_write64 = 70;
constexpr std::size_t slot_wait32 = 71;
constexpr std::size_t slot_wait64 = 72;
constexpr std::size_t slot_dispatch_mesh = 127;

struct SlotMarks {
    std::mutex mutex;
    std::array<bool, command_list_slot_count> logged{};
    std::array<bool, command_list_slot_count> skipped{};
    std::array<bool, command_list_slot_count> noted{};
};

SlotMarks& slot_marks()
{
    static SlotMarks value;
    return value;
}

bool take_mark(std::array<bool, command_list_slot_count>& marks, std::size_t slot)
{
    if (slot >= command_list_slot_count) {
        return true;
    }
    std::scoped_lock lock(slot_marks().mutex);
    bool& seen = marks[slot];
    const bool first = !seen;
    seen = true;
    return first;
}

bool first_log(std::size_t slot)
{
    return take_mark(slot_marks().logged, slot);
}

bool first_skip(std::size_t slot)
{
    return take_mark(slot_marks().skipped, slot);
}

bool first_note(std::size_t slot)
{
    return take_mark(slot_marks().noted, slot);
}

std::size_t slot_for(CommandLayout layout, std::size_t legacy_slot)
{
    return layout == CommandLayout::Newer && legacy_slot >= 59 ? legacy_slot + 2 : legacy_slot;
}

std::string hex_text(std::uint64_t value)
{
    char buffer[19]{};
    const int written = std::snprintf(buffer,
                                      sizeof(buffer),
                                      "0x%llx",
                                      static_cast<unsigned long long>(value));
    if (written <= 0) {
        return std::string("0x0");
    }
    return std::string(buffer, static_cast<std::size_t>(written));
}

std::string pointer_text(const void* value)
{
    return hex_text(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value)));
}

template <typename Detail>
void note(CommandLayout layout,
          std::size_t legacy_slot,
          const char* operation,
          Detail detail)
{
    const std::size_t slot = slot_for(layout, legacy_slot);
    if (!first_log(slot)) {
        return;
    }
    std::string text = "d3d12: draw ";
    text += operation;
    text += " slot ";
    text += std::to_string(slot);
    const std::string extra = detail();
    if (!extra.empty()) {
        text += ' ';
        text += extra;
    }
    log::info(text);
}

void warn_skip(CommandLayout layout, std::size_t legacy_slot, const char* what)
{
    const std::size_t slot = slot_for(layout, legacy_slot);
    if (!first_skip(slot)) {
        return;
    }
    std::string text = "d3d12: draw ";
    text += what;
    text += " skipped at slot ";
    text += std::to_string(slot);
    log::warn(text);
}

void warn_text(CommandLayout layout, std::size_t legacy_slot, const std::string& message)
{
    const std::size_t slot = slot_for(layout, legacy_slot);
    if (!first_note(slot)) {
        return;
    }
    std::string text = "d3d12: draw ";
    text += message;
    text += " (slot ";
    text += std::to_string(slot);
    text += ')';
    log::warn(text);
}

ID3D12GraphicsCommandList* resolve_host(CommandListProxy* proxy, ID3D12GraphicsCommandList* host) noexcept
{
    if (host != nullptr) {
        return host;
    }
    return unwrap_command_list(proxy);
}

ID3D12Resource* resolve_resource(void* object) noexcept
{
    if (object == nullptr) {
        return nullptr;
    }
    if (is_resource_proxy(object)) {
        return unwrap_resource(object);
    }
    return static_cast<ID3D12Resource*>(object);
}

ID3D12DescriptorHeap* resolve_descriptor_heap(void* object) noexcept
{
    if (object == nullptr) {
        return nullptr;
    }
    if (is_descriptor_heap_proxy(object)) {
        return unwrap_descriptor_heap(object);
    }
    return static_cast<ID3D12DescriptorHeap*>(object);
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(UINT64 value) noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = static_cast<SIZE_T>(value);
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(UINT64 value) noexcept
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = value;
    return handle;
}

D3D12_PRIMITIVE_TOPOLOGY host_topology(CommandLayout layout, UINT topology)
{
    switch (topology) {
    case k_console_rectangles:
    case k_console_line_loop:
    case k_console_quads:
    case k_console_quad_strip:
        warn_text(layout,
                  slot_primitive_topology,
                  "console primitive topology " + std::to_string(topology)
                      + " recorded as a triangle list");
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:
        return static_cast<D3D12_PRIMITIVE_TOPOLOGY>(topology);
    }
}

void store_constants(DrawState& state,
                     bool compute,
                     UINT root_parameter_index,
                     const void* values,
                     UINT count,
                     UINT offset)
{
    RootArgument* argument = root_argument(state, compute, root_parameter_index);
    if (argument == nullptr) {
        return;
    }
    if (argument->kind != RootArgumentKind::Constants) {
        *argument = RootArgument{};
        argument->kind = RootArgumentKind::Constants;
    }
    const std::size_t begin = static_cast<std::size_t>(offset);
    const std::size_t total = begin + static_cast<std::size_t>(count);
    if (begin >= draw_root_constant_limit || total > draw_root_constant_limit) {
        return;
    }
    const auto* source = static_cast<const std::uint32_t*>(values);
    for (std::size_t index = 0; index < count; ++index) {
        argument->constants[begin + index] = source[index];
    }
    const std::uint32_t end = static_cast<std::uint32_t>(total);
    if (argument->constant_count < end) {
        argument->constant_count = end;
    }
}

void store_view(DrawState& state,
                bool compute,
                UINT root_parameter_index,
                D3D12_GPU_VIRTUAL_ADDRESS address)
{
    RootArgument* argument = root_argument(state, compute, root_parameter_index);
    if (argument == nullptr) {
        return;
    }
    *argument = RootArgument{};
    argument->kind = RootArgumentKind::View;
    argument->view = address;
}

void set_root_signature(DrawState& state,
                        CommandListProxy* proxy,
                        ID3D12GraphicsCommandList* host,
                        ID3D12RootSignature* root_signature,
                        bool compute,
                        std::size_t legacy_slot)
{
    note(state.layout, legacy_slot, compute ? "set compute root signature" : "set graphics root signature", [root_signature] {
        return root_signature != nullptr ? pointer_text(root_signature) : std::string("null");
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, legacy_slot, "root signature with no host list");
        return;
    }
    if (root_signature == nullptr) {
        warn_skip(state.layout, legacy_slot, "null root signature");
        return;
    }
    if (compute) {
        list->SetComputeRootSignature(root_signature);
        state.compute_root_signature = root_signature;
    } else {
        list->SetGraphicsRootSignature(root_signature);
        state.graphics_root_signature = root_signature;
    }
}

void set_root_descriptor_table(DrawState& state,
                               CommandListProxy* proxy,
                               ID3D12GraphicsCommandList* host,
                               UINT root_parameter_index,
                               D3D12_GPU_DESCRIPTOR_HANDLE handle,
                               bool compute,
                               std::size_t legacy_slot)
{
    note(state.layout, legacy_slot, compute ? "set compute root descriptor table" : "set graphics root descriptor table", [root_parameter_index, handle] {
        return "parameter " + std::to_string(root_parameter_index) + " table " + hex_text(handle.ptr);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, legacy_slot, "descriptor table with no host list");
        return;
    }
    if (handle.ptr == 0) {
        warn_skip(state.layout, legacy_slot, "descriptor table with a null handle");
        return;
    }
    if (compute) {
        list->SetComputeRootDescriptorTable(root_parameter_index, handle);
    } else {
        list->SetGraphicsRootDescriptorTable(root_parameter_index, handle);
    }
    RootArgument* argument = root_argument(state, compute, root_parameter_index);
    if (argument != nullptr) {
        *argument = RootArgument{};
        argument->kind = RootArgumentKind::Table;
        argument->table = handle;
    }
}

enum class RootViewKind {
    ConstantBuffer,
    ShaderResource,
    UnorderedAccess,
};

void set_root_view(DrawState& state,
                   CommandListProxy* proxy,
                   ID3D12GraphicsCommandList* host,
                   UINT root_parameter_index,
                   D3D12_GPU_VIRTUAL_ADDRESS address,
                   bool compute,
                   std::size_t legacy_slot,
                   const char* operation,
                   RootViewKind kind)
{
    note(state.layout, legacy_slot, operation, [root_parameter_index, address] {
        return "parameter " + std::to_string(root_parameter_index) + " address " + hex_text(address);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, legacy_slot, "root view with no host list");
        return;
    }
    if (address == 0) {
        warn_skip(state.layout, legacy_slot, "root view with a null address");
        return;
    }
    if (compute) {
        switch (kind) {
        case RootViewKind::ConstantBuffer:
            list->SetComputeRootConstantBufferView(root_parameter_index, address);
            break;
        case RootViewKind::ShaderResource:
            list->SetComputeRootShaderResourceView(root_parameter_index, address);
            break;
        case RootViewKind::UnorderedAccess:
            list->SetComputeRootUnorderedAccessView(root_parameter_index, address);
            break;
        }
    } else {
        switch (kind) {
        case RootViewKind::ConstantBuffer:
            list->SetGraphicsRootConstantBufferView(root_parameter_index, address);
            break;
        case RootViewKind::ShaderResource:
            list->SetGraphicsRootShaderResourceView(root_parameter_index, address);
            break;
        case RootViewKind::UnorderedAccess:
            list->SetGraphicsRootUnorderedAccessView(root_parameter_index, address);
            break;
        }
    }
    store_view(state, compute, root_parameter_index, address);
}

HRESULT clear_unordered(DrawState& state,
                        CommandListProxy* proxy,
                        ID3D12GraphicsCommandList* host,
                        UINT64 gpu_descriptor,
                        UINT64 cpu_descriptor,
                        void* resource,
                        const UINT* values,
                        UINT rect_count,
                        const D3D12_RECT* rects,
                        bool as_float,
                        std::size_t legacy_slot)
{
    note(state.layout, legacy_slot, as_float ? "clear unordered access view float" : "clear unordered access view uint", [gpu_descriptor, cpu_descriptor] {
        return "gpu " + hex_text(gpu_descriptor) + " cpu " + hex_text(cpu_descriptor);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, legacy_slot, "unordered access clear with no host list");
        return E_FAIL;
    }
    if (gpu_descriptor == 0 || cpu_descriptor == 0) {
        warn_skip(state.layout, legacy_slot, "unordered access clear with a null descriptor");
        return E_INVALIDARG;
    }
    ID3D12Resource* target = resolve_resource(resource);
    if (target == nullptr) {
        warn_skip(state.layout, legacy_slot, "unordered access clear with a null resource");
        return E_INVALIDARG;
    }
    if (values == nullptr) {
        warn_skip(state.layout, legacy_slot, "unordered access clear with null values");
        return E_INVALIDARG;
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu = gpu_handle(gpu_descriptor);
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu = cpu_handle(cpu_descriptor);
    if (as_float) {
        list->ClearUnorderedAccessViewFloat(gpu,
                                            cpu,
                                            target,
                                            reinterpret_cast<const FLOAT*>(values),
                                            rect_count,
                                            rects);
    } else {
        list->ClearUnorderedAccessViewUint(gpu, cpu, target, values, rect_count, rects);
    }
    return S_OK;
}

}

void DrawState::reset() noexcept
{
    const CommandLayout kept = layout;
    *this = DrawState{};
    layout = kept;
}

void draw_set_pipeline_state(DrawState& state,
                             CommandListProxy* proxy,
                             ID3D12GraphicsCommandList* host,
                             ID3D12PipelineState* pipeline)
{
    note(state.layout, slot_pipeline_state, "set pipeline state", [pipeline] {
        return pipeline != nullptr ? "pipeline " + pointer_text(pipeline) : std::string("null");
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_pipeline_state, "pipeline state with no host list");
        return;
    }
    if (pipeline == nullptr) {
        warn_skip(state.layout, slot_pipeline_state, "null pipeline state");
        return;
    }
    list->SetPipelineState(pipeline);
    state.pipeline = pipeline;
}

void draw_set_descriptor_heaps(DrawState& state,
                               CommandListProxy* proxy,
                               ID3D12GraphicsCommandList* host,
                               UINT count,
                               void* const* heaps)
{
    note(state.layout, slot_descriptor_heaps, "set descriptor heaps", [count, heaps] {
        std::string text = "count " + std::to_string(count);
        if (heaps != nullptr && count > 0) {
            text += " first " + pointer_text(heaps[0]);
        }
        return text;
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_descriptor_heaps, "descriptor heaps with no host list");
        return;
    }
    if (heaps == nullptr || count == 0) {
        warn_skip(state.layout, slot_descriptor_heaps, "descriptor heaps without a heap array");
        return;
    }
    UINT wanted = count;
    if (wanted > 2) {
        warn_text(state.layout, slot_descriptor_heaps, "more than two descriptor heaps bound");
        wanted = 2;
    }
    std::array<ID3D12DescriptorHeap*, 2> resolved{};
    UINT bound = 0;
    for (UINT index = 0; index < wanted; ++index) {
        ID3D12DescriptorHeap* heap = resolve_descriptor_heap(heaps[index]);
        if (heap == nullptr) {
            warn_skip(state.layout, slot_descriptor_heaps, "null descriptor heap");
            break;
        }
        resolved[bound] = heap;
        ++bound;
    }
    if (bound == 0) {
        return;
    }
    list->SetDescriptorHeaps(bound, resolved.data());
    state.descriptor_heaps = resolved;
    state.descriptor_heap_count = bound;
}

void draw_set_graphics_root_signature(DrawState& state,
                                      CommandListProxy* proxy,
                                      ID3D12GraphicsCommandList* host,
                                      ID3D12RootSignature* root_signature)
{
    set_root_signature(state,
                       proxy,
                       host,
                       root_signature,
                       false,
                       slot_graphics_root_signature);
}

void draw_set_compute_root_signature(DrawState& state,
                                     CommandListProxy* proxy,
                                     ID3D12GraphicsCommandList* host,
                                     ID3D12RootSignature* root_signature)
{
    set_root_signature(state,
                       proxy,
                       host,
                       root_signature,
                       true,
                       slot_compute_root_signature);
}

void draw_set_graphics_root_descriptor_table(DrawState& state,
                                             CommandListProxy* proxy,
                                             ID3D12GraphicsCommandList* host,
                                             UINT root_parameter_index,
                                             D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
    set_root_descriptor_table(state,
                              proxy,
                              host,
                              root_parameter_index,
                              handle,
                              false,
                              slot_graphics_root_descriptor_table);
}

void draw_set_compute_root_descriptor_table(DrawState& state,
                                            CommandListProxy* proxy,
                                            ID3D12GraphicsCommandList* host,
                                            UINT root_parameter_index,
                                            D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
    set_root_descriptor_table(state,
                              proxy,
                              host,
                              root_parameter_index,
                              handle,
                              true,
                              slot_compute_root_descriptor_table);
}

void draw_set_graphics_root_constant(DrawState& state,
                                     CommandListProxy* proxy,
                                     ID3D12GraphicsCommandList* host,
                                     UINT root_parameter_index,
                                     UINT value,
                                     UINT offset)
{
    note(state.layout, slot_graphics_root_constant, "set graphics root constant", [root_parameter_index, value, offset] {
        return "parameter " + std::to_string(root_parameter_index) + " value "
            + hex_text(value) + " offset " + std::to_string(offset);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_graphics_root_constant, "root constant with no host list");
        return;
    }
    if (root_parameter_index >= draw_root_parameter_limit) {
        warn_skip(state.layout, slot_graphics_root_constant, "root constant past the parameter limit");
        return;
    }
    list->SetGraphicsRoot32BitConstant(root_parameter_index, value, offset);
    store_constants(state, false, root_parameter_index, &value, 1, offset);
}

void draw_set_compute_root_constant(DrawState& state,
                                    CommandListProxy* proxy,
                                    ID3D12GraphicsCommandList* host,
                                    UINT root_parameter_index,
                                    UINT value,
                                    UINT offset)
{
    note(state.layout, slot_compute_root_constant, "set compute root constant", [root_parameter_index, value, offset] {
        return "parameter " + std::to_string(root_parameter_index) + " value "
            + hex_text(value) + " offset " + std::to_string(offset);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_compute_root_constant, "root constant with no host list");
        return;
    }
    if (root_parameter_index >= draw_root_parameter_limit) {
        warn_skip(state.layout, slot_compute_root_constant, "root constant past the parameter limit");
        return;
    }
    list->SetComputeRoot32BitConstant(root_parameter_index, value, offset);
    store_constants(state, true, root_parameter_index, &value, 1, offset);
}

void draw_set_graphics_root_constants(DrawState& state,
                                      CommandListProxy* proxy,
                                      ID3D12GraphicsCommandList* host,
                                      UINT root_parameter_index,
                                      UINT count,
                                      const void* values,
                                      UINT offset)
{
    note(state.layout, slot_graphics_root_constants, "set graphics root constants", [root_parameter_index, count, offset] {
        return "parameter " + std::to_string(root_parameter_index) + " count "
            + std::to_string(count) + " offset " + std::to_string(offset);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_graphics_root_constants, "root constants with no host list");
        return;
    }
    if (values == nullptr || count == 0) {
        warn_skip(state.layout, slot_graphics_root_constants, "root constants without values");
        return;
    }
    if (root_parameter_index >= draw_root_parameter_limit) {
        warn_skip(state.layout, slot_graphics_root_constants, "root constants past the parameter limit");
        return;
    }
    list->SetGraphicsRoot32BitConstants(root_parameter_index, count, values, offset);
    store_constants(state, false, root_parameter_index, values, count, offset);
}

void draw_set_compute_root_constants(DrawState& state,
                                     CommandListProxy* proxy,
                                     ID3D12GraphicsCommandList* host,
                                     UINT root_parameter_index,
                                     UINT count,
                                     const void* values,
                                     UINT offset)
{
    note(state.layout, slot_compute_root_constants, "set compute root constants", [root_parameter_index, count, offset] {
        return "parameter " + std::to_string(root_parameter_index) + " count "
            + std::to_string(count) + " offset " + std::to_string(offset);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_compute_root_constants, "root constants with no host list");
        return;
    }
    if (values == nullptr || count == 0) {
        warn_skip(state.layout, slot_compute_root_constants, "root constants without values");
        return;
    }
    if (root_parameter_index >= draw_root_parameter_limit) {
        warn_skip(state.layout, slot_compute_root_constants, "root constants past the parameter limit");
        return;
    }
    list->SetComputeRoot32BitConstants(root_parameter_index, count, values, offset);
    store_constants(state, true, root_parameter_index, values, count, offset);
}

void draw_set_graphics_root_constant_buffer_view(DrawState& state,
                                                 CommandListProxy* proxy,
                                                 ID3D12GraphicsCommandList* host,
                                                 UINT root_parameter_index,
                                                 D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_view(state,
                  proxy,
                  host,
                  root_parameter_index,
                  address,
                  false,
                  slot_graphics_root_cbv,
                  "set graphics root cbv",
                  RootViewKind::ConstantBuffer);
}

void draw_set_compute_root_constant_buffer_view(DrawState& state,
                                                CommandListProxy* proxy,
                                                ID3D12GraphicsCommandList* host,
                                                UINT root_parameter_index,
                                                D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_view(state,
                  proxy,
                  host,
                  root_parameter_index,
                  address,
                  true,
                  slot_compute_root_cbv,
                  "set compute root cbv",
                  RootViewKind::ConstantBuffer);
}

void draw_set_graphics_root_shader_resource_view(DrawState& state,
                                                 CommandListProxy* proxy,
                                                 ID3D12GraphicsCommandList* host,
                                                 UINT root_parameter_index,
                                                 D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_view(state,
                  proxy,
                  host,
                  root_parameter_index,
                  address,
                  false,
                  slot_graphics_root_srv,
                  "set graphics root srv",
                  RootViewKind::ShaderResource);
}

void draw_set_compute_root_shader_resource_view(DrawState& state,
                                                CommandListProxy* proxy,
                                                ID3D12GraphicsCommandList* host,
                                                UINT root_parameter_index,
                                                D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_view(state,
                  proxy,
                  host,
                  root_parameter_index,
                  address,
                  true,
                  slot_compute_root_srv,
                  "set compute root srv",
                  RootViewKind::ShaderResource);
}

void draw_set_graphics_root_unordered_access_view(DrawState& state,
                                                  CommandListProxy* proxy,
                                                  ID3D12GraphicsCommandList* host,
                                                  UINT root_parameter_index,
                                                  D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_view(state,
                  proxy,
                  host,
                  root_parameter_index,
                  address,
                  false,
                  slot_graphics_root_uav,
                  "set graphics root uav",
                  RootViewKind::UnorderedAccess);
}

void draw_set_compute_root_unordered_access_view(DrawState& state,
                                                 CommandListProxy* proxy,
                                                 ID3D12GraphicsCommandList* host,
                                                 UINT root_parameter_index,
                                                 D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_view(state,
                  proxy,
                  host,
                  root_parameter_index,
                  address,
                  true,
                  slot_compute_root_uav,
                  "set compute root uav",
                  RootViewKind::UnorderedAccess);
}

void draw_set_vertex_buffers(DrawState& state,
                             CommandListProxy* proxy,
                             ID3D12GraphicsCommandList* host,
                             UINT start_slot,
                             UINT count,
                             const D3D12_VERTEX_BUFFER_VIEW* views)
{
    note(state.layout, slot_vertex_buffers, "set vertex buffers", [start_slot, count] {
        return "start " + std::to_string(start_slot) + " count " + std::to_string(count);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_vertex_buffers, "vertex buffers with no host list");
        return;
    }
    if (views == nullptr) {
        warn_skip(state.layout, slot_vertex_buffers, "vertex buffers without views");
        return;
    }
    const auto limit = static_cast<UINT>(draw_vertex_buffer_limit);
    if (start_slot >= limit) {
        warn_skip(state.layout, slot_vertex_buffers, "vertex buffers past the slot limit");
        return;
    }
    UINT bound = count;
    if (bound > limit - start_slot) {
        warn_text(state.layout, slot_vertex_buffers, "vertex buffer views past the slot limit");
        bound = limit - start_slot;
    }
    if (bound == 0) {
        return;
    }
    list->IASetVertexBuffers(start_slot, bound, views);
    for (UINT index = 0; index < bound; ++index) {
        const std::size_t target = static_cast<std::size_t>(start_slot + index);
        state.vertex_buffers[target] = views[index];
        state.vertex_bound[target] = true;
    }
    const UINT end = start_slot + bound;
    if (state.vertex_buffer_count < end) {
        state.vertex_buffer_count = end;
    }
}

void draw_set_index_buffer(DrawState& state,
                           CommandListProxy* proxy,
                           ID3D12GraphicsCommandList* host,
                           const D3D12_INDEX_BUFFER_VIEW* view)
{
    note(state.layout, slot_index_buffer, "set index buffer", [view] {
        if (view == nullptr) {
            return std::string("null");
        }
        return "location " + hex_text(view->BufferLocation) + " size "
            + std::to_string(view->SizeInBytes) + " format "
            + std::to_string(static_cast<std::uint32_t>(view->Format));
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_index_buffer, "index buffer with no host list");
        return;
    }
    if (view == nullptr) {
        list->IASetIndexBuffer(nullptr);
        state.index_buffer = D3D12_INDEX_BUFFER_VIEW{};
        state.index_bound = false;
        return;
    }
    if (view->BufferLocation == 0) {
        warn_skip(state.layout, slot_index_buffer, "index buffer with a null location");
        return;
    }
    list->IASetIndexBuffer(view);
    state.index_buffer = *view;
    state.index_bound = true;
}

void draw_set_primitive_topology(DrawState& state,
                                 CommandListProxy* proxy,
                                 ID3D12GraphicsCommandList* host,
                                 UINT topology)
{
    note(state.layout, slot_primitive_topology, "set primitive topology", [topology] {
        return "topology " + std::to_string(topology);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_primitive_topology, "primitive topology with no host list");
        return;
    }
    const D3D12_PRIMITIVE_TOPOLOGY translated = host_topology(state.layout, topology);
    list->IASetPrimitiveTopology(translated);
    state.console_topology = topology;
    state.topology = translated;
}

void draw_set_viewports(DrawState& state,
                        CommandListProxy* proxy,
                        ID3D12GraphicsCommandList* host,
                        UINT count,
                        const D3D12_VIEWPORT* viewports)
{
    note(state.layout, slot_viewports, "set viewports", [count] {
        return "count " + std::to_string(count);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_viewports, "viewports with no host list");
        return;
    }
    if (viewports == nullptr || count == 0) {
        warn_skip(state.layout, slot_viewports, "viewports without a viewport array");
        return;
    }
    const auto limit = static_cast<UINT>(draw_viewport_limit);
    UINT bound = count;
    if (bound > limit) {
        warn_text(state.layout, slot_viewports, "more than sixteen viewports bound");
        bound = limit;
    }
    list->RSSetViewports(bound, viewports);
    for (UINT index = 0; index < bound; ++index) {
        state.viewports[index] = viewports[index];
    }
    state.viewport_count = bound;
}

void draw_set_scissor_rects(DrawState& state,
                            CommandListProxy* proxy,
                            ID3D12GraphicsCommandList* host,
                            UINT count,
                            const D3D12_RECT* rects)
{
    note(state.layout, slot_scissor_rects, "set scissor rects", [count] {
        return "count " + std::to_string(count);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_scissor_rects, "scissor rects with no host list");
        return;
    }
    if (rects == nullptr || count == 0) {
        warn_skip(state.layout, slot_scissor_rects, "scissor rects without a rect array");
        return;
    }
    const auto limit = static_cast<UINT>(draw_scissor_limit);
    UINT bound = count;
    if (bound > limit) {
        warn_text(state.layout, slot_scissor_rects, "more than sixteen scissor rects bound");
        bound = limit;
    }
    list->RSSetScissorRects(bound, rects);
    for (UINT index = 0; index < bound; ++index) {
        state.scissors[index] = rects[index];
    }
    state.scissor_count = bound;
}

void draw_set_render_targets(DrawState& state,
                             CommandListProxy* proxy,
                             ID3D12GraphicsCommandList* host,
                             UINT count,
                             const UINT64* handles,
                             INT one_range,
                             const UINT64* depth)
{
    note(state.layout, slot_render_targets, "set render targets", [count, one_range, depth] {
        return "count " + std::to_string(count) + " one range " + std::to_string(one_range)
            + " depth " + (depth != nullptr ? hex_text(*depth) : std::string("none"));
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_render_targets, "render targets with no host list");
        return;
    }
    if (count != 0 && handles == nullptr) {
        warn_skip(state.layout, slot_render_targets, "render targets without handles");
        return;
    }
    UINT used = std::min(count, k_render_target_limit);
    if (count > k_render_target_limit) {
        warn_text(state.layout, slot_render_targets, "more than eight render targets bound");
    }
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    UINT bound = 0;
    for (UINT index = 0; index < used; ++index) {
        const UINT64 raw = one_range != 0 ? handles[0] + k_render_target_stride * index
                                          : handles[index];
        if (raw == 0) {
            warn_skip(state.layout, slot_render_targets, "render target with a null handle");
            break;
        }
        targets[bound] = cpu_handle(raw);
        ++bound;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE depth_handle{};
    D3D12_CPU_DESCRIPTOR_HANDLE* depth_pointer = nullptr;
    if (depth != nullptr && *depth != 0) {
        depth_handle = cpu_handle(*depth);
        depth_pointer = &depth_handle;
    }
    list->OMSetRenderTargets(bound, targets.data(), FALSE, depth_pointer);
    state.render_targets = targets;
    state.render_target_count = bound;
    state.depth_stencil = depth_handle;
    state.depth_stencil_bound = depth_pointer != nullptr;
}

void draw_clear_render_target_view(DrawState& state,
                                   CommandListProxy* proxy,
                                   ID3D12GraphicsCommandList* host,
                                   UINT64 handle,
                                   const FLOAT* color,
                                   UINT rect_count,
                                   const D3D12_RECT* rects)
{
    note(state.layout, slot_clear_render_target, "clear render target view", [handle] {
        return "view " + hex_text(handle);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_clear_render_target, "render target clear with no host list");
        return;
    }
    if (handle == 0) {
        warn_skip(state.layout, slot_clear_render_target, "render target clear with a null handle");
        return;
    }
    if (color == nullptr) {
        warn_skip(state.layout, slot_clear_render_target, "render target clear without a color");
        return;
    }
    list->ClearRenderTargetView(cpu_handle(handle), color, rect_count, rects);
}

void draw_clear_depth_stencil_view(DrawState& state,
                                   CommandListProxy* proxy,
                                   ID3D12GraphicsCommandList* host,
                                   UINT64 handle,
                                   D3D12_CLEAR_FLAGS flags,
                                   FLOAT depth,
                                   UINT8 stencil,
                                   UINT rect_count,
                                   const D3D12_RECT* rects)
{
    note(state.layout, slot_clear_depth_stencil, "clear depth stencil view", [handle, depth, stencil] {
        return "view " + hex_text(handle) + " depth " + std::to_string(depth) + " stencil "
            + std::to_string(static_cast<std::uint32_t>(stencil));
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_clear_depth_stencil, "depth stencil clear with no host list");
        return;
    }
    if (handle == 0) {
        warn_skip(state.layout, slot_clear_depth_stencil, "depth stencil clear with a null handle");
        return;
    }
    list->ClearDepthStencilView(cpu_handle(handle), flags, depth, stencil, rect_count, rects);
}

HRESULT draw_clear_unordered_access_view_uint(DrawState& state,
                                              CommandListProxy* proxy,
                                              ID3D12GraphicsCommandList* host,
                                              UINT64 gpu_descriptor,
                                              UINT64 cpu_descriptor,
                                              void* resource,
                                              const UINT* values,
                                              UINT rect_count,
                                              const D3D12_RECT* rects)
{
    return clear_unordered(state,
                           proxy,
                           host,
                           gpu_descriptor,
                           cpu_descriptor,
                           resource,
                           values,
                           rect_count,
                           rects,
                           false,
                           slot_clear_uav_uint);
}

HRESULT draw_clear_unordered_access_view_float(DrawState& state,
                                               CommandListProxy* proxy,
                                               ID3D12GraphicsCommandList* host,
                                               UINT64 gpu_descriptor,
                                               UINT64 cpu_descriptor,
                                               void* resource,
                                               const UINT* values,
                                               UINT rect_count,
                                               const D3D12_RECT* rects)
{
    return clear_unordered(state,
                           proxy,
                           host,
                           gpu_descriptor,
                           cpu_descriptor,
                           resource,
                           values,
                           rect_count,
                           rects,
                           true,
                           slot_clear_uav_float);
}

void draw_resource_barrier(DrawState& state,
                           CommandListProxy* proxy,
                           ID3D12GraphicsCommandList* host,
                           UINT count,
                           const D3D12_RESOURCE_BARRIER* barriers)
{
    note(state.layout, slot_resource_barrier, "resource barrier", [count] {
        return "count " + std::to_string(count);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_resource_barrier, "resource barrier with no host list");
        return;
    }
    if (count == 0) {
        return;
    }
    if (barriers == nullptr) {
        warn_skip(state.layout, slot_resource_barrier, "resource barrier without barriers");
        return;
    }
    std::vector<D3D12_RESOURCE_BARRIER> converted;
    converted.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        D3D12_RESOURCE_BARRIER barrier = barriers[index];
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            if (barrier.Transition.pResource != nullptr) {
                ID3D12Resource* resource = resolve_resource(barrier.Transition.pResource);
                if (resource == nullptr) {
                    warn_skip(state.layout, slot_resource_barrier, "transition barrier with a null resource");
                    continue;
                }
                barrier.Transition.pResource = resource;
            }
        } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            if (barrier.Aliasing.pResourceBefore != nullptr) {
                barrier.Aliasing.pResourceBefore = resolve_resource(barrier.Aliasing.pResourceBefore);
            }
            if (barrier.Aliasing.pResourceAfter != nullptr) {
                barrier.Aliasing.pResourceAfter = resolve_resource(barrier.Aliasing.pResourceAfter);
            }
        }
        converted.push_back(barrier);
    }
    if (converted.empty()) {
        return;
    }
    list->ResourceBarrier(static_cast<UINT>(converted.size()), converted.data());
}

void draw_draw_instanced(DrawState& state,
                         CommandListProxy* proxy,
                         ID3D12GraphicsCommandList* host,
                         UINT vertex_count,
                         UINT instance_count,
                         UINT start_vertex,
                         UINT start_instance)
{
    note(state.layout, slot_draw_instanced, "draw instanced", [vertex_count, instance_count, start_vertex, start_instance] {
        return "vertices " + std::to_string(vertex_count) + " instances "
            + std::to_string(instance_count) + " first vertex " + std::to_string(start_vertex)
            + " first instance " + std::to_string(start_instance);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_draw_instanced, "draw with no host list");
        return;
    }
    list->DrawInstanced(vertex_count, instance_count, start_vertex, start_instance);
}

void draw_draw_indexed_instanced(DrawState& state,
                                 CommandListProxy* proxy,
                                 ID3D12GraphicsCommandList* host,
                                 UINT index_count,
                                 UINT instance_count,
                                 UINT start_index,
                                 INT base_vertex,
                                 UINT start_instance)
{
    note(state.layout, slot_draw_indexed_instanced, "draw indexed instanced", [index_count, instance_count, start_index, base_vertex, start_instance] {
        return "indices " + std::to_string(index_count) + " instances "
            + std::to_string(instance_count) + " first index " + std::to_string(start_index)
            + " base vertex " + std::to_string(base_vertex) + " first instance "
            + std::to_string(start_instance);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_draw_indexed_instanced, "indexed draw with no host list");
        return;
    }
    list->DrawIndexedInstanced(index_count,
                               instance_count,
                               start_index,
                               base_vertex,
                               start_instance);
}

void draw_dispatch(DrawState& state,
                   CommandListProxy* proxy,
                   ID3D12GraphicsCommandList* host,
                   UINT thread_group_count_x,
                   UINT thread_group_count_y,
                   UINT thread_group_count_z)
{
    note(state.layout, slot_dispatch, "dispatch", [thread_group_count_x, thread_group_count_y, thread_group_count_z] {
        return "groups " + std::to_string(thread_group_count_x) + "x"
            + std::to_string(thread_group_count_y) + "x" + std::to_string(thread_group_count_z);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_dispatch, "dispatch with no host list");
        return;
    }
    list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z);
}

HRESULT draw_dispatch_mesh(DrawState& state,
                           CommandListProxy* proxy,
                           ID3D12GraphicsCommandList* host,
                           UINT thread_group_count_x,
                           UINT thread_group_count_y,
                           UINT thread_group_count_z)
{
    note(state.layout, slot_dispatch_mesh, "dispatch mesh", [thread_group_count_x, thread_group_count_y, thread_group_count_z] {
        return "groups " + std::to_string(thread_group_count_x) + "x"
            + std::to_string(thread_group_count_y) + "x" + std::to_string(thread_group_count_z);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_dispatch_mesh, "mesh dispatch with no host list");
        return E_FAIL;
    }
    ID3D12GraphicsCommandList6* mesh_list = nullptr;
    const HRESULT queried = list->QueryInterface(IID_PPV_ARGS(&mesh_list));
    if (FAILED(queried) || mesh_list == nullptr) {
        warn_text(state.layout, slot_dispatch_mesh, "mesh dispatch needs an ID3D12GraphicsCommandList6 host");
        return E_NOTIMPL;
    }
    mesh_list->DispatchMesh(thread_group_count_x,
                            thread_group_count_y,
                            thread_group_count_z);
    mesh_list->Release();
    return S_OK;
}

HRESULT draw_execute_indirect(DrawState& state,
                              CommandListProxy* proxy,
                              ID3D12GraphicsCommandList* host,
                              ID3D12CommandSignature* command_signature,
                              UINT max_command_count,
                              void* argument_buffer,
                              UINT64 argument_buffer_offset,
                              void* count_buffer,
                              UINT64 count_buffer_offset)
{
    note(state.layout, slot_execute_indirect, "execute indirect", [command_signature, max_command_count, argument_buffer_offset] {
        return "signature " + pointer_text(command_signature) + " max "
            + std::to_string(max_command_count) + " offset " + hex_text(argument_buffer_offset);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_execute_indirect, "indirect execution with no host list");
        return E_FAIL;
    }
    if (command_signature == nullptr) {
        warn_skip(state.layout, slot_execute_indirect, "indirect execution without a signature");
        return E_INVALIDARG;
    }
    ID3D12Resource* arguments = resolve_resource(argument_buffer);
    if (arguments == nullptr) {
        warn_skip(state.layout, slot_execute_indirect, "indirect execution without an argument buffer");
        return E_INVALIDARG;
    }
    ID3D12Resource* counted = resolve_resource(count_buffer);
    if (count_buffer != nullptr && counted == nullptr) {
        warn_skip(state.layout, slot_execute_indirect, "indirect execution without a count buffer");
        return E_INVALIDARG;
    }
    list->ExecuteIndirect(command_signature,
                          max_command_count,
                          arguments,
                          argument_buffer_offset,
                          counted,
                          count_buffer_offset);
    return S_OK;
}

void draw_copy_buffer_region(DrawState& state,
                             CommandListProxy* proxy,
                             ID3D12GraphicsCommandList* host,
                             void* destination,
                             UINT64 destination_offset,
                             void* source,
                             UINT64 source_offset,
                             UINT64 bytes)
{
    note(state.layout, slot_copy_buffer_region, "copy buffer region", [bytes, destination_offset, source_offset] {
        return "bytes " + std::to_string(bytes) + " destination offset "
            + hex_text(destination_offset) + " source offset " + hex_text(source_offset);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_copy_buffer_region, "buffer copy with no host list");
        return;
    }
    ID3D12Resource* destination_resource = resolve_resource(destination);
    ID3D12Resource* source_resource = resolve_resource(source);
    if (destination_resource == nullptr || source_resource == nullptr) {
        warn_skip(state.layout, slot_copy_buffer_region, "buffer copy with a null resource");
        return;
    }
    list->CopyBufferRegion(destination_resource,
                           destination_offset,
                           source_resource,
                           source_offset,
                           bytes);
}

void draw_copy_texture_region(DrawState& state,
                              CommandListProxy* proxy,
                              ID3D12GraphicsCommandList* host,
                              const D3D12_TEXTURE_COPY_LOCATION* destination,
                              UINT destination_x,
                              UINT destination_y,
                              UINT destination_z,
                              const D3D12_TEXTURE_COPY_LOCATION* source,
                              const D3D12_BOX* source_box)
{
    note(state.layout, slot_copy_texture_region, "copy texture region", [destination_x, destination_y, destination_z] {
        return "destination " + std::to_string(destination_x) + "," + std::to_string(destination_y)
            + "," + std::to_string(destination_z);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_copy_texture_region, "texture copy with no host list");
        return;
    }
    if (destination == nullptr || source == nullptr) {
        warn_skip(state.layout, slot_copy_texture_region, "texture copy without locations");
        return;
    }
    D3D12_TEXTURE_COPY_LOCATION destination_location = *destination;
    D3D12_TEXTURE_COPY_LOCATION source_location = *source;
    if (destination_location.pResource != nullptr) {
        destination_location.pResource = resolve_resource(destination_location.pResource);
    }
    if (source_location.pResource != nullptr) {
        source_location.pResource = resolve_resource(source_location.pResource);
    }
    if (destination_location.pResource == nullptr || source_location.pResource == nullptr) {
        warn_skip(state.layout, slot_copy_texture_region, "texture copy with a null resource");
        return;
    }
    list->CopyTextureRegion(&destination_location,
                            destination_x,
                            destination_y,
                            destination_z,
                            &source_location,
                            source_box);
}

void draw_copy_resource(DrawState& state,
                        CommandListProxy* proxy,
                        ID3D12GraphicsCommandList* host,
                        void* destination,
                        void* source)
{
    note(state.layout, slot_copy_resource, "copy resource", [] {
        return std::string();
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_copy_resource, "resource copy with no host list");
        return;
    }
    ID3D12Resource* destination_resource = resolve_resource(destination);
    ID3D12Resource* source_resource = resolve_resource(source);
    if (destination_resource == nullptr || source_resource == nullptr) {
        warn_skip(state.layout, slot_copy_resource, "resource copy with a null resource");
        return;
    }
    list->CopyResource(destination_resource, source_resource);
}

void draw_resolve_subresource(DrawState& state,
                              CommandListProxy* proxy,
                              ID3D12GraphicsCommandList* host,
                              void* destination,
                              UINT destination_subresource,
                              void* source,
                              UINT source_subresource,
                              DXGI_FORMAT format)
{
    note(state.layout, slot_resolve_subresource, "resolve subresource", [destination_subresource, source_subresource, format] {
        return "destination subresource " + std::to_string(destination_subresource)
            + " source subresource " + std::to_string(source_subresource) + " format "
            + std::to_string(static_cast<std::uint32_t>(format));
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_resolve_subresource, "resolve with no host list");
        return;
    }
    ID3D12Resource* destination_resource = resolve_resource(destination);
    ID3D12Resource* source_resource = resolve_resource(source);
    if (destination_resource == nullptr || source_resource == nullptr) {
        warn_skip(state.layout, slot_resolve_subresource, "resolve with a null resource");
        return;
    }
    list->ResolveSubresource(destination_resource,
                             destination_subresource,
                             source_resource,
                             source_subresource,
                             format);
}

HRESULT draw_write32_bit_value_bottom_of_pipe_x(DrawState& state,
                                                CommandListProxy* proxy,
                                                ID3D12GraphicsCommandList* host,
                                                UINT64 address,
                                                UINT32 value,
                                                UINT32 flush,
                                                UINT32 flags)
{
    note(state.layout, slot_write32, "write 32 bit value at the bottom of the pipe", [address, value, flush, flags] {
        return "address " + hex_text(address) + " value " + hex_text(value) + " flush "
            + std::to_string(flush) + " flags " + std::to_string(flags);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_write32, "bottom of pipe write with no host list");
        return E_FAIL;
    }
    if (address == 0) {
        warn_skip(state.layout, slot_write32, "bottom of pipe write with a null address");
        return E_INVALIDARG;
    }
    ValueWrite write{};
    write.address = address;
    write.value = value;
    write.bytes = 4;
    write.kind = ValueWriteKind::Value;
    state.writes.push_back(write);
    warn_text(state.layout,
              slot_write32,
              "bottom of pipe writes are recorded for the queue, the host list has no equivalent");
    return S_OK;
}

HRESULT draw_write64_bit_value_bottom_of_pipe_x(DrawState& state,
                                                CommandListProxy* proxy,
                                                ID3D12GraphicsCommandList* host,
                                                UINT64 address,
                                                UINT64 value,
                                                UINT32 flush,
                                                UINT32 flags)
{
    note(state.layout, slot_write64, "write 64 bit value at the bottom of the pipe", [address, value, flush, flags] {
        return "address " + hex_text(address) + " value " + hex_text(value) + " flush "
            + std::to_string(flush) + " flags " + std::to_string(flags);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_write64, "bottom of pipe write with no host list");
        return E_FAIL;
    }
    if (address == 0) {
        warn_skip(state.layout, slot_write64, "bottom of pipe write with a null address");
        return E_INVALIDARG;
    }
    constexpr UINT32 gpu_timestamp = 1;
    constexpr UINT32 system_timestamp = 2;
    ValueWrite write{};
    write.address = address;
    write.value = value;
    write.bytes = 8;
    if (flags == gpu_timestamp) {
        write.kind = ValueWriteKind::GpuTimestamp;
    } else if (flags == system_timestamp) {
        write.kind = ValueWriteKind::SystemTimestamp;
    } else {
        write.kind = ValueWriteKind::Value;
    }
    state.writes.push_back(write);
    warn_text(state.layout,
              slot_write64,
              "bottom of pipe writes are recorded for the queue, the host list has no equivalent");
    return S_OK;
}

HRESULT draw_wait32_bit_value_x(DrawState& state,
                                CommandListProxy* proxy,
                                ID3D12GraphicsCommandList* host,
                                UINT64 address,
                                UINT32 comparison,
                                UINT32 reference,
                                UINT32 mask,
                                UINT32 flags)
{
    note(state.layout, slot_wait32, "wait on a 32 bit value", [address, comparison, reference, mask, flags] {
        return "address " + hex_text(address) + " comparison " + std::to_string(comparison)
            + " reference " + hex_text(reference) + " mask " + hex_text(mask) + " flags "
            + std::to_string(flags);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_wait32, "value wait with no host list");
        return E_FAIL;
    }
    if (address == 0) {
        warn_skip(state.layout, slot_wait32, "value wait with a null address");
        return E_INVALIDARG;
    }
    ValueWait wait{};
    wait.address = address;
    wait.reference = reference;
    wait.mask = mask;
    wait.comparison = comparison;
    wait.bytes = 4;
    state.waits.push_back(wait);
    warn_text(state.layout,
              slot_wait32,
              "value waits are recorded for the queue, the host list has no equivalent");
    return S_OK;
}

HRESULT draw_wait64_bit_value_x(DrawState& state,
                                CommandListProxy* proxy,
                                ID3D12GraphicsCommandList* host,
                                UINT64 address,
                                UINT32 comparison,
                                UINT64 reference,
                                UINT32 flags)
{
    note(state.layout, slot_wait64, "wait on a 64 bit value", [address, comparison, reference, flags] {
        return "address " + hex_text(address) + " comparison " + std::to_string(comparison)
            + " reference " + hex_text(reference) + " flags " + std::to_string(flags);
    });
    ID3D12GraphicsCommandList* list = resolve_host(proxy, host);
    if (list == nullptr) {
        warn_skip(state.layout, slot_wait64, "value wait with no host list");
        return E_FAIL;
    }
    if (address == 0) {
        warn_skip(state.layout, slot_wait64, "value wait with a null address");
        return E_INVALIDARG;
    }
    ValueWait wait{};
    wait.address = address;
    wait.reference = reference;
    wait.mask = 0xffffffffffffffffull;
    wait.comparison = comparison;
    wait.bytes = 8;
    state.waits.push_back(wait);
    warn_text(state.layout,
              slot_wait64,
              "value waits are recorded for the queue, the host list has no equivalent");
    return S_OK;
}

HRESULT draw_unimplemented(DrawState& state,
                           CommandListProxy* proxy,
                           std::size_t legacy_slot,
                           const char* operation)
{
    (void)proxy;
    const std::size_t slot = slot_for(state.layout, legacy_slot);
    if (first_note(slot)) {
        std::string text = "d3d12: unimplemented command list slot ";
        text += std::to_string(slot);
        if (operation != nullptr && operation[0] != '\0') {
            text += " (";
            text += operation;
            text += ')';
        }
        log::warn(text);
    }
    return k_not_implemented;
}

DrawTranslator::DrawTranslator(CommandListProxy* proxy, ID3D12GraphicsCommandList* host) noexcept
    : proxy_(proxy)
    , host_(host != nullptr ? host : unwrap_command_list(proxy))
    , state_{}
{
}

DrawState& DrawTranslator::state() noexcept
{
    return state_;
}

const DrawState& DrawTranslator::state() const noexcept
{
    return state_;
}

CommandListProxy* DrawTranslator::proxy() const noexcept
{
    return proxy_;
}

ID3D12GraphicsCommandList* DrawTranslator::host() const noexcept
{
    return host_;
}

void DrawTranslator::reset() noexcept
{
    state_.reset();
}

HRESULT DrawTranslator::unimplemented(std::size_t legacy_slot, const char* operation)
{
    return draw_unimplemented(state_, proxy_, legacy_slot, operation);
}

}
