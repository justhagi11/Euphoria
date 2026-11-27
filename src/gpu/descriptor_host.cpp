#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <euphoria/gpu/descriptor_host.hpp>
#include <euphoria/gpu/title_memory.hpp>
#include <euphoria/log/log.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>

namespace euphoria::gpu {
namespace {

constexpr std::uint32_t kViewDescriptorSize = 32;
constexpr std::uint32_t kSamplerDescriptorSize = 16;
constexpr std::uint32_t kRenderTargetDescriptorSize = 128;
constexpr std::uint32_t kDepthStencilDescriptorSize = 128;

std::mutex g_mutex;
ID3D12Device* g_device = nullptr;

std::mutex g_format_mutex;
std::array<bool, 512> g_format_logged{};

const char* kind_name(ConsoleDescriptorKind kind) noexcept
{
    switch (kind) {
    case ConsoleDescriptorKind::Views:
        return "views";
    case ConsoleDescriptorKind::Samplers:
        return "samplers";
    case ConsoleDescriptorKind::RenderTargets:
        return "render targets";
    case ConsoleDescriptorKind::DepthStencil:
        return "depth stencil";
    }
    return "unknown";
}

std::string hex(std::uint64_t value)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(D3D12_CPU_DESCRIPTOR_HANDLE base, std::uint32_t index,
                                   std::uint32_t step) noexcept
{
    return D3D12_CPU_DESCRIPTOR_HANDLE{
        base.ptr + static_cast<SIZE_T>(index) * static_cast<SIZE_T>(step)};
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(D3D12_GPU_DESCRIPTOR_HANDLE base, std::uint32_t index,
                                   std::uint32_t step) noexcept
{
    return D3D12_GPU_DESCRIPTOR_HANDLE{
        base.ptr + static_cast<UINT64>(index) * static_cast<UINT64>(step)};
}

struct Record {
    ConsoleDescriptorView view = ConsoleDescriptorView::None;
    ID3D12Resource* resource = nullptr;
    ID3D12Resource* counter = nullptr;
    std::uint64_t address = 0;
    std::uint32_t size = 0;
    bool described = false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    D3D12_SAMPLER_DESC sampler{};

    Record() = default;

    Record(const Record& other)
    {
        copy_from(other);
    }

    Record& operator=(const Record& other)
    {
        if (this != &other) {
            release();
            copy_from(other);
        }
        return *this;
    }

    ~Record()
    {
        release();
    }

    void copy_from(const Record& other) noexcept
    {
        view = other.view;
        resource = other.resource;
        counter = other.counter;
        address = other.address;
        size = other.size;
        described = other.described;
        srv = other.srv;
        uav = other.uav;
        rtv = other.rtv;
        dsv = other.dsv;
        sampler = other.sampler;
        if (resource != nullptr) {
            resource->AddRef();
        }
        if (counter != nullptr) {
            counter->AddRef();
        }
    }

    void release() noexcept
    {
        if (resource != nullptr) {
            resource->Release();
            resource = nullptr;
        }
        if (counter != nullptr) {
            counter->Release();
            counter = nullptr;
        }
    }
};

struct HeapState {
    ConsoleHeapRegistration info{};
    ID3D12DescriptorHeap* host = nullptr;
    std::uint32_t capacity = 0;
    std::uint32_t step = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    bool identity = true;
    std::map<std::uint32_t, std::uint32_t> slots;
    std::uint32_t next_slot = 0;

    HeapState() = default;

    HeapState(const HeapState&) = delete;
    HeapState& operator=(const HeapState&) = delete;

    ~HeapState()
    {
        if (host != nullptr) {
            host->Release();
        }
    }
};

std::map<std::uint64_t, HeapState> g_heaps;
std::map<std::uint64_t, Record> g_records;

ID3D12Device* device_for(const HeapState& heap) noexcept
{
    if (g_device != nullptr) {
        return g_device;
    }
    if (heap.info.proxy != nullptr && heap.info.proxy->owner_device != nullptr) {
        return reinterpret_cast<ID3D12Device*>(heap.info.proxy->owner_device);
    }
    return nullptr;
}

HeapState* heap_at_locked(std::uint64_t address, std::uint32_t& index) noexcept
{
    if (address == 0) {
        return nullptr;
    }
    auto entry = g_heaps.upper_bound(address);
    if (entry == g_heaps.begin()) {
        return nullptr;
    }
    --entry;
    HeapState& heap = entry->second;
    const std::uint64_t start = heap.info.start;
    const std::uint64_t end = start + heap.info.size;
    if (address < start || address >= end) {
        return nullptr;
    }
    const std::uint32_t step = heap.step != 0 ? heap.step
                                              : console_descriptor_size(heap.info.kind);
    if (step == 0) {
        return nullptr;
    }
    const std::uint64_t at = address - start;
    const std::uint64_t slot = at / step;
    if (slot >= heap.info.count) {
        return nullptr;
    }
    index = static_cast<std::uint32_t>(slot);
    return &heap;
}

bool make_null(ID3D12Device* device, ConsoleDescriptorKind kind,
               D3D12_CPU_DESCRIPTOR_HANDLE at) noexcept
{
    if (device == nullptr) {
        return false;
    }
    if (kind == ConsoleDescriptorKind::Views) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = 0;
        desc.Buffer.NumElements = 1;
        desc.Buffer.StructureByteStride = 0;
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(nullptr, &desc, at);
        return true;
    }
    if (kind == ConsoleDescriptorKind::Samplers) {
        D3D12_SAMPLER_DESC desc{};
        desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        desc.MinLOD = 0.0f;
        desc.MaxLOD = 0.0f;
        device->CreateSampler(&desc, at);
        return true;
    }
    return false;
}

bool fill_null(HeapState& heap, ID3D12Device* device)
{
    if (heap.info.kind != ConsoleDescriptorKind::Views
        && heap.info.kind != ConsoleDescriptorKind::Samplers) {
        return true;
    }
    if (heap.capacity == 0) {
        return true;
    }
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = heap.info.host_type;
    desc.NumDescriptors = heap.capacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask = 0;
    ID3D12DescriptorHeap* block = nullptr;
    const HRESULT hr = device->CreateDescriptorHeap(
        &desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&block));
    if (FAILED(hr) || block == nullptr) {
        log::warn(std::string("d3d12: no null descriptor block for ")
                  + kind_name(heap.info.kind) + " heap at " + hex(heap.info.start));
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE at = block->GetCPUDescriptorHandleForHeapStart();
    const std::uint32_t step = device->GetDescriptorHandleIncrementSize(heap.info.host_type);
    make_null(device, heap.info.kind, at);
    std::uint32_t filled = 1;
    while (filled < heap.capacity) {
        const std::uint32_t count = std::min(filled, heap.capacity - filled);
        D3D12_CPU_DESCRIPTOR_HANDLE target = at;
        target.ptr += static_cast<SIZE_T>(filled) * step;
        device->CopyDescriptorsSimple(count, target, at, heap.info.host_type);
        filled += count;
    }
    device->CopyDescriptorsSimple(heap.capacity, heap.cpu_start, at, heap.info.host_type);
    block->Release();
    return true;
}

bool ensure_host_heap(HeapState& heap)
{
    if (heap.host != nullptr) {
        return true;
    }
    ID3D12Device* device = device_for(heap);
    if (device == nullptr) {
        log::warn(std::string("d3d12: no device for console descriptor heap at ")
                  + hex(heap.info.start));
        return false;
    }
    if (heap.info.count == 0) {
        log::warn(std::string("d3d12: empty console descriptor heap at ")
                  + hex(heap.info.start));
        return false;
    }
    const std::uint32_t capacity = std::min(heap.info.count, kMaxMirrorDescriptors);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = heap.info.host_type;
    desc.NumDescriptors = capacity;
    desc.Flags = (heap.info.kind == ConsoleDescriptorKind::Views
                  || heap.info.kind == ConsoleDescriptorKind::Samplers)
        ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
        : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask = 0;
    ID3D12DescriptorHeap* host = nullptr;
    const HRESULT hr = device->CreateDescriptorHeap(
        &desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&host));
    if (FAILED(hr) || host == nullptr) {
        log::warn(std::string("d3d12: no host descriptor heap for console heap at ")
                  + hex(heap.info.start) + " kind " + kind_name(heap.info.kind)
                  + " count " + std::to_string(heap.info.count) + " type "
                  + std::to_string(static_cast<std::uint32_t>(heap.info.host_type))
                  + " hr " + hex(static_cast<std::uint32_t>(hr)));
        return false;
    }
    heap.host = host;
    heap.capacity = capacity;
    heap.identity = heap.info.count <= capacity;
    heap.step = device->GetDescriptorHandleIncrementSize(heap.info.host_type);
    heap.cpu_start = host->GetCPUDescriptorHandleForHeapStart();
    heap.gpu_start = host->GetGPUDescriptorHandleForHeapStart();
    log::info(std::string("d3d12: host descriptor heap for ") + kind_name(heap.info.kind)
              + " at " + hex(heap.info.start) + " count " + std::to_string(heap.info.count)
              + " capacity " + std::to_string(capacity));
    if (capacity <= 4096) {
        fill_null(heap, device);
    } else {
        log::info(std::string("d3d12: deferring null fill for large ") + kind_name(heap.info.kind)
                  + " mirror at " + hex(heap.info.start) + " capacity "
                  + std::to_string(capacity));
    }
    return true;
}

bool host_slot_for(HeapState& heap, std::uint32_t index, std::uint32_t& slot) noexcept
{
    if (index >= heap.info.count) {
        return false;
    }
    if (heap.identity) {
        if (index >= heap.capacity) {
            return false;
        }
        slot = index;
        return true;
    }
    const auto existing = heap.slots.find(index);
    if (existing != heap.slots.end()) {
        slot = existing->second;
        return true;
    }
    if (heap.next_slot >= heap.capacity) {
        return false;
    }
    slot = heap.next_slot;
    ++heap.next_slot;
    heap.slots.emplace(index, slot);
    return true;
}

DXGI_FORMAT typed_for(DXGI_FORMAT format) noexcept
{
    switch (static_cast<std::uint32_t>(format)) {
    case 1:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 5:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case 9:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 15:
        return DXGI_FORMAT_R32G32_FLOAT;
    case 23:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 27:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 33:
        return DXGI_FORMAT_R16G16_FLOAT;
    case 39:
        return DXGI_FORMAT_R32_FLOAT;
    case 48:
        return DXGI_FORMAT_R8G8_UNORM;
    case 53:
        return DXGI_FORMAT_R16_FLOAT;
    case 60:
        return DXGI_FORMAT_R8_UNORM;
    case 70:
        return DXGI_FORMAT_BC1_UNORM;
    case 73:
        return DXGI_FORMAT_BC2_UNORM;
    case 76:
        return DXGI_FORMAT_BC3_UNORM;
    case 79:
        return DXGI_FORMAT_BC4_UNORM;
    case 82:
        return DXGI_FORMAT_BC5_UNORM;
    case 90:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case 92:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case 94:
        return DXGI_FORMAT_BC6H_UF16;
    case 97:
        return DXGI_FORMAT_BC7_UNORM;
    default:
        return format;
    }
}

int depth_family(std::uint32_t format) noexcept
{
    switch (format) {
    case 19:
    case 20:
    case 21:
    case 22:
        return 0;
    case 39:
    case 40:
    case 41:
        return 1;
    case 44:
    case 45:
    case 46:
    case 47:
        return 2;
    case 53:
    case 55:
        return 3;
    default:
        return -1;
    }
}

DXGI_FORMAT typeless_for_depth(DXGI_FORMAT format) noexcept
{
    static constexpr DXGI_FORMAT typeless[] = {DXGI_FORMAT_R32G8X24_TYPELESS,
                                               DXGI_FORMAT_R32_TYPELESS,
                                               DXGI_FORMAT_R24G8_TYPELESS,
                                               DXGI_FORMAT_R16_TYPELESS};
    const int family = depth_family(static_cast<std::uint32_t>(format));
    return family < 0 ? format : typeless[family];
}

DXGI_FORMAT view_for_depth(DXGI_FORMAT format) noexcept
{
    static constexpr DXGI_FORMAT depth[] = {DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                                            DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT,
                                            DXGI_FORMAT_D16_UNORM};
    const int family = depth_family(static_cast<std::uint32_t>(format));
    return family < 0 ? format : depth[family];
}

DXGI_FORMAT depth_read_format(DXGI_FORMAT view, DXGI_FORMAT resource) noexcept
{
    static constexpr DXGI_FORMAT read[] = {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
                                           DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                                           DXGI_FORMAT_R16_UNORM};
    const int family = depth_family(static_cast<std::uint32_t>(resource));
    if (family < 0) {
        return view;
    }
    if (view == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT || view == DXGI_FORMAT_X24_TYPELESS_G8_UINT) {
        return view;
    }
    if (depth_family(static_cast<std::uint32_t>(view)) == family || view == DXGI_FORMAT_UNKNOWN) {
        return read[family];
    }
    return view;
}

bool translate_view_format(DXGI_FORMAT& format, std::uint64_t handle,
                           const char* what) noexcept
{
    if (format == DXGI_FORMAT_UNKNOWN) {
        return true;
    }
    const std::uint32_t translated = pc_format(static_cast<std::uint32_t>(format));
    if (translated == 0) {
        log::warn(std::string("d3d12: unsupported ") + what + " format "
                  + std::to_string(static_cast<std::uint32_t>(format)) + " at "
                  + hex(handle));
        return false;
    }
    format = static_cast<DXGI_FORMAT>(translated);
    return true;
}

bool build_descriptor_now(HeapState& heap, const Record& record,
                          D3D12_CPU_DESCRIPTOR_HANDLE at, std::uint64_t handle);

const char* view_name(ConsoleDescriptorView view) noexcept
{
    switch (view) {
    case ConsoleDescriptorView::ConstantBuffer:
        return "cbv";
    case ConsoleDescriptorView::ShaderResource:
        return "srv";
    case ConsoleDescriptorView::UnorderedAccess:
        return "uav";
    case ConsoleDescriptorView::RenderTarget:
        return "rtv";
    case ConsoleDescriptorView::DepthStencil:
        return "dsv";
    default:
        return "other";
    }
}

bool build_descriptor(HeapState& heap, const Record& record,
                      D3D12_CPU_DESCRIPTOR_HANDLE at, std::uint64_t handle)
{
    const bool built = build_descriptor_now(heap, record, at, handle);
    static std::atomic<bool> reported{false};
    ID3D12Device* device = device_for(heap);
    if (device != nullptr && !reported.load(std::memory_order_relaxed)
        && FAILED(device->GetDeviceRemovedReason())) {
        reported.store(true, std::memory_order_relaxed);
        char text[400]{};
        D3D12_RESOURCE_DESC resource{};
        if (record.resource != nullptr) {
            resource = record.resource->GetDesc();
        }
        const auto* words = reinterpret_cast<const std::uint32_t*>(
            record.view == ConsoleDescriptorView::ShaderResource
                ? static_cast<const void*>(&record.srv)
                : static_cast<const void*>(&record.uav));
        std::snprintf(text, sizeof(text),
                      "d3d12: device removed by %s at %llx: resource dim %u %llux%u array %u mips %u "
                      "format %u flags 0x%x; described %d view words %08x %08x %08x %08x %08x %08x %08x",
                      view_name(record.view), static_cast<unsigned long long>(handle),
                      static_cast<unsigned>(resource.Dimension),
                      static_cast<unsigned long long>(resource.Width), resource.Height,
                      resource.DepthOrArraySize, resource.MipLevels,
                      static_cast<unsigned>(resource.Format), static_cast<unsigned>(resource.Flags),
                      record.described ? 1 : 0, words[0], words[1], words[2], words[3], words[4],
                      words[5], words[6]);
        log::error(text);
    }
    return built;
}
bool build_descriptor_now(HeapState& heap, const Record& record,
                          D3D12_CPU_DESCRIPTOR_HANDLE at, std::uint64_t handle)
{
    ID3D12Device* device = device_for(heap);
    if (device == nullptr) {
        return false;
    }
    switch (record.view) {
    case ConsoleDescriptorView::ConstantBuffer:
        if (record.address == 0 || record.size == 0) {
            log::warn(std::string("d3d12: empty constant buffer view at ") + hex(handle));
            return false;
        }
        {
            const std::uint32_t size = (record.size + 255u) & ~255u;
            D3D12_GPU_VIRTUAL_ADDRESS location = record.address;
            MEMORY_BASIC_INFORMATION info{};
            const bool title_address =
                VirtualQuery(reinterpret_cast<const void*>(record.address), &info, sizeof(info))
                    != 0
                && info.State == MEM_COMMIT;
            if (title_address) {
                const TitleShadowBuffer shadow =
                    create_shadow_buffer(device, record.address, size);
                if (shadow.host == nullptr || shadow.gpu_address == 0) {
                    log::warn(std::string("d3d12: no shadow for constant buffer view at ")
                              + hex(handle));
                    make_null(device, heap.info.kind, at);
                    return true;
                }
                location = shadow.gpu_address;
            }
            if ((location & 255u) != 0) {
                log::warn(std::string("d3d12: unaligned constant buffer view at ") + hex(handle));
                make_null(device, heap.info.kind, at);
                return true;
            }
            D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
            desc.BufferLocation = location;
            desc.SizeInBytes = size;
            device->CreateConstantBufferView(&desc, at);
        }
        return true;
    case ConsoleDescriptorView::ShaderResource:
        if (record.resource == nullptr && !record.described) {
            log::warn(std::string("d3d12: shader resource view without resource at ")
                      + hex(handle));
            return false;
        }
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = record.srv;
            if (record.described
                && !translate_view_format(desc.Format, handle, "shader resource view")) {
                return false;
            }
            bool described = record.described;
            if (record.resource != nullptr) {
                const D3D12_RESOURCE_DESC resource = record.resource->GetDesc();
                if ((resource.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
                    return false;
                }
                if (described && desc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
                    auto& buffer = desc.Buffer;
                    const bool raw = desc.Format == DXGI_FORMAT_R32_TYPELESS
                                     || (buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
                    if (raw) {
                        desc.Format = DXGI_FORMAT_R32_TYPELESS;
                        buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                        buffer.StructureByteStride = 0;
                    } else if (desc.Format != DXGI_FORMAT_UNKNOWN) {
                        desc.Format = typed_for(desc.Format);
                        buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
                        buffer.StructureByteStride = 0;
                    } else {
                        buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
                        if (buffer.StructureByteStride == 0) {
                            desc.Format = DXGI_FORMAT_R32_TYPELESS;
                            buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                        }
                    }
                    const std::uint64_t element =
                        desc.Format == DXGI_FORMAT_UNKNOWN ? buffer.StructureByteStride : 4;
                    if (element != 0 && resource.Width / element < buffer.FirstElement + buffer.NumElements) {
                        buffer.NumElements = buffer.FirstElement < resource.Width / element
                                                 ? static_cast<UINT>(resource.Width / element
                                                                     - buffer.FirstElement)
                                                 : 0;
                    }
                    if (buffer.NumElements == 0) {
                        return false;
                    }
                } else if (described) {
                    desc.Format = typed_for(desc.Format);
                } else if (typed_for(resource.Format) != resource.Format
                           && resource.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER
                           && depth_family(static_cast<std::uint32_t>(resource.Format)) < 0) {
                    desc = {};
                    desc.Format = typed_for(resource.Format);
                    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                        desc.Texture3D.MipLevels = static_cast<UINT>(-1);
                    } else if (resource.DepthOrArraySize > 1) {
                        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                        desc.Texture2DArray.MipLevels = static_cast<UINT>(-1);
                        desc.Texture2DArray.ArraySize = resource.DepthOrArraySize;
                    } else {
                        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        desc.Texture2D.MipLevels = static_cast<UINT>(-1);
                    }
                    described = true;
                }
                if (depth_family(static_cast<std::uint32_t>(resource.Format)) >= 0) {
                    if (described) {
                        desc.Format = depth_read_format(desc.Format, resource.Format);
                    } else {
                        desc = {};
                        desc.Format = depth_read_format(DXGI_FORMAT_UNKNOWN, resource.Format);
                        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        if (resource.DepthOrArraySize > 1) {
                            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                            desc.Texture2DArray.MipLevels = static_cast<UINT>(-1);
                            desc.Texture2DArray.ArraySize = resource.DepthOrArraySize;
                        } else {
                            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                            desc.Texture2D.MipLevels = static_cast<UINT>(-1);
                        }
                        described = true;
                    }
                }
            }
            device->CreateShaderResourceView(record.resource, described ? &desc : nullptr, at);
        }
        return true;
    case ConsoleDescriptorView::UnorderedAccess:
        if (record.resource == nullptr && !record.described) {
            log::warn(std::string("d3d12: unordered access view without resource at ")
                      + hex(handle));
            return false;
        }
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc = record.uav;
            if (record.described
                && !translate_view_format(desc.Format, handle, "unordered access view")) {
                return false;
            }
            bool described = record.described;
            ID3D12Resource* counter = record.counter;
            if (record.resource != nullptr) {
                const D3D12_RESOURCE_DESC resource = record.resource->GetDesc();
                if ((resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
                    return false;
                }
                if (described && desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
                    auto& buffer = desc.Buffer;
                    const bool raw = desc.Format == DXGI_FORMAT_R32_TYPELESS
                                     || (buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
                    if (raw) {
                        desc.Format = DXGI_FORMAT_R32_TYPELESS;
                        buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                        buffer.StructureByteStride = 0;
                        buffer.CounterOffsetInBytes = 0;
                        counter = nullptr;
                    } else if (desc.Format != DXGI_FORMAT_UNKNOWN) {
                        desc.Format = typed_for(desc.Format);
                        buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
                        buffer.StructureByteStride = 0;
                        buffer.CounterOffsetInBytes = 0;
                        counter = nullptr;
                    } else {
                        buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
                        if (buffer.StructureByteStride == 0) {
                            desc.Format = DXGI_FORMAT_R32_TYPELESS;
                            buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                            counter = nullptr;
                        }
                    }
                    if (counter == nullptr) {
                        buffer.CounterOffsetInBytes = 0;
                    }
                    const std::uint64_t element =
                        desc.Format == DXGI_FORMAT_UNKNOWN ? buffer.StructureByteStride : 4;
                    if (element != 0
                        && resource.Width / element < buffer.FirstElement + buffer.NumElements) {
                        buffer.NumElements = buffer.FirstElement < resource.Width / element
                                                 ? static_cast<UINT>(resource.Width / element
                                                                     - buffer.FirstElement)
                                                 : 0;
                    }
                    if (buffer.NumElements == 0) {
                        return false;
                    }
                } else if (described) {
                    desc.Format = typed_for(desc.Format);
                } else if (typed_for(resource.Format) != resource.Format) {
                    desc = {};
                    desc.Format = typed_for(resource.Format);
                    if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                        desc.Texture3D.WSize = static_cast<UINT>(-1);
                    } else if (resource.DepthOrArraySize > 1) {
                        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                        desc.Texture2DArray.ArraySize = resource.DepthOrArraySize;
                    } else {
                        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    }
                    described = true;
                }
            }
            device->CreateUnorderedAccessView(record.resource, counter,
                                              described ? &desc : nullptr, at);
        }
        return true;
    case ConsoleDescriptorView::RenderTarget:
        if (record.resource == nullptr) {
            log::warn(std::string("d3d12: render target view without resource at ")
                      + hex(handle));
            return false;
        }
        {
            D3D12_RENDER_TARGET_VIEW_DESC desc = record.rtv;
            if (record.described
                && !translate_view_format(desc.Format, handle, "render target view")) {
                return false;
            }
            const D3D12_RESOURCE_DESC resource = record.resource->GetDesc();
            if ((resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
                return false;
            }
            const DXGI_FORMAT typed = typed_for(resource.Format);
            if (typed != resource.Format) {
                desc = {};
                desc.Format = typed;
                if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                    desc.Texture3D.WSize = static_cast<UINT>(-1);
                } else if (resource.DepthOrArraySize > 1) {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    desc.Texture2DArray.ArraySize = resource.DepthOrArraySize;
                } else {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                }
                device->CreateRenderTargetView(record.resource, &desc, at);
            } else {
                device->CreateRenderTargetView(record.resource, nullptr, at);
            }
        }
        return true;
    case ConsoleDescriptorView::DepthStencil:
        if (record.resource == nullptr) {
            log::warn(std::string("d3d12: depth stencil view without resource at ")
                      + hex(handle));
            return false;
        }
        {
            const D3D12_RESOURCE_DESC resource = record.resource->GetDesc();
            if ((resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                log::warn(std::string("d3d12: depth stencil view skipped, resource lacks "
                                      "ALLOW_DEPTH_STENCIL at ")
                          + hex(handle));
                return false;
            }
            D3D12_DEPTH_STENCIL_VIEW_DESC desc = record.dsv;
            if (record.described
                && !translate_view_format(desc.Format, handle, "depth stencil view")) {
                return false;
            }
            const DXGI_FORMAT depth = view_for_depth(resource.Format);
            if (record.described) {
                desc.Format = depth;
            } else if (depth != resource.Format) {
                desc = {};
                desc.Format = depth;
                desc.ViewDimension = resource.DepthOrArraySize > 1
                                         ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY
                                         : D3D12_DSV_DIMENSION_TEXTURE2D;
                if (desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY) {
                    desc.Texture2DArray.ArraySize = resource.DepthOrArraySize;
                }
            }
            bool repaired = false;
            if (record.described || depth != resource.Format) {
                const bool stencil = desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                                     || desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
                if (!stencil && (desc.Flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL) != 0) {
                    desc.Flags = static_cast<D3D12_DSV_FLAGS>(desc.Flags
                                                              & ~D3D12_DSV_FLAG_READ_ONLY_STENCIL);
                    repaired = true;
                }
                if ((desc.Flags & ~0x3u) != 0) {
                    desc.Flags = static_cast<D3D12_DSV_FLAGS>(desc.Flags & 0x3u);
                    repaired = true;
                }
                const UINT mips = resource.MipLevels == 0 ? 1 : resource.MipLevels;
                const UINT array = resource.DepthOrArraySize == 0 ? 1 : resource.DepthOrArraySize;
                switch (desc.ViewDimension) {
                case D3D12_DSV_DIMENSION_TEXTURE2D:
                    if (desc.Texture2D.MipSlice >= mips) {
                        desc.Texture2D.MipSlice = mips - 1;
                        repaired = true;
                    }
                    break;
                case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
                    if (desc.Texture2DArray.MipSlice >= mips) {
                        desc.Texture2DArray.MipSlice = mips - 1;
                        repaired = true;
                    }
                    if (desc.Texture2DArray.FirstArraySlice >= array) {
                        desc.Texture2DArray.FirstArraySlice = 0;
                        repaired = true;
                    }
                    if (desc.Texture2DArray.ArraySize == 0
                        || desc.Texture2DArray.FirstArraySlice + desc.Texture2DArray.ArraySize
                               > array) {
                        desc.Texture2DArray.ArraySize = array - desc.Texture2DArray.FirstArraySlice;
                        repaired = true;
                    }
                    break;
                case D3D12_DSV_DIMENSION_TEXTURE2DMS:
                case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
                    if (resource.SampleDesc.Count <= 1) {
                        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                        desc.Texture2D.MipSlice = 0;
                        repaired = true;
                    }
                    break;
                case D3D12_DSV_DIMENSION_TEXTURE1D:
                case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
                    if (resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
                        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                        desc.Texture2D.MipSlice = 0;
                        repaired = true;
                    }
                    break;
                default:
                    desc.ViewDimension = array > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY
                                                   : D3D12_DSV_DIMENSION_TEXTURE2D;
                    desc.Texture2DArray = {};
                    desc.Texture2DArray.ArraySize = array > 1 ? array : 0;
                    if (array <= 1) {
                        desc.Texture2D.MipSlice = 0;
                    }
                    repaired = true;
                    break;
                }
                if (resource.SampleDesc.Count > 1
                    && desc.ViewDimension != D3D12_DSV_DIMENSION_TEXTURE2DMS
                    && desc.ViewDimension != D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY) {
                    desc.ViewDimension = array > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY
                                                   : D3D12_DSV_DIMENSION_TEXTURE2DMS;
                    desc.Texture2DMSArray.FirstArraySlice = 0;
                    desc.Texture2DMSArray.ArraySize = array;
                    repaired = true;
                }
            }
            {
                static std::atomic<std::uint32_t> logged{0};
                if (repaired && logged.fetch_add(1, std::memory_order_relaxed) < 64) {
                    char text[220]{};
                    std::snprintf(text, sizeof(text),
                                  "d3d12: dsv %llux%u array %u mips %u format %u flags 0x%x -> "
                                  "view format %u dim %u described %d mip %u first %u size %u",
                                  static_cast<unsigned long long>(resource.Width),
                                  resource.Height, resource.DepthOrArraySize, resource.MipLevels,
                                  static_cast<unsigned>(resource.Format),
                                  static_cast<unsigned>(resource.Flags),
                                  static_cast<unsigned>(desc.Format),
                                  static_cast<unsigned>(desc.ViewDimension),
                                  record.described ? 1 : 0, desc.Texture2DArray.MipSlice,
                                  desc.Texture2DArray.FirstArraySlice,
                                  desc.Texture2DArray.ArraySize);
                    log::info(text);
                }
            }
            device->CreateDepthStencilView(
                record.resource, record.described || depth != resource.Format ? &desc : nullptr,
                at);
        }
        return true;
    case ConsoleDescriptorView::Sampler:
        device->CreateSampler(&record.sampler, at);
        return true;
    case ConsoleDescriptorView::None:
        break;
    }
    log::warn(std::string("d3d12: unsupported descriptor view kind at ") + hex(handle)
              + " in " + kind_name(heap.info.kind) + " heap");
    return false;
}

bool mirror_locked(HeapState& heap, std::uint64_t handle, std::uint32_t index)
{
    if (!ensure_host_heap(heap)) {
        return false;
    }
    std::uint32_t slot = 0;
    if (!host_slot_for(heap, index, slot)) {
        log::warn(std::string("d3d12: no host slot for ") + hex(handle) + " in "
                  + kind_name(heap.info.kind) + " heap at " + hex(heap.info.start));
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE at = cpu_at(heap.cpu_start, slot, heap.step);
    const auto entry = g_records.find(handle);
    if (entry == g_records.end() || entry->second.view == ConsoleDescriptorView::None) {
        if (!make_null(device_for(heap), heap.info.kind, at)) {
            log::info(std::string("d3d12: no record for ") + hex(handle) + " in "
                      + kind_name(heap.info.kind) + " heap");
            return false;
        }
        return true;
    }
    if (!build_descriptor(heap, entry->second, at, handle)) {
        make_null(device_for(heap), heap.info.kind, at);
        log::warn(std::string("d3d12: slot left null for ") + hex(handle) + " in "
                  + kind_name(heap.info.kind) + " heap slot "
                  + std::to_string(index));
        return false;
    }
    return true;
}

bool begin_write(std::uint64_t handle, ConsoleDescriptorKind expected, const char* what,
                 HeapState*& heap, std::uint32_t& index)
{
    if (handle == 0) {
        log::warn(std::string("d3d12: ") + what + " with null handle");
        return false;
    }
    HeapState* found = heap_at_locked(handle, index);
    if (found == nullptr) {
        log::warn(std::string("d3d12: ") + what + " outside a console heap at "
                  + hex(handle));
        return false;
    }
    if (found->info.kind != expected) {
        log::warn(std::string("d3d12: ") + what + " at " + hex(handle) + " in "
                  + kind_name(found->info.kind) + " heap, not "
                  + kind_name(expected));
        return false;
    }
    heap = found;
    return true;
}

void store_record(std::uint64_t handle, const Record& record)
{
    g_records[handle] = record;
}

void copy_one_locked(std::uint64_t dst, std::uint64_t src, std::uint32_t step)
{
    if (dst == 0 || src == 0 || step == 0) {
        return;
    }
    std::memmove(reinterpret_cast<void*>(static_cast<std::uintptr_t>(dst)),
                 reinterpret_cast<const void*>(static_cast<std::uintptr_t>(src)), step);
    const auto source = g_records.find(src);
    if (source != g_records.end()) {
        g_records[dst] = source->second;
    } else {
        g_records.erase(dst);
    }
    std::uint32_t index = 0;
    if (HeapState* heap = heap_at_locked(dst, index)) {
        mirror_locked(*heap, dst, index);
    }
}

void log_format_once(std::uint32_t format, const char* text, bool warning)
{
    if (format >= g_format_logged.size()) {
        return;
    }
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_format_mutex);
        first = !g_format_logged[format];
        g_format_logged[format] = true;
    }
    if (!first) {
        return;
    }
    std::string message = std::string("d3d12: ") + text + " console format "
        + std::to_string(format);
    if (warning) {
        log::warn(message);
    } else {
        log::info(message);
    }
}

float finite_lod(float value, float fallback) noexcept
{
    if (std::isnan(value)) {
        return fallback;
    }
    const float maximum = std::numeric_limits<float>::max();
    return std::clamp(value, -maximum, maximum);
}

D3D12_TEXTURE_ADDRESS_MODE address_mode(std::uint32_t mode) noexcept
{
    if (mode >= 1 && mode <= 5) {
        return static_cast<D3D12_TEXTURE_ADDRESS_MODE>(static_cast<int>(mode));
    }
    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

struct FormatMapping {
    std::uint32_t console;
    std::uint32_t pc;
    bool exact;
    const char* name;
};

constexpr FormatMapping kFormatMappings[] = {
    {116, 26, false, "R10G10B10_7E3_A2_FLOAT"},
    {117, 26, false, "R10G10B10_6E4_A2_FLOAT"},
    {118, 20, true, "D16_UNORM_S8_UINT"},
    {119, 21, true, "R16_UNORM_X8_TYPELESS"},
    {120, 22, true, "X16_TYPELESS_G8_UINT"},
    {189, 24, false, "R10G10B10_SNORM_A2_UNORM"},
    {190, 115, false, "R4G4_UNORM"},
    {191, 28, false, "24/8 color"},
};

}

std::uint32_t console_descriptor_size(ConsoleDescriptorKind kind) noexcept
{
    switch (kind) {
    case ConsoleDescriptorKind::Views:
        return kViewDescriptorSize;
    case ConsoleDescriptorKind::Samplers:
        return kSamplerDescriptorSize;
    case ConsoleDescriptorKind::RenderTargets:
        return kRenderTargetDescriptorSize;
    case ConsoleDescriptorKind::DepthStencil:
        return kDepthStencilDescriptorSize;
    }
    return 0;
}

D3D12_DESCRIPTOR_HEAP_TYPE console_host_heap_type(ConsoleDescriptorKind kind) noexcept
{
    switch (kind) {
    case ConsoleDescriptorKind::Views:
        return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    case ConsoleDescriptorKind::Samplers:
        return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    case ConsoleDescriptorKind::RenderTargets:
        return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    case ConsoleDescriptorKind::DepthStencil:
        return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    }
    return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
}

void set_descriptor_device(ID3D12Device* device) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_device = device;
}

