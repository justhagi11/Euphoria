#include <euphoria/gpu/memory.hpp>

#include <cstring>
#include <unordered_map>

#include <d3d12.h>
#include <windows.h>

namespace euphoria::gpu {
namespace {

D3D12_HEAP_PROPERTIES heap_properties(Placement placement)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    switch (placement) {
    case Placement::DeviceLocal:
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        break;
    case Placement::Unified:
        properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        break;
    case Placement::Upload:
        properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        break;
    case Placement::Readback:
        properties.Type = D3D12_HEAP_TYPE_READBACK;
        break;
    }
    return properties;
}

D3D12_RESOURCE_DESC buffer_description(std::uint64_t size)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

D3D12_RESOURCE_FLAGS resource_flags(Placement placement)
{
    (void)placement;
    return D3D12_RESOURCE_FLAG_NONE;
}

D3D12_RESOURCE_STATES initial_state(Placement placement)
{
    return placement == Placement::DeviceLocal ? D3D12_RESOURCE_STATE_COPY_DEST
                                               : D3D12_RESOURCE_STATE_COMMON;
}

}

struct MemoryManager::Impl {
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    ID3D12CommandAllocator* allocator{};
    ID3D12GraphicsCommandList* list{};
    ID3D12Fence* fence{};
    HANDLE event{};
    std::uint64_t fence_value{};
    MemoryStats stats;

    struct Tracked {
        std::uint64_t size{};
        Placement placement{};
    };
    std::unordered_map<ID3D12Resource*, Tracked> tracked;

    bool create_device()
    {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue)))) {
            return false;
        }
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator)))) {
            return false;
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                             IID_PPV_ARGS(&list)))) {
            return false;
        }
        list->Close();

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            return false;
        }
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }

    void wait_for_gpu()
    {
        ++fence_value;
        queue->Signal(fence, fence_value);
        if (fence->GetCompletedValue() < fence_value) {
            fence->SetEventOnCompletion(fence_value, event);
            WaitForSingleObject(event, INFINITE);
        }
    }

    ID3D12Resource* make(std::uint64_t size, Placement placement)
    {
        const D3D12_HEAP_PROPERTIES properties = heap_properties(placement);
        const D3D12_RESOURCE_DESC description = buffer_description(size);
        ID3D12Resource* resource = nullptr;
        if (FAILED(device->CreateCommittedResource(
                &properties, D3D12_HEAP_FLAG_NONE, &description, initial_state(placement),
                nullptr, IID_PPV_ARGS(&resource)))) {
            return nullptr;
        }
        tracked[resource] = {size, placement};
        switch (placement) {
        case Placement::DeviceLocal:
            stats.device_bytes += size;
            break;
        case Placement::Unified:
            stats.unified_bytes += size;
            break;
        case Placement::Upload:
            stats.upload_bytes += size;
            break;
        case Placement::Readback:
            stats.readback_bytes += size;
            break;
        }
        return resource;
    }

    ID3D12Resource* upload(std::span<const std::uint8_t> data, Placement placement)
    {
        if (data.empty()) {
            return nullptr;
        }

        if (placement == Placement::Unified || placement == Placement::Upload) {
            ID3D12Resource* resource = make(data.size(), placement);
            if (resource == nullptr) {
                return nullptr;
            }
            void* mapped = nullptr;
            const D3D12_RANGE range{0, data.size()};
            if (FAILED(resource->Map(0, &range, &mapped))) {
                release(resource);
                return nullptr;
            }
            std::memcpy(mapped, data.data(), data.size());
            resource->Unmap(0, nullptr);
            ++stats.uploads;
            return resource;
        }

        if (placement != Placement::DeviceLocal) {
            return nullptr;
        }

        ID3D12Resource* target = make(data.size(), placement);
        if (target == nullptr) {
            return nullptr;
        }

        ID3D12Resource* staging = make(data.size(), Placement::Upload);
        if (staging == nullptr) {
            release(target);
            return nullptr;
        }
        stats.staging_live += data.size();
        if (stats.staging_live > stats.staging_peak) {
            stats.staging_peak = stats.staging_live;
        }

        void* mapped = nullptr;
        const D3D12_RANGE range{0, data.size()};
        if (FAILED(staging->Map(0, &range, &mapped))) {
            stats.staging_live -= data.size();
            release(staging);
            release(target);
            return nullptr;
        }
        std::memcpy(mapped, data.data(), data.size());
        staging->Unmap(0, nullptr);

        allocator->Reset();
        list->Reset(allocator, nullptr);
        list->CopyBufferRegion(target, 0, staging, 0, data.size());
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[]{list};
        queue->ExecuteCommandLists(1, lists);
        wait_for_gpu();

        stats.staging_live -= data.size();
        release(staging);
        ++stats.uploads;
        return target;
    }

    void release(ID3D12Resource* resource)
    {
        if (resource == nullptr) {
            return;
        }
        const auto entry = tracked.find(resource);
        if (entry != tracked.end()) {
            const Tracked tracked_value = entry->second;
            switch (tracked_value.placement) {
            case Placement::DeviceLocal:
                stats.device_bytes -= tracked_value.size;
                break;
            case Placement::Unified:
                stats.unified_bytes -= tracked_value.size;
                break;
            case Placement::Upload:
                stats.upload_bytes -= tracked_value.size;
                break;
            case Placement::Readback:
                stats.readback_bytes -= tracked_value.size;
                break;
            }
            tracked.erase(entry);
        }
        resource->Release();
    }

    ~Impl()
    {
        for (auto& [resource, tracked_value] : tracked) {
            (void)tracked_value;
            if (resource != nullptr) {
                resource->Release();
            }
        }
        if (event != nullptr) {
            CloseHandle(event);
        }
        if (fence != nullptr) {
            fence->Release();
        }
        if (list != nullptr) {
            list->Release();
        }
        if (allocator != nullptr) {
            allocator->Release();
        }
        if (queue != nullptr) {
            queue->Release();
        }
        if (device != nullptr) {
            device->Release();
        }
    }
};

MemoryManager::MemoryManager()
    : impl_(std::make_unique<Impl>())
{
}

MemoryManager::~MemoryManager() = default;

bool MemoryManager::initialize()
{
    return impl_->create_device();
}

ID3D12Resource* MemoryManager::create(std::uint64_t size, Placement placement)
{
    return impl_->make(size, placement);
}

ID3D12Resource* MemoryManager::upload(std::span<const std::uint8_t> data, Placement placement)
{
    return impl_->upload(data, placement);
}

void MemoryManager::release(ID3D12Resource* resource)
{
    impl_->release(resource);
}

const MemoryStats& MemoryManager::stats() const noexcept
{
    return impl_->stats;
}

std::string MemoryManager::description() const
{
    return "device " + std::to_string(impl_->stats.device_bytes)
         + " B, unified " + std::to_string(impl_->stats.unified_bytes)
         + " B, upload " + std::to_string(impl_->stats.upload_bytes)
         + " B, staging peak " + std::to_string(impl_->stats.staging_peak)
         + " B, uploads " + std::to_string(impl_->stats.uploads);
}

}
