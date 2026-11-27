#include <euphoria/formats/texture.hpp>
#include <euphoria/gpu/swapchain.hpp>
#include <euphoria/gpu/command.hpp>
#include <euphoria/gpu/descriptor_host.hpp>
#include <euphoria/gpu/present.hpp>
#include <euphoria/gpu/pipeline.hpp>
#include <euphoria/gpu/resources.hpp>
#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

namespace euphoria::shim {

void* title_allocate(std::size_t size, std::uint64_t attributes);
void title_release(void* address, std::uint64_t attributes);

namespace {

struct VirtualMemory {
    std::uint32_t flags;
    std::uint32_t page_table_pages;
    std::uint32_t preserved_bytes;
    std::uint64_t lowest;
    std::uint64_t highest;
};

constexpr std::int32_t kInvalidArgument = static_cast<std::int32_t>(0x80070057);
constexpr std::int32_t kNotImplemented = static_cast<std::int32_t>(0x80004001);

std::int32_t d3d12_configure_virtual_memory(VirtualMemory* config)
{
    if (config == nullptr) {
        return kInvalidArgument;
    }

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    config->lowest = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    config->highest = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    log::info("d3d12: virtual memory range 0x" + std::to_string(config->lowest) + " - 0x"
              + std::to_string(config->highest));
    return 0;
}

using SerializeVersionedFn = std::int32_t(__stdcall*)(const void*, void**, void**);

std::int32_t d3d12_serialize_versioned_root_signature(const void* description, void** blob, void** error)
{
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    if (d3d12 == nullptr) {
        return kNotImplemented;
    }
    const auto function = reinterpret_cast<SerializeVersionedFn>(
        GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature"));
    if (function == nullptr) {
        return kNotImplemented;
    }
    if (description == nullptr) {
        return static_cast<std::int32_t>(0x80070057);
    }
    std::uint8_t storage[48]{};
    std::memcpy(storage, description, sizeof(storage));
    auto* flags = reinterpret_cast<std::uint32_t*>(storage + 40);
    const std::uint32_t original = *flags;
    *flags = original & ~0xFF000000u;
    if (original != *flags) {
        log::info("d3d12: dropped console root signature flags 0x" + [&] {
            char text[16]{};
            std::snprintf(text, sizeof(text), "%08x", original);
            return std::string(text);
        }());
    }
    const std::int32_t result = function(storage, blob, error);
    if (result < 0) {
        log::warn("d3d12: D3D12SerializeVersionedRootSignature failed 0x"
                  + std::to_string(static_cast<std::uint32_t>(result)) + " version "
                  + std::to_string(*reinterpret_cast<const std::uint32_t*>(storage)));
    }
    return result;
}

using SerializeFn = std::int32_t(__stdcall*)(const void*, std::uint32_t, void**, void**);

std::int32_t d3d12_serialize_root_signature(const void* description, std::uint32_t version,
                                            void** blob, void** error)
{
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    if (d3d12 == nullptr || description == nullptr) {
        return kNotImplemented;
    }
    const auto function =
        reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"));
    if (function == nullptr) {
        return kNotImplemented;
    }
    std::uint8_t storage[40]{};
    std::memcpy(storage, description, sizeof(storage));
    auto* flags = reinterpret_cast<std::uint32_t*>(storage + 32);
    *flags &= ~0xFF000000u;
    const std::int32_t result = function(storage, version, blob, error);
    if (result < 0) {
        log::warn("d3d12: D3D12SerializeRootSignature failed 0x"
                  + std::to_string(static_cast<std::uint32_t>(result)));
    }
    return result;
}

std::uintptr_t d3d12_create_device(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("d3d12: D3D12XboxCreateDevice is not implemented yet");
    return static_cast<std::uintptr_t>(static_cast<std::uint32_t>(kNotImplemented));
}

std::uintptr_t d3d12_stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("d3d12: unimplemented shim called");
    return 0;
}

constexpr std::uint32_t kForwardSlots = 96;
constexpr std::uint32_t kTotalSlots = 256;

struct DeviceProxy {
    void** vtable;
    void* device;
};

void* wrap_object(void* object, DeviceProxy* device);
std::int32_t __stdcall proxy_residency(DeviceProxy* self, UINT count, void* const* objects);
bool skip_foreign_view(std::uint64_t handle);
std::int32_t __stdcall proxy_create_pipeline_stream(DeviceProxy* self,
                                                    const euphoria::gpu::StreamDesc* description,
                                                    const GUID& iid, void** out);
D3D12_RESOURCE_FLAGS sanitize_resource_flags(const D3D12_RESOURCE_DESC& desc,
                                             D3D12_RESOURCE_FLAGS flags);
void* unwrap_object(void* object);
void** object_vtable();
bool is_dxgi_device_iid(const GUID& iid) noexcept;
void* dxgi_device() noexcept;
void check_removed(const char* where);
std::int32_t __stdcall proxy_create_compute_pso_x(DeviceProxy* self,
                                                  const euphoria::gpu::ComputeHead* description,
                                                  std::uint32_t extension_count,
                                                  const void* extensions, const GUID& iid,
                                                  void** out);
void __stdcall proxy_copy_descriptors_simple(DeviceProxy* self, UINT count,
                                             D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                             D3D12_CPU_DESCRIPTOR_HANDLE source,
                                             D3D12_DESCRIPTOR_HEAP_TYPE type);
void __stdcall proxy_copy_descriptors(DeviceProxy* self, UINT destination_ranges,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE* destination_starts,
                                      const UINT* destination_sizes, UINT source_ranges,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE* source_starts,
                                      const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type);

std::int32_t __stdcall proxy_create_command_queue(DeviceProxy* self,
                                                  const D3D12_COMMAND_QUEUE_DESC* description,
                                                  const GUID& iid, void** out);
std::int32_t __stdcall proxy_create_allocator(DeviceProxy* self, D3D12_COMMAND_LIST_TYPE type,
                                              const GUID& iid, void** out);
std::int32_t __stdcall proxy_create_descriptor_heap(DeviceProxy* self,
                                                    const D3D12_DESCRIPTOR_HEAP_DESC* description,
                                                    const GUID& iid, void** out);
std::int32_t __stdcall proxy_set_frame_interval(DeviceProxy* self, std::uint32_t output,
                                                std::uint32_t length, std::uint32_t period,
                                                std::uint32_t flags);
std::int32_t __stdcall proxy_schedule_frame_event(DeviceProxy* self, std::uint32_t kind,
                                                  std::uint32_t offset, const void* list,
                                                  std::uint32_t flags);
std::int32_t __stdcall proxy_wait_frame_event(DeviceProxy* self, std::uint32_t kind,
                                              std::uint32_t timeout, const void* list,
                                              std::uint32_t flags, std::uint64_t* token);
std::int32_t __stdcall proxy_frame_statistics(DeviceProxy* self, std::uint64_t token,
                                              std::uint32_t kinds, std::uint32_t* count,
                                              void* out);
std::int32_t __stdcall proxy_register_page_pool(DeviceProxy* self, std::uint64_t address,
                                                std::uint32_t pages, std::uintptr_t* out);
std::int32_t __stdcall proxy_unregister_page_pool(DeviceProxy* self, std::uintptr_t handle);
void __stdcall proxy_create_cbv(DeviceProxy* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* description,
                                D3D12_CPU_DESCRIPTOR_HANDLE destination);
void __stdcall proxy_create_srv(DeviceProxy* self, void* resource, const void* description,
                                D3D12_CPU_DESCRIPTOR_HANDLE destination);
void __stdcall proxy_create_uav(DeviceProxy* self, void* resource, void* counter,
                                const void* description, D3D12_CPU_DESCRIPTOR_HANDLE destination);
D3D12_RESOURCE_ALLOCATION_INFO __stdcall proxy_resource_allocation_info(
    DeviceProxy* self, UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC* descriptions);
D3D12_RESOURCE_ALLOCATION_INFO __stdcall proxy_resource_allocation_info2(
    DeviceProxy* self, UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC1* descriptions,
    D3D12_RESOURCE_ALLOCATION_INFO1* each);
void __stdcall proxy_create_rtv(DeviceProxy* self, void* resource, const void* description,
                                D3D12_CPU_DESCRIPTOR_HANDLE destination);
void __stdcall proxy_create_dsv(DeviceProxy* self, void* resource, const void* description,
                                D3D12_CPU_DESCRIPTOR_HANDLE destination);
std::int32_t __stdcall proxy_create_fence(DeviceProxy* self, std::uint64_t value,
                                          D3D12_FENCE_FLAGS flags, const GUID& iid, void** out);
std::int32_t __stdcall proxy_create_graphics_pso(DeviceProxy* self,
                                                 const euphoria::gpu::GraphicsHead* description,
                                                 const GUID& iid, void** out);
std::int32_t __stdcall proxy_create_compute_pso(DeviceProxy* self,
                                                const euphoria::gpu::ComputeHead* description,
                                                const GUID& iid, void** out);
std::int32_t __stdcall proxy_create_root_signature(DeviceProxy* self, UINT node, const void* blob,
                                                   SIZE_T size, const GUID& iid, void** out);
std::int32_t __stdcall proxy_create_heap(DeviceProxy* self, const D3D12_HEAP_DESC* description,
                                         const GUID& iid, void** out);
std::int32_t __stdcall proxy_create_placed_resource(DeviceProxy* self, void* heap,
                                                    std::uint64_t offset,
                                                    const D3D12_RESOURCE_DESC* description,
                                                    D3D12_RESOURCE_STATES state,
                                                    const D3D12_CLEAR_VALUE* clear, const GUID& iid,
                                                    void** out);
std::int32_t __stdcall proxy_create_query_heap(DeviceProxy* self,
                                               const D3D12_QUERY_HEAP_DESC* description,
                                               const GUID& iid, void** out);
std::int32_t __stdcall proxy_create_command_signature(
    DeviceProxy* self, const D3D12_COMMAND_SIGNATURE_DESC* description, void* root_signature,
    const GUID& iid, void** out);
std::int32_t __stdcall proxy_check_feature_support(DeviceProxy* self, UINT feature, void* data,
                                                   UINT size);

std::uint32_t __stdcall proxy_increment_size(DeviceProxy* self, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    (void)self;
    const std::uint32_t value = euphoria::gpu::console_descriptor_size(
        static_cast<euphoria::gpu::ConsoleDescriptorKind>(type));
    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        log::info("d3d12: GetDescriptorHandleIncrementSize type "
                  + std::to_string(static_cast<int>(type)) + " -> console "
                  + std::to_string(value));
    }
    return value;
}
D3D12_HEAP_PROPERTIES __stdcall proxy_custom_heap_properties(DeviceProxy* self, UINT node,
                                                             D3D12_HEAP_TYPE type);

struct ConsoleQueueDesc {
    std::uint32_t kind;
    std::uint32_t flags;
    std::uint32_t engine_or_pipe;
    std::uint32_t queue;
};

struct ConsoleListDesc {
    std::uint32_t kind;
    std::uint32_t flags;
    std::uint32_t nodes;
    std::uint32_t barrier_batch;
};

std::int32_t __stdcall proxy_ok()
{
    return 0;
}

std::int32_t __stdcall proxy_not_implemented()
{
    return kNotImplemented;
}

void* charge_title_memory(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                         std::uint32_t heap_kind)
{
    if (device == nullptr) {
        return nullptr;
    }
    const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &desc);
    if (info.SizeInBytes == 0 || info.SizeInBytes == UINT64_MAX) {
        return nullptr;
    }
    const std::uint64_t alignment = info.Alignment == 0 ? 65536 : info.Alignment;
    std::uint64_t log2 = 0;
    while ((1ULL << log2) < alignment && log2 < 31) {
        ++log2;
    }
    const std::uint64_t pages = info.SizeInBytes >= (2ULL << 20) ? 2 : 1;
    const std::uint64_t memory = heap_kind == 1 ? 1 : 3;
    const std::uint64_t attributes = (128ULL << 16) | (pages << 14) | (log2 << 24) | (memory << 29);
    void* block = title_allocate(static_cast<std::size_t>(info.SizeInBytes), attributes);
    char text[160]{};
    std::snprintf(text, sizeof(text), "size %llu alignment %llu attributes 0x%llx -> %p",
                  static_cast<unsigned long long>(info.SizeInBytes),
                  static_cast<unsigned long long>(alignment),
                  static_cast<unsigned long long>(attributes), block);
    log::info(std::string("d3d12: resource memory from title allocator ") + text);
    return block;
}

