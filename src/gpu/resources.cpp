#include <euphoria/gpu/resources.hpp>
#include <euphoria/log/log.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace euphoria::gpu {
namespace {

constexpr std::uint32_t kResourceSlots = 32;
constexpr std::uint32_t kHeapSlots = 24;
constexpr std::uint32_t kDescriptorHeapSlots = 24;
constexpr std::uint32_t kResourceFirstExtension = 15;
constexpr std::uint32_t kHeapFirstExtension = 9;
constexpr std::uint32_t kDescriptorHeapFirstExtension = 11;

constexpr HRESULT kNotFound = static_cast<HRESULT>(0x887A0002u);
constexpr HRESULT kMoreData = static_cast<HRESULT>(0x887A0003u);

enum class ProxyKind : std::uint32_t {
    Resource,
    Heap,
    DescriptorHeap,
};

constexpr std::size_t kind_index(ProxyKind kind) noexcept
{
    switch (kind) {
    case ProxyKind::Resource:
        return 0;
    case ProxyKind::Heap:
        return 1;
    case ProxyKind::DescriptorHeap:
        return 2;
    }
    return 0;
}

constexpr const char* kind_name(ProxyKind kind) noexcept
{
    switch (kind) {
    case ProxyKind::Resource:
        return "resource";
    case ProxyKind::Heap:
        return "heap";
    case ProxyKind::DescriptorHeap:
        return "descriptor heap";
    }
    return "object";
}

struct UnsupportedLog {
    std::mutex mutex;
    std::array<std::array<bool, kResourceSlots>, 3> logged{};
};

void log_unsupported(ProxyKind kind, std::uint32_t slot)
{
    if (slot >= kResourceSlots) {
        return;
    }
    static UnsupportedLog state;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        bool& seen = state.logged[kind_index(kind)][slot];
        first = !seen;
        seen = true;
    }
    if (!first) {
        return;
    }
    log::warn(std::string("d3d12: unimplemented ") + kind_name(kind) + " vtable slot "
              + std::to_string(slot));
}

using GuidBytes = std::array<std::uint8_t, 16>;