ID3D12Device* descriptor_device() noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_device;
}

bool register_console_heap(std::uint64_t start, std::uint64_t byte_size,
                           ConsoleDescriptorKind kind, std::uint32_t count,
                           DescriptorHeapProxy* proxy) noexcept
{
    if (start == 0 || count == 0) {
        log::warn(std::string("d3d12: descriptor heap registration with no address or count at ")
                  + hex(start));
        return false;
    }
    const std::uint32_t step = console_descriptor_size(kind);
    if (step == 0) {
        log::warn(std::string("d3d12: descriptor heap with unknown kind at ") + hex(start));
        return false;
    }
    const std::uint64_t required = static_cast<std::uint64_t>(step) * count;
    std::uint64_t size = byte_size;
    if (size == 0) {
        size = required;
    } else if (size < required) {
        log::warn(std::string("d3d12: descriptor heap at ") + hex(start) + " holds "
                  + std::to_string(size) + " bytes but needs " + std::to_string(required));
        size = required;
    }
    if (start > std::numeric_limits<std::uint64_t>::max() - size) {
        log::warn(std::string("d3d12: descriptor heap at ") + hex(start)
                  + " wraps the address space");
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::uint64_t end = start + size;
    for (const auto& entry : g_heaps) {
        const std::uint64_t other_start = entry.second.info.start;
        if (other_start == start) {
            continue;
        }
        const std::uint64_t other_end = other_start + entry.second.info.size;
        if (start < other_end && other_start < end) {
            log::warn(std::string("d3d12: descriptor heap at ") + hex(start)
                      + " overlaps one at " + hex(other_start));
            return false;
        }
    }
    const auto existing = g_heaps.find(start);
    if (existing != g_heaps.end()) {
        for (auto entry = g_records.lower_bound(start);
             entry != g_records.end() && entry->first < start + existing->second.info.size;) {
            entry = g_records.erase(entry);
        }
        g_heaps.erase(existing);
    }
    auto insertion = g_heaps.try_emplace(start);
    HeapState& heap = insertion.first->second;
    heap.info.start = start;
    heap.info.size = size;
    heap.info.kind = kind;
    heap.info.count = count;
    heap.info.host_type = console_host_heap_type(kind);
    heap.info.proxy = proxy;
    heap.step = step;
    heap.identity = count <= kMaxMirrorDescriptors;
    log::info(std::string("d3d12: console descriptor heap at ") + hex(start) + " kind "
              + kind_name(kind) + " count " + std::to_string(count) + " size "
              + std::to_string(size));
    return true;
}

void unregister_console_heap(std::uint64_t start) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto heap = g_heaps.find(start);
    if (heap == g_heaps.end()) {
        return;
    }
    const std::uint64_t end = start + heap->second.info.size;
    for (auto entry = g_records.lower_bound(start);
         entry != g_records.end() && entry->first < end;) {
        entry = g_records.erase(entry);
    }
    g_heaps.erase(heap);
}