std::int32_t __stdcall proxy_create_committed3(DeviceProxy* self,
                                               const D3D12_HEAP_PROPERTIES* heap,
                                               D3D12_HEAP_FLAGS flags, const void* desc_ptr,
                                               std::uint32_t initial_layout,
                                               const D3D12_CLEAR_VALUE* clear,
                                               void* protected_session,
                                               std::uint32_t castable_count,
                                               const DXGI_FORMAT* castable_formats,
                                               const GUID& iid, void** out)
{
    check_removed("create_committed3");
    (void)iid;
    (void)initial_layout;
    (void)protected_session;
    (void)castable_count;
    (void)castable_formats;
    if (self == nullptr || heap == nullptr || desc_ptr == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    D3D12_RESOURCE_DESC host = *static_cast<const D3D12_RESOURCE_DESC*>(desc_ptr);
    host.Alignment = 0;
    host.Layout = host.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                      ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                      : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    host.Format = static_cast<DXGI_FORMAT>(euphoria::gpu::pc_format(static_cast<std::uint32_t>(host.Format)));
    host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host.Flags & 0x3F);
    if ((host.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
        host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            host.Flags & ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                           | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                           | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS));
        host.Format = euphoria::gpu::depth_typeless_format(host.Format);
    }
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    default_heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    default_heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    default_heap.CreationNodeMask = 1;
    default_heap.VisibleNodeMask = 1;
    if (host.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        host.Height = 1;
        host.DepthOrArraySize = 1;
        host.MipLevels = 1;
        host.SampleDesc.Count = 1;
        host.SampleDesc.Quality = 0;
    } else {
        if (host.Height == 0) {
            host.Height = 1;
        }
        if (host.DepthOrArraySize == 0) {
            host.DepthOrArraySize = 1;
        }
        if (host.MipLevels == 0) {
            host.MipLevels = 1;
        }
        if (host.SampleDesc.Count == 0) {
            host.SampleDesc.Count = 1;
        }
        host.SampleDesc.Quality = 0;
    }
    using Function = std::int32_t(__stdcall*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                              const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[27]);
    const GUID& host_iid = __uuidof(ID3D12Resource);
    std::int32_t result = function(
        self->device, &default_heap, D3D12_HEAP_FLAG_NONE, &host, D3D12_RESOURCE_STATE_COMMON,
        nullptr, host_iid, out);
    if (result < 0 && host.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        D3D12_RESOURCE_DESC retry{};
        retry.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        retry.Width = 256;
        retry.Height = 256;
        retry.DepthOrArraySize = 1;
        retry.MipLevels = 1;
        retry.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        retry.SampleDesc.Count = 1;
        retry.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        retry.Flags = D3D12_RESOURCE_FLAG_NONE;
        log::warn("d3d12: CreateCommittedResource3 first attempt failed 0x"
                  + std::to_string(static_cast<std::uint32_t>(result))
                  + ", retrying with a plain R8G8B8A8 texture");
        result = function(self->device, &default_heap, D3D12_HEAP_FLAG_NONE, &retry,
                          D3D12_RESOURCE_STATE_COMMON, nullptr, host_iid, out);
        log::warn("d3d12: CreateCommittedResource3 retry -> 0x"
                  + std::to_string(static_cast<std::uint32_t>(result)));
        if (result >= 0) {
            host = retry;
        }
    }
    if (result >= 0 && out != nullptr) {
        auto* created = static_cast<ID3D12Resource*>(*out);
        auto* proxy = euphoria::gpu::create_resource_proxy(created, self, host);
        *out = proxy;
        D3D12_RESOURCE_DESC original = *static_cast<const D3D12_RESOURCE_DESC*>(desc_ptr);
        original.Layout = host.Layout;
        original.Flags = host.Flags;
        original.Alignment = 0;
        void* block = charge_title_memory(static_cast<ID3D12Device*>(self->device), original,
                                          static_cast<std::uint32_t>(heap->Type));
        if (block != nullptr) {
            proxy->title_address = reinterpret_cast<std::uint64_t>(block);
            if (host.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                euphoria::gpu::register_title_range(created, proxy->title_address, host.Width);
            }
        }
    } else {
        if (result == static_cast<std::int32_t>(0x887A0005u)) {
            const HRESULT reason =
                static_cast<ID3D12Device*>(self->device)->GetDeviceRemovedReason();
            log::warn("d3d12: device removed, reason 0x"
                      + std::to_string(static_cast<std::uint32_t>(reason)));
        }
        ID3D12InfoQueue* queue = nullptr;
        auto* host_device = static_cast<ID3D12Device*>(self->device);
        if (host_device->QueryInterface(__uuidof(ID3D12InfoQueue),
                                        reinterpret_cast<void**>(&queue)) >= 0
            && queue != nullptr) {
            const UINT64 stored = queue->GetNumStoredMessages();
            const UINT64 first = stored > 4 ? stored - 4 : 0;
            for (UINT64 index = first; index < stored; ++index) {
                SIZE_T length = 0;
                if (queue->GetMessage(index, nullptr, &length) < 0 || length == 0) {
                    continue;
                }
                std::vector<std::uint8_t> buffer(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if (queue->GetMessage(index, message, &length) >= 0
                    && message->pDescription != nullptr) {
                    log::warn("d3d12: debug layer: "
                              + std::string(message->pDescription));
                }
            }
            queue->Release();
        }
    }
    char text[192]{};
    std::snprintf(text, sizeof(text),
                  "dim %u width %llu height %u depth %u mip %u format %u sample %u layout %u "
                  "flags 0x%x -> hr 0x%08x",
                  static_cast<unsigned>(host.Dimension),
                  static_cast<unsigned long long>(host.Width),
                  static_cast<unsigned>(host.Height),
                  static_cast<unsigned>(host.DepthOrArraySize),
                  static_cast<unsigned>(host.MipLevels),
                  static_cast<unsigned>(host.Format),
                  static_cast<unsigned>(host.SampleDesc.Count),
                  static_cast<unsigned>(initial_layout),
                  static_cast<unsigned>(host.Flags), static_cast<std::uint32_t>(result));
    log::info(std::string("d3d12: CreateCommittedResource3 ") + text);
    return result;
}

std::int32_t __stdcall proxy_create_committed_x(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap,
                                                D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* description,
                                                D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                                const GUID& iid, void** out)
{
    check_removed("create_committed_x");
    (void)iid;
    if (self == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    using Function = std::int32_t(__stdcall*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                              const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[27]);
    const GUID& host_iid = __uuidof(ID3D12Resource);
    const std::int32_t result = function(self->device, heap, flags, description, state, clear,
                                         host_iid, out);
    if (result >= 0 && out != nullptr) {
        *out = euphoria::gpu::create_resource_proxy(
            static_cast<ID3D12Resource*>(*out), self, *description);
    }
    log::info("d3d12: CreateCommittedResourceX -> hr 0x"
              + [&] {
                    char text[16]{};
                    std::snprintf(text, sizeof(text), "%08x", static_cast<std::uint32_t>(result));
                    return std::string(text);
                }());
    return result;
}

std::int32_t __stdcall proxy_create_resource_at_address(
    DeviceProxy* self, std::uint64_t address, const D3D12_RESOURCE_DESC* description,
    std::uint32_t layout, const D3D12_CLEAR_VALUE* clear, const GUID& iid, void** out)
{
    check_removed("create_resource_at_address");
    (void)iid;
    (void)layout;
    if (self == nullptr || description == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC host = *description;
    host.Alignment = 0;
    if (host.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        host.Height = 1;
        host.DepthOrArraySize = 1;
        host.MipLevels = 1;
        host.SampleDesc.Count = 1;
        host.SampleDesc.Quality = 0;
    }
    host.Layout = host.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                      ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                      : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    host.Format = static_cast<DXGI_FORMAT>(euphoria::gpu::pc_format(static_cast<std::uint32_t>(host.Format)));
    host.Flags = sanitize_resource_flags(host,
                                         static_cast<D3D12_RESOURCE_FLAGS>(host.Flags & 0xFF));
    D3D12_CLEAR_VALUE depth_clear{};
    if ((host.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
        const DXGI_FORMAT view = euphoria::gpu::depth_view_format(host.Format);
        host.Format = euphoria::gpu::depth_typeless_format(host.Format);
        if (clear != nullptr) {
            depth_clear = *clear;
            depth_clear.Format = view;
            clear = &depth_clear;
        }
    }
    using Function = std::int32_t(__stdcall*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                              const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[27]);
    const GUID& host_iid = __uuidof(ID3D12Resource);
    std::int32_t result = function(self->device, &heap, D3D12_HEAP_FLAG_NONE, &host,
                                   D3D12_RESOURCE_STATE_COMMON, clear, host_iid, out);
    if (result < 0) {
        result = function(self->device, &heap, D3D12_HEAP_FLAG_NONE, &host,
                          D3D12_RESOURCE_STATE_COMMON, nullptr, host_iid, out);
    }
    for (const D3D12_RESOURCE_FLAGS shed :
         {D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL}) {
        if (result >= 0 || (host.Flags & shed) == 0) {
            continue;
        }
        host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host.Flags & ~shed);
        result = function(self->device, &heap, D3D12_HEAP_FLAG_NONE, &host,
                          D3D12_RESOURCE_STATE_COMMON, nullptr, host_iid, out);
        log::warn("d3d12: CreateResourceAtAddress retried without flag 0x"
                  + std::to_string(static_cast<unsigned>(shed)) + " -> 0x"
                  + std::to_string(static_cast<std::uint32_t>(result)));
    }
    if (result >= 0 && out != nullptr) {
        auto* created = static_cast<ID3D12Resource*>(*out);
        if (host.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            euphoria::gpu::register_title_range(created, address, host.Width);
        }
        auto* proxy = euphoria::gpu::create_resource_proxy(created, self, host);
        proxy->title_address = address;
        *out = proxy;
    }
    char text[200]{};
    std::snprintf(text, sizeof(text),
                  "addr %llx dim %u width %llu height %u depth %u mip %u format %u "
                  "sample %u flags 0x%x -> hr 0x%08x",
                  static_cast<unsigned long long>(address),
                  static_cast<unsigned>(description->Dimension),
                  static_cast<unsigned long long>(description->Width),
                  static_cast<unsigned>(description->Height),
                  static_cast<unsigned>(description->DepthOrArraySize),
                  static_cast<unsigned>(description->MipLevels),
                  static_cast<unsigned>(description->Format),
                  static_cast<unsigned>(description->SampleDesc.Count),
                  static_cast<unsigned>(description->Flags), static_cast<std::uint32_t>(result));
    log::info(std::string("d3d12: CreateResourceAtAddress ") + text);
    return result;
}

std::int32_t __stdcall proxy_create_command_list_x(DeviceProxy* self, const ConsoleListDesc* description,
                                                   void* allocator, void* pipeline, const GUID& iid,
                                                   void** out)
{
    check_removed("create_command_list_x");
    (void)iid;
    if (self == nullptr || description == nullptr || allocator == nullptr || out == nullptr) {
        return kInvalidArgument;
    }

    using Function = std::int32_t(__stdcall*)(void*, UINT, D3D12_COMMAND_LIST_TYPE, void*, void*,
                                              const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[12]);
    const UINT node = description->nodes == 0 ? 1 : description->nodes;
    const GUID& host_iid = __uuidof(ID3D12GraphicsCommandList);
    const std::int32_t result = function(
        self->device, node, static_cast<D3D12_COMMAND_LIST_TYPE>(description->kind),
        unwrap_object(allocator),
        euphoria::gpu::is_host_pipeline(unwrap_object(pipeline)) ? unwrap_object(pipeline)
                                                                  : nullptr,
        host_iid, out);
    if (result >= 0 && out != nullptr) {
        *out = euphoria::gpu::create_command_list(
            static_cast<ID3D12GraphicsCommandList*>(*out), allocator, self);
    }
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<std::uint32_t>(result));
    log::info("d3d12: CreateCommandListX kind " + std::to_string(description->kind) + " -> "
              + text);
    return result;
}

std::int32_t __stdcall proxy_create_command_queue_x(DeviceProxy* self, const ConsoleQueueDesc* description,
                                                    const GUID& iid, void** out)
{
    check_removed("create_command_queue_x");
    if (self == nullptr || description == nullptr || out == nullptr) {
        return kInvalidArgument;
    }

    D3D12_COMMAND_QUEUE_DESC host{};
    host.Type = static_cast<D3D12_COMMAND_LIST_TYPE>(description->kind);
    host.Priority = 0;
    host.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(description->flags & 1);
    host.NodeMask = 1;

    using Function = std::int32_t(__stdcall*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const GUID&,
                                              void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[8]);
    const std::int32_t result = function(self->device, &host, iid, out);
    if (result >= 0 && out != nullptr) {
        *out = euphoria::gpu::create_command_queue(static_cast<ID3D12CommandQueue*>(*out), static_cast<ID3D12Device*>(self->device), self);
    }
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<std::uint32_t>(result));
    log::info("d3d12: CreateCommandQueueX kind " + std::to_string(description->kind)
              + " flags " + std::to_string(description->flags) + " -> " + text);
    return result;
}

std::array<std::atomic<std::uint32_t>, kTotalSlots> g_slot_calls{};
std::array<std::atomic<std::uint32_t>, kTotalSlots> g_unknown_calls{};

struct PlacedRange {
    std::uint64_t begin;
    std::uint64_t end;
};

std::vector<PlacedRange> g_placed_ranges;