GuidBytes guid_bytes(const GUID& value)
{
    GuidBytes bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

struct PrivateKey {
    const void* object;
    GuidBytes guid;

    bool operator<(const PrivateKey& other) const
    {
        if (object != other.object) {
            return std::less<const void*>{}(object, other.object);
        }
        return guid < other.guid;
    }
};

std::mutex& private_mutex()
{
    static std::mutex value;
    return value;
}

std::map<PrivateKey, std::vector<std::uint8_t>>& private_store()
{
    static std::map<PrivateKey, std::vector<std::uint8_t>> value;
    return value;
}

HRESULT get_private_data(void* object, const GUID& guid, UINT* size, void* data)
{
    if (size == nullptr) {
        return E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(private_mutex());
    const auto& store = private_store();
    const auto entry = store.find(PrivateKey{object, guid_bytes(guid)});
    if (entry == store.end()) {
        *size = 0;
        return kNotFound;
    }
    const std::size_t required = entry->second.size();
    const std::size_t room = *size;
    *size = static_cast<UINT>(required);
    if (data == nullptr) {
        return S_OK;
    }
    if (room < required) {
        return kMoreData;
    }
    std::memcpy(data, entry->second.data(), required);
    return S_OK;
}

HRESULT set_private_data(void* object, const GUID& guid, UINT size, const void* data)
{
    if (size != 0 && data == nullptr) {
        return E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(private_mutex());
    const PrivateKey key{object, guid_bytes(guid)};
    if (data == nullptr) {
        private_store().erase(key);
        return S_OK;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    private_store()[key] = std::vector<std::uint8_t>(bytes, bytes + size);
    return S_OK;
}

void erase_private_data(const void* object)
{
    std::lock_guard<std::mutex> lock(private_mutex());
    auto& store = private_store();
    auto entry = store.lower_bound(PrivateKey{object, GuidBytes{}});
    while (entry != store.end() && entry->first.object == object) {
        entry = store.erase(entry);
    }
}

HRESULT return_device(void* device, void** out)
{
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = device;
    return device != nullptr ? S_OK : E_FAIL;
}

HRESULT set_name(ID3D12Object* host, LPCWSTR name)
{
    if (host == nullptr) {
        return S_OK;
    }
    return host->SetName(name);
}

HRESULT set_private_data_interface(ID3D12Object* host, const GUID& guid, const IUnknown* data)
{
    if (host == nullptr) {
        return E_NOTIMPL;
    }
    return host->SetPrivateDataInterface(guid, data);
}

template <typename Proxy>
std::uint32_t __stdcall add_ref(void* object)
{
    auto* proxy = static_cast<Proxy*>(object);
    return proxy->references.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <typename Proxy>
std::uint32_t __stdcall release(void* object)
{
    auto* proxy = static_cast<Proxy*>(object);
    const std::uint32_t previous = proxy->references.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0) {
        proxy->references.store(0, std::memory_order_relaxed);
        return 0;
    }
    if (previous == 1) {
        delete proxy;
        return 0;
    }
    return previous - 1;
}

template <typename Proxy>
HRESULT answer_query(void* object, const GUID* iid, const GUID& interface_id, void** out)
{
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (iid == nullptr) {
        return E_INVALIDARG;
    }
    if (!IsEqualIID(*iid, __uuidof(IUnknown)) && !IsEqualIID(*iid, interface_id)) {
        return E_NOINTERFACE;
    }
    auto* proxy = static_cast<Proxy*>(object);
    proxy->references.fetch_add(1, std::memory_order_relaxed);
    *out = object;
    return S_OK;
}

HRESULT __stdcall resource_query(void* object, const GUID* iid, void** out)
{
    return answer_query<ResourceProxy>(object, iid, __uuidof(ID3D12Resource), out);
}

HRESULT __stdcall heap_query(void* object, const GUID* iid, void** out)
{
    return answer_query<HeapProxy>(object, iid, __uuidof(ID3D12Heap), out);
}

HRESULT __stdcall descriptor_heap_query(void* object, const GUID* iid, void** out)
{
    return answer_query<DescriptorHeapProxy>(object, iid, __uuidof(ID3D12DescriptorHeap), out);
}

HRESULT __stdcall resource_get_device(void* object, void* first, void* second)
{
    void** out = device_out_slot(first, second);
    return return_device(static_cast<ResourceProxy*>(object)->owner_device, out);
}

HRESULT __stdcall heap_get_device(void* object, void* first, void* second)
{
    void** out = device_out_slot(first, second);
    return return_device(static_cast<HeapProxy*>(object)->owner_device, out);
}

HRESULT __stdcall descriptor_heap_get_device(void* object, void* first, void* second)
{
    void** out = device_out_slot(first, second);
    return return_device(static_cast<DescriptorHeapProxy*>(object)->owner_device, out);
}

HRESULT __stdcall resource_set_name(void* object, LPCWSTR name)
{
    return set_name(static_cast<ResourceProxy*>(object)->host, name);
}

HRESULT __stdcall heap_set_name(void* object, LPCWSTR name)
{
    return set_name(static_cast<HeapProxy*>(object)->host, name);
}

HRESULT __stdcall descriptor_heap_set_name(void* object, LPCWSTR name)
{
    return set_name(static_cast<DescriptorHeapProxy*>(object)->host, name);
}

HRESULT __stdcall resource_set_private_data_interface(void* object, const GUID& guid,
                                                      const IUnknown* data)
{
    return set_private_data_interface(static_cast<ResourceProxy*>(object)->host, guid, data);
}

HRESULT __stdcall heap_set_private_data_interface(void* object, const GUID& guid,
                                                  const IUnknown* data)
{
    return set_private_data_interface(static_cast<HeapProxy*>(object)->host, guid, data);
}

HRESULT __stdcall descriptor_heap_set_private_data_interface(void* object, const GUID& guid,
                                                             const IUnknown* data)
{
    return set_private_data_interface(static_cast<DescriptorHeapProxy*>(object)->host, guid,
                                      data);
}

HRESULT __stdcall resource_map(void* object, UINT subresource, const D3D12_RANGE* read_range,
                               void** data)
{
    auto* proxy = static_cast<ResourceProxy*>(object);
    if (proxy->title_address != 0) {
        if (data != nullptr) {
            *data = reinterpret_cast<void*>(proxy->title_address);
        }
        if (!proxy->cpu_mapped) {
            proxy->cpu_mapped = true;
            forget_gpu_ranges(proxy->host);
        }
        return S_OK;
    }
    if (proxy->host == nullptr) {
        return E_NOTIMPL;
    }
    const HRESULT result = proxy->host->Map(subresource, read_range, data);
    if (FAILED(result)) {
        static std::atomic<std::uint32_t> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 16) {
            log::warn("d3d12: resource Map failed 0x"
                      + std::to_string(static_cast<std::uint32_t>(result)));
        }
        if (data != nullptr) {
            *data = nullptr;
        }
    }
    return result;
}

void __stdcall resource_unmap(void* object, UINT subresource, const D3D12_RANGE* written_range)
{
    auto* proxy = static_cast<ResourceProxy*>(object);
    if (proxy->host != nullptr && proxy->title_address == 0) {
        proxy->host->Unmap(subresource, written_range);
    }
}

void __stdcall resource_get_desc(void* object, D3D12_RESOURCE_DESC* out)
{
    if (out != nullptr) {
        *out = static_cast<ResourceProxy*>(object)->description;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS __stdcall resource_gpu_address(void* object)
{
    auto* proxy = static_cast<ResourceProxy*>(object);
    if (proxy->title_address != 0) {
        return proxy->title_address;
    }
    return proxy->host != nullptr ? proxy->host->GetGPUVirtualAddress() : 0;
}

HRESULT __stdcall resource_write_to_subresource(void* object, UINT subresource,
                                                const D3D12_BOX* box, const void* data,
                                                UINT row_pitch, UINT depth_pitch)
{
    auto* proxy = static_cast<ResourceProxy*>(object);
    if (proxy->host == nullptr) {
        return E_NOTIMPL;
    }
    return proxy->host->WriteToSubresource(subresource, box, data, row_pitch, depth_pitch);
}

HRESULT __stdcall resource_read_from_subresource(void* object, void* data, UINT row_pitch,
                                                 UINT depth_pitch, UINT subresource,
                                                 const D3D12_BOX* box)
{
    auto* proxy = static_cast<ResourceProxy*>(object);
    if (proxy->host == nullptr) {
        return E_NOTIMPL;
    }
    return proxy->host->ReadFromSubresource(data, row_pitch, depth_pitch, subresource, box);
}

HRESULT __stdcall resource_get_heap_properties(void* object,
                                               D3D12_HEAP_PROPERTIES* properties,
                                               D3D12_HEAP_FLAGS* flags)
{
    auto* proxy = static_cast<ResourceProxy*>(object);
    if (proxy->host == nullptr) {
        return E_NOTIMPL;
    }
    return proxy->host->GetHeapProperties(properties, flags);
}

void __stdcall heap_get_desc(void* object, D3D12_HEAP_DESC* out)
{
    if (out != nullptr) {
        *out = static_cast<HeapProxy*>(object)->description;
    }
}

void __stdcall descriptor_heap_get_desc(void* object, D3D12_DESCRIPTOR_HEAP_DESC* out)
{
    if (out != nullptr) {
        *out = static_cast<DescriptorHeapProxy*>(object)->description;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE __stdcall descriptor_heap_cpu_start(void* object)
{
    return static_cast<DescriptorHeapProxy*>(object)->cpu_start;
}

D3D12_GPU_DESCRIPTOR_HANDLE __stdcall descriptor_heap_gpu_start(void* object)
{
    return static_cast<DescriptorHeapProxy*>(object)->gpu_start;
}

template <ProxyKind Kind, std::uint32_t Slot>
HRESULT __stdcall unsupported_slot(void*)
{
    log_unsupported(Kind, Slot);
    return E_NOTIMPL;
}

template <ProxyKind Kind, std::uint32_t Start, std::size_t... Index>
void fill_unsupported(void** table, std::index_sequence<Index...>)
{
    ((table[Start + Index] = reinterpret_cast<void*>(
          &unsupported_slot<Kind, static_cast<std::uint32_t>(Start + Index)>)),
     ...);
}

void** make_resource_vtable()
{
    auto* table = new void*[kResourceSlots]{};
    table[0] = reinterpret_cast<void*>(&resource_query);
    table[1] = reinterpret_cast<void*>(&add_ref<ResourceProxy>);
    table[2] = reinterpret_cast<void*>(&release<ResourceProxy>);
    table[3] = reinterpret_cast<void*>(&get_private_data);
    table[4] = reinterpret_cast<void*>(&set_private_data);
    table[5] = reinterpret_cast<void*>(&resource_set_private_data_interface);
    table[6] = reinterpret_cast<void*>(&resource_set_name);
    table[7] = reinterpret_cast<void*>(&resource_get_device);
    table[8] = reinterpret_cast<void*>(&resource_map);
    table[9] = reinterpret_cast<void*>(&resource_unmap);
    table[10] = reinterpret_cast<void*>(&resource_get_desc);
    table[11] = reinterpret_cast<void*>(&resource_gpu_address);
    table[12] = reinterpret_cast<void*>(&resource_write_to_subresource);
    table[13] = reinterpret_cast<void*>(&resource_read_from_subresource);
    table[14] = reinterpret_cast<void*>(&resource_get_heap_properties);
    fill_unsupported<ProxyKind::Resource, kResourceFirstExtension>(
        table, std::make_index_sequence<kResourceSlots - kResourceFirstExtension>{});
    return table;
}

void** make_heap_vtable()
{
    auto* table = new void*[kHeapSlots]{};
    table[0] = reinterpret_cast<void*>(&heap_query);
    table[1] = reinterpret_cast<void*>(&add_ref<HeapProxy>);
    table[2] = reinterpret_cast<void*>(&release<HeapProxy>);
    table[3] = reinterpret_cast<void*>(&get_private_data);
    table[4] = reinterpret_cast<void*>(&set_private_data);
    table[5] = reinterpret_cast<void*>(&heap_set_private_data_interface);
    table[6] = reinterpret_cast<void*>(&heap_set_name);
    table[7] = reinterpret_cast<void*>(&heap_get_device);
    table[8] = reinterpret_cast<void*>(&heap_get_desc);
    fill_unsupported<ProxyKind::Heap, kHeapFirstExtension>(
        table, std::make_index_sequence<kHeapSlots - kHeapFirstExtension>{});
    return table;
}

void** make_descriptor_heap_vtable()
{
    auto* table = new void*[kDescriptorHeapSlots]{};
    table[0] = reinterpret_cast<void*>(&descriptor_heap_query);
    table[1] = reinterpret_cast<void*>(&add_ref<DescriptorHeapProxy>);
    table[2] = reinterpret_cast<void*>(&release<DescriptorHeapProxy>);
    table[3] = reinterpret_cast<void*>(&get_private_data);
    table[4] = reinterpret_cast<void*>(&set_private_data);
    table[5] = reinterpret_cast<void*>(&descriptor_heap_set_private_data_interface);
    table[6] = reinterpret_cast<void*>(&descriptor_heap_set_name);
    table[7] = reinterpret_cast<void*>(&descriptor_heap_get_device);
    table[8] = reinterpret_cast<void*>(&descriptor_heap_get_desc);
    table[9] = reinterpret_cast<void*>(&descriptor_heap_cpu_start);
    table[10] = reinterpret_cast<void*>(&descriptor_heap_gpu_start);
    fill_unsupported<ProxyKind::DescriptorHeap, kDescriptorHeapFirstExtension>(
        table, std::make_index_sequence<kDescriptorHeapSlots - kDescriptorHeapFirstExtension>{});
    return table;
}

void** resource_vtable()
{
    static void** value = make_resource_vtable();
    return value;
}

void** heap_vtable()
{
    static void** value = make_heap_vtable();
    return value;
}

void** descriptor_heap_vtable()
{
    static void** value = make_descriptor_heap_vtable();
    return value;
}

static_assert(std::is_standard_layout_v<ResourceProxy>);
static_assert(std::is_standard_layout_v<HeapProxy>);
static_assert(std::is_standard_layout_v<DescriptorHeapProxy>);
static_assert(offsetof(ResourceProxy, vtable) == 0);
static_assert(offsetof(HeapProxy, vtable) == 0);
static_assert(offsetof(DescriptorHeapProxy, vtable) == 0);

}

ResourceProxy::ResourceProxy(ID3D12Resource* resource, void* device)
    : vtable(resource_vtable())
    , bits(0)
    , references(1)
    , host(resource)
    , owner_device(device)
    , description{}
{
    if (host != nullptr) {
        host->AddRef();
        description = host->GetDesc();
    }
}

ResourceProxy::~ResourceProxy()
{
    erase_private_data(this);
    forget_resource_state(host);
    forget_gpu_ranges(host);
    if (host != nullptr) {
        host->Release();
    }
}

HeapProxy::HeapProxy(ID3D12Heap* heap, void* device)
    : vtable(heap_vtable())
    , bits(0)
    , references(1)
    , host(heap)
    , owner_device(device)
    , description{}
{
    if (host != nullptr) {
        host->AddRef();
        description = host->GetDesc();
    }
}

HeapProxy::~HeapProxy()
{
    erase_private_data(this);
    if (host != nullptr) {
        host->Release();
    }
}

DescriptorHeapProxy::DescriptorHeapProxy(ID3D12DescriptorHeap* heap, void* device)
    : vtable(descriptor_heap_vtable())
    , bits(0)
    , references(1)
    , host(heap)
    , owner_device(device)
    , description{}
    , cpu_start{}
    , gpu_start{}
{
    if (host != nullptr) {
        host->AddRef();
        description = host->GetDesc();
        cpu_start = host->GetCPUDescriptorHandleForHeapStart();
        gpu_start = host->GetGPUDescriptorHandleForHeapStart();
    }
}

DescriptorHeapProxy::~DescriptorHeapProxy()
{
    erase_private_data(this);
    if (host != nullptr) {
        host->Release();
    }
}

ResourceProxy* create_resource_proxy(ID3D12Resource* host, void* owner_device)
{
    return new ResourceProxy(host, owner_device);
}

ResourceProxy* create_resource_proxy(ID3D12Resource* host, void* owner_device,
                                     const D3D12_RESOURCE_DESC& description)
{
    auto* proxy = new ResourceProxy(host, owner_device);
    proxy->description = description;
    D3D12_RESOURCE_STATES state{};
    bool fixed = false;
    if (host != nullptr && !tracked_resource_state(host, state, fixed)) {
        track_resource_state(host, D3D12_RESOURCE_STATE_COMMON);
    }
    register_host_buffer(host);
    return proxy;
}

namespace {

struct TrackedState {
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool fixed = false;
};

std::mutex g_state_mutex;
std::unordered_map<ID3D12Resource*, TrackedState> g_states;

}

void track_resource_state(ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                          bool fixed) noexcept
{
    if (resource == nullptr) {
        return;
    }
    std::scoped_lock lock(g_state_mutex);
    auto& entry = g_states[resource];
    entry.state = state;
    entry.fixed = entry.fixed || fixed;
}

bool tracked_resource_state(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                            bool& fixed) noexcept
{
    std::scoped_lock lock(g_state_mutex);
    const auto found = g_states.find(resource);
    if (found == g_states.end()) {
        return false;
    }
    state = found->second.state;
    fixed = found->second.fixed;
    return true;
}

namespace {

struct GpuRange {
    std::uint64_t size = 0;
    D3D12_GPU_VIRTUAL_ADDRESS host = 0;
    ID3D12Resource* owner = nullptr;
};

std::mutex g_range_mutex;
std::map<std::uint64_t, GpuRange> g_title_ranges;
std::map<std::uint64_t, GpuRange> g_host_ranges;

bool find_range(const std::map<std::uint64_t, GpuRange>& ranges, std::uint64_t address,
                D3D12_GPU_VIRTUAL_ADDRESS& host)
{
    auto found = ranges.upper_bound(address);
    if (found == ranges.begin()) {
        return false;
    }
    --found;
    if (address >= found->first + found->second.size) {
        return false;
    }
    host = found->second.host + (address - found->first);
    return true;
}

void erase_owner(std::map<std::uint64_t, GpuRange>& ranges, ID3D12Resource* owner)
{
    for (auto entry = ranges.begin(); entry != ranges.end();) {
        entry = entry->second.owner == owner ? ranges.erase(entry) : std::next(entry);
    }
}

}

void register_host_buffer(ID3D12Resource* resource) noexcept
{
    if (resource == nullptr) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS base = resource->GetGPUVirtualAddress();
    if (base == 0) {
        return;
    }
    std::scoped_lock lock(g_range_mutex);
    g_host_ranges[base] = GpuRange{desc.Width, base, resource};
}

void forget_gpu_ranges(ID3D12Resource* resource) noexcept
{
    std::scoped_lock lock(g_range_mutex);
    erase_owner(g_title_ranges, resource);
    erase_owner(g_host_ranges, resource);
}

void register_title_range(ID3D12Resource* resource, std::uint64_t title_address,
                          std::uint64_t size) noexcept
{
    if (resource == nullptr || title_address == 0 || size == 0) {
        return;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS base = resource->GetGPUVirtualAddress();
    if (base == 0) {
        return;
    }
    std::scoped_lock lock(g_range_mutex);
    g_title_ranges[title_address] = GpuRange{size, base, resource};
}

bool translate_gpu_address(std::uint64_t address, D3D12_GPU_VIRTUAL_ADDRESS& host) noexcept
{
    std::scoped_lock lock(g_range_mutex);
    return find_range(g_title_ranges, address, host) || find_range(g_host_ranges, address, host);
}

namespace {

std::mutex g_pipeline_mutex;
std::unordered_map<void*, bool> g_pipelines;

}

void note_host_pipeline(void* pipeline) noexcept
{
    if (pipeline == nullptr) {
        return;
    }
    std::scoped_lock lock(g_pipeline_mutex);
    g_pipelines[pipeline] = true;
}

bool is_host_pipeline(void* pipeline) noexcept
{
    std::scoped_lock lock(g_pipeline_mutex);
    return g_pipelines.contains(pipeline);
}

void forget_resource_state(ID3D12Resource* resource) noexcept
{
    std::scoped_lock lock(g_state_mutex);
    g_states.erase(resource);
}

HeapProxy* create_heap_proxy(ID3D12Heap* host, void* owner_device)
{
    return new HeapProxy(host, owner_device);
}

HeapProxy* create_heap_proxy(ID3D12Heap* host, void* owner_device,
                             const D3D12_HEAP_DESC& description)
{
    auto* proxy = new HeapProxy(host, owner_device);
    proxy->description = description;
    return proxy;
}

DescriptorHeapProxy* create_descriptor_heap_proxy(ID3D12DescriptorHeap* host, void* owner_device)
{
    return new DescriptorHeapProxy(host, owner_device);
}

DescriptorHeapProxy* create_descriptor_heap_proxy(ID3D12DescriptorHeap* host, void* owner_device,
                                                  const D3D12_DESCRIPTOR_HEAP_DESC& description)
{
    auto* proxy = new DescriptorHeapProxy(host, owner_device);
    proxy->description = description;
    return proxy;
}

DescriptorHeapProxy* create_descriptor_heap_proxy(ID3D12DescriptorHeap* host, void* owner_device,
                                                  const D3D12_DESCRIPTOR_HEAP_DESC& description,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE cpu_start,
                                                  D3D12_GPU_DESCRIPTOR_HANDLE gpu_start)
{
    auto* proxy = new DescriptorHeapProxy(host, owner_device);
    proxy->description = description;
    proxy->cpu_start = cpu_start;
    proxy->gpu_start = gpu_start;
    return proxy;
}

bool is_resource_proxy(const void* object) noexcept
{
    return object != nullptr && *static_cast<void* const* const>(const_cast<void*>(object))
                                    == resource_vtable();
}

bool is_heap_proxy(const void* object) noexcept
{
    return object != nullptr
        && *static_cast<void* const* const>(const_cast<void*>(object)) == heap_vtable();
}

bool is_descriptor_heap_proxy(const void* object) noexcept
{
    return object != nullptr
        && *static_cast<void* const* const>(const_cast<void*>(object)) == descriptor_heap_vtable();
}

ID3D12Resource* unwrap(ResourceProxy* proxy) noexcept
{
    return proxy != nullptr ? proxy->host : nullptr;
}

ID3D12Heap* unwrap(HeapProxy* proxy) noexcept
{
    return proxy != nullptr ? proxy->host : nullptr;
}

ID3D12DescriptorHeap* unwrap(DescriptorHeapProxy* proxy) noexcept
{
    return proxy != nullptr ? proxy->host : nullptr;
}

ID3D12Resource* unwrap_resource(void* object) noexcept
{
    if (!is_resource_proxy(object)) {
        return nullptr;
    }
    return static_cast<ResourceProxy*>(object)->host;
}

ID3D12Heap* unwrap_heap(void* object) noexcept
{
    if (!is_heap_proxy(object)) {
        return nullptr;
    }
    return static_cast<HeapProxy*>(object)->host;
}

ID3D12DescriptorHeap* unwrap_descriptor_heap(void* object) noexcept
{
    if (!is_descriptor_heap_proxy(object)) {
        return nullptr;
    }
    return static_cast<DescriptorHeapProxy*>(object)->host;
}

void* to_unknown(ResourceProxy* proxy) noexcept
{
    return proxy;
}

namespace {

struct InertObject {
    void** vtable;
    void* owner_device;
};

HRESULT __stdcall inert_query_interface(InertObject* self, REFIID, void** out)
{
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = self;
    return S_OK;
}

ULONG __stdcall inert_add_ref(InertObject*)
{
    return 2;
}

ULONG __stdcall inert_release(InertObject*)
{
    return 1;
}

HRESULT __stdcall inert_get_device(InertObject* self, void* first, void* second)
{
    void** out = device_out_slot(first, second);
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = self != nullptr ? self->owner_device : nullptr;
    return S_OK;
}

HRESULT __stdcall inert_ok()
{
    return S_OK;
}

void** inert_vtable()
{
    static void** table = [] {
        auto* built = new void*[256];
        for (std::size_t slot = 0; slot < 256; ++slot) {
            built[slot] = reinterpret_cast<void*>(&inert_ok);
        }
        built[0] = reinterpret_cast<void*>(&inert_query_interface);
        built[1] = reinterpret_cast<void*>(&inert_add_ref);
        built[2] = reinterpret_cast<void*>(&inert_release);
        built[7] = reinterpret_cast<void*>(&inert_get_device);
        return built;
    }();
    return table;
}

}

void** device_out_slot(void* first, void* second) noexcept
{
    if (first == nullptr) {
        return static_cast<void**>(second);
    }
    MEMORY_BASIC_INFORMATION info{};
    if (second != nullptr && VirtualQuery(first, &info, sizeof(info)) != 0) {
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE
                             | PAGE_EXECUTE_WRITECOPY;
        if ((info.Protect & writable) == 0) {
            return static_cast<void**>(second);
        }
    }
    return static_cast<void**>(first);
}

void* create_inert_object(void* owner_device)
{
    return new InertObject{inert_vtable(), owner_device};
}

bool is_inert_object(const void* object) noexcept
{
    return object != nullptr && *static_cast<void** const*>(object) == inert_vtable();
}

namespace {

std::atomic<ObjectUnwrapper> g_object_unwrapper{nullptr};

}

void set_object_unwrapper(ObjectUnwrapper unwrapper) noexcept
{
    g_object_unwrapper.store(unwrapper, std::memory_order_release);
}

void* unwrap_any(void* object) noexcept
{
    if (object == nullptr || is_inert_object(object)) {
        return nullptr;
    }
    if (is_resource_proxy(object)) {
        return unwrap_resource(object);
    }
    if (is_descriptor_heap_proxy(object)) {
        return unwrap_descriptor_heap(object);
    }
    if (is_heap_proxy(object)) {
        return unwrap_heap(object);
    }
    if (const ObjectUnwrapper unwrapper = g_object_unwrapper.load(std::memory_order_acquire);
        unwrapper != nullptr) {
        return unwrapper(object);
    }
    return object;
}

void* to_unknown(HeapProxy* proxy) noexcept
{
    return proxy;
}

void* to_unknown(DescriptorHeapProxy* proxy) noexcept
{
    return proxy;
}

}