void clear_console_heaps() noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_records.clear();
    g_heaps.clear();
}

const ConsoleHeapRegistration* lookup_console_heap(std::uint64_t address) noexcept
{
    static thread_local ConsoleHeapRegistration result;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t index = 0;
    const HeapState* heap = heap_at_locked(address, index);
    if (heap == nullptr) {
        return nullptr;
    }
    result = heap->info;
    return &result;
}

bool lookup_console_heap(std::uint64_t address, ConsoleHeapRegistration& out) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t index = 0;
    const HeapState* heap = heap_at_locked(address, index);
    if (heap == nullptr) {
        return false;
    }
    out = heap->info;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE resolve_console_descriptor(std::uint64_t address) noexcept
{
    if (address == 0) {
        return {};
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t index = 0;
    HeapState* heap = heap_at_locked(address, index);
    if (heap == nullptr) {
        log::warn(std::string("d3d12: descriptor resolution outside a console heap at ")
                  + hex(address));
        return {};
    }
    const bool mirrored = mirror_locked(*heap, address, index);
    if (!mirrored && (heap->info.kind == ConsoleDescriptorKind::RenderTargets
                      || heap->info.kind == ConsoleDescriptorKind::DepthStencil)) {
        log::warn(std::string("d3d12: no host descriptor for ") + hex(address) + " in "
                  + kind_name(heap->info.kind) + " heap");
        return {};
    }
    std::uint32_t slot = 0;
    if (!host_slot_for(*heap, index, slot)) {
        log::warn(std::string("d3d12: no host descriptor for ") + hex(address));
        return {};
    }
    return cpu_at(heap->cpu_start, slot, heap->step);
}

D3D12_GPU_DESCRIPTOR_HANDLE resolve_console_gpu(std::uint64_t address) noexcept
{
    if (address == 0) {
        return {};
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t index = 0;
    HeapState* heap = heap_at_locked(address, index);
    if (heap == nullptr) {
        log::warn(std::string("d3d12: descriptor resolution outside a console heap at ")
                  + hex(address));
        return {};
    }
    if (heap->info.kind != ConsoleDescriptorKind::Views
        && heap->info.kind != ConsoleDescriptorKind::Samplers) {
        log::warn(std::string("d3d12: gpu descriptor of a non shader visible heap at ")
                  + hex(address) + " kind " + kind_name(heap->info.kind));
        return {};
    }
    mirror_locked(*heap, address, index);
    std::uint32_t slot = 0;
    if (!host_slot_for(*heap, index, slot)) {
        log::warn(std::string("d3d12: no host descriptor for ") + hex(address));
        return {};
    }
    return gpu_at(heap->gpu_start, slot, heap->step);
}

std::uint32_t pc_format(std::uint32_t console_format) noexcept
{
    if (console_format == 0) {
        return 0;
    }
    for (const FormatMapping& mapping : kFormatMappings) {
        if (mapping.console == console_format) {
            if (!mapping.exact) {
                log_format_once(console_format, mapping.name, false);
            }
            return mapping.pc;
        }
    }
    if (console_format <= 115) {
        return console_format;
    }
    log_format_once(console_format, "unsupported console format", true);
    return 0;
}

bool console_format_supported(std::uint32_t console_format) noexcept
{
    if (console_format == 0) {
        return false;
    }
    if (console_format <= 115) {
        return true;
    }
    for (const FormatMapping& mapping : kFormatMappings) {
        if (mapping.console == console_format) {
            return true;
        }
    }
    return false;
}

D3D12_SAMPLER_DESC translate_console_sampler(const std::uint32_t (&words)[27]) noexcept
{
    const auto f = [&words](std::size_t index) {
        return std::bit_cast<float>(words[index]);
    };
    const std::uint32_t mag = words[1];
    const std::uint32_t min = words[2];
    const std::uint32_t mip = words[3];
    const std::uint32_t mode = words[5];
    const std::uint32_t anisotropy = words[13];
    const std::uint32_t compare = words[16];
    const auto linear = [](std::uint32_t filter) {
        return static_cast<std::uint32_t>(filter == 1 || filter == 3);
    };
    const bool aniso = mag >= 2 || min >= 2;
    const bool compares = compare >= 2 && compare <= 7;
    const std::uint32_t reduction = mode == 1 ? 2u : (mode == 2 ? 3u : (compares ? 1u : 0u));
    const std::uint32_t filters = aniso
        ? 0x55u
        : (linear(min) << 4 | linear(mag) << 2 | static_cast<std::uint32_t>(mip == 2));
    const float min_lod = finite_lod(f(9), 0.0f);
    const float max_lod = mip == 0 ? min_lod : std::max(finite_lod(f(10), min_lod), min_lod);

    D3D12_SAMPLER_DESC desc{};
    desc.Filter = static_cast<D3D12_FILTER>(static_cast<int>(reduction << 7 | filters));
    desc.AddressU = address_mode(words[6]);
    desc.AddressV = address_mode(words[7]);
    desc.AddressW = address_mode(words[8]);
    desc.MipLODBias = std::isnan(f(11))
        ? 0.0f
        : std::clamp(f(11), D3D12_MIP_LOD_BIAS_MIN, D3D12_MIP_LOD_BIAS_MAX);
    desc.MaxAnisotropy = aniso
        ? std::clamp(anisotropy, 1u, 16u)
        : 1u;
    desc.ComparisonFunc = reduction == 1
        ? static_cast<D3D12_COMPARISON_FUNC>(static_cast<int>(compare))
        : D3D12_COMPARISON_FUNC_NEVER;
    desc.BorderColor[0] = f(17);
    desc.BorderColor[1] = f(18);
    desc.BorderColor[2] = f(19);
    desc.BorderColor[3] = f(20);
    desc.MinLOD = min_lod;
    desc.MaxLOD = max_lod;
    return desc;
}

void write_constant_buffer_view(std::uint64_t handle, std::uint64_t gpu_address,
                                std::uint32_t size) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    HeapState* heap = nullptr;
    std::uint32_t index = 0;
    if (!begin_write(handle, ConsoleDescriptorKind::Views, "constant buffer view", heap,
                     index)) {
        return;
    }
    Record record{};
    record.view = ConsoleDescriptorView::ConstantBuffer;
    record.address = gpu_address;
    record.size = size;
    store_record(handle, record);
    mirror_locked(*heap, handle, index);
}

void write_shader_resource_view(std::uint64_t handle, ID3D12Resource* resource,
                                const D3D12_SHADER_RESOURCE_VIEW_DESC* desc) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    HeapState* heap = nullptr;
    std::uint32_t index = 0;
    if (!begin_write(handle, ConsoleDescriptorKind::Views, "shader resource view", heap,
                     index)) {
        return;
    }
    if (resource == nullptr && desc == nullptr) {
        log::warn(std::string("d3d12: shader resource view with no resource at ")
                  + hex(handle));
        return;
    }
    Record record{};
    record.view = ConsoleDescriptorView::ShaderResource;
    record.resource = resource;
    if (resource != nullptr) {
        resource->AddRef();
    }
    if (desc != nullptr) {
        record.described = true;
        record.srv = *desc;
    }
    store_record(handle, record);
    mirror_locked(*heap, handle, index);
}

