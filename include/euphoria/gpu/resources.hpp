#pragma once

#include <atomic>
#include <cstdint>

#include <d3d12.h>

namespace euphoria::gpu {

class ResourceProxy {
public:
    void** vtable;
    std::uint32_t bits;
    std::atomic<std::uint32_t> references;
    ID3D12Resource* host;
    void* owner_device;
    D3D12_RESOURCE_DESC description;
    std::uint64_t title_address = 0;
    bool cpu_mapped = false;

    ResourceProxy(ID3D12Resource* resource, void* device);
    ~ResourceProxy();

    ResourceProxy(const ResourceProxy&) = delete;
    ResourceProxy& operator=(const ResourceProxy&) = delete;
};

class HeapProxy {
public:
    void** vtable;
    std::uint32_t bits;
    std::atomic<std::uint32_t> references;
    ID3D12Heap* host;
    void* owner_device;
    D3D12_HEAP_DESC description;

    HeapProxy(ID3D12Heap* heap, void* device);
    ~HeapProxy();

    HeapProxy(const HeapProxy&) = delete;
    HeapProxy& operator=(const HeapProxy&) = delete;
};

class DescriptorHeapProxy {
public:
    void** vtable;
    std::uint32_t bits;
    std::atomic<std::uint32_t> references;
    ID3D12DescriptorHeap* host;
    void* owner_device;
    D3D12_DESCRIPTOR_HEAP_DESC description;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start;

    DescriptorHeapProxy(ID3D12DescriptorHeap* heap, void* device);
    ~DescriptorHeapProxy();

    DescriptorHeapProxy(const DescriptorHeapProxy&) = delete;
    DescriptorHeapProxy& operator=(const DescriptorHeapProxy&) = delete;
};

ResourceProxy* create_resource_proxy(ID3D12Resource* host, void* owner_device);
ResourceProxy* create_resource_proxy(ID3D12Resource* host, void* owner_device,
                                     const D3D12_RESOURCE_DESC& description);

HeapProxy* create_heap_proxy(ID3D12Heap* host, void* owner_device);
HeapProxy* create_heap_proxy(ID3D12Heap* host, void* owner_device,
                             const D3D12_HEAP_DESC& description);

DescriptorHeapProxy* create_descriptor_heap_proxy(ID3D12DescriptorHeap* host, void* owner_device);
DescriptorHeapProxy* create_descriptor_heap_proxy(ID3D12DescriptorHeap* host, void* owner_device,
                                                  const D3D12_DESCRIPTOR_HEAP_DESC& description);
DescriptorHeapProxy* create_descriptor_heap_proxy(ID3D12DescriptorHeap* host, void* owner_device,
                                                  const D3D12_DESCRIPTOR_HEAP_DESC& description,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE cpu_start,
                                                  D3D12_GPU_DESCRIPTOR_HANDLE gpu_start);

bool is_resource_proxy(const void* object) noexcept;
bool is_heap_proxy(const void* object) noexcept;
bool is_descriptor_heap_proxy(const void* object) noexcept;

ID3D12Resource* unwrap(ResourceProxy* proxy) noexcept;
ID3D12Heap* unwrap(HeapProxy* proxy) noexcept;
ID3D12DescriptorHeap* unwrap(DescriptorHeapProxy* proxy) noexcept;

ID3D12Resource* unwrap_resource(void* object) noexcept;

void track_resource_state(ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                          bool fixed = false) noexcept;
bool tracked_resource_state(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                            bool& fixed) noexcept;
void forget_resource_state(ID3D12Resource* resource) noexcept;

void register_host_buffer(ID3D12Resource* resource) noexcept;
void forget_gpu_ranges(ID3D12Resource* resource) noexcept;
void register_title_range(ID3D12Resource* resource, std::uint64_t title_address,
                          std::uint64_t size) noexcept;
bool translate_gpu_address(std::uint64_t address, D3D12_GPU_VIRTUAL_ADDRESS& host) noexcept;

void note_host_pipeline(void* pipeline) noexcept;
bool is_host_pipeline(void* pipeline) noexcept;
ID3D12Heap* unwrap_heap(void* object) noexcept;
ID3D12DescriptorHeap* unwrap_descriptor_heap(void* object) noexcept;

void** device_out_slot(void* first, void* second) noexcept;

void* create_inert_object(void* owner_device);
bool is_inert_object(const void* object) noexcept;

using ObjectUnwrapper = void* (*)(void*);
void set_object_unwrapper(ObjectUnwrapper unwrapper) noexcept;
void* unwrap_any(void* object) noexcept;

void* to_unknown(ResourceProxy* proxy) noexcept;
void* to_unknown(HeapProxy* proxy) noexcept;
void* to_unknown(DescriptorHeapProxy* proxy) noexcept;

}
