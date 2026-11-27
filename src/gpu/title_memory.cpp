#include <euphoria/gpu/title_memory.hpp>
#include <euphoria/log/log.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include <windows.h>

namespace euphoria::gpu {
namespace {

constexpr std::uint64_t page_mask = ~(title_page_size - 1);
constexpr std::uint64_t maximum_pages_per_call = 1ull << 20;
constexpr UINT page_table_alignment = 256;

std::uint64_t align_page(std::uint64_t value) noexcept
{
    return (value + title_page_size - 1) & page_mask;
}

bool page_bounds(std::uint64_t address, std::size_t size, std::uint64_t& first,
                 std::uint64_t& last) noexcept
{
    if (size == 0) {
        return false;
    }
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (static_cast<std::uint64_t>(size) > maximum - address) {
        return false;
    }
    const std::uint64_t end = address + static_cast<std::uint64_t>(size);
    if (end > maximum - (title_page_size - 1)) {
        return false;
    }
    first = address & page_mask;
    last = align_page(end);
    return first < last;
}

std::string hex_value(std::uint64_t value)
{
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::string hresult_value(HRESULT result)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                  static_cast<unsigned long>(static_cast<std::uint32_t>(result)));
    return buffer;
}

bool readable(const void* pointer, std::size_t size) noexcept
{
    if (pointer == nullptr || size == 0) {
        return false;
    }
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(pointer);
    const std::uintptr_t maximum = std::numeric_limits<std::uintptr_t>::max();
    if (static_cast<std::uintptr_t>(size) > maximum - start) {
        return false;
    }
    const std::uintptr_t end = start + static_cast<std::uintptr_t>(size);
    std::uintptr_t cursor = start;
    const DWORD readable_protection = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                                    | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                    | PAGE_EXECUTE_WRITECOPY;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &information,
                           sizeof(information))
            == 0) {
            return false;
        }
        if (information.State != MEM_COMMIT) {
            return false;
        }
        if ((information.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        if ((information.Protect & readable_protection) == 0) {
            return false;
        }
        const std::uintptr_t base
            = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        if (information.RegionSize > maximum - base) {
            return false;
        }
        const std::uintptr_t region_end = base + information.RegionSize;
        if (region_end <= cursor) {
            return false;
        }
        cursor = std::min(region_end, end);
    }
    return true;
}

bool copy_title_memory(std::uint64_t address, void* destination, std::size_t size,
                       const char* operation)
{
    if (destination == nullptr || size == 0) {
        log::warn(std::string("title memory: ") + operation + " has no destination");
        return false;
    }
    const void* source = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address));
    if (!readable(source, size)) {
        log::warn(std::string("title memory: ") + operation + " cannot read "
                  + std::to_string(size) + " bytes at " + hex_value(address));
        return false;
    }
    std::memcpy(destination, source, size);
    return true;
}

bool make_upload_buffer(ID3D12Device* device, std::uint64_t size, ID3D12Resource** out,
                        void** mapped)
{
    if (device == nullptr || out == nullptr || mapped == nullptr || size == 0) {
        return false;
    }
    *out = nullptr;
    *mapped = nullptr;

    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const HRESULT created = device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(out));
    if (FAILED(created)) {
        log::warn("title memory: upload buffer creation failed with "
                  + hresult_value(created));
        return false;
    }

    const HRESULT mapped_result = (*out)->Map(0, nullptr, mapped);
    if (FAILED(mapped_result) || *mapped == nullptr) {
        log::warn("title memory: upload buffer map failed with "
                  + hresult_value(mapped_result));
        (*out)->Release();
        *out = nullptr;
        return false;
    }
    return true;
}

}

struct TitleMemory::Impl {
    struct Page {
        std::uint64_t address{};
        std::size_t size{};
        std::size_t offset{};
        ID3D12Resource* host{};
        D3D12_GPU_VIRTUAL_ADDRESS host_gpu_address{};
        TitlePageFlags flags{TitlePageFlags::None};
        bool texture{};
    };

    struct Shadow {
        ID3D12Resource* host{};
        void* mapped{};
        D3D12_GPU_VIRTUAL_ADDRESS gpu_address{};
        std::uint64_t address{};
        std::size_t size{};
    };