void add_placed_range(std::uint64_t begin, std::uint64_t size)
{
    g_placed_ranges.push_back({begin, begin + size});
}

bool in_placed_range(std::uint64_t address)
{
    for (const PlacedRange& range : g_placed_ranges) {
        if (address >= range.begin && address < range.end) {
            return true;
        }
    }
    return false;
}

ID3D12Device* g_watched_device = nullptr;
std::atomic<bool> g_removal_reported{false};
std::atomic<const char*> g_last_call{"none"};

const char* dred_op_name(D3D12_AUTO_BREADCRUMB_OP op)
{
    static constexpr const char* names[] = {
        "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
        "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource",
        "CopyTiles", "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView",
        "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present",
        "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames",
        "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
        "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2",
        "ProcessFrames1", "BuildRaytracingAccelerationStructure",
        "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand",
        "ExecuteMetaCommand", "EstimateMotion", "ResolveMotionVectorHeap",
        "SetPipelineState1", "InitializeExtensionCommand", "ExecuteExtensionCommand",
        "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata", "Barrier"};
    const auto index = static_cast<std::size_t>(op);
    if (index < std::size(names)) {
        return names[index];
    }
    thread_local char raw[32]{};
    std::snprintf(raw, sizeof(raw), "op %u", static_cast<unsigned>(index));
    return raw;
}

void report_dred(ID3D12Device* device)
{
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || dred == nullptr) {
        log::warn("d3d12: no DRED data on the removed device");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
        int lists = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode;
             node != nullptr && lists < 24; node = node->pNext, ++lists) {
            const std::uint32_t done = node->pLastBreadcrumbValue != nullptr
                                           ? *node->pLastBreadcrumbValue
                                           : 0;
            if (done >= node->BreadcrumbCount) {
                std::string ops;
                for (std::uint32_t index = 0; index < node->BreadcrumbCount && index < 12; ++index) {
                    ops += std::string(" ") + dred_op_name(node->pCommandHistory[index]);
                }
                char text[160]{};
                std::snprintf(text, sizeof(text), "d3d12: DRED finished list %p queue %p (%u ops):",
                              static_cast<void*>(node->pCommandList),
                              static_cast<void*>(node->pCommandQueue), node->BreadcrumbCount);
                log::error(text + ops);
                continue;
            }
            char text[200]{};
            std::snprintf(text, sizeof(text),
                          "d3d12: DRED unfinished list %p queue %p: %u of %u ops done", 
                          static_cast<void*>(node->pCommandList),
                          static_cast<void*>(node->pCommandQueue), done, node->BreadcrumbCount);
            log::error(text);
            const std::uint32_t first = done > 6 ? done - 6 : 0;
            const std::uint32_t last = std::min(node->BreadcrumbCount, done + 16);
            for (std::uint32_t index = first; index < last; ++index) {
                log::error(std::string(index == done ? "  -> " : "     ") + std::to_string(index)
                           + " " + dred_op_name(node->pCommandHistory[index]));
            }
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault))) {
        char text[96]{};
        std::snprintf(text, sizeof(text), "d3d12: DRED page fault at gpu va 0x%llx",
                      static_cast<unsigned long long>(fault.PageFaultVA));
        log::error(text);
        int shown = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* node = fault.pHeadExistingAllocationNode;
             node != nullptr && shown < 6; node = node->pNext, ++shown) {
            log::error("d3d12:   existing allocation type " + std::to_string(node->AllocationType));
        }
        shown = 0;
        for (const D3D12_DRED_ALLOCATION_NODE1* node = fault.pHeadRecentFreedAllocationNode;
             node != nullptr && shown < 6; node = node->pNext, ++shown) {
            log::error("d3d12:   recently freed allocation type "
                       + std::to_string(node->AllocationType));
        }
    }
    dred->Release();
}

void check_removed(const char* where)
{
    if (g_watched_device == nullptr || g_removal_reported.load(std::memory_order_relaxed)) {
        g_last_call.store(where, std::memory_order_relaxed);
        return;
    }
    const HRESULT reason = g_watched_device->GetDeviceRemovedReason();
    if (reason != S_OK) {
        g_removal_reported.store(true, std::memory_order_relaxed);
        char text[160]{};
        std::snprintf(text, sizeof(text), "reason 0x%08x, last call %s, now %s",
                      static_cast<std::uint32_t>(reason),
                      g_last_call.load(std::memory_order_relaxed), where);
        log::error(std::string("d3d12: device removed detected: ") + text);
        report_dred(g_watched_device);
    }
    g_last_call.store(where, std::memory_order_relaxed);
}

thread_local char g_slot_name[32]{};

void __stdcall log_device_slot(std::uint64_t slot)
{
    if (slot >= kTotalSlots) {
        return;
    }
    std::snprintf(g_slot_name, sizeof(g_slot_name), "device slot %llu",
                  static_cast<unsigned long long>(slot));
    check_removed(g_slot_name);
    const std::uint32_t count = ++g_slot_calls[static_cast<std::size_t>(slot)];
    if (count <= 4) {
        log::info("d3d12: device slot " + std::to_string(slot) + " called");
    }
}

std::int32_t __stdcall log_unknown_slot(std::uint64_t slot)
{
    if (slot < kTotalSlots) {
        const std::uint32_t count = ++g_unknown_calls[static_cast<std::size_t>(slot)];
        if (count <= 4) {
            log::warn("d3d12: unimplemented console device slot " + std::to_string(slot));
        }
    }
    return kNotImplemented;
}

std::int32_t __stdcall proxy_query_interface(DeviceProxy* self, const GUID& iid, void** out)
{
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }
    if (is_dxgi_device_iid(iid)) {
        *out = dxgi_device();
        return 0;
    }
    *out = self;
    return 0;
}

std::uint32_t __stdcall proxy_add_ref(DeviceProxy*)
{
    return 2;
}

std::uint32_t __stdcall proxy_release(DeviceProxy*)
{
    return 1;
}

void append(std::vector<std::uint8_t>& bytes, std::initializer_list<std::uint8_t> values)
{
    bytes.insert(bytes.end(), values.begin(), values.end());
}

std::uint8_t* allocate_thunk()
{
    return static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
}

std::uint8_t* make_forward_thunk(std::uint32_t slot)
{
    std::uint8_t* code = allocate_thunk();
    if (code == nullptr) {
        return nullptr;
    }

    const std::uint64_t id = slot;
    const std::uint64_t handler = reinterpret_cast<std::uint64_t>(&log_device_slot);

    std::vector<std::uint8_t> bytes;
    bytes.reserve(64);
    append(bytes, {0x51, 0x52, 0x41, 0x50, 0x41, 0x51});
    append(bytes, {0x48, 0x83, 0xEC, 0x28});
    append(bytes, {0x48, 0xB9});
    const auto id_bytes = reinterpret_cast<const std::uint8_t*>(&id);
    bytes.insert(bytes.end(), id_bytes, id_bytes + 8);
    append(bytes, {0x48, 0xB8});
    const auto handler_bytes = reinterpret_cast<const std::uint8_t*>(&handler);
    bytes.insert(bytes.end(), handler_bytes, handler_bytes + 8);
    append(bytes, {0xFF, 0xD0});
    append(bytes, {0x48, 0x83, 0xC4, 0x28});
    append(bytes, {0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});
    append(bytes, {0x4C, 0x8B, 0x51, 0x08});
    append(bytes, {0x4C, 0x89, 0xD1});
    append(bytes, {0x49, 0x8B, 0x02});
    append(bytes, {0x48, 0x8B, 0x80});
    const std::uint32_t offset = slot * 8;
    const auto offset_bytes = reinterpret_cast<const std::uint8_t*>(&offset);
    bytes.insert(bytes.end(), offset_bytes, offset_bytes + 4);
    append(bytes, {0xFF, 0xE0});

    std::memcpy(code, bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), code, bytes.size());
    return code;
}

std::uint8_t* make_hidden_return_thunk(std::uint32_t slot)
{
    std::uint8_t* code = allocate_thunk();
    if (code == nullptr) {
        return nullptr;
    }

    std::vector<std::uint8_t> bytes;
    append(bytes, {0x48, 0x8B, 0x52, 0x08});
    append(bytes, {0x48, 0x8B, 0x02});
    append(bytes, {0x48, 0x8B, 0x80});
    const std::uint32_t offset = slot * 8;
    const auto offset_bytes = reinterpret_cast<const std::uint8_t*>(&offset);
    bytes.insert(bytes.end(), offset_bytes, offset_bytes + 4);
    append(bytes, {0xFF, 0xE0});

    std::memcpy(code, bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), code, bytes.size());
    return code;
}

std::uint8_t* make_unknown_thunk(std::uint32_t slot)
{
    std::uint8_t* code = allocate_thunk();
    if (code == nullptr) {
        return nullptr;
    }

    const std::uint64_t id = slot;
    const std::uint64_t handler = reinterpret_cast<std::uint64_t>(&log_unknown_slot);

    std::vector<std::uint8_t> bytes;
    append(bytes, {0x48, 0x83, 0xEC, 0x28});
    append(bytes, {0x48, 0xB9});
    const auto id_bytes = reinterpret_cast<const std::uint8_t*>(&id);
    bytes.insert(bytes.end(), id_bytes, id_bytes + 8);
    append(bytes, {0x48, 0xB8});
    const auto handler_bytes = reinterpret_cast<const std::uint8_t*>(&handler);
    bytes.insert(bytes.end(), handler_bytes, handler_bytes + 8);
    append(bytes, {0xFF, 0xD0});
    append(bytes, {0x48, 0x83, 0xC4, 0x28});
    append(bytes, {0xB8, 0x01, 0x00, 0x40, 0x80});
    append(bytes, {0xC3});

    std::memcpy(code, bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), code, bytes.size());
    return code;
}

void** device_vtable()
{
    static void** table = [] {
        auto* built = new void*[kTotalSlots]{};
        built[0] = reinterpret_cast<void*>(&proxy_query_interface);
        built[1] = reinterpret_cast<void*>(&proxy_add_ref);
        built[2] = reinterpret_cast<void*>(&proxy_release);
        const auto host_compatible = [](std::uint32_t slot) {
            return (slot >= 3 && slot <= 7) || (slot >= 30 && slot <= 33) || slot == 37
                || slot == 38 || slot == 40 || slot == 42 || slot == 43;
        };
        for (std::uint32_t slot = 3; slot < kTotalSlots; ++slot) {
            built[slot] = host_compatible(slot) && slot < kForwardSlots
                              ? reinterpret_cast<void*>(make_forward_thunk(slot))
                              : reinterpret_cast<void*>(make_unknown_thunk(slot));
        }

        built[8] = reinterpret_cast<void*>(&proxy_create_command_queue);
        built[9] = reinterpret_cast<void*>(&proxy_create_allocator);
        built[10] = reinterpret_cast<void*>(&proxy_create_graphics_pso);
        built[11] = reinterpret_cast<void*>(&proxy_create_compute_pso);
        built[13] = reinterpret_cast<void*>(&proxy_check_feature_support);
        built[14] = reinterpret_cast<void*>(&proxy_create_descriptor_heap);
        built[15] = reinterpret_cast<void*>(&proxy_increment_size);
        built[16] = reinterpret_cast<void*>(&proxy_create_root_signature);
        built[17] = reinterpret_cast<void*>(&proxy_create_cbv);
        built[18] = reinterpret_cast<void*>(&proxy_create_srv);
        built[19] = reinterpret_cast<void*>(&proxy_create_uav);
        built[20] = reinterpret_cast<void*>(&proxy_create_rtv);
        built[21] = reinterpret_cast<void*>(&proxy_create_dsv);
        built[23] = reinterpret_cast<void*>(&proxy_copy_descriptors);
        built[24] = reinterpret_cast<void*>(&proxy_copy_descriptors_simple);
        built[25] = reinterpret_cast<void*>(&proxy_resource_allocation_info);
        built[26] = reinterpret_cast<void*>(&proxy_custom_heap_properties);
        built[27] = reinterpret_cast<void*>(&proxy_create_committed_x);
        built[28] = reinterpret_cast<void*>(&proxy_create_heap);
        built[29] = reinterpret_cast<void*>(&proxy_create_placed_resource);
        built[34] = reinterpret_cast<void*>(&proxy_residency);
        built[35] = reinterpret_cast<void*>(&proxy_residency);
        built[36] = reinterpret_cast<void*>(&proxy_create_fence);
        built[39] = reinterpret_cast<void*>(&proxy_create_query_heap);
        built[41] = reinterpret_cast<void*>(&proxy_create_command_signature);
        built[48] = reinterpret_cast<void*>(&proxy_create_command_queue_x);
        built[61] = reinterpret_cast<void*>(&proxy_not_implemented);
        built[62] = reinterpret_cast<void*>(&proxy_create_compute_pso_x);
        built[69] = reinterpret_cast<void*>(&proxy_ok);
        built[70] = reinterpret_cast<void*>(&proxy_not_implemented);
        built[71] = reinterpret_cast<void*>(&proxy_not_implemented);
        built[73] = reinterpret_cast<void*>(&proxy_create_command_list_x);
        built[74] = reinterpret_cast<void*>(&proxy_register_page_pool);
        built[75] = reinterpret_cast<void*>(&proxy_unregister_page_pool);
        built[76] = reinterpret_cast<void*>(&proxy_create_committed_x);
        built[78] = reinterpret_cast<void*>(&proxy_set_frame_interval);
        built[79] = reinterpret_cast<void*>(&proxy_schedule_frame_event);
        built[80] = reinterpret_cast<void*>(&proxy_wait_frame_event);
        built[81] = reinterpret_cast<void*>(&proxy_frame_statistics);
        built[86] = reinterpret_cast<void*>(&proxy_create_pipeline_stream);
        built[108] = reinterpret_cast<void*>(&proxy_resource_allocation_info2);
        built[127] = reinterpret_cast<void*>(&proxy_create_committed3);
        built[133] = reinterpret_cast<void*>(&proxy_create_resource_at_address);
        return built;
    }();
    return table;
}