void write_unordered_access_view(std::uint64_t handle, ID3D12Resource* resource,
                                 ID3D12Resource* counter,
                                 const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    HeapState* heap = nullptr;
    std::uint32_t index = 0;
    if (!begin_write(handle, ConsoleDescriptorKind::Views, "unordered access view", heap,
                     index)) {
        return;
    }
    if (resource == nullptr && desc == nullptr) {
        log::warn(std::string("d3d12: unordered access view with no resource at ")
                  + hex(handle));
        return;
    }
    Record record{};
    record.view = ConsoleDescriptorView::UnorderedAccess;
    record.resource = resource;
    record.counter = counter;
    if (resource != nullptr) {
        resource->AddRef();
    }
    if (counter != nullptr) {
        counter->AddRef();
    }
    if (desc != nullptr) {
        record.described = true;
        record.uav = *desc;
    }
    store_record(handle, record);
    mirror_locked(*heap, handle, index);
}

void write_render_target_view(std::uint64_t handle, ID3D12Resource* resource,
                              const D3D12_RENDER_TARGET_VIEW_DESC* desc) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    HeapState* heap = nullptr;
    std::uint32_t index = 0;
    if (!begin_write(handle, ConsoleDescriptorKind::RenderTargets, "render target view",
                     heap, index)) {
        return;
    }
    if (resource == nullptr) {
        log::warn(std::string("d3d12: render target view with no resource at ")
                  + hex(handle));
        return;
    }
    Record record{};
    record.view = ConsoleDescriptorView::RenderTarget;
    record.resource = resource;
    resource->AddRef();
    if (desc != nullptr) {
        record.described = true;
        record.rtv = *desc;
    }
    store_record(handle, record);
    mirror_locked(*heap, handle, index);
}