    struct Texture {
        ID3D12Resource* host{};
        D3D12_RESOURCE_DESC resource_description{};
        TitleTextureDesc description;
        std::uint64_t address{};
        std::uint64_t size{};
        bool uploaded{};
        bool warned_tiled{};
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COPY_DEST};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        std::vector<UINT> rows;
        std::vector<UINT64> row_sizes;
    };

    mutable std::mutex mutex;
    std::map<std::uint64_t, Page> pages;
    std::set<std::uint64_t> dirty_pages;
    bool dirty_all{};
    bool fallback_logged{};
    std::unordered_map<std::uint64_t, Shadow> shadows;
    std::map<std::uint64_t, Texture> textures;

    ID3D12Resource* page_table{};
    void* page_table_data{};
    D3D12_GPU_VIRTUAL_ADDRESS page_table_gpu{};
    std::size_t page_table_entries{};
    std::vector<ID3D12Resource*> retired_tables;

    ID3D12Device* device{};
    ID3D12CommandQueue* upload_queue{};
    ID3D12CommandAllocator* upload_allocator{};
    ID3D12GraphicsCommandList* upload_list{};
    ID3D12Fence* upload_fence{};
    HANDLE upload_event{};
    UINT64 upload_fence_value{};

    ~Impl();

    bool ensure_device(ID3D12Device* requested);
    bool ensure_upload();
    void release_upload();

    void release_shadow(std::uint64_t key);
    void release_texture(std::uint64_t key);
    void clear_dirty_range(std::uint64_t first, std::uint64_t last);
    bool any_dirty(std::uint64_t first, std::uint64_t last) const;
    void note_fallback(const char* reason);

    bool register_range(std::uint64_t address, std::size_t size, TitlePageFlags flags);
    void unregister_range(std::uint64_t address, std::size_t size);
    TitleSubmitStats prepare(std::span<const TitleRange> ranges);
    TitleShadowBuffer shadow(std::uint64_t address, std::size_t size,
                             ID3D12Device* requested);
    std::size_t refresh(std::uint64_t address, std::size_t size);
    TitlePageTable bind(ID3D12Device* requested);
    void mark(std::uint64_t address, std::size_t size);
    void consume(std::vector<TitleRange>& ranges);
    ID3D12Resource* texture(const TitleTextureDesc& description, std::uint64_t address,
                            ID3D12Device* requested);
    bool copy_texture(std::uint64_t address, std::size_t size);
};

TitleMemory::Impl::~Impl()
{
    for (auto& entry : textures) {
        if (entry.second.host != nullptr) {
            entry.second.host->Release();
        }
    }
    textures.clear();

    for (auto& entry : shadows) {
        if (entry.second.host != nullptr) {
            entry.second.host->Unmap(0, nullptr);
            entry.second.host->Release();
        }
    }
    shadows.clear();

    if (page_table != nullptr) {
        page_table->Release();
        page_table = nullptr;
    }
    for (ID3D12Resource* retired : retired_tables) {
        if (retired != nullptr) {
            retired->Release();
        }
    }
    retired_tables.clear();

    release_upload();

    if (device != nullptr) {
        device->Release();
        device = nullptr;
    }
}

bool TitleMemory::Impl::ensure_device(ID3D12Device* requested)
{
    if (requested == nullptr) {
        log::warn("title memory: a null D3D12 device was passed");
        return false;
    }
    if (device == nullptr) {
        device = requested;
        device->AddRef();
        return true;
    }
    if (device != requested) {
        log::warn("title memory: a different D3D12 device was passed; using the first one");
        return false;
    }
    return true;
}

void TitleMemory::Impl::release_upload()
{
    if (upload_event != nullptr) {
        ::CloseHandle(upload_event);
        upload_event = nullptr;
    }
    if (upload_fence != nullptr) {
        upload_fence->Release();
        upload_fence = nullptr;
    }
    if (upload_list != nullptr) {
        upload_list->Release();
        upload_list = nullptr;
    }
    if (upload_allocator != nullptr) {
        upload_allocator->Release();
        upload_allocator = nullptr;
    }
    if (upload_queue != nullptr) {
        upload_queue->Release();
        upload_queue = nullptr;
    }
    upload_fence_value = 0;
}

bool TitleMemory::Impl::ensure_upload()
{
    if (upload_queue != nullptr) {
        return true;
    }
    if (device == nullptr) {
        log::warn("title memory: no device for the texture upload queue");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT result = device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&upload_queue));
    if (FAILED(result)) {
        log::warn("title memory: upload queue creation failed with "
                  + hresult_value(result));
        release_upload();
        return false;
    }
    result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&upload_allocator));
    if (FAILED(result)) {
        log::warn("title memory: upload allocator creation failed with "
                  + hresult_value(result));
        release_upload();
        return false;
    }
    result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload_allocator,
                                       nullptr, IID_PPV_ARGS(&upload_list));
    if (FAILED(result)) {
        log::warn("title memory: upload command list creation failed with "
                  + hresult_value(result));
        release_upload();
        return false;
    }
    upload_list->Close();
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&upload_fence));
    if (FAILED(result)) {
        log::warn("title memory: upload fence creation failed with "
                  + hresult_value(result));
        release_upload();
        return false;
    }
    upload_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (upload_event == nullptr) {
        log::warn("title memory: upload event creation failed");
        release_upload();
        return false;
    }
    return true;
}

void TitleMemory::Impl::release_shadow(std::uint64_t key)
{
    const auto entry = shadows.find(key);
    if (entry == shadows.end()) {
        return;
    }
    for (auto& page_entry : pages) {
        Page& page = page_entry.second;
        if (page.host == entry->second.host) {
            page.host = nullptr;
            page.host_gpu_address = 0;
            page.offset = 0;
            page.texture = false;
        }
    }
    if (entry->second.host != nullptr) {
        entry->second.host->Unmap(0, nullptr);
        entry->second.host->Release();
    }
    shadows.erase(entry);
}

void TitleMemory::Impl::release_texture(std::uint64_t key)
{
    const auto entry = textures.find(key);
    if (entry == textures.end()) {
        return;
    }
    for (auto& page_entry : pages) {
        Page& page = page_entry.second;
        if (page.host == entry->second.host) {
            page.host = nullptr;
            page.host_gpu_address = 0;
            page.offset = 0;
            page.texture = false;
        }
    }
    if (entry->second.host != nullptr) {
        entry->second.host->Release();
    }
    textures.erase(entry);
}