void* wrap_device(void* device)
{
    euphoria::gpu::set_descriptor_device(static_cast<ID3D12Device*>(device));
    euphoria::gpu::set_object_unwrapper(&unwrap_object);
    g_watched_device = static_cast<ID3D12Device*>(device);
    return new DeviceProxy{device_vtable(), device};
}

struct ObjectProxy {
    void** vtable;
    void* object;
    DeviceProxy* device;
};

void __stdcall log_object_slot(std::uint64_t slot)
{
    if (slot >= kTotalSlots) {
        return;
    }
    const std::uint32_t count = ++g_slot_calls[static_cast<std::size_t>(slot)];
    if (count <= 2) {
        log::info("d3d12: object slot " + std::to_string(slot) + " called");
    }
}

std::int32_t __stdcall object_query_interface(ObjectProxy* self, const GUID& iid, void** out)
{
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }
    (void)iid;
    *out = self;
    return 0;
}

std::uint32_t __stdcall object_add_ref(ObjectProxy*)
{
    return 2;
}

std::uint32_t __stdcall object_release(ObjectProxy*)
{
    return 1;
}

std::int32_t __stdcall object_get_device(ObjectProxy* self, void* first, void* second)
{
    void** out = euphoria::gpu::device_out_slot(first, second);
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }
    *out = self->device;
    return 0;
}

std::uint8_t* make_object_forward_thunk(std::uint32_t slot)
{
    std::uint8_t* code = allocate_thunk();
    if (code == nullptr) {
        return nullptr;
    }
    const std::uint64_t id = slot;
    const std::uint64_t handler = reinterpret_cast<std::uint64_t>(&log_object_slot);
    std::vector<std::uint8_t> bytes;
    append(bytes, {0x51, 0x52, 0x41, 0x50, 0x41, 0x51});
    append(bytes, {0x48, 0x83, 0xEC, 0x28});
    append(bytes, {0x48, 0xB9});
    const auto id_bytes = reinterpret_cast<const std::uint8_t*>(&id);
    bytes.insert(bytes.end(), id_bytes, id_bytes + 8);
    append(bytes, {0x48, 0xB8});
    const auto handler_bytes = reinterpret_cast<const std::uint8_t*>(&handler);
    bytes.insert(bytes.end(), handler_bytes, handler_bytes + 8);
    append(bytes, {0xFF, 0xD0});
    append(bytes, {0x48, 0x83, 0xC4, 0x28});
    append(bytes, {0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});
    append(bytes, {0x4C, 0x8B, 0x51, 0x08});
    append(bytes, {0x4C, 0x89, 0xD1});
    append(bytes, {0x49, 0x8B, 0x02});
    append(bytes, {0x48, 0x8B, 0x80});
    const std::uint32_t offset = slot * 8;
    const auto offset_bytes = reinterpret_cast<const std::uint8_t*>(&offset);
    bytes.insert(bytes.end(), offset_bytes, offset_bytes + 4);
    append(bytes, {0xFF, 0xE0});
    std::memcpy(code, bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), code, bytes.size());
    return code;
}

void** object_vtable()
{
    static void** table = [] {
        auto* built = new void*[kTotalSlots]{};
        built[0] = reinterpret_cast<void*>(&object_query_interface);
        built[1] = reinterpret_cast<void*>(&object_add_ref);
        built[2] = reinterpret_cast<void*>(&object_release);
        built[7] = reinterpret_cast<void*>(&object_get_device);
        for (std::uint32_t slot = 3; slot < kTotalSlots; ++slot) {
            if (slot == 7) {
                continue;
            }
            built[slot] = slot < kForwardSlots
                            ? reinterpret_cast<void*>(make_object_forward_thunk(slot))
                            : reinterpret_cast<void*>(&proxy_not_implemented);
        }
        return built;
    }();
    return table;
}

void* wrap_object(void* object, DeviceProxy* device)
{
    return object == nullptr ? nullptr : new ObjectProxy{object_vtable(), object, device};
}

void* unwrap_object(void* object)
{
    if (object == nullptr) {
        return nullptr;
    }
    if (*reinterpret_cast<void***>(object) == object_vtable()) {
        return static_cast<ObjectProxy*>(object)->object;
    }
    if (euphoria::gpu::is_inert_object(object)) {
        return nullptr;
    }
    return object;
}

enum class DxgiKind : std::uint8_t {
    Device,
    Adapter,
    Factory,
    Output,
};

struct DxgiProxy {
    void** vtable;
    DxgiKind kind;
};

std::int32_t __stdcall dxgi_stub()
{
    return kNotImplemented;
}

std::int32_t __stdcall dxgi_ok()
{
    return 0;
}

std::int32_t __stdcall dxgi_query_interface(DxgiProxy* self, const GUID& iid, void** out)
{
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }

    bool known = IsEqualGUID(iid, IID_IUnknown) != 0;
    switch (self->kind) {
    case DxgiKind::Device:
        known = known || IsEqualGUID(iid, __uuidof(IDXGIDevice)) != 0
             || IsEqualGUID(iid, __uuidof(IDXGIDevice1)) != 0
             || IsEqualGUID(iid, __uuidof(IDXGIDevice2)) != 0;
        break;
    case DxgiKind::Adapter:
        known = known || IsEqualGUID(iid, __uuidof(IDXGIAdapter)) != 0
             || IsEqualGUID(iid, __uuidof(IDXGIAdapter1)) != 0
             || IsEqualGUID(iid, __uuidof(IDXGIAdapter2)) != 0;
        break;
    case DxgiKind::Factory:
        known = known || IsEqualGUID(iid, __uuidof(IDXGIFactory)) != 0
             || IsEqualGUID(iid, __uuidof(IDXGIFactory1)) != 0
             || IsEqualGUID(iid, __uuidof(IDXGIFactory2)) != 0;
        break;
    case DxgiKind::Output:
        known = known || IsEqualGUID(iid, __uuidof(IDXGIOutput)) != 0;
        break;
    }

    *out = known ? self : nullptr;
    return known ? 0 : static_cast<std::int32_t>(0x80004002);
}

std::uint32_t __stdcall dxgi_add_ref(DxgiProxy*)
{
    return 2;
}

std::uint32_t __stdcall dxgi_release(DxgiProxy*)
{
    return 1;
}

DxgiProxy& dxgi_object(DxgiKind kind);

std::int32_t __stdcall dxgi_get_parent(DxgiProxy* self, const GUID& iid, void** out)
{
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }
    switch (self->kind) {
    case DxgiKind::Device: {
        DxgiProxy& adapter = dxgi_object(DxgiKind::Adapter);
        *out = &adapter;
        return 0;
    }
    case DxgiKind::Adapter: {
        DxgiProxy& factory = dxgi_object(DxgiKind::Factory);
        *out = &factory;
        return 0;
    }
    case DxgiKind::Output: {
        DxgiProxy& adapter = dxgi_object(DxgiKind::Adapter);
        *out = &adapter;
        return 0;
    }
    case DxgiKind::Factory:
        break;
    }
    (void)iid;
    *out = nullptr;
    return static_cast<std::int32_t>(0x80004002);
}

std::int32_t __stdcall dxgi_device_get_adapter(DxgiProxy*, void** out)
{
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }
    *out = &dxgi_object(DxgiKind::Adapter);
    return 0;
}

std::int32_t __stdcall dxgi_device_set_latency(DxgiProxy*, UINT)
{
    return 0;
}

std::int32_t __stdcall dxgi_device_get_latency(DxgiProxy*, UINT* latency)
{
    if (latency != nullptr) {
        *latency = 1;
    }
    return 0;
}

std::int32_t __stdcall dxgi_enum_outputs(DxgiProxy*, UINT index, void** out)
{
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }
    if (index != 0) {
        *out = nullptr;
        return static_cast<std::int32_t>(0x887A0002);
    }
    *out = &dxgi_object(DxgiKind::Output);
    return 0;
}

std::int32_t __stdcall dxgi_adapter_desc(DxgiProxy*, DXGI_ADAPTER_DESC* description)
{
    if (description == nullptr) {
        return static_cast<std::int32_t>(0x80070057);
    }
    std::memset(description, 0, sizeof(*description));
    const wchar_t* name = L"Euphoria (Xbox Series)";
    std::wcsncpy(description->Description, name, 127);
    description->VendorId = 0x1002;
    description->DeviceId = 0x73FF;
    description->DedicatedVideoMemory = 10ull * 1024 * 1024 * 1024;
    description->DedicatedSystemMemory = 0;
    description->SharedSystemMemory = 16ull * 1024 * 1024 * 1024;
    return 0;
}

std::int32_t __stdcall dxgi_adapter_desc1(DxgiProxy* self, DXGI_ADAPTER_DESC1* description)
{
    if (description == nullptr) {
        return static_cast<std::int32_t>(0x80070057);
    }
    DXGI_ADAPTER_DESC base{};
    dxgi_adapter_desc(self, &base);
    std::memset(description, 0, sizeof(*description));
    std::memcpy(description->Description, base.Description, sizeof(base.Description));
    description->VendorId = base.VendorId;
    description->DeviceId = base.DeviceId;
    description->DedicatedVideoMemory = base.DedicatedVideoMemory;
    description->DedicatedSystemMemory = base.DedicatedSystemMemory;
    description->SharedSystemMemory = base.SharedSystemMemory;
    description->AdapterLuid = base.AdapterLuid;
    description->Flags = DXGI_ADAPTER_FLAG_NONE;
    return 0;
}

std::int32_t __stdcall dxgi_check_interface(DxgiProxy*, const GUID&, LARGE_INTEGER* version)
{
    if (version != nullptr) {
        version->QuadPart = 0;
    }
    return 0;
}

std::int32_t __stdcall dxgi_enum_adapters(DxgiProxy*, UINT index, void** out)
{
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }
    if (index != 0) {
        *out = nullptr;
        return static_cast<std::int32_t>(0x887A0002);
    }
    *out = &dxgi_object(DxgiKind::Adapter);
    return 0;
}

IDXGIFactory2* host_factory()
{
    static IDXGIFactory2* factory = [] () -> IDXGIFactory2* {
        IDXGIFactory2* created = nullptr;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&created)))) {
            return nullptr;
        }
        return created;
    }();
    return factory;
}

void* unwrap_device(void* object)
{
    if (object != nullptr && *reinterpret_cast<void***>(object) == device_vtable()) {
        return static_cast<DeviceProxy*>(object)->device;
    }
    return unwrap_object(object);
}

std::int32_t __stdcall dxgi_factory_swap_chain(DxgiProxy*, void* device,
                                               const DXGI_SWAP_CHAIN_DESC* description, void** out)
{
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }
    *out = nullptr;
    IDXGIFactory2* factory = host_factory();
    if (factory == nullptr) {
        return static_cast<std::int32_t>(0x80004005);
    }
    IDXGISwapChain* chain = nullptr;
    const std::int32_t result = factory->CreateSwapChain(
        static_cast<IUnknown*>(unwrap_device(device)),
        const_cast<DXGI_SWAP_CHAIN_DESC*>(description), &chain);
    *out = chain != nullptr ? euphoria::gpu::create_swap_chain_proxy(chain, static_cast<IUnknown*>(device)) : nullptr;
    log::info("dxgi: CreateSwapChain -> hr 0x"
              + [&] {
                    char text[16]{};
                    std::snprintf(text, sizeof(text), "%08x", static_cast<std::uint32_t>(result));
                    return std::string(text);
                }());
    return result;
}

