#pragma once

#include <cstdint>

#include <d3d12.h>

#include <euphoria/gpu/resources.hpp>

namespace euphoria::gpu {

inline constexpr std::uint32_t kMaxMirrorDescriptors = 65536;

enum class ConsoleDescriptorKind : std::uint32_t {
    Views = 0,
    Samplers = 1,
    RenderTargets = 2,
    DepthStencil = 3,
};

enum class ConsoleDescriptorView : std::uint32_t {
    None = 0,
    ConstantBuffer = 1,
    ShaderResource = 2,
    UnorderedAccess = 3,
    RenderTarget = 4,
    DepthStencil = 5,
    Sampler = 6,
};

struct ConsoleHeapRegistration {
    std::uint64_t start = 0;
    std::uint64_t size = 0;
    ConsoleDescriptorKind kind = ConsoleDescriptorKind::Views;
    std::uint32_t count = 0;
    D3D12_DESCRIPTOR_HEAP_TYPE host_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    DescriptorHeapProxy* proxy = nullptr;
};

std::uint32_t console_descriptor_size(ConsoleDescriptorKind kind) noexcept;
D3D12_DESCRIPTOR_HEAP_TYPE console_host_heap_type(ConsoleDescriptorKind kind) noexcept;

void set_descriptor_device(ID3D12Device* device) noexcept;
ID3D12Device* descriptor_device() noexcept;

bool register_console_heap(std::uint64_t start, std::uint64_t byte_size,
                           ConsoleDescriptorKind kind, std::uint32_t count,
                           DescriptorHeapProxy* proxy = nullptr) noexcept;
void unregister_console_heap(std::uint64_t start) noexcept;
void clear_console_heaps() noexcept;

const ConsoleHeapRegistration* lookup_console_heap(std::uint64_t address) noexcept;
bool lookup_console_heap(std::uint64_t address, ConsoleHeapRegistration& out) noexcept;

D3D12_CPU_DESCRIPTOR_HANDLE resolve_console_descriptor(std::uint64_t address) noexcept;
D3D12_GPU_DESCRIPTOR_HANDLE resolve_console_gpu(std::uint64_t address) noexcept;

std::uint32_t pc_format(std::uint32_t console_format) noexcept;
bool console_format_supported(std::uint32_t console_format) noexcept;

D3D12_SAMPLER_DESC translate_console_sampler(const std::uint32_t (&words)[27]) noexcept;

void write_constant_buffer_view(std::uint64_t handle, std::uint64_t gpu_address,
                                std::uint32_t size) noexcept;
void write_shader_resource_view(std::uint64_t handle, ID3D12Resource* resource,
                                const D3D12_SHADER_RESOURCE_VIEW_DESC* desc) noexcept;
void write_unordered_access_view(std::uint64_t handle, ID3D12Resource* resource,
                                 ID3D12Resource* counter,
                                 const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc) noexcept;
void write_render_target_view(std::uint64_t handle, ID3D12Resource* resource,
                              const D3D12_RENDER_TARGET_VIEW_DESC* desc) noexcept;
void write_depth_stencil_view(std::uint64_t handle, ID3D12Resource* resource,
                              const D3D12_DEPTH_STENCIL_VIEW_DESC* desc) noexcept;
void write_sampler(std::uint64_t handle, const D3D12_SAMPLER_DESC& desc) noexcept;
void write_sampler_words(std::uint64_t handle, const std::uint32_t (&words)[13]) noexcept;
void write_console_sampler(std::uint64_t handle, const std::uint32_t (&words)[27]) noexcept;

bool mirror_slot(std::uint64_t handle) noexcept;
bool mirror_slots(std::uint64_t first_handle, std::uint32_t count) noexcept;

ID3D12DescriptorHeap* console_host_heap(std::uint64_t address) noexcept;

void copy_console_descriptors_simple(std::uint32_t count, std::uint64_t dst,
                                     std::uint64_t src, ConsoleDescriptorKind kind) noexcept;
void copy_console_descriptors(std::uint32_t dst_range_count, const std::uint64_t* dst_starts,
                              const std::uint32_t* dst_sizes, std::uint32_t src_range_count,
                              const std::uint64_t* src_starts, const std::uint32_t* src_sizes,
                              ConsoleDescriptorKind kind) noexcept;

ConsoleDescriptorView descriptor_view_kind(std::uint64_t handle) noexcept;
bool is_console_handle(std::uint64_t address) noexcept;
DXGI_FORMAT depth_typeless_format(DXGI_FORMAT format) noexcept;
DXGI_FORMAT depth_view_format(DXGI_FORMAT format) noexcept;
ID3D12Resource* console_view_resource(std::uint64_t handle) noexcept;
bool console_handle_index(std::uint64_t address, std::uint32_t& index) noexcept;

ID3D12Resource* unwrap_console_resource(void* object) noexcept;
ID3D12DescriptorHeap* unwrap_console_descriptor_heap(void* object) noexcept;
bool is_console_descriptor_heap(const void* object) noexcept;

}