void TitleMemory::Impl::clear_dirty_range(std::uint64_t first, std::uint64_t last)
{
    if (dirty_all) {
        return;
    }
    dirty_pages.erase(dirty_pages.lower_bound(first), dirty_pages.lower_bound(last));
    for (auto entry = pages.lower_bound(first); entry != pages.end() && entry->first < last;
         ++entry) {
        entry->second.flags = entry->second.flags & ~TitlePageFlags::Written;
    }
}

bool TitleMemory::Impl::any_dirty(std::uint64_t first, std::uint64_t last) const
{
    if (dirty_all) {
        return true;
    }
    const auto entry = dirty_pages.lower_bound(first);
    return entry != dirty_pages.end() && *entry < last;
}

void TitleMemory::Impl::note_fallback(const char* reason)
{
    if (fallback_logged) {
        return;
    }
    fallback_logged = true;
    log::warn(std::string("title memory: conservative dirty-all fallback used (")
              + reason + ")");
}

bool TitleMemory::Impl::register_range(std::uint64_t address, std::size_t size,
                                       TitlePageFlags flags)
{
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (!page_bounds(address, size, first, last)) {
        log::warn("title memory: register_page rejected " + hex_value(address) + " + "
                  + std::to_string(size));
        return false;
    }

    const std::uint64_t end = address + static_cast<std::uint64_t>(size);
    std::size_t added = 0;
    std::size_t touched = 0;
    for (std::uint64_t base = first; base < last; base += title_page_size) {
        const auto insertion = pages.try_emplace(base);
        Page& page = insertion.first->second;
        if (insertion.second) {
            page.address = base;
            ++added;
        }
        const std::uint64_t page_end = std::min(end, base + title_page_size);
        const std::size_t page_size = static_cast<std::size_t>(page_end - base);
        page.size = std::max(page.size, page_size);
        page.flags = page.flags | flags | TitlePageFlags::Committed;
        ++touched;
    }
    log::info("title memory: registered " + std::to_string(touched) + " pages ("
              + std::to_string(added) + " new, " + std::to_string(pages.size())
              + " total)");
    return true;
}

void TitleMemory::Impl::unregister_range(std::uint64_t address, std::size_t size)
{
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (!page_bounds(address, size, first, last)) {
        log::warn("title memory: unregister_pages rejected " + hex_value(address) + " + "
                  + std::to_string(size));
        return;
    }

    std::size_t removed = 0;
    auto entry = pages.lower_bound(first);
    while (entry != pages.end() && entry->first < last) {
        dirty_pages.erase(entry->first);
        entry = pages.erase(entry);
        ++removed;
    }
    if (dirty_pages.empty()) {
        dirty_all = false;
    }
    log::info("title memory: unregistered " + std::to_string(removed) + " pages ("
              + std::to_string(pages.size()) + " remain)");
}

TitleSubmitStats TitleMemory::Impl::prepare(std::span<const TitleRange> ranges)
{
    TitleSubmitStats stats;
    std::set<std::uint64_t> refreshed;
    for (const TitleRange& range : ranges) {
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        if (!page_bounds(range.address, range.size, first, last)) {
            log::warn("title memory: prepare_for_submit skipped " + hex_value(range.address)
                      + " + " + std::to_string(range.size));
            continue;
        }
        stats.pages += static_cast<std::size_t>((last - first) / title_page_size);
        for (auto entry = pages.lower_bound(first); entry != pages.end()
             && entry->first < last; ++entry) {
            entry->second.flags = entry->second.flags | TitlePageFlags::Graphics;
        }

        const std::uint64_t end = range.address + static_cast<std::uint64_t>(range.size);
        for (const auto& shadow_entry : shadows) {
            const Shadow& shadow = shadow_entry.second;
            if (shadow.address < end && range.address < shadow.address + shadow.size) {
                refreshed.insert(shadow_entry.first);
            }
        }
    }
    for (std::uint64_t key : refreshed) {
        auto shadow_entry = shadows.find(key);
        if (shadow_entry == shadows.end()) {
            continue;
        }
        Shadow& shadow = shadow_entry->second;
        if (copy_title_memory(shadow.address, shadow.mapped, shadow.size,
                              "prepare_for_submit refresh")) {
            ++stats.refreshed;
            clear_dirty_range(shadow.address, shadow.address + shadow.size);
        }
    }
    stats.dirty = dirty_all ? pages.size() : dirty_pages.size();
    stats.dirty_all = dirty_all;
    log::info("title memory: prepare_for_submit " + std::to_string(stats.pages)
              + " pages, " + std::to_string(stats.refreshed) + " shadows refreshed, "
              + std::to_string(stats.dirty) + " dirty");
    return stats;
}