void write_depth_stencil_view(std::uint64_t handle, ID3D12Resource* resource,
                              const D3D12_DEPTH_STENCIL_VIEW_DESC* desc) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    HeapState* heap = nullptr;
    std::uint32_t index = 0;
    if (!begin_write(handle, ConsoleDescriptorKind::DepthStencil, "depth stencil view",
                     heap, index)) {
        return;
    }
    if (resource == nullptr) {
        log::warn(std::string("d3d12: depth stencil view with no resource at ")
                  + hex(handle));
        return;
    }
    Record record{};
    record.view = ConsoleDescriptorView::DepthStencil;
    record.resource = resource;
    resource->AddRef();
    if (desc != nullptr) {
        record.described = true;
        record.dsv = *desc;
    }
    store_record(handle, record);
    mirror_locked(*heap, handle, index);
}

D3D12_SAMPLER_DESC sanitize_sampler(D3D12_SAMPLER_DESC desc) noexcept
{
    const auto address = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
        const int value = static_cast<int>(mode);
        return value >= 1 && value <= 5 ? mode : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    };
    desc.AddressU = address(desc.AddressU);
    desc.AddressV = address(desc.AddressV);
    desc.AddressW = address(desc.AddressW);
    desc.MaxAnisotropy = std::clamp(desc.MaxAnisotropy, 1u, 16u);
    desc.MipLODBias = std::isnan(desc.MipLODBias)
        ? 0.0f
        : std::clamp(desc.MipLODBias, D3D12_MIP_LOD_BIAS_MIN, D3D12_MIP_LOD_BIAS_MAX);
    if (std::isnan(desc.MinLOD)) {
        desc.MinLOD = 0.0f;
    }
    if (std::isnan(desc.MaxLOD) || desc.MaxLOD < desc.MinLOD) {
        desc.MaxLOD = desc.MinLOD;
    }
    return desc;
}