std::int32_t __stdcall dxgi_factory_swap_chain_hwnd(DxgiProxy*, void* device, HWND window,
                                                    const DXGI_SWAP_CHAIN_DESC1* description,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                    void* restrict_output, void** out)
{
    if (out == nullptr) {
        return static_cast<std::int32_t>(0x80004003);
    }
    *out = nullptr;
    IDXGIFactory2* factory = host_factory();
    if (factory == nullptr) {
        return static_cast<std::int32_t>(0x80004005);
    }
    IDXGISwapChain1* chain = nullptr;
    const std::int32_t result = factory->CreateSwapChainForHwnd(
        static_cast<IUnknown*>(unwrap_device(device)), window, description, fullscreen,
        static_cast<IDXGIOutput*>(restrict_output), &chain);
    *out = chain != nullptr ? euphoria::gpu::create_swap_chain_proxy(chain, static_cast<IUnknown*>(device)) : nullptr;
    log::info("dxgi: CreateSwapChainForHwnd -> hr 0x"
              + [&] {
                    char text[16]{};
                    std::snprintf(text, sizeof(text), "%08x", static_cast<std::uint32_t>(result));
                    return std::string(text);
                }());
    return result;
}

std::int32_t __stdcall dxgi_output_desc(DxgiProxy*, DXGI_OUTPUT_DESC* description)
{
    if (description == nullptr) {
        return static_cast<std::int32_t>(0x80070057);
    }
    std::memset(description, 0, sizeof(*description));
    const wchar_t* name = L"\\\\.\\DISPLAY1";
    std::wcsncpy(description->DeviceName, name, 31);
    description->DesktopCoordinates = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
                                           GetSystemMetrics(SM_CYSCREEN)};
    description->AttachedToDesktop = TRUE;
    description->Rotation = DXGI_MODE_ROTATION_IDENTITY;
    return 0;
}

std::int32_t __stdcall dxgi_output_modes(DxgiProxy*, DXGI_FORMAT format, UINT capacity,
                                         UINT* count, DXGI_MODE_DESC* modes)
{
    if (count == nullptr) {
        return static_cast<std::int32_t>(0x80070057);
    }
    if (modes != nullptr && capacity >= 1) {
        modes[0] = DXGI_MODE_DESC{};
        modes[0].Width = 1920;
        modes[0].Height = 1080;
        modes[0].RefreshRate = DXGI_RATIONAL{60, 1};
        modes[0].Format = format;
        modes[0].ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
        modes[0].Scaling = DXGI_MODE_SCALING_CENTERED;
        *count = 1;
    } else {
        *count = 1;
    }
    return 0;
}

std::int32_t __stdcall dxgi_output_closest(DxgiProxy*, const DXGI_MODE_DESC* mode,
                                           DXGI_MODE_DESC* closest, void*)
{
    if (mode == nullptr || closest == nullptr) {
        return static_cast<std::int32_t>(0x80070057);
    }
    *closest = *mode;
    return 0;
}

void** dxgi_vtable(DxgiKind kind)
{
    static void** device_table = nullptr;
    static void** adapter_table = nullptr;
    static void** factory_table = nullptr;
    static void** output_table = nullptr;

    void**& table = kind == DxgiKind::Device    ? device_table
                  : kind == DxgiKind::Adapter   ? adapter_table
                  : kind == DxgiKind::Factory   ? factory_table
                                                : output_table;
    if (table == nullptr) {
        table = new void*[32]{};
        for (std::size_t index = 0; index < 32; ++index) {
            table[index] = reinterpret_cast<void*>(&dxgi_stub);
        }
        table[0] = reinterpret_cast<void*>(&dxgi_query_interface);
        table[1] = reinterpret_cast<void*>(&dxgi_add_ref);
        table[2] = reinterpret_cast<void*>(&dxgi_release);
        table[3] = reinterpret_cast<void*>(&dxgi_ok);
        table[4] = reinterpret_cast<void*>(&dxgi_ok);
        table[5] = reinterpret_cast<void*>(&dxgi_stub);
        table[6] = reinterpret_cast<void*>(&dxgi_get_parent);
    }

    if (kind == DxgiKind::Device) {
        device_table[7] = reinterpret_cast<void*>(&dxgi_device_get_adapter);
        device_table[12] = reinterpret_cast<void*>(&dxgi_device_set_latency);
        device_table[13] = reinterpret_cast<void*>(&dxgi_device_get_latency);
    } else if (kind == DxgiKind::Adapter) {
        adapter_table[7] = reinterpret_cast<void*>(&dxgi_enum_outputs);
        adapter_table[8] = reinterpret_cast<void*>(&dxgi_adapter_desc);
        adapter_table[9] = reinterpret_cast<void*>(&dxgi_check_interface);
        adapter_table[10] = reinterpret_cast<void*>(&dxgi_adapter_desc1);
    } else if (kind == DxgiKind::Factory) {
        factory_table[7] = reinterpret_cast<void*>(&dxgi_enum_adapters);
        factory_table[8] = reinterpret_cast<void*>(&dxgi_ok);
        factory_table[9] = reinterpret_cast<void*>(&dxgi_stub);
        factory_table[10] = reinterpret_cast<void*>(&dxgi_factory_swap_chain);
        factory_table[12] = reinterpret_cast<void*>(&dxgi_enum_adapters);
        factory_table[13] = reinterpret_cast<void*>(&dxgi_ok);
        factory_table[15] = reinterpret_cast<void*>(&dxgi_factory_swap_chain_hwnd);
    } else {
        output_table[7] = reinterpret_cast<void*>(&dxgi_output_desc);
        output_table[8] = reinterpret_cast<void*>(&dxgi_output_modes);
        output_table[9] = reinterpret_cast<void*>(&dxgi_output_closest);
        output_table[10] = reinterpret_cast<void*>(&dxgi_ok);
        output_table[11] = reinterpret_cast<void*>(&dxgi_ok);
    }
    return table;
}

DxgiProxy& dxgi_object(DxgiKind kind)
{
    static DxgiProxy device{nullptr, DxgiKind::Device};
    static DxgiProxy adapter{nullptr, DxgiKind::Adapter};
    static DxgiProxy factory{nullptr, DxgiKind::Factory};
    static DxgiProxy output{nullptr, DxgiKind::Output};
    DxgiProxy& object = kind == DxgiKind::Device    ? device
                      : kind == DxgiKind::Adapter   ? adapter
                      : kind == DxgiKind::Factory   ? factory
                                                    : output;
    object.vtable = dxgi_vtable(kind);
    return object;
}

bool is_dxgi_device_iid(const GUID& iid) noexcept
{
    return IsEqualGUID(iid, __uuidof(IDXGIDevice)) != 0
        || IsEqualGUID(iid, __uuidof(IDXGIDevice1)) != 0
        || IsEqualGUID(iid, __uuidof(IDXGIDevice2)) != 0;
}

void* dxgi_device() noexcept
{
    return &dxgi_object(DxgiKind::Device);
}

std::int32_t __stdcall proxy_create_command_queue(DeviceProxy* self,
                                                  const D3D12_COMMAND_QUEUE_DESC* description,
                                                  const GUID& iid, void** out)
{
    check_removed("create_command_queue");
    (void)iid;
    using Function = std::int32_t(__stdcall*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const GUID&,
                                              void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[8]);
    const GUID& host_iid = __uuidof(ID3D12CommandQueue);
    const std::int32_t result = function(self->device, description, host_iid, out);
    if (result >= 0 && out != nullptr) {
        *out = euphoria::gpu::create_command_queue(static_cast<ID3D12CommandQueue*>(*out), static_cast<ID3D12Device*>(self->device), self);
    }
    char text[160]{};
    if (description != nullptr) {
        std::snprintf(text, sizeof(text), "type %d priority %d flags %u node %u -> hr 0x%08x",
                      static_cast<int>(description->Type), description->Priority,
                      static_cast<unsigned>(description->Flags), description->NodeMask,
                      static_cast<std::uint32_t>(result));
    } else {
        std::snprintf(text, sizeof(text), "null desc -> hr 0x%08x",
                      static_cast<std::uint32_t>(result));
    }
    log::info(std::string("d3d12: CreateCommandQueue ") + text);
    return result;
}

std::int32_t __stdcall proxy_create_allocator(DeviceProxy* self, D3D12_COMMAND_LIST_TYPE type,
                                              const GUID& iid, void** out)
{
    check_removed("create_allocator");
    (void)iid;
    using Function = std::int32_t(__stdcall*)(void*, D3D12_COMMAND_LIST_TYPE, const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[9]);
    const GUID& host_iid = __uuidof(ID3D12CommandAllocator);
    const std::int32_t result = function(self->device, type, host_iid, out);
    if (result >= 0 && out != nullptr) {
        *out = wrap_object(*out, self);
    }
    char text[96]{};
    std::snprintf(text, sizeof(text), "type %d -> hr 0x%08x",
                  static_cast<int>(type), static_cast<std::uint32_t>(result));
    log::info(std::string("d3d12: CreateCommandAllocator ") + text);
    return result;
}

D3D12_RESOURCE_FLAGS sanitize_resource_flags(const D3D12_RESOURCE_DESC& desc,
                                             D3D12_RESOURCE_FLAGS flags)
{
    auto value = static_cast<std::uint32_t>(flags);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        value &= ~static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                             | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    }
    if ((value & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
        value &= ~static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                             | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
                                             | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    }
    if ((value & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0
        && (value & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
        value &= ~static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    }
    if (desc.SampleDesc.Count > 1) {
        value &= ~static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    return static_cast<D3D12_RESOURCE_FLAGS>(value);
}

std::int32_t __stdcall proxy_create_pipeline_stream(DeviceProxy* self,
                                                    const euphoria::gpu::StreamDesc* description,
                                                    const GUID& iid, void** out)
{
    check_removed("create_pipeline_stream");
    (void)iid;
    if (self == nullptr || description == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    euphoria::gpu::Pipeline pipeline{};
    const HRESULT result = euphoria::gpu::create_pipeline_from_stream(
        self, static_cast<ID3D12Device*>(self->device), description, &pipeline);
    static std::atomic<std::uint32_t> logged{0};
    const std::uint32_t count = logged.fetch_add(1, std::memory_order_relaxed);
    if (SUCCEEDED(result) && pipeline.state != nullptr) {
        euphoria::gpu::note_host_pipeline(pipeline.state);
        *out = pipeline.state;
        if (count < 8 || (count & 255) == 0) {
            log::info("d3d12: CreatePipelineState stream ok (" + std::to_string(count + 1) + ")");
        }
        return 0;
    }
    if (count < 16) {
        log::warn("d3d12: CreatePipelineState stream failed 0x"
                  + std::to_string(static_cast<std::uint32_t>(result))
                  + "; handing out a console-only pipeline");
    }
    *out = euphoria::gpu::create_inert_object(self);
    return 0;
}

bool skip_foreign_view(std::uint64_t handle)
{
    if (in_placed_range(handle)) {
        return true;
    }
    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        char text[96]{};
        std::snprintf(text, sizeof(text), "d3d12: view for unknown descriptor handle 0x%llx skipped",
                      static_cast<unsigned long long>(handle));
        log::warn(text);
    }
    return true;
}

std::int32_t __stdcall proxy_residency(DeviceProxy* self, UINT count, void* const* objects)
{
    (void)self;
    (void)objects;
    static std::atomic<std::uint32_t> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 4) {
        log::info("d3d12: residency change for " + std::to_string(count)
                  + " objects kept resident");
    }
    return 0;
}

std::int32_t __stdcall proxy_create_fence(DeviceProxy* self, std::uint64_t value,
                                          D3D12_FENCE_FLAGS flags, const GUID& iid, void** out)
{
    check_removed("create_fence");
    (void)iid;
    using Function = std::int32_t(__stdcall*)(void*, std::uint64_t, D3D12_FENCE_FLAGS, const GUID&,
                                              void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[36]);
    const GUID& host_iid = __uuidof(ID3D12Fence);
    const std::int32_t result = function(self->device, value, flags, host_iid, out);
    if (result >= 0 && out != nullptr) {
        *out = wrap_object(*out, self);
    }
    return result;
}

std::int32_t __stdcall proxy_create_descriptor_heap(DeviceProxy* self,
                                                    const D3D12_DESCRIPTOR_HEAP_DESC* description,
                                                    const GUID& iid, void** out)
{
    check_removed("create_descriptor_heap");
    (void)iid;
    using Function = std::int32_t(__stdcall*)(void*, const D3D12_DESCRIPTOR_HEAP_DESC*,
                                              const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[14]);
    const GUID& host_iid = __uuidof(ID3D12DescriptorHeap);
    std::uint32_t stride = 32;
    switch (description->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
        stride = 32;
        break;
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
        stride = 16;
        break;
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
        stride = 128;
        break;
    default:
        stride = 32;
        break;
    }

    struct Placement {
        std::uint64_t address;
        std::uint64_t size;
    };
    Placement placement{};
    std::memcpy(&placement, description + 1, sizeof(placement));
    const std::uint64_t expected =
        static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(description->NumDescriptors);
    const bool placed = placement.address != 0 && placement.size == expected;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    if (placed) {
        cpu.ptr = static_cast<SIZE_T>(placement.address);
        gpu.ptr = placement.address;
        add_placed_range(placement.address, placement.size);
    } else {
        const std::uint64_t bytes = expected == 0 ? stride : expected;
        auto* memory = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_COMMIT | MEM_RESERVE,
                         PAGE_READWRITE));
        cpu.ptr = reinterpret_cast<SIZE_T>(memory);
        gpu.ptr = memory != nullptr
                      ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(memory))
                      : 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC host = *description;
    if (host.NumDescriptors > 65536) {
        host.NumDescriptors = 65536;
    }
    const std::int32_t result = function(self->device, &host, host_iid, out);
    if (result >= 0 && out != nullptr) {
        auto* proxy = euphoria::gpu::create_descriptor_heap_proxy(
            static_cast<ID3D12DescriptorHeap*>(*out), self, *description, cpu, gpu);
        *out = proxy;
        euphoria::gpu::register_console_heap(
            static_cast<std::uint64_t>(cpu.ptr), expected,
            static_cast<euphoria::gpu::ConsoleDescriptorKind>(description->Type),
            description->NumDescriptors, proxy);
    }
    char text[160]{};
    if (description != nullptr) {
        std::snprintf(text, sizeof(text), "type %d count %u flags %u node %u -> hr 0x%08x",
                      static_cast<int>(description->Type), description->NumDescriptors,
                      static_cast<unsigned>(description->Flags), description->NodeMask,
                      static_cast<std::uint32_t>(result));
    } else {
        std::snprintf(text, sizeof(text), "null desc -> hr 0x%08x",
                      static_cast<std::uint32_t>(result));
    }
    log::info(std::string("d3d12: CreateDescriptorHeap ") + text);
    if (placed) {
        char placement_text[160]{};
        std::snprintf(placement_text, sizeof(placement_text),
                      "d3d12: descriptor heap placement addr 0x%llx size %llu stride %u",
                      static_cast<unsigned long long>(placement.address),
                      static_cast<unsigned long long>(placement.size), stride);
        log::info(placement_text);
    }
    return result;
}