TitleShadowBuffer TitleMemory::Impl::shadow(std::uint64_t address, std::size_t size,
                                            ID3D12Device* requested)
{
    TitleShadowBuffer result;
    if (!ensure_device(requested)) {
        return result;
    }
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (!page_bounds(address, size, first, last)) {
        log::warn("title memory: create_shadow_buffer rejected " + hex_value(address) + " + "
                  + std::to_string(size));
        return result;
    }
    const std::uint64_t end = address + static_cast<std::uint64_t>(size);

    for (auto& entry : shadows) {
        Shadow& existing = entry.second;
        if (existing.address <= first && last <= existing.address + existing.size) {
            if (copy_title_memory(existing.address, existing.mapped, existing.size,
                                  "create_shadow_buffer refresh")) {
                clear_dirty_range(existing.address, existing.address + existing.size);
            }
            result.host = existing.host;
            result.gpu_address = existing.gpu_address + (address - existing.address);
            result.address = address;
            result.size = size;
            log::info("title memory: reused shadow buffer for " + hex_value(address) + " + "
                      + std::to_string(size));
            return result;
        }
    }

    std::vector<std::uint64_t> overlapping;
    for (const auto& entry : shadows) {
        const Shadow& existing = entry.second;
        if (existing.address < last && first < existing.address + existing.size) {
            overlapping.push_back(entry.first);
        }
    }
    for (std::uint64_t key : overlapping) {
        release_shadow(key);
    }

    const std::uint64_t length = last - first;
    ID3D12Resource* buffer = nullptr;
    void* mapped = nullptr;
    if (!make_upload_buffer(requested, length, &buffer, &mapped)) {
        log::warn("title memory: no shadow buffer for " + hex_value(address) + " + "
                  + std::to_string(size));
        return result;
    }
    if (!copy_title_memory(first, mapped, static_cast<std::size_t>(length),
                           "create_shadow_buffer copy")) {
        buffer->Unmap(0, nullptr);
        buffer->Release();
        return result;
    }
    if (end < last) {
        void* tail = static_cast<std::uint8_t*>(mapped) + (end - first);
        std::memset(tail, 0, static_cast<std::size_t>(last - end));
    }

    const D3D12_GPU_VIRTUAL_ADDRESS gpu = buffer->GetGPUVirtualAddress();
    Shadow stored;
    stored.host = buffer;
    stored.mapped = mapped;
    stored.gpu_address = gpu;
    stored.address = first;
    stored.size = static_cast<std::size_t>(length);
    shadows.emplace(first, stored);

    std::size_t touched = 0;
    for (std::uint64_t base = first; base < last; base += title_page_size) {
        const auto insertion = pages.try_emplace(base);
        Page& page = insertion.first->second;
        if (insertion.second) {
            page.address = base;
        }
        const std::uint64_t page_end = std::min(end, base + title_page_size);
        page.size = std::max(page.size, static_cast<std::size_t>(page_end - base));
        page.host = buffer;
        page.host_gpu_address = gpu + (base - first);
        page.offset = static_cast<std::size_t>(base - first);
        page.texture = false;
        page.flags = page.flags | TitlePageFlags::Committed | TitlePageFlags::Graphics;
        ++touched;
    }
    clear_dirty_range(first, last);

    result.host = buffer;
    result.gpu_address = gpu + (address - first);
    result.address = address;
    result.size = size;
    log::info("title memory: shadow buffer " + hex_value(address) + " + "
              + std::to_string(size) + " covers " + std::to_string(touched)
              + " pages, gpu " + hex_value(result.gpu_address));
    return result;
}

std::size_t TitleMemory::Impl::refresh(std::uint64_t address, std::size_t size)
{
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (!page_bounds(address, size, first, last)) {
        log::warn("title memory: refresh_shadow rejected " + hex_value(address) + " + "
                  + std::to_string(size));
        return 0;
    }
    const std::uint64_t end = address + static_cast<std::uint64_t>(size);
    std::vector<std::uint64_t> keys;
    for (const auto& entry : shadows) {
        const Shadow& shadow = entry.second;
        if (shadow.address < end && address < shadow.address + shadow.size) {
            keys.push_back(entry.first);
        }
    }
    if (keys.empty()) {
        log::warn("title memory: refresh_shadow found no shadow for " + hex_value(address)
                  + " + " + std::to_string(size));
        return 0;
    }

    std::size_t refreshed = 0;
    std::size_t bytes = 0;
    for (std::uint64_t key : keys) {
        auto entry = shadows.find(key);
        if (entry == shadows.end()) {
            continue;
        }
        Shadow& shadow = entry->second;
        if (!copy_title_memory(shadow.address, shadow.mapped, shadow.size,
                               "refresh_shadow")) {
            continue;
        }
        clear_dirty_range(shadow.address, shadow.address + shadow.size);
        ++refreshed;
        bytes += shadow.size;
    }
    log::info("title memory: refreshed " + std::to_string(refreshed) + " shadows, "
              + std::to_string(bytes) + " bytes");
    return refreshed;
}