void write_sampler(std::uint64_t handle, const D3D12_SAMPLER_DESC& desc) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    HeapState* heap = nullptr;
    std::uint32_t index = 0;
    if (!begin_write(handle, ConsoleDescriptorKind::Samplers, "sampler", heap, index)) {
        return;
    }
    Record record{};
    record.view = ConsoleDescriptorView::Sampler;
    record.sampler = sanitize_sampler(desc);
    store_record(handle, record);
    mirror_locked(*heap, handle, index);
}

void write_sampler_words(std::uint64_t handle, const std::uint32_t (&words)[13]) noexcept
{
    D3D12_SAMPLER_DESC desc{};
    static_assert(sizeof(D3D12_SAMPLER_DESC) >= sizeof(words));
    std::memcpy(&desc, words, sizeof(words));
    write_sampler(handle, desc);
}

void write_console_sampler(std::uint64_t handle,
                           const std::uint32_t (&words)[27]) noexcept
{
    write_sampler(handle, translate_console_sampler(words));
}

bool mirror_slot(std::uint64_t handle) noexcept
{
    if (handle == 0) {
        log::warn(std::string("d3d12: mirror of a null descriptor handle"));
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t index = 0;
    HeapState* heap = heap_at_locked(handle, index);
    if (heap == nullptr) {
        log::warn(std::string("d3d12: mirror of ") + hex(handle)
                  + " outside a console heap");
        return false;
    }
    return mirror_locked(*heap, handle, index);
}

bool mirror_slots(std::uint64_t first_handle, std::uint32_t count) noexcept
{
    if (first_handle == 0 || count == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t index = 0;
    HeapState* heap = heap_at_locked(first_handle, index);
    if (heap == nullptr) {
        log::warn(std::string("d3d12: mirror of ") + hex(first_handle)
                  + " outside a console heap");
        return false;
    }
    bool all = true;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t handle = first_handle
            + static_cast<std::uint64_t>(i) * heap->step;
        if (!mirror_locked(*heap, handle, index + i)) {
            all = false;
        }
    }
    return all;
}

ID3D12DescriptorHeap* console_host_heap(std::uint64_t address) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t index = 0;
    HeapState* heap = heap_at_locked(address, index);
    if (heap == nullptr || !ensure_host_heap(*heap)) {
        return nullptr;
    }
    return heap->host;
}