void __stdcall proxy_create_cbv(DeviceProxy* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* description,
                                D3D12_CPU_DESCRIPTOR_HANDLE destination)
{
    check_removed("create_cbv");
    char text[160]{};
    if (description != nullptr) {
        std::snprintf(text, sizeof(text), "location 0x%llx size %u handle 0x%llx",
                      static_cast<unsigned long long>(description->BufferLocation),
                      description->SizeInBytes,
                      static_cast<unsigned long long>(destination.ptr));
    } else {
        std::snprintf(text, sizeof(text), "null desc handle 0x%llx",
                      static_cast<unsigned long long>(destination.ptr));
    }
    log::info(std::string("d3d12: CreateConstantBufferView ") + text);
    if (destination.ptr != 0
        && euphoria::gpu::is_console_handle(static_cast<std::uint64_t>(destination.ptr))) {
        if (description != nullptr && description->BufferLocation != 0
            && description->SizeInBytes != 0) {
            euphoria::gpu::write_constant_buffer_view(
                static_cast<std::uint64_t>(destination.ptr), description->BufferLocation,
                description->SizeInBytes);
        }
        return;
    }
    if (destination.ptr != 0 && in_placed_range(static_cast<std::uint64_t>(destination.ptr))) {
        return;
    }
    if (description == nullptr || destination.ptr == 0
        || description->BufferLocation == 0 || description->SizeInBytes == 0) {
        if (destination.ptr != 0) {
            std::memset(reinterpret_cast<void*>(destination.ptr), 0, 32);
        }
        return;
    }
    using Function = void(__stdcall*)(void*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*,
                                      D3D12_CPU_DESCRIPTOR_HANDLE);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[17]);
    function(self->device, description, destination);
}

std::optional<D3D12_RESOURCE_ALLOCATION_INFO> resource_footprint(const D3D12_RESOURCE_DESC& desc)
{
    constexpr std::uint64_t kib64 = 64 * 1024;
    const auto round_up = [](std::uint64_t value, std::uint64_t alignment) {
        return (value + alignment - 1) / alignment * alignment;
    };
    const auto power_of_two = [](std::uint64_t value) {
        return value != 0 && (value & (value - 1)) == 0;
    };
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        namespace fmt = euphoria::formats;
        std::optional<fmt::Dimension> dimension;
        switch (desc.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            dimension = fmt::Dimension::Tex1d;
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            dimension = fmt::Dimension::Tex2d;
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            dimension = fmt::Dimension::Tex3d;
            break;
        default:
            break;
        }
        const std::uint32_t layout = static_cast<std::uint32_t>(desc.Layout);
        std::optional<fmt::Swizzle> swizzle;
        if (layout == 0) {
            if (dimension == fmt::Dimension::Tex1d) {
                swizzle = fmt::Swizzle::LINEAR;
            } else if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
                swizzle = fmt::Swizzle::SW_64KB_Z_X;
            } else if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) {
                swizzle = fmt::Swizzle::SW_64KB_R_X;
            } else {
                swizzle = fmt::Swizzle::SW_4KB_S;
            }
        } else if (layout == 1) {
            swizzle = fmt::Swizzle::LINEAR;
        } else if ((layout & ~0xFFu) == 0x100) {
            swizzle = fmt::Swizzle{layout & 0xFF};
        }
        if (dimension && swizzle && desc.Width <= 0xFFFFFFFFull) {
            fmt::TextureDesc texture{};
            texture.dimension = *dimension;
            texture.width = static_cast<std::uint32_t>(desc.Width);
            texture.height = desc.Height;
            texture.depth_or_array = desc.DepthOrArraySize;
            texture.mips = desc.MipLevels;
            texture.format = static_cast<std::uint32_t>(desc.Format);
            texture.samples = desc.SampleDesc.Count > 1 ? desc.SampleDesc.Count : 1;
            texture.swizzle = *swizzle;
            if (const auto made = fmt::Texture::create(texture)) {
                const std::uint64_t alignment = std::max<std::uint64_t>(desc.Alignment, made->alignment);
                if (!power_of_two(alignment)) {
                    return std::nullopt;
                }
                return D3D12_RESOURCE_ALLOCATION_INFO{round_up(made->size, alignment), alignment};
            }
        }
    }
    std::uint64_t bytes = 0;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        bytes = desc.Width;
    } else {
        const auto format = euphoria::formats::format_info(static_cast<std::uint32_t>(desc.Format));
        if (!format || format->bytes == 0) {
            return std::nullopt;
        }
        const std::uint64_t block = format->block == 0 ? 1 : format->block;
        const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const std::uint64_t depth = volume ? desc.DepthOrArraySize : 1;
        const std::uint64_t slices = volume ? 1 : desc.DepthOrArraySize;
        std::uint64_t mips = desc.MipLevels;
        if (mips == 0) {
            std::uint64_t largest = std::max<std::uint64_t>({desc.Width, desc.Height, depth, 1});
            mips = 0;
            while (largest != 0) {
                ++mips;
                largest >>= 1;
            }
        }
        std::uint64_t chain = 0;
        for (std::uint64_t mip = 0; mip < std::min<std::uint64_t>(mips, 64); ++mip) {
            const std::uint64_t w = (std::max<std::uint64_t>(desc.Width >> mip, 1) + block - 1) / block;
            const std::uint64_t h =
                (std::max<std::uint64_t>(std::uint64_t{desc.Height} >> mip, 1) + block - 1) / block;
            const std::uint64_t d = std::max<std::uint64_t>(depth >> mip, 1);
            chain += w * h * d * format->bytes;
        }
        bytes = chain * std::max<std::uint64_t>(slices, 1)
              * std::max<std::uint64_t>(desc.SampleDesc.Count, 1);
    }
    if (bytes == 0) {
        return std::nullopt;
    }
    const std::uint64_t minimum = desc.SampleDesc.Count > 1 ? 4 * 1024 * 1024 : kib64;
    const std::uint64_t alignment = std::max<std::uint64_t>(desc.Alignment, minimum);
    if (!power_of_two(alignment)) {
        return std::nullopt;
    }
    return D3D12_RESOURCE_ALLOCATION_INFO{round_up(bytes, kib64), alignment};
}

D3D12_RESOURCE_ALLOCATION_INFO place_resources(UINT count, const void* descriptions,
                                               std::size_t stride,
                                               D3D12_RESOURCE_ALLOCATION_INFO1* each)
{
    D3D12_RESOURCE_ALLOCATION_INFO total{0, 64 * 1024};
    if (descriptions == nullptr) {
        return total;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(descriptions);
    for (UINT index = 0; index < count; ++index) {
        const auto& desc = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(bytes + index * stride);
        const auto one = resource_footprint(desc);
        if (!one) {
            log::warn("d3d12: no allocation size for dim " + std::to_string(desc.Dimension)
                      + " format " + std::to_string(desc.Format) + " layout "
                      + std::to_string(desc.Layout));
            if (each != nullptr) {
                each[index] = {UINT64_MAX, 0, UINT64_MAX};
            }
            return {UINT64_MAX, 0};
        }
        const std::uint64_t offset =
            (total.SizeInBytes + one->Alignment - 1) / one->Alignment * one->Alignment;
        if (each != nullptr) {
            each[index] = {offset, one->Alignment, one->SizeInBytes};
        }
        total.SizeInBytes = offset + one->SizeInBytes;
        total.Alignment = std::max(total.Alignment, one->Alignment);
    }
    static std::atomic<std::uint32_t> logged{0};
    if (count != 0 && logged.fetch_add(1, std::memory_order_relaxed) < 24) {
        const auto& first = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(bytes);
        log::info("d3d12: allocation info dim " + std::to_string(first.Dimension) + " "
                  + std::to_string(first.Width) + "x" + std::to_string(first.Height) + " format "
                  + std::to_string(first.Format) + " layout " + std::to_string(first.Layout)
                  + " count " + std::to_string(count) + " -> size "
                  + std::to_string(total.SizeInBytes) + " align "
                  + std::to_string(total.Alignment));
    }
    return total;
}

D3D12_RESOURCE_ALLOCATION_INFO __stdcall proxy_resource_allocation_info(
    DeviceProxy* self, UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC* descriptions)
{
    check_removed("resource_allocation_info");
    (void)self;
    (void)visible_mask;
    return place_resources(count, descriptions, sizeof(D3D12_RESOURCE_DESC), nullptr);
}

D3D12_RESOURCE_ALLOCATION_INFO __stdcall proxy_resource_allocation_info2(
    DeviceProxy* self, UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC1* descriptions,
    D3D12_RESOURCE_ALLOCATION_INFO1* each)
{
    check_removed("resource_allocation_info2");
    (void)self;
    (void)visible_mask;
    return place_resources(count, descriptions, sizeof(D3D12_RESOURCE_DESC1), each);
}

void __stdcall proxy_copy_descriptors_simple(DeviceProxy* self, UINT count,
                                             D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                             D3D12_CPU_DESCRIPTOR_HANDLE source,
                                             D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    (void)self;
    check_removed("copy_descriptors_simple");
    if (count == 0 || destination.ptr == 0 || source.ptr == 0) {
        return;
    }
    euphoria::gpu::copy_console_descriptors_simple(
        count, static_cast<std::uint64_t>(destination.ptr),
        static_cast<std::uint64_t>(source.ptr),
        static_cast<euphoria::gpu::ConsoleDescriptorKind>(type));
}

void __stdcall proxy_copy_descriptors(DeviceProxy* self, UINT destination_ranges,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE* destination_starts,
                                      const UINT* destination_sizes, UINT source_ranges,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE* source_starts,
                                      const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    (void)self;
    check_removed("copy_descriptors");
    if (destination_starts == nullptr || source_starts == nullptr) {
        return;
    }
    std::vector<std::uint64_t> destinations(destination_ranges);
    std::vector<std::uint64_t> sources(source_ranges);
    for (UINT index = 0; index < destination_ranges; ++index) {
        destinations[index] = static_cast<std::uint64_t>(destination_starts[index].ptr);
    }
    for (UINT index = 0; index < source_ranges; ++index) {
        sources[index] = static_cast<std::uint64_t>(source_starts[index].ptr);
    }
    euphoria::gpu::copy_console_descriptors(
        destination_ranges, destinations.data(), destination_sizes, source_ranges,
        sources.data(), source_sizes, static_cast<euphoria::gpu::ConsoleDescriptorKind>(type));
}

std::mutex g_page_pool_mutex;
std::vector<std::pair<std::uint64_t, std::uint32_t>> g_page_pools;

std::int32_t __stdcall proxy_register_page_pool(DeviceProxy* self, std::uint64_t address,
                                                std::uint32_t pages, std::uintptr_t* out)
{
    (void)self;
    if (out == nullptr || pages == 0 || (address & 0xFFFF) != 0) {
        return kInvalidArgument;
    }
    {
        std::scoped_lock lock(g_page_pool_mutex);
        g_page_pools.emplace_back(address, pages);
        *out = g_page_pools.size();
    }
    char text[128]{};
    std::snprintf(text, sizeof(text), "address 0x%llx pages %u -> handle %llu",
                  static_cast<unsigned long long>(address), pages,
                  static_cast<unsigned long long>(*out));
    log::info(std::string("d3d12: RegisterPagePoolX ") + text);
    return 0;
}

std::int32_t __stdcall proxy_unregister_page_pool(DeviceProxy* self, std::uintptr_t handle)
{
    (void)self;
    std::scoped_lock lock(g_page_pool_mutex);
    if (handle == 0 || handle > g_page_pools.size()) {
        return kInvalidArgument;
    }
    g_page_pools[handle - 1] = {0, 0};
    log::info("d3d12: UnregisterPagePoolX handle " + std::to_string(handle));
    return 0;
}

std::int32_t __stdcall proxy_set_frame_interval(DeviceProxy* self, std::uint32_t output,
                                                std::uint32_t length, std::uint32_t period,
                                                std::uint32_t flags)
{
    (void)self;
    const HRESULT result =
        euphoria::gpu::frame_pacer().SetFrameIntervalX(length, period, flags);
    log::info("d3d12: SetFrameIntervalX output " + std::to_string(output) + " length "
              + std::to_string(length) + " -> 0x"
              + std::to_string(static_cast<std::uint32_t>(result)));
    return result;
}

std::int32_t __stdcall proxy_schedule_frame_event(DeviceProxy* self, std::uint32_t kind,
                                                  std::uint32_t offset, const void* list,
                                                  std::uint32_t flags)
{
    (void)self;
    return euphoria::gpu::frame_pacer().ScheduleFrameEventX(kind, offset, list, flags);
}

std::int32_t __stdcall proxy_wait_frame_event(DeviceProxy* self, std::uint32_t kind,
                                              std::uint32_t timeout, const void* list,
                                              std::uint32_t flags, std::uint64_t* token)
{
    (void)self;
    return euphoria::gpu::frame_pacer().WaitForFrameEventX(kind, timeout, list, flags, token);
}

std::int32_t __stdcall proxy_frame_statistics(DeviceProxy* self, std::uint64_t token,
                                              std::uint32_t kinds, std::uint32_t* count,
                                              void* out)
{
    (void)self;
    return euphoria::gpu::frame_pacer().GetFrameStatisticsX(token, kinds, count, out);
}

ID3D12Resource* unwrap_resource_any(void* object)
{
    if (object == nullptr) {
        return nullptr;
    }
    if (euphoria::gpu::is_resource_proxy(object)) {
        return euphoria::gpu::unwrap_resource(object);
    }
    return static_cast<ID3D12Resource*>(unwrap_object(object));
}

void __stdcall proxy_create_srv(DeviceProxy* self, void* resource, const void* description,
                                D3D12_CPU_DESCRIPTOR_HANDLE destination)
{
    check_removed("create_srv");
    if (destination.ptr == 0) {
        return;
    }
    if (euphoria::gpu::is_console_handle(static_cast<std::uint64_t>(destination.ptr))) {
        euphoria::gpu::write_shader_resource_view(
            static_cast<std::uint64_t>(destination.ptr), unwrap_resource_any(resource),
            static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(description));
        return;
    }
    if (skip_foreign_view(static_cast<std::uint64_t>(destination.ptr))) {
        return;
    }
    using Function = void(__stdcall*)(void*, ID3D12Resource*,
                                      const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                      D3D12_CPU_DESCRIPTOR_HANDLE);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[18]);
    function(self->device, unwrap_resource_any(resource),
             static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(description), destination);
}