TitlePageTable TitleMemory::Impl::bind(ID3D12Device* requested)
{
    TitlePageTable result;
    if (!ensure_device(requested)) {
        return result;
    }
    const std::uint64_t count = static_cast<std::uint64_t>(pages.size());
    if (count > (std::numeric_limits<std::uint64_t>::max()
                 / sizeof(TitlePageTableEntry))) {
        log::warn("title memory: page table entry count overflows");
        return result;
    }
    std::uint64_t bytes = count * sizeof(TitlePageTableEntry);
    bytes = std::max<std::uint64_t>(bytes, page_table_alignment);
    if (bytes > std::numeric_limits<std::uint64_t>::max() - (page_table_alignment - 1)) {
        log::warn("title memory: page table byte size overflows");
        return result;
    }
    bytes = (bytes + page_table_alignment - 1) & ~static_cast<std::uint64_t>(
        page_table_alignment - 1);

    ID3D12Resource* table = nullptr;
    void* data = nullptr;
    if (!make_upload_buffer(requested, bytes, &table, &data)) {
        log::warn("title memory: page table constant buffer creation failed");
        return result;
    }
    std::memset(data, 0, static_cast<std::size_t>(bytes));

    auto* entries = static_cast<TitlePageTableEntry*>(data);
    std::size_t index = 0;
    for (const auto& entry : pages) {
        const Page& page = entry.second;
        TitlePageTableEntry& output = entries[index];
        output.address_low = static_cast<std::uint32_t>(page.address & 0xffffffffull);
        output.address_high = static_cast<std::uint32_t>(page.address >> 32);
        output.gpu_low
            = static_cast<std::uint32_t>(page.host_gpu_address & 0xffffffffull);
        output.gpu_high = static_cast<std::uint32_t>(page.host_gpu_address >> 32);
        output.size = static_cast<std::uint32_t>(
            std::min<std::size_t>(page.size, std::numeric_limits<std::uint32_t>::max()));
        output.flags = static_cast<std::uint32_t>(page.flags);
        ++index;
    }

    if (page_table != nullptr) {
        retired_tables.push_back(page_table);
    }
    page_table = table;
    page_table_data = data;
    page_table_gpu = table->GetGPUVirtualAddress();
    page_table_entries = pages.size();

    result.host = page_table;
    result.gpu_address = page_table_gpu;
    result.entries = page_table_entries;
    log::info("title memory: bound page table with " + std::to_string(result.entries)
              + " entries at gpu " + hex_value(result.gpu_address));
    return result;
}

void TitleMemory::Impl::mark(std::uint64_t address, std::size_t size)
{
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (!page_bounds(address, size, first, last)) {
        log::warn("title memory: mark_written rejected " + hex_value(address) + " + "
                  + std::to_string(size));
        note_fallback("invalid write range");
        dirty_all = true;
        return;
    }
    if (pages.empty()) {
        note_fallback("no registered pages");
        dirty_all = true;
        return;
    }
    const std::uint64_t page_count = (last - first) / title_page_size;
    if (page_count > maximum_pages_per_call) {
        note_fallback("write range is too large");
        dirty_all = true;
        return;
    }

    std::size_t marked = 0;
    bool missing = false;
    for (std::uint64_t base = first; base < last; base += title_page_size) {
        const auto entry = pages.find(base);
        if (entry == pages.end()) {
            missing = true;
            continue;
        }
        entry->second.flags = entry->second.flags | TitlePageFlags::Written;
        dirty_pages.insert(base);
        ++marked;
    }
    if (missing) {
        note_fallback("write range spans unregistered pages");
        dirty_all = true;
        dirty_pages.clear();
    }
    log::debug("title memory: marked " + std::to_string(marked) + " pages written");
}

void TitleMemory::Impl::consume(std::vector<TitleRange>& ranges)
{
    ranges.clear();
    const auto append = [&ranges](std::uint64_t address, std::size_t size,
                                  std::vector<TitleRange>& output) {
        if (!output.empty()) {
            TitleRange& previous = output.back();
            if (previous.address + previous.size == address) {
                previous.size += size;
                return;
            }
        }
        output.push_back(TitleRange{address, size});
    };

    std::size_t count = 0;
    if (dirty_all) {
        for (auto& entry : pages) {
            Page& page = entry.second;
            append(page.address, page.size, ranges);
            page.flags = page.flags & ~TitlePageFlags::Written;
            ++count;
        }
        dirty_pages.clear();
        dirty_all = false;
        log::info("title memory: consume_dirty used the dirty-all fallback for "
                  + std::to_string(count) + " pages, " + std::to_string(ranges.size())
                  + " ranges");
        return;
    }

    for (std::uint64_t base : dirty_pages) {
        const auto entry = pages.find(base);
        if (entry == pages.end()) {
            continue;
        }
        Page& page = entry->second;
        append(page.address, page.size, ranges);
        page.flags = page.flags & ~TitlePageFlags::Written;
        ++count;
    }
    dirty_pages.clear();
    log::info("title memory: consume_dirty " + std::to_string(count) + " pages, "
              + std::to_string(ranges.size()) + " ranges");
}