void copy_console_descriptors_simple(std::uint32_t count, std::uint64_t dst,
                                     std::uint64_t src,
                                     ConsoleDescriptorKind kind) noexcept
{
    const std::uint32_t step = console_descriptor_size(kind);
    if (count == 0) {
        return;
    }
    if (step == 0 || dst == 0 || src == 0) {
        log::warn(std::string("d3d12: descriptor copy with null handle at ") + hex(src));
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t offset = static_cast<std::uint64_t>(i) * step;
        copy_one_locked(dst + offset, src + offset, step);
    }
}

void copy_console_descriptors(std::uint32_t dst_range_count, const std::uint64_t* dst_starts,
                              const std::uint32_t* dst_sizes, std::uint32_t src_range_count,
                              const std::uint64_t* src_starts, const std::uint32_t* src_sizes,
                              ConsoleDescriptorKind kind) noexcept
{
    const std::uint32_t step = console_descriptor_size(kind);
    if (step == 0 || dst_starts == nullptr || src_starts == nullptr
        || dst_range_count == 0 || src_range_count == 0) {
        log::warn(std::string("d3d12: descriptor copy with no ranges"));
        return;
    }
    const auto range_size = [](const std::uint32_t* sizes, std::uint32_t index) {
        return sizes != nullptr ? sizes[index] : 1u;
    };
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t dst_range = 0;
    std::uint64_t to = dst_starts[0];
    std::uint32_t room = range_size(dst_sizes, 0);
    for (std::uint32_t source_index = 0; source_index < src_range_count; ++source_index) {
        std::uint64_t from = src_starts[source_index];
        std::uint32_t left = range_size(src_sizes, source_index);
        while (left > 0) {
            while (room == 0) {
                ++dst_range;
                if (dst_range >= dst_range_count) {
                    return;
                }
                to = dst_starts[dst_range];
                room = range_size(dst_sizes, dst_range);
            }
            const std::uint32_t take = std::min(left, room);
            for (std::uint32_t i = 0; i < take; ++i) {
                const std::uint64_t offset = static_cast<std::uint64_t>(i) * step;
                copy_one_locked(to + offset, from + offset, step);
            }
            const std::uint64_t bytes = static_cast<std::uint64_t>(take) * step;
            to += bytes;
            from += bytes;
            room -= take;
            left -= take;
        }
    }
}

ConsoleDescriptorView descriptor_view_kind(std::uint64_t handle) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto entry = g_records.find(handle);
    return entry != g_records.end() ? entry->second.view : ConsoleDescriptorView::None;
}

DXGI_FORMAT depth_typeless_format(DXGI_FORMAT format) noexcept
{
    return typeless_for_depth(format);
}

DXGI_FORMAT depth_view_format(DXGI_FORMAT format) noexcept
{
    return view_for_depth(format);
}

ID3D12Resource* console_view_resource(std::uint64_t handle) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto entry = g_records.find(handle);
    return entry != g_records.end() ? entry->second.resource : nullptr;
}

bool is_console_handle(std::uint64_t address) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t index = 0;
    return heap_at_locked(address, index) != nullptr;
}

bool console_handle_index(std::uint64_t address, std::uint32_t& index) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return heap_at_locked(address, index) != nullptr;
}

ID3D12Resource* unwrap_console_resource(void* object) noexcept
{
    return unwrap_resource(object);
}

ID3D12DescriptorHeap* unwrap_console_descriptor_heap(void* object) noexcept
{
    return unwrap_descriptor_heap(object);
}

bool is_console_descriptor_heap(const void* object) noexcept
{
    return is_descriptor_heap_proxy(object);
}

}