void __stdcall proxy_create_uav(DeviceProxy* self, void* resource, void* counter,
                                const void* description, D3D12_CPU_DESCRIPTOR_HANDLE destination)
{
    check_removed("create_uav");
    if (destination.ptr == 0) {
        return;
    }
    if (ID3D12Resource* host_resource = unwrap_resource_any(resource); host_resource != nullptr) {
        const D3D12_RESOURCE_DESC desc = host_resource->GetDesc();
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
            static std::atomic<std::uint32_t> skipped{0};
            if (skipped.fetch_add(1, std::memory_order_relaxed) < 8) {
                log::warn("d3d12: CreateUnorderedAccessView skipped: resource dim "
                          + std::to_string(static_cast<int>(desc.Dimension)) + " flags 0x"
                          + std::to_string(static_cast<unsigned>(desc.Flags))
                          + " lacks ALLOW_UNORDERED_ACCESS");
            }
            return;
        }
    }
    if (euphoria::gpu::is_console_handle(static_cast<std::uint64_t>(destination.ptr))) {
        euphoria::gpu::write_unordered_access_view(
            static_cast<std::uint64_t>(destination.ptr), unwrap_resource_any(resource),
            unwrap_resource_any(counter),
            static_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(description));
        return;
    }
    if (skip_foreign_view(static_cast<std::uint64_t>(destination.ptr))) {
        return;
    }
    using Function = void(__stdcall*)(void*, ID3D12Resource*, ID3D12Resource*,
                                      const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
                                      D3D12_CPU_DESCRIPTOR_HANDLE);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[19]);
    function(self->device, unwrap_resource_any(resource), unwrap_resource_any(counter),
             static_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(description), destination);
}

void __stdcall proxy_create_rtv(DeviceProxy* self, void* resource, const void* description,
                                D3D12_CPU_DESCRIPTOR_HANDLE destination)
{
    check_removed("create_rtv");
    if (destination.ptr == 0) {
        return;
    }
    if (ID3D12Resource* host_resource = unwrap_resource_any(resource); host_resource != nullptr) {
        const D3D12_RESOURCE_DESC desc = host_resource->GetDesc();
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
            log::warn("d3d12: CreateRenderTargetView skipped: resource lacks ALLOW_RENDER_TARGET");
            return;
        }
    }
    if (euphoria::gpu::is_console_handle(static_cast<std::uint64_t>(destination.ptr))) {
        euphoria::gpu::write_render_target_view(
            static_cast<std::uint64_t>(destination.ptr), unwrap_resource_any(resource),
            static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(description));
        return;
    }
    if (skip_foreign_view(static_cast<std::uint64_t>(destination.ptr))) {
        return;
    }
    using Function = void(__stdcall*)(void*, ID3D12Resource*,
                                      const D3D12_RENDER_TARGET_VIEW_DESC*,
                                      D3D12_CPU_DESCRIPTOR_HANDLE);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[20]);
    function(self->device, unwrap_resource_any(resource),
             static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(description), destination);
}

void __stdcall proxy_create_dsv(DeviceProxy* self, void* resource, const void* description,
                                D3D12_CPU_DESCRIPTOR_HANDLE destination)
{
    check_removed("create_dsv");
    if (destination.ptr == 0) {
        return;
    }
    if (ID3D12Resource* host_resource = unwrap_resource_any(resource); host_resource != nullptr) {
        const D3D12_RESOURCE_DESC desc = host_resource->GetDesc();
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
            log::warn("d3d12: CreateDepthStencilView skipped: resource lacks ALLOW_DEPTH_STENCIL");
            return;
        }
    }
    if (euphoria::gpu::is_console_handle(static_cast<std::uint64_t>(destination.ptr))) {
        euphoria::gpu::write_depth_stencil_view(
            static_cast<std::uint64_t>(destination.ptr), unwrap_resource_any(resource),
            static_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(description));
        return;
    }
    if (skip_foreign_view(static_cast<std::uint64_t>(destination.ptr))) {
        return;
    }
    using Function = void(__stdcall*)(void*, ID3D12Resource*,
                                      const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                      D3D12_CPU_DESCRIPTOR_HANDLE);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[21]);
    function(self->device, unwrap_resource_any(resource),
             static_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(description), destination);
}