ID3D12Resource* TitleMemory::Impl::texture(const TitleTextureDesc& description,
                                           std::uint64_t address,
                                           ID3D12Device* requested)
{
    if (!ensure_device(requested)) {
        return nullptr;
    }
    const auto existing = textures.find(address);
    if (existing != textures.end()) {
        log::info("title memory: reusing title texture at " + hex_value(address));
        return existing->second.host;
    }
    if (description.width == 0 || description.height == 0 || description.depth == 0
        || description.mips == 0 || description.array_size == 0
        || description.format == DXGI_FORMAT_UNKNOWN) {
        log::warn("title memory: invalid title texture description at "
                  + hex_value(address));
        return nullptr;
    }
    if (description.mips > 65535 || description.array_size > 65535
        || description.depth > 65535) {
        log::warn("title memory: title texture dimensions exceed D3D12 limits at "
                  + hex_value(address));
        return nullptr;
    }
    if (description.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D
        && description.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        && description.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        log::warn("title memory: unsupported title texture dimension at "
                  + hex_value(address));
        return nullptr;
    }

    D3D12_RESOURCE_DESC resource_description{};
    resource_description.Dimension = description.dimension;
    resource_description.Width = description.width;
    resource_description.Height
        = description.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1
                                                                      : description.height;
    resource_description.DepthOrArraySize = static_cast<UINT16>(
        description.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? description.depth
            : description.array_size);
    resource_description.MipLevels = static_cast<UINT16>(description.mips);
    resource_description.Format = description.format;
    resource_description.SampleDesc.Count = 1;
    resource_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_description.Flags = D3D12_RESOURCE_FLAG_NONE;

    const std::uint64_t subresources
        = static_cast<std::uint64_t>(description.mips)
        * (description.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
               ? 1ull
               : static_cast<std::uint64_t>(description.array_size));
    if (subresources == 0 || subresources > std::numeric_limits<UINT>::max()) {
        log::warn("title memory: title texture subresource count is invalid at "
                  + hex_value(address));
        return nullptr;
    }

    Texture stored;
    stored.description = description;
    stored.resource_description = resource_description;
    stored.address = address;
    stored.footprints.resize(static_cast<std::size_t>(subresources));
    stored.rows.resize(static_cast<std::size_t>(subresources));
    stored.row_sizes.resize(static_cast<std::size_t>(subresources));

    UINT64 total = 0;
    requested->GetCopyableFootprints(
        &resource_description, 0, static_cast<UINT>(subresources), 0,
        stored.footprints.data(), stored.rows.data(), stored.row_sizes.data(), &total);
    if (total == 0) {
        log::warn("title memory: title texture has no copyable footprint at "
                  + hex_value(address));
        return nullptr;
    }
    if (total > std::numeric_limits<std::uint64_t>::max() - address) {
        log::warn("title memory: title texture range overflows at " + hex_value(address));
        return nullptr;
    }
    stored.size = total;

    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;

    const HRESULT created = requested->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &resource_description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&stored.host));
    if (FAILED(created)) {
        log::warn("title memory: title texture creation failed with "
                  + hresult_value(created) + " at " + hex_value(address));
        return nullptr;
    }

    const std::uint64_t first = address & page_mask;
    const std::uint64_t last = align_page(address + total);
    std::size_t touched = 0;
    for (std::uint64_t base = first; base < last; base += title_page_size) {
        const auto insertion = pages.try_emplace(base);
        Page& page = insertion.first->second;
        if (insertion.second) {
            page.address = base;
        }
        const std::uint64_t page_end = std::min(address + total, base + title_page_size);
        page.size = std::max(page.size, static_cast<std::size_t>(page_end - base));
        page.host = stored.host;
        page.host_gpu_address = 0;
        page.offset = base >= address ? static_cast<std::size_t>(base - address) : 0;
        page.texture = true;
        page.flags = page.flags | TitlePageFlags::Committed | TitlePageFlags::Graphics;
        ++touched;
    }

    ID3D12Resource* host = stored.host;
    textures.emplace(address, std::move(stored));
    log::info("title memory: title texture " + std::to_string(description.width) + "x"
              + std::to_string(description.height) + "x"
              + std::to_string(description.depth) + " mips "
              + std::to_string(description.mips) + " array "
              + std::to_string(description.array_size) + " format "
              + std::to_string(static_cast<std::uint32_t>(description.format)) + " at "
              + hex_value(address) + " covers " + std::to_string(touched) + " pages");
    return host;
}

bool TitleMemory::Impl::copy_texture(std::uint64_t address, std::size_t size)
{
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (!page_bounds(address, size, first, last)) {
        log::warn("title memory: copy_title_texture rejected " + hex_value(address) + " + "
                  + std::to_string(size));
        return false;
    }
    const std::uint64_t end = address + static_cast<std::uint64_t>(size);
    std::vector<std::uint64_t> keys;
    for (const auto& entry : textures) {
        const Texture& texture_entry = entry.second;
        if (texture_entry.address < end
            && address < texture_entry.address + texture_entry.size) {
            keys.push_back(entry.first);
        }
    }
    if (keys.empty()) {
        log::warn("title memory: copy_title_texture found no texture for "
                  + hex_value(address) + " + " + std::to_string(size));
        return false;
    }

    bool success = true;
    for (std::uint64_t key : keys) {
        auto entry = textures.find(key);
        if (entry == textures.end()) {
            continue;
        }
        Texture& texture_entry = entry->second;
        if (texture_entry.uploaded
            && !any_dirty(texture_entry.address,
                          texture_entry.address + texture_entry.size)) {
            log::info("title memory: title texture at " + hex_value(texture_entry.address)
                      + " unchanged");
            continue;
        }
        if (texture_entry.description.tiled && !texture_entry.warned_tiled) {
            texture_entry.warned_tiled = true;
            log::warn("title memory: tiled title texture uploaded as linear rows at "
                      + hex_value(texture_entry.address));
        }
        if (!readable(reinterpret_cast<const void*>(
                          static_cast<std::uintptr_t>(texture_entry.address)),
                      static_cast<std::size_t>(texture_entry.size))) {
            log::warn("title memory: title texture memory is not readable at "
                      + hex_value(texture_entry.address));
            success = false;
            continue;
        }
        if (!ensure_upload()) {
            success = false;
            continue;
        }

        ID3D12Resource* staging = nullptr;
        void* mapped = nullptr;
        if (!make_upload_buffer(device, texture_entry.size, &staging, &mapped)) {
            log::warn("title memory: title texture staging creation failed at "
                      + hex_value(texture_entry.address));
            success = false;
            continue;
        }

        const auto* source = reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(texture_entry.address));
        auto* destination = static_cast<std::uint8_t*>(mapped);
        std::uint64_t source_offset = 0;
        bool copied_rows = true;
        for (std::size_t index = 0; index < texture_entry.footprints.size(); ++index) {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint
                = texture_entry.footprints[index];
            const std::uint64_t row_bytes = texture_entry.row_sizes[index];
            const std::uint64_t row_count = texture_entry.rows[index];
            const std::uint64_t row_pitch = footprint.Footprint.RowPitch;
            if (row_bytes > row_pitch
                || source_offset + row_bytes * row_count > texture_entry.size) {
                log::warn("title memory: title texture row layout does not fit at "
                          + hex_value(texture_entry.address));
                copied_rows = false;
                break;
            }
            for (std::uint64_t row = 0; row < row_count; ++row) {
                const std::uint64_t destination_offset
                    = footprint.Offset + row * row_pitch;
                if (destination_offset + row_bytes > texture_entry.size) {
                    copied_rows = false;
                    break;
                }
                std::memcpy(destination + destination_offset,
                            source + source_offset + row * row_bytes,
                            static_cast<std::size_t>(row_bytes));
            }
            if (!copied_rows) {
                break;
            }
            source_offset += row_bytes * row_count;
        }
        staging->Unmap(0, nullptr);
        if (!copied_rows) {
            staging->Release();
            success = false;
            continue;
        }

        const HRESULT reset_allocator = upload_allocator->Reset();
        const HRESULT reset_list = upload_list->Reset(upload_allocator, nullptr);
        if (FAILED(reset_allocator) || FAILED(reset_list)) {
            log::warn("title memory: upload list reset failed with "
                      + hresult_value(FAILED(reset_allocator) ? reset_allocator
                                                              : reset_list));
            staging->Release();
            success = false;
            continue;
        }

        if (texture_entry.state != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER before{};
            before.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            before.Transition.pResource = texture_entry.host;
            before.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            before.Transition.StateBefore = texture_entry.state;
            before.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            upload_list->ResourceBarrier(1, &before);
        }

        for (std::size_t index = 0; index < texture_entry.footprints.size(); ++index) {
            D3D12_TEXTURE_COPY_LOCATION target{};
            target.pResource = texture_entry.host;
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.SubresourceIndex = static_cast<UINT>(index);

            D3D12_TEXTURE_COPY_LOCATION source_location{};
            source_location.pResource = staging;
            source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source_location.PlacedFootprint = texture_entry.footprints[index];
            upload_list->CopyTextureRegion(&target, 0, 0, 0, &source_location, nullptr);
        }

        D3D12_RESOURCE_BARRIER after{};
        after.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        after.Transition.pResource = texture_entry.host;
        after.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        after.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        after.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        upload_list->ResourceBarrier(1, &after);

        const HRESULT closed = upload_list->Close();
        if (FAILED(closed)) {
            log::warn("title memory: upload list close failed with "
                      + hresult_value(closed));
            staging->Release();
            success = false;
            continue;
        }
        ID3D12CommandList* lists[]{upload_list};
        upload_queue->ExecuteCommandLists(1, lists);
        ++upload_fence_value;
        const HRESULT signalled = upload_queue->Signal(upload_fence, upload_fence_value);
        if (FAILED(signalled)) {
            log::warn("title memory: upload fence signal failed with "
                      + hresult_value(signalled));
            staging->Release();
            success = false;
            continue;
        }
        if (upload_fence->GetCompletedValue() < upload_fence_value) {
            const HRESULT waiting
                = upload_fence->SetEventOnCompletion(upload_fence_value, upload_event);
            if (FAILED(waiting) || ::WaitForSingleObject(upload_event, INFINITE) != WAIT_OBJECT_0) {
                log::warn("title memory: upload wait failed with "
                          + hresult_value(waiting));
                staging->Release();
                success = false;
                continue;
            }
        }
        staging->Release();

        texture_entry.uploaded = true;
        texture_entry.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        clear_dirty_range(texture_entry.address,
                          texture_entry.address + texture_entry.size);
        log::info("title memory: uploaded title texture "
                  + std::to_string(texture_entry.description.width) + "x"
                  + std::to_string(texture_entry.description.height) + "x"
                  + std::to_string(texture_entry.description.depth) + " mips "
                  + std::to_string(texture_entry.description.mips) + " format "
                  + std::to_string(static_cast<std::uint32_t>(
                      texture_entry.description.format))
                  + " bytes " + std::to_string(texture_entry.size) + " at "
                  + hex_value(texture_entry.address));
    }
    return success;
}

TitleMemory::TitleMemory()
    : impl_(std::make_unique<Impl>())
{
}

TitleMemory::~TitleMemory() = default;

bool TitleMemory::register_page(std::uint64_t address, std::size_t size)
{
    return register_page(address, size, TitlePageFlags::Committed);
}

bool TitleMemory::register_page(std::uint64_t address, std::size_t size,
                                TitlePageFlags flags)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->register_range(address, size, flags);
}