std::int32_t __stdcall proxy_create_graphics_pso(DeviceProxy* self,
                                                 const euphoria::gpu::GraphicsHead* description,
                                                 const GUID& iid, void** out)
{
    check_removed("create_graphics_pso");
    (void)iid;
    if (self == nullptr || description == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = static_cast<ID3D12RootSignature*>(unwrap_object(description->root));
    state.SampleMask = 0xFFFFFFFFu;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    state.RasterizerState.FrontCounterClockwise = FALSE;
    state.RasterizerState.DepthBias = 0;
    state.RasterizerState.DepthBiasClamp = 0.0f;
    state.RasterizerState.SlopeScaledDepthBias = 0.0f;
    state.RasterizerState.DepthClipEnable = TRUE;
    state.RasterizerState.MultisampleEnable = FALSE;
    state.RasterizerState.AntialiasedLineEnable = FALSE;
    state.RasterizerState.ForcedSampleCount = 0;
    state.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    state.BlendState.AlphaToCoverageEnable = FALSE;
    state.BlendState.IndependentBlendEnable = FALSE;
    state.DepthStencilState.DepthEnable = FALSE;
    state.DepthStencilState.StencilEnable = FALSE;
    state.SampleDesc.Count = 1;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    state.NodeMask = 1;
    euphoria::gpu::GraphicsHead head = *description;
    head.root = unwrap_object(head.root);
    euphoria::gpu::Pipeline pipeline{};
    const std::int32_t result = euphoria::gpu::create_graphics_pipeline(
        self, static_cast<ID3D12Device*>(self->device), &head, &state, nullptr, &pipeline);
    if (result >= 0 && pipeline.state != nullptr) {
        euphoria::gpu::note_host_pipeline(pipeline.state);
        *out = pipeline.state;
        log::info("d3d12: CreateGraphicsPipelineState ok");
        return result;
    }
    static std::atomic<std::uint32_t> failures{0};
    if (failures.fetch_add(1, std::memory_order_relaxed) < 16) {
        log::warn("d3d12: CreateGraphicsPipelineState host pipeline unavailable 0x"
                  + std::to_string(static_cast<std::uint32_t>(result))
                  + "; handing out a console-only pipeline");
    }
    *out = euphoria::gpu::create_inert_object(self);
    return 0;
}

std::int32_t __stdcall proxy_create_compute_pso(DeviceProxy* self,
                                                const euphoria::gpu::ComputeHead* description,
                                                const GUID& iid, void** out)
{
    check_removed("create_compute_pso");
    (void)iid;
    if (self == nullptr || description == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    euphoria::gpu::ComputeHead head = *description;
    head.root = unwrap_object(head.root);
    euphoria::gpu::Pipeline pipeline{};
    const std::int32_t result = euphoria::gpu::create_compute_pipeline(
        self, static_cast<ID3D12Device*>(self->device), &head, nullptr, &pipeline);
    if (result >= 0 && pipeline.state != nullptr) {
        euphoria::gpu::note_host_pipeline(pipeline.state);
        *out = pipeline.state;
        log::info("d3d12: CreateComputePipelineState ok");
        return result;
    }
    static std::atomic<std::uint32_t> failures{0};
    if (failures.fetch_add(1, std::memory_order_relaxed) < 16) {
        log::warn("d3d12: CreateComputePipelineState host pipeline unavailable 0x"
                  + std::to_string(static_cast<std::uint32_t>(result))
                  + "; handing out a console-only pipeline");
    }
    *out = euphoria::gpu::create_inert_object(self);
    return 0;
}

std::int32_t __stdcall proxy_create_compute_pso_x(DeviceProxy* self,
                                                  const euphoria::gpu::ComputeHead* description,
                                                  std::uint32_t extension_count,
                                                  const void* extensions, const GUID& iid,
                                                  void** out)
{
    check_removed("create_compute_pso_x");
    (void)extension_count;
    (void)extensions;
    return proxy_create_compute_pso(self, description, iid, out);
}

std::int32_t __stdcall proxy_create_root_signature(DeviceProxy* self, UINT node, const void* blob,
                                                   SIZE_T size, const GUID& iid, void** out)
{
    check_removed("create_root_signature");
    (void)iid;
    using Function = std::int32_t(__stdcall*)(void*, UINT, const void*, SIZE_T, const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[16]);
    const GUID& host_iid = __uuidof(ID3D12RootSignature);
    std::int32_t result = function(self->device, node, blob, size, host_iid, out);
    if (result < 0 && node != 0) {
        result = function(self->device, 0, blob, size, host_iid, out);
    }
    if (result >= 0 && out != nullptr) {
        *out = wrap_object(*out, self);
    }
    static std::atomic<std::uint32_t> logged{0};
    if (result < 0 || logged.fetch_add(1, std::memory_order_relaxed) < 4) {
        char text[128]{};
        std::snprintf(text, sizeof(text), "node %u size %zu -> hr 0x%08x", node,
                      static_cast<std::size_t>(size), static_cast<std::uint32_t>(result));
        if (result < 0) {
            log::warn(std::string("d3d12: CreateRootSignature failed ") + text);
        } else {
            log::info(std::string("d3d12: CreateRootSignature ") + text);
        }
    }
    return result;
}

std::int32_t __stdcall proxy_create_heap(DeviceProxy* self, const D3D12_HEAP_DESC* description,
                                         const GUID& iid, void** out)
{
    check_removed("create_heap");
    (void)iid;
    using Function = std::int32_t(__stdcall*)(void*, const D3D12_HEAP_DESC*, const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[28]);
    const GUID& host_iid = __uuidof(ID3D12Heap);
    const std::int32_t result = function(self->device, description, host_iid, out);
    if (result >= 0 && out != nullptr) {
        *out = euphoria::gpu::create_heap_proxy(
            static_cast<ID3D12Heap*>(*out), self, *description);
    }
    return result;
}

std::int32_t __stdcall proxy_create_placed_resource(DeviceProxy* self, void* heap,
                                                    std::uint64_t offset,
                                                    const D3D12_RESOURCE_DESC* description,
                                                    D3D12_RESOURCE_STATES state,
                                                    const D3D12_CLEAR_VALUE* clear, const GUID& iid,
                                                    void** out)
{
    check_removed("create_placed_resource");
    (void)iid;
    using Function = std::int32_t(__stdcall*)(void*, void*, std::uint64_t,
                                              const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[29]);
    const GUID& host_iid = __uuidof(ID3D12Resource);
    const std::int32_t result = function(self->device, unwrap_object(heap), offset, description,
                                         state, clear, host_iid, out);
    if (result >= 0 && out != nullptr) {
        *out = wrap_object(*out, self);
    }
    return result;
}

std::int32_t __stdcall proxy_create_query_heap(DeviceProxy* self,
                                               const D3D12_QUERY_HEAP_DESC* description,
                                               const GUID& iid, void** out)
{
    (void)iid;
    using Function = std::int32_t(__stdcall*)(void*, const D3D12_QUERY_HEAP_DESC*, const GUID&,
                                              void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[39]);
    const GUID& host_iid = __uuidof(ID3D12QueryHeap);
    const std::int32_t result = function(self->device, description, host_iid, out);
    if (result >= 0 && out != nullptr) {
        *out = wrap_object(*out, self);
    }
    return result;
}

std::int32_t __stdcall proxy_create_command_signature(
    DeviceProxy* self, const D3D12_COMMAND_SIGNATURE_DESC* description, void* root_signature,
    const GUID& iid, void** out)
{
    (void)iid;
    using Function = std::int32_t(__stdcall*)(void*, const D3D12_COMMAND_SIGNATURE_DESC*, void*,
                                              const GUID&, void**);
    void** vtable = *reinterpret_cast<void***>(self->device);
    const auto function = reinterpret_cast<Function>(vtable[41]);
    const GUID& host_iid = __uuidof(ID3D12CommandSignature);
    std::int32_t result = function(self->device, description,
                                   unwrap_object(root_signature), host_iid, out);
    if (result >= 0 && out != nullptr) {
        *out = wrap_object(*out, self);
    } else if (out != nullptr && description != nullptr) {
        *out = euphoria::gpu::create_inert_object(self);
        log::warn("d3d12: host refused a command signature; keeping a console-only signature");
        result = 0;
    }
    char text[200]{};
    if (description != nullptr) {
        const std::uint32_t first_type =
            description->NumArgumentDescs != 0 && description->pArgumentDescs != nullptr
                ? static_cast<std::uint32_t>(description->pArgumentDescs[0].Type)
                : 0xFFFFFFFFu;
        std::snprintf(text, sizeof(text),
                      "stride %u args %u first type %u node %u root %p -> hr 0x%08x",
                      description->ByteStride, description->NumArgumentDescs, first_type,
                      description->NodeMask, root_signature,
                      static_cast<std::uint32_t>(result));
    } else {
        std::snprintf(text, sizeof(text), "null desc -> hr 0x%08x",
                      static_cast<std::uint32_t>(result));
    }
    log::info(std::string("d3d12: CreateCommandSignature ") + text);
    return result;
}

std::int32_t __stdcall proxy_check_feature_support(DeviceProxy*, UINT feature, void* data,
                                                   UINT size)
{
    constexpr std::uint32_t kInvalidArgument = 0x80070057;
    constexpr std::uint32_t kUnsupported = 0x887A0004;

    static constexpr std::uint32_t options[16]{
        1, 1, 2, 3, 3, 1, 1, 1, 3, 40, 0, 0, 0, 0, 1, 2,
    };

    if (data == nullptr) {
        return kInvalidArgument;
    }

    switch (feature) {
    case 0:
        if (size < sizeof(options)) {
            return kInvalidArgument;
        }
        std::memcpy(data, options, sizeof(options));
        return 0;
    case 1: {
        if (size < 4 * sizeof(std::uint32_t)) {
            return kInvalidArgument;
        }
        auto* architecture = static_cast<std::uint32_t*>(data);
        architecture[0] = 0;
        architecture[1] = 0;
        architecture[2] = 1;
        architecture[3] = 1;
        return 0;
    }
    case 2: {
        struct FeatureLevels {
            UINT count;
            std::uint32_t padding;
            const D3D_FEATURE_LEVEL* requested;
            D3D_FEATURE_LEVEL highest;
        };
        if (size < sizeof(FeatureLevels)) {
            return kInvalidArgument;
        }
        auto* query = static_cast<FeatureLevels*>(data);
        if (query->requested == nullptr) {
            return kInvalidArgument;
        }
        constexpr D3D_FEATURE_LEVEL highest_supported = static_cast<D3D_FEATURE_LEVEL>(0xC100);
        D3D_FEATURE_LEVEL best = static_cast<D3D_FEATURE_LEVEL>(0);
        for (UINT index = 0; index < query->count; ++index) {
            const D3D_FEATURE_LEVEL level = query->requested[index];
            if (level <= highest_supported && level > best) {
                best = level;
            }
        }
        if (best == 0) {
            return kUnsupported;
        }
        query->highest = best;
        return 0;
    }
    case 3: {
        if (size < 3 * sizeof(std::uint32_t)) {
            return kInvalidArgument;
        }
        auto* support = static_cast<std::uint32_t*>(data);
        support[1] = 0x7FFFFFFF;
        support[2] = 0xFFFF;
        return 0;
    }
    case 4: {
        if (size < 4 * sizeof(std::uint32_t)) {
            return kInvalidArgument;
        }
        auto* multisample = static_cast<std::uint32_t*>(data);
        const std::uint32_t samples = multisample[1];
        multisample[3] = (samples == 1 || samples == 2 || samples == 4 || samples == 8) ? 1 : 0;
        return 0;
    }
    case 7: {
        if (size < sizeof(std::uint32_t)) {
            return kInvalidArgument;
        }
        auto* model = static_cast<std::uint32_t*>(data);
        if (*model > 0x66) {
            *model = 0x66;
        }
        return 0;
    }
    case 8: {
        if (size < 6 * sizeof(std::uint32_t)) {
            return kInvalidArgument;
        }
        auto* waves = static_cast<std::uint32_t*>(data);
        waves[0] = 1;
        waves[1] = 32;
        waves[2] = 64;
        waves[3] = 52 * 64;
        waves[4] = 1;
        waves[5] = 1;
        return 0;
    }
    case 23: {
        if (size < 3 * sizeof(std::uint32_t)) {
            return kInvalidArgument;
        }
        auto* options4 = static_cast<std::uint32_t*>(data);
        options4[0] = 0;
        options4[1] = 0;
        options4[2] = 1;
        return 0;
    }
    default:
        return kNotImplemented;
    }
}

D3D12_HEAP_PROPERTIES __stdcall proxy_custom_heap_properties(DeviceProxy* self, UINT node,
                                                             D3D12_HEAP_TYPE type)
{
    (void)self;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    switch (type) {
    case D3D12_HEAP_TYPE_DEFAULT:
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        break;
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        break;
    default:
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        break;
    }
    properties.CreationNodeMask = node == 0 ? 1 : node;
    properties.VisibleNodeMask = properties.CreationNodeMask;

    log::info("d3d12: GetCustomHeapProperties type " + std::to_string(static_cast<int>(type))
              + " -> custom, cpu " + std::to_string(static_cast<int>(properties.CPUPageProperty))
              + ", pool " + std::to_string(static_cast<int>(properties.MemoryPoolPreference)));
    return properties;
}

using CreateDeviceFn = std::int32_t(__stdcall*)(void*, std::int32_t, const void*, void**);

std::int32_t d3d12_xbox_create_device(void* adapter, const std::uint32_t* parameters, const void* iid, void** device)
{
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    if (d3d12 == nullptr) {
        return kNotImplemented;
    }
    const auto function = reinterpret_cast<CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (function == nullptr) {
        return kNotImplemented;
    }

    std::int32_t feature_level = 0xC000;
    if (parameters != nullptr) {
        const std::int32_t requested = static_cast<std::int32_t>(parameters[1]);
        if (requested >= 0xB000 && requested <= 0xC300) {
            feature_level = requested;
        }
    }

    if (GetEnvironmentVariableW(L"EUPHORIA_D3D12_DEBUG", nullptr, 0) != 0) {
        using DebugFn = std::int32_t(__stdcall*)(const GUID&, void**);
        const auto get_debug = reinterpret_cast<DebugFn>(
            GetProcAddress(d3d12, "D3D12GetDebugInterface"));
        if (get_debug != nullptr) {
            ID3D12Debug* debug = nullptr;
            const GUID& debug_iid = __uuidof(ID3D12Debug);
            const std::int32_t debug_result =
                get_debug(debug_iid, reinterpret_cast<void**>(&debug));
            log::info("d3d12: D3D12GetDebugInterface hr "
                      + std::to_string(static_cast<std::uint32_t>(debug_result)));
            if (debug_result >= 0 && debug != nullptr) {
                debug->EnableDebugLayer();
                debug->Release();
                log::info("d3d12: debug layer enabled");
            }
        }
    }

    {
        using DebugFn = std::int32_t(__stdcall*)(const GUID&, void**);
        const auto get_debug =
            reinterpret_cast<DebugFn>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
        ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
        if (get_debug != nullptr
            && get_debug(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                         reinterpret_cast<void**>(&dred))
                   >= 0
            && dred != nullptr) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
            log::info("d3d12: DRED breadcrumbs and page faults enabled");
        } else {
            log::warn("d3d12: DRED unavailable");
        }
    }

    IDXGIAdapter* warp = nullptr;
    if (GetEnvironmentVariableW(L"EUPHORIA_WARP", nullptr, 0) != 0) {
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) && factory != nullptr) {
            if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp != nullptr) {
                adapter = warp;
                log::info("d3d12: using the WARP software adapter");
            }
            factory->Release();
        }
    }
    const std::int32_t result = function(adapter, feature_level, iid, device);
    if (warp != nullptr) {
        warp->Release();
    }
    log::info("d3d12: D3D12XboxCreateDevice -> D3D12CreateDevice, hr 0x"
              + std::to_string(static_cast<std::uint32_t>(result)));
    if (result >= 0 && device != nullptr && *device != nullptr
        && GetEnvironmentVariableW(L"EUPHORIA_D3D12_DEBUG", nullptr, 0) != 0) {
        auto* host = static_cast<IUnknown*>(*device);
        ID3D12InfoQueue1* queue = nullptr;
        if (SUCCEEDED(host->QueryInterface(IID_PPV_ARGS(&queue))) && queue != nullptr) {
            DWORD cookie = 0;
            const auto callback = [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                                     D3D12_MESSAGE_ID id, LPCSTR text, void*) {
                if (severity > D3D12_MESSAGE_SEVERITY_WARNING) {
                    return;
                }
                static std::atomic<std::uint32_t> count{0};
                if (count.fetch_add(1, std::memory_order_relaxed) >= 400) {
                    return;
                }
                const std::string line = "d3d12 debug [" + std::to_string(id) + "]: "
                                       + (text != nullptr ? text : "");
                if (severity == D3D12_MESSAGE_SEVERITY_WARNING) {
                    log::warn(line);
                } else {
                    log::error(line);
                }
            };
            const HRESULT registered = queue->RegisterMessageCallback(
                callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
            log::info("d3d12: debug message callback registered, hr 0x"
                      + std::to_string(static_cast<std::uint32_t>(registered)));
            queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        } else {
            log::warn("d3d12: debug layer requested but no info queue");
        }
    }
    if (result >= 0 && device != nullptr && *device != nullptr) {
        void* raw = *device;
        void** vtable = *reinterpret_cast<void***>(raw);
        char text[64]{};
        std::snprintf(text, sizeof(text), "device %p vtable %p", raw,
                      reinterpret_cast<void*>(vtable));
        log::info("d3d12: " + std::string(text));

        *device = wrap_device(raw);
        log::info("d3d12: device proxy installed");
    }
    return result;
}

const std::array<Entry, 4> kEntries{{
    {"D3D12ConfigureVirtualMemoryX", 0, reinterpret_cast<Thunk>(&d3d12_configure_virtual_memory)},
    {"D3D12XboxCreateDevice", 0, reinterpret_cast<Thunk>(&d3d12_xbox_create_device)},
    {"D3D12SerializeVersionedRootSignature", 0,
     reinterpret_cast<Thunk>(&d3d12_serialize_versioned_root_signature)},
    {"D3D12SerializeRootSignature", 0,
     reinterpret_cast<Thunk>(&d3d12_serialize_root_signature)},
}};

const Library kLibrary{"d3d12", kEntries};

}

const Library& dx_library() noexcept
{
    return kLibrary;
}

}