void TitleMemory::unregister_pages(std::uint64_t address, std::size_t size)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->unregister_range(address, size);
}

std::uint64_t TitleMemory::page_of(std::uint64_t address) const noexcept
{
    return address & page_mask;
}

TitleSubmitStats TitleMemory::prepare_for_submit(std::span<const TitleRange> ranges)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->prepare(ranges);
}

TitleSubmitStats TitleMemory::prepare_for_submit(ID3D12Device* device,
                                                 std::span<const TitleRange> ranges)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    TitleSubmitStats stats = impl_->prepare(ranges);
    impl_->bind(device);
    return stats;
}

TitleShadowBuffer TitleMemory::create_shadow_buffer(ID3D12Device* device,
                                                    std::uint64_t address,
                                                    std::size_t size)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->shadow(address, size, device);
}

ID3D12Resource* TitleMemory::create_shadow_buffer(ID3D12Device* device,
                                                  std::uint64_t address,
                                                  std::size_t size,
                                                  D3D12_GPU_VIRTUAL_ADDRESS* gpu_address)
{
    if (gpu_address == nullptr) {
        log::warn("title memory: create_shadow_buffer got a null GPU address output");
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const TitleShadowBuffer buffer = impl_->shadow(address, size, device);
    *gpu_address = buffer.gpu_address;
    return buffer.host;
}

bool TitleMemory::refresh_shadow(std::uint64_t address, std::size_t size)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->refresh(address, size) > 0;
}

TitlePageTable TitleMemory::bind_page_table(ID3D12Device* device)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->bind(device);
}

void TitleMemory::mark_written(std::uint64_t address, std::size_t size)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->mark(address, size);
}

void TitleMemory::consume_dirty(std::vector<TitleRange>& ranges)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->consume(ranges);
}

std::vector<TitleRange> TitleMemory::consume_dirty()
{
    std::vector<TitleRange> ranges;
    consume_dirty(ranges);
    return ranges;
}

ID3D12Resource* TitleMemory::create_title_texture(ID3D12Device* device,
                                                  const void* console_desc,
                                                  std::uint64_t address)
{
    if (console_desc == nullptr) {
        log::warn("title memory: create_title_texture got a null console descriptor");
        return nullptr;
    }
    if (!readable(console_desc, sizeof(TitleTextureDesc))) {
        log::warn("title memory: create_title_texture console descriptor is not readable");
        return nullptr;
    }
    TitleTextureDesc description;
    std::memcpy(&description, console_desc, sizeof(description));
    return create_title_texture(device, description, address);
}

ID3D12Resource* TitleMemory::create_title_texture(ID3D12Device* device,
                                                  const TitleTextureDesc& console_desc,
                                                  std::uint64_t address)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->texture(console_desc, address, device);
}

bool TitleMemory::copy_title_texture(std::uint64_t address, std::size_t size)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->copy_texture(address, size);
}

TitleMemory& title_memory()
{
    static TitleMemory instance;
    return instance;
}

bool register_page(std::uint64_t address, std::size_t size)
{
    return title_memory().register_page(address, size);
}

bool register_page(std::uint64_t address, std::size_t size, TitlePageFlags flags)
{
    return title_memory().register_page(address, size, flags);
}

void unregister_pages(std::uint64_t address, std::size_t size)
{
    title_memory().unregister_pages(address, size);
}

std::uint64_t page_of(std::uint64_t address) noexcept
{
    return address & page_mask;
}

TitleSubmitStats prepare_for_submit(std::span<const TitleRange> ranges)
{
    return title_memory().prepare_for_submit(ranges);
}

TitleSubmitStats prepare_for_submit(ID3D12Device* device,
                                    std::span<const TitleRange> ranges)
{
    return title_memory().prepare_for_submit(device, ranges);
}

TitleShadowBuffer create_shadow_buffer(ID3D12Device* device, std::uint64_t address,
                                       std::size_t size)
{
    return title_memory().create_shadow_buffer(device, address, size);
}

ID3D12Resource* create_shadow_buffer(ID3D12Device* device, std::uint64_t address,
                                     std::size_t size,
                                     D3D12_GPU_VIRTUAL_ADDRESS* gpu_address)
{
    return title_memory().create_shadow_buffer(device, address, size, gpu_address);
}

bool refresh_shadow(std::uint64_t address, std::size_t size)
{
    return title_memory().refresh_shadow(address, size);
}

TitlePageTable bind_page_table(ID3D12Device* device)
{
    return title_memory().bind_page_table(device);
}

void mark_written(std::uint64_t address, std::size_t size)
{
    title_memory().mark_written(address, size);
}

void consume_dirty(std::vector<TitleRange>& ranges)
{
    title_memory().consume_dirty(ranges);
}

std::vector<TitleRange> consume_dirty()
{
    return title_memory().consume_dirty();
}

ID3D12Resource* create_title_texture(ID3D12Device* device, const void* console_desc,
                                     std::uint64_t address)
{
    return title_memory().create_title_texture(device, console_desc, address);
}

ID3D12Resource* create_title_texture(ID3D12Device* device,
                                     const TitleTextureDesc& console_desc,
                                     std::uint64_t address)
{
    return title_memory().create_title_texture(device, console_desc, address);
}

bool copy_title_texture(std::uint64_t address, std::size_t size)
{
    return title_memory().copy_title_texture(address, size);
}

}
