#include <euphoria/gpu/command.hpp>

#include <euphoria/gpu/draw.hpp>
#include <euphoria/gpu/present.hpp>
#include <euphoria/gpu/descriptor_host.hpp>
#include <euphoria/gpu/resources.hpp>
#include <euphoria/gpu/title_memory.hpp>
#include <euphoria/log/log.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace euphoria::gpu {
namespace {

constexpr HRESULT k_not_implemented = static_cast<HRESULT>(0x80004001u);
constexpr UINT k_render_target_limit = static_cast<UINT>(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
constexpr UINT64 k_render_target_stride = 128;

void** queue_table(CommandLayout layout);
void** list_table(CommandLayout layout);

bool same_iid(REFIID left, REFIID right)
{
    return IsEqualGUID(left, right) != 0;
}

bool queue_iid_known(REFIID iid)
{
    return same_iid(iid, __uuidof(IUnknown)) || same_iid(iid, __uuidof(ID3D12Object))
        || same_iid(iid, __uuidof(ID3D12DeviceChild)) || same_iid(iid, __uuidof(ID3D12Pageable))
        || same_iid(iid, __uuidof(ID3D12CommandQueue));
}

bool list_iid_known(REFIID iid)
{
    return same_iid(iid, __uuidof(IUnknown)) || same_iid(iid, __uuidof(ID3D12Object))
        || same_iid(iid, __uuidof(ID3D12DeviceChild))
        || same_iid(iid, __uuidof(ID3D12GraphicsCommandList))
        || same_iid(iid, __uuidof(ID3D12GraphicsCommandList1))
        || same_iid(iid, __uuidof(ID3D12GraphicsCommandList2))
        || same_iid(iid, __uuidof(ID3D12GraphicsCommandList3))
        || same_iid(iid, __uuidof(ID3D12GraphicsCommandList4))
        || same_iid(iid, __uuidof(ID3D12GraphicsCommandList5))
        || same_iid(iid, __uuidof(ID3D12GraphicsCommandList6))
        || same_iid(iid, __uuidof(ID3D12GraphicsCommandList7));
}

struct PrivateEntry {
    const void* object;
    GUID id;
    std::vector<std::uint8_t> data;
    IUnknown* iface;
};

std::mutex g_private_mutex;
std::vector<PrivateEntry> g_private;

PrivateEntry* find_private(const void* object, REFGUID id)
{
    for (PrivateEntry& entry : g_private) {
        if (entry.object == object && same_iid(entry.id, id)) {
            return &entry;
        }
    }
    return nullptr;
}

HRESULT get_private(const void* object, REFGUID id, UINT* size, void* data)
{
    if (size == nullptr) {
        return E_INVALIDARG;
    }
    std::scoped_lock lock(g_private_mutex);
    PrivateEntry* entry = find_private(object, id);
    if (entry == nullptr) {
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    const UINT required = static_cast<UINT>(entry->data.size());
    const UINT capacity = *size;
    *size = required;
    if (data == nullptr) {
        return S_OK;
    }
    if (capacity < required) {
        return DXGI_ERROR_MORE_DATA;
    }
    if (required != 0) {
        std::memcpy(data, entry->data.data(), required);
    }
    return S_OK;
}

HRESULT set_private(const void* object, REFGUID id, UINT size, const void* data)
{
    std::scoped_lock lock(g_private_mutex);
    if (data == nullptr) {
        for (auto it = g_private.begin(); it != g_private.end(); ++it) {
            if (it->object == object && same_iid(it->id, id)) {
                if (it->iface != nullptr) {
                    it->iface->Release();
                }
                g_private.erase(it);
                break;
            }
        }
        return S_OK;
    }
    PrivateEntry* entry = find_private(object, id);
    if (entry == nullptr) {
        g_private.push_back(PrivateEntry{object, id, {}, nullptr});
        entry = &g_private.back();
    }
    if (entry->iface != nullptr) {
        entry->iface->Release();
        entry->iface = nullptr;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    entry->data.assign(bytes, bytes + size);
    return S_OK;
}

HRESULT set_private_interface(const void* object, REFGUID id, const IUnknown* unknown)
{
    IUnknown* keep = const_cast<IUnknown*>(unknown);
    if (keep != nullptr) {
        keep->AddRef();
    }
    std::scoped_lock lock(g_private_mutex);
    PrivateEntry* entry = find_private(object, id);
    if (entry == nullptr) {
        g_private.push_back(PrivateEntry{object, id, {}, nullptr});
        entry = &g_private.back();
    }
    if (entry->iface != nullptr) {
        entry->iface->Release();
    }
    entry->iface = keep;
    entry->data.clear();
    return S_OK;
}

void forget_private(const void* object)
{
    std::vector<IUnknown*> released;
    {
        std::scoped_lock lock(g_private_mutex);
        for (auto it = g_private.begin(); it != g_private.end();) {
            if (it->object == object) {
                if (it->iface != nullptr) {
                    released.push_back(it->iface);
                }
                it = g_private.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (IUnknown* iface : released) {
        iface->Release();
    }
}

std::string slot_text(const char* kind, std::size_t slot, bool implemented)
{
    std::string text = "d3d12: ";
    if (!implemented) {
        text += "unimplemented ";
    }
    text += kind;
    text += " slot ";
    text += std::to_string(slot);
    return text;
}

void note_queue_slot(CommandQueueProxy* self, std::size_t slot, bool implemented)
{
    if (self == nullptr || slot >= command_queue_slot_count) {
        return;
    }
    const std::uint32_t count =
        self->slot_calls[slot].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 3) {
        return;
    }
    const std::string text = slot_text("command queue", slot, implemented);
    if (implemented) {
        log::info(text);
    } else {
        log::warn(text);
    }
}

void note_list_slot(CommandListProxy* self, std::size_t slot, bool implemented)
{
    if (self == nullptr || slot >= command_list_slot_count) {
        return;
    }
    const std::uint32_t count =
        self->slot_calls[slot].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 3) {
        return;
    }
    const std::string text = slot_text("command list", slot, implemented);
    if (implemented) {
        log::info(text);
    } else {
        log::warn(text);
    }
}

CommandLayout queue_layout(const CommandQueueProxy* self)
{
    if (self != nullptr && self->vtable == queue_table(CommandLayout::Newer)) {
        return CommandLayout::Newer;
    }
    return CommandLayout::Legacy;
}

CommandLayout list_layout(const CommandListProxy* self)
{
    if (self != nullptr && self->vtable == list_table(CommandLayout::Newer)) {
        return CommandLayout::Newer;
    }
    return CommandLayout::Legacy;
}

std::size_t queue_slot(CommandLayout layout, std::size_t legacy_slot)
{
    return layout == CommandLayout::Newer && legacy_slot >= 14 ? legacy_slot + 2 : legacy_slot;
}

std::size_t list_slot(CommandLayout layout, std::size_t legacy_slot)
{
    return layout == CommandLayout::Newer && legacy_slot >= 59 ? legacy_slot + 2 : legacy_slot;
}

void note_queue(CommandQueueProxy* self, std::size_t legacy_slot)
{
    note_queue_slot(self, queue_slot(queue_layout(self), legacy_slot), true);
}

void note_list(CommandListProxy* self, std::size_t legacy_slot)
{
    note_list_slot(self, list_slot(list_layout(self), legacy_slot), true);
}

HRESULT __stdcall queue_query_interface(CommandQueueProxy* self, REFIID iid, void** out)
{
    note_queue(self, 0);
    if (out == nullptr) {
        return E_POINTER;
    }
    if (self != nullptr && queue_iid_known(iid)) {
        self->references.fetch_add(1, std::memory_order_relaxed);
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    log::warn("d3d12: command queue does not answer a requested interface");
    return E_NOINTERFACE;
}

std::uint32_t __stdcall queue_add_ref(CommandQueueProxy* self)
{
    if (self == nullptr) {
        return 0;
    }
    return self->references.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint32_t __stdcall queue_release(CommandQueueProxy* self)
{
    if (self == nullptr) {
        return 0;
    }
    const std::uint32_t previous =
        self->references.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1) {
        forget_private(self);
        delete self;
        return 0;
    }
    return previous - 1;
}

HRESULT __stdcall queue_set_private_data(CommandQueueProxy* self,
                                         REFGUID id,
                                         UINT size,
                                         const void* data)
{
    note_queue(self, 4);
    return set_private(self, id, size, data);
}

HRESULT __stdcall queue_set_private_data_interface(CommandQueueProxy* self,
                                                   REFGUID id,
                                                   const IUnknown* unknown)
{
    note_queue(self, 5);
    return set_private_interface(self, id, unknown);
}

HRESULT __stdcall queue_get_private_data(CommandQueueProxy* self,
                                         REFGUID id,
                                         UINT* size,
                                         void* data)
{
    note_queue(self, 3);
    return get_private(self, id, size, data);
}

HRESULT __stdcall queue_set_name(CommandQueueProxy* self, const wchar_t* name)
{
    note_queue(self, 6);
    if (name != nullptr) {
        log::debug("d3d12: command queue named");
    }
    return S_OK;
}

HRESULT __stdcall queue_get_device(CommandQueueProxy* self, void* first, void* second)
{
    note_queue(self, 7);
    void** out = device_out_slot(first, second);
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = self != nullptr ? self->owner_device : nullptr;
    return S_OK;
}

void complete_submission(CommandQueueProxy* self)
{
    std::scoped_lock lock(self->completion_mutex);
    if (self->completion == nullptr) {
        if (self->device == nullptr
            || FAILED(self->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                IID_PPV_ARGS(&self->completion)))) {
            log::warn("d3d12: queue completion fence creation failed");
            self->completion = nullptr;
            return;
        }
        self->completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    const std::uint64_t value = ++self->completion_value;
    if (FAILED(self->host->Signal(self->completion, value))) {
        return;
    }
    if (self->completion->GetCompletedValue() >= value) {
        return;
    }
    if (self->completion_event != nullptr
        && SUCCEEDED(self->completion->SetEventOnCompletion(value, self->completion_event))) {
        if (WaitForSingleObject(self->completion_event, 5000) == WAIT_TIMEOUT) {
            log::warn("d3d12: queue submission did not complete within 5 s");
        }
    }
}

void apply_writes(CommandQueueProxy* self, const std::vector<PendingWrite>& writes)
{
    std::uint64_t frequency = 0;
    if (self->host != nullptr) {
        self->host->GetTimestampFrequency(&frequency);
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    LARGE_INTEGER performance{};
    QueryPerformanceFrequency(&performance);
    static std::atomic<std::uint32_t> logged{0};
    for (const PendingWrite& write : writes) {
        std::uint64_t value = write.value;
        if (write.kind == 1 && frequency != 0 && performance.QuadPart != 0) {
            value = static_cast<std::uint64_t>(
                static_cast<double>(counter.QuadPart) * static_cast<double>(frequency)
                / static_cast<double>(performance.QuadPart));
        } else if (write.kind == 2) {
            value = static_cast<std::uint64_t>(counter.QuadPart);
        }
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(write.address), &info, sizeof(info)) == 0
            || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) == 0) {
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                log::warn("d3d12: bottom-of-pipe write to unwritable 0x"
                          + std::to_string(write.address));
            }
            continue;
        }
        if (write.bytes == 4) {
            *reinterpret_cast<volatile std::uint32_t*>(write.address) =
                static_cast<std::uint32_t>(value);
        } else {
            *reinterpret_cast<volatile std::uint64_t*>(write.address) = value;
        }
    }
}

void __stdcall queue_execute_command_lists(CommandQueueProxy* self,
                                           UINT count,
                                           void* const* lists)
{
    note_queue(self, 10);
    if (self == nullptr || self->host == nullptr || lists == nullptr || count == 0) {
        return;
    }
    std::vector<ID3D12CommandList*> host_lists;
    host_lists.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        ID3D12CommandList* host = unwrap_command_list(lists[index]);
        if (host == nullptr) {
            continue;
        }
        if (!is_command_list_proxy(lists[index])) {
            void* unwrapped = unwrap_any(lists[index]);
            static std::atomic<std::uint32_t> foreign{0};
            if (foreign.fetch_add(1, std::memory_order_relaxed) < 16) {
                char text[160]{};
                void* table = *static_cast<void**>(lists[index]);
                std::snprintf(text, sizeof(text),
                              "d3d12: submitted object %p (vtable %p) is no list proxy, unwraps to %p",
                              lists[index], table, unwrapped);
                log::warn(text);
            }
            if (unwrapped == nullptr || unwrapped == lists[index]) {
                continue;
            }
            host = static_cast<ID3D12CommandList*>(unwrapped);
        }
        if (is_command_list_proxy(lists[index])
            && !static_cast<CommandListProxy*>(lists[index])->closed.load(
                std::memory_order_acquire)) {
            static std::atomic<std::uint32_t> skipped{0};
            if (skipped.fetch_add(1, std::memory_order_relaxed) < 16) {
                char text[96]{};
                std::snprintf(text, sizeof(text), "d3d12: skipped unclosed command list %p",
                              static_cast<void*>(host));
                log::warn(text);
            }
            continue;
        }
        host_lists.push_back(host);
    }
    static std::atomic<std::uint64_t> submissions{0};
    const std::uint64_t submission = submissions.fetch_add(1, std::memory_order_relaxed);
    if (submission < 16 || (submission & 1023) == 0) {
        std::string names;
        for (ID3D12CommandList* list : host_lists) {
            char text[48]{};
            std::snprintf(text, sizeof(text), " %p/t%d", static_cast<void*>(list),
                          static_cast<int>(list->GetType()));
            names += text;
        }
        names += " queue type " + std::to_string(static_cast<int>(self->host->GetDesc().Type));
        char queue_text[32]{};
        std::snprintf(queue_text, sizeof(queue_text), "%p", static_cast<void*>(self->host));
        log::info("d3d12: ExecuteCommandLists #" + std::to_string(submission + 1) + " queue "
                  + queue_text + " count " + std::to_string(count) + " host"
                  + names);
    }
    static const bool no_execute = GetEnvironmentVariableW(L"EUPHORIA_NO_EXECUTE", nullptr, 0) != 0;
    if (!host_lists.empty() && !no_execute) {
        self->host->ExecuteCommandLists(static_cast<UINT>(host_lists.size()), host_lists.data());
    }
    std::vector<PendingWrite> writes;
    for (UINT index = 0; index < count; ++index) {
        if (!is_command_list_proxy(lists[index])) {
            continue;
        }
        auto* list = static_cast<CommandListProxy*>(lists[index]);
        std::scoped_lock lock(list->pending_mutex);
        writes.insert(writes.end(), list->pending_writes.begin(), list->pending_writes.end());
        list->pending_writes.clear();
    }
    if (!host_lists.empty() && !no_execute) {
        complete_submission(self);
    }
    if (!writes.empty()) {
        apply_writes(self, writes);
    }
}

void __stdcall queue_execute_command_lists_x(CommandQueueProxy* self, UINT count,
                                             void* const* lists, UINT64 token, UINT flags)
{
    (void)token;
    (void)flags;
    queue_execute_command_lists(self, count, lists);
}

HRESULT __stdcall queue_signal(CommandQueueProxy* self, ID3D12Fence* fence, UINT64 value);

D3D12_GPU_VIRTUAL_ADDRESS host_gpu_address(std::uint64_t address, std::size_t size);

HRESULT __stdcall queue_signal2_x(CommandQueueProxy* self, ID3D12Fence* fence, UINT64 value,
                                  const void* bottom, const void* top)
{
    (void)bottom;
    (void)top;
    return queue_signal(self, fence, value);
}

HRESULT __stdcall queue_copy_page_mappings(CommandQueueProxy* self, UINT count, const void* batches,
                                           UINT64 pool, UINT pool_pages, UINT flags)
{
    (void)self;
    (void)batches;
    (void)flags;
    static std::atomic<std::uint32_t> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 8) {
        log::warn("d3d12: CopyPageMappingsBatchX count " + std::to_string(count) + " pool 0x"
                  + std::to_string(pool) + " pages " + std::to_string(pool_pages)
                  + " not mapped");
    }
    return S_OK;
}

void __stdcall queue_set_marker(CommandQueueProxy* self, UINT, const void*, UINT)
{
    note_queue(self, 11);
}

void __stdcall queue_begin_event(CommandQueueProxy* self, UINT, const void*, UINT)
{
    note_queue(self, 12);
}

void __stdcall queue_end_event(CommandQueueProxy* self)
{
    note_queue(self, 13);
}

HRESULT __stdcall queue_signal(CommandQueueProxy* self, ID3D12Fence* fence, UINT64 value)
{
    note_queue(self, 14);
    if (fence == nullptr) {
        return E_INVALIDARG;
    }
    if (self == nullptr || self->host == nullptr) {
        return E_FAIL;
    }
    auto* host_fence = static_cast<ID3D12Fence*>(unwrap_any(fence));
    ID3D12Fence* signalled = host_fence != nullptr ? host_fence : fence;
    {
        static std::atomic<std::uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 24) {
            char text[160]{};
            std::snprintf(text, sizeof(text), "d3d12: queue %p Signal fence %p value %llu",
                          static_cast<void*>(self->host), static_cast<void*>(signalled),
                          static_cast<unsigned long long>(value));
            log::info(text);
        }
    }
    const HRESULT result = self->host->Signal(signalled, value);
    if (FAILED(result)) {
        log::warn("d3d12: queue Signal failed 0x" + std::to_string(static_cast<std::uint32_t>(result)));
    }
    return result;
}

HRESULT __stdcall queue_wait(CommandQueueProxy* self, ID3D12Fence* fence, UINT64 value)
{
    note_queue(self, 15);
    if (fence == nullptr) {
        return E_INVALIDARG;
    }
    if (self == nullptr || self->host == nullptr) {
        return E_FAIL;
    }
    auto* host_fence = static_cast<ID3D12Fence*>(unwrap_any(fence));
    ID3D12Fence* waited = host_fence != nullptr ? host_fence : fence;
    {
        static std::atomic<std::uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 24) {
            char text[160]{};
            std::snprintf(text, sizeof(text), "d3d12: queue %p Wait fence %p value %llu completed %llu",
                          static_cast<void*>(self->host), static_cast<void*>(waited),
                          static_cast<unsigned long long>(value),
                          static_cast<unsigned long long>(waited->GetCompletedValue()));
            log::info(text);
        }
    }
    const HRESULT result = self->host->Wait(waited, value);
    if (FAILED(result)) {
        log::warn("d3d12: queue Wait failed 0x" + std::to_string(static_cast<std::uint32_t>(result)));
    }
    return result;
}

HRESULT __stdcall queue_present_x(CommandQueueProxy* self, UINT planes,
                                  const euphoria::gpu::Plane* plane_parameters,
                                  const void* parameters)
{
    note_queue_slot(self, 30, true);
    return euphoria::gpu::present_plane(self, planes, plane_parameters, parameters);
}

HRESULT __stdcall queue_get_timestamp_frequency(CommandQueueProxy* self, UINT64* frequency)
{
    note_queue(self, 16);
    if (frequency == nullptr) {
        return E_INVALIDARG;
    }
    if (self == nullptr || self->host == nullptr) {
        return E_FAIL;
    }
    return self->host->GetTimestampFrequency(frequency);
}

HRESULT __stdcall queue_get_clock_calibration(CommandQueueProxy* self,
                                              UINT64* gpu_timestamp,
                                              UINT64* cpu_timestamp)
{
    note_queue(self, 17);
    if (gpu_timestamp == nullptr || cpu_timestamp == nullptr) {
        return E_INVALIDARG;
    }
    if (self == nullptr || self->host == nullptr) {
        return E_FAIL;
    }
    return self->host->GetClockCalibration(gpu_timestamp, cpu_timestamp);
}

void __stdcall queue_get_desc(CommandQueueProxy* self, D3D12_COMMAND_QUEUE_DESC* description)
{
    note_queue(self, 26);
    if (description != nullptr && self != nullptr && self->host != nullptr) {
        *description = self->host->GetDesc();
    }
}

HRESULT __stdcall list_query_interface(CommandListProxy* self, REFIID iid, void** out)
{
    note_list(self, 0);
    if (out == nullptr) {
        return E_POINTER;
    }
    if (self != nullptr && list_iid_known(iid)) {
        self->references.fetch_add(1, std::memory_order_relaxed);
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    log::warn("d3d12: command list does not answer a requested interface");
    return E_NOINTERFACE;
}

std::uint32_t __stdcall list_add_ref(CommandListProxy* self)
{
    if (self == nullptr) {
        return 0;
    }
    return self->references.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint32_t __stdcall list_release(CommandListProxy* self)
{
    if (self == nullptr) {
        return 0;
    }
    const std::uint32_t previous =
        self->references.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1) {
        forget_private(self);
        delete static_cast<DrawState*>(self->draw_state);
        delete self;
        return 0;
    }
    return previous - 1;
}

HRESULT __stdcall list_get_private_data(CommandListProxy* self,
                                        REFGUID id,
                                        UINT* size,
                                        void* data)
{
    note_list(self, 3);
    return get_private(self, id, size, data);
}

HRESULT __stdcall list_set_private_data(CommandListProxy* self,
                                        REFGUID id,
                                        UINT size,
                                        const void* data)
{
    note_list(self, 4);
    return set_private(self, id, size, data);
}

HRESULT __stdcall list_set_private_data_interface(CommandListProxy* self,
                                                  REFGUID id,
                                                  const IUnknown* unknown)
{
    note_list(self, 5);
    return set_private_interface(self, id, unknown);
}

HRESULT __stdcall list_set_name(CommandListProxy* self, const wchar_t* name)
{
    note_list(self, 6);
    if (name != nullptr) {
        log::debug("d3d12: command list named");
    }
    return S_OK;
}

HRESULT __stdcall list_get_device(CommandListProxy* self, void* first, void* second)
{
    note_list(self, 7);
    void** out = device_out_slot(first, second);
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = self != nullptr ? self->device : nullptr;
    return S_OK;
}

D3D12_COMMAND_LIST_TYPE __stdcall list_get_type(CommandListProxy* self)
{
    note_list(self, 8);
    if (self == nullptr || self->host == nullptr) {
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    return self->host->GetType();
}

HRESULT __stdcall list_close(CommandListProxy* self)
{
    note_list(self, 9);
    if (self == nullptr || self->host == nullptr) {
        return E_FAIL;
    }
    const HRESULT result = self->host->Close();
    self->closed.store(SUCCEEDED(result), std::memory_order_release);
    if (FAILED(result)) {
        static std::atomic<std::uint32_t> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 16) {
            char text[96]{};
            std::snprintf(text, sizeof(text), "d3d12: command list %p Close failed 0x%08x",
                          static_cast<void*>(self->host), static_cast<unsigned>(result));
            log::warn(text);
        }
    }
    return result;
}

bool pipeline_ready(CommandListProxy* self)
{
    const auto* state = static_cast<const DrawState*>(self->draw_state);
    if (state != nullptr && state->pipeline != nullptr) {
        return true;
    }
    static std::atomic<std::uint32_t> dropped{0};
    const std::uint32_t count = dropped.fetch_add(1, std::memory_order_relaxed);
    if (count < 8 || (count & 1023) == 0) {
        log::warn("d3d12: dropped work without a host pipeline (" + std::to_string(count + 1)
                  + " so far)");
    }
    return false;
}

HRESULT __stdcall list_reset(CommandListProxy* self, void* allocator, void* pipeline)
{
    note_list(self, 10);
    if (allocator == nullptr) {
        return E_INVALIDARG;
    }
    if (self == nullptr || self->host == nullptr) {
        return E_FAIL;
    }
    self->allocator = allocator;
    self->closed.store(false, std::memory_order_release);
    self->graphics_root = nullptr;
    self->compute_root = nullptr;
    {
        std::scoped_lock lock(self->pending_mutex);
        self->pending_writes.clear();
        self->uploads.clear();
    }
    if (self->draw_state != nullptr) {
        static_cast<DrawState*>(self->draw_state)->reset();
    }
    void* host_allocator = unwrap_any(allocator);
    void* host_pipeline = unwrap_any(pipeline);
    if (host_pipeline != nullptr && !is_host_pipeline(host_pipeline)) {
        static std::atomic<std::uint32_t> rejected{0};
        if (rejected.fetch_add(1, std::memory_order_relaxed) < 8) {
            char text[96]{};
            std::snprintf(text, sizeof(text), "d3d12: list reset ignores non-pipeline %p",
                          pipeline);
            log::warn(text);
        }
        host_pipeline = nullptr;
    }
    if (self->draw_state != nullptr) {
        static_cast<DrawState*>(self->draw_state)->pipeline =
            static_cast<ID3D12PipelineState*>(host_pipeline);
    }
    HRESULT result = self->host->Reset(static_cast<ID3D12CommandAllocator*>(host_allocator),
                                       static_cast<ID3D12PipelineState*>(host_pipeline));
    if (FAILED(result)) {
        self->host->Close();
        result = self->host->Reset(static_cast<ID3D12CommandAllocator*>(host_allocator),
                                   static_cast<ID3D12PipelineState*>(host_pipeline));
        if (FAILED(result)) {
            static std::atomic<std::uint32_t> failures{0};
            if (failures.fetch_add(1, std::memory_order_relaxed) < 8) {
                log::warn("d3d12: command list Reset failed 0x"
                          + std::to_string(static_cast<std::uint32_t>(result)));
            }
        }
    }
    return result;
}

void note_console_list_call(const char* what)
{
    static std::mutex mutex;
    static std::vector<std::string> seen;
    std::scoped_lock lock(mutex);
    for (const std::string& name : seen) {
        if (name == what) {
            return;
        }
    }
    seen.emplace_back(what);
    log::info(std::string("d3d12: command list ") + what + " handled");
}

void defer_write(CommandListProxy* self, std::uint64_t address, std::uint64_t value,
                 std::uint32_t bytes, std::uint32_t kind)
{
    if (self == nullptr || address == 0) {
        return;
    }
    std::scoped_lock lock(self->pending_mutex);
    self->pending_writes.push_back({address, value, bytes, kind});
}

void __stdcall list_write32(CommandListProxy* self, UINT64 address, UINT value, UINT flush,
                            UINT flags)
{
    (void)flush;
    (void)flags;
    note_console_list_call("Write32BottomOfPipe");
    defer_write(self, address, value, 4, 0);
}

void __stdcall list_write64(CommandListProxy* self, UINT64 address, UINT64 value, UINT flush,
                            UINT flags)
{
    (void)flush;
    note_console_list_call("Write64BottomOfPipe");
    defer_write(self, address, value, 8, flags == 1 || flags == 2 ? flags : 0);
}

void __stdcall list_wait_value(CommandListProxy* self, UINT64 address, UINT comparison,
                               UINT64 reference)
{
    (void)self;
    static std::atomic<std::uint32_t> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 8) {
        log::info("d3d12: command list WaitValue at 0x" + std::to_string(address) + " comparison "
                  + std::to_string(comparison) + " reference " + std::to_string(reference)
                  + " not enforced");
    }
}

void __stdcall list_ignored_state(CommandListProxy* self)
{
    (void)self;
}

void __stdcall list_sync(CommandListProxy* self, UINT flags, UINT control, UINT64 address,
                         UINT64 size)
{
    (void)self;
    (void)flags;
    (void)control;
    (void)address;
    (void)size;
    note_console_list_call("pipeline sync");
}

void __stdcall list_depth_bounds(CommandListProxy* self, float minimum, float maximum)
{
    note_console_list_call("depth bounds");
    if (self == nullptr || self->host == nullptr) {
        return;
    }
    ID3D12GraphicsCommandList1* list = nullptr;
    if (SUCCEEDED(self->host->QueryInterface(IID_PPV_ARGS(&list))) && list != nullptr) {
        list->OMSetDepthBounds(minimum, maximum);
        list->Release();
    }
}

void __stdcall list_barrier_groups(CommandListProxy* self, UINT count, const void* groups)
{
    (void)groups;
    note_console_list_call("barrier groups");
    if (self == nullptr || self->host == nullptr || count == 0) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    self->host->ResourceBarrier(1, &barrier);
}

void* __stdcall list_allocate_upload(CommandListProxy* self, UINT64 size, UINT64 alignment)
{
    note_console_list_call("upload allocation");
    const std::uint64_t align = alignment < 16 ? 16 : alignment;
    if (self == nullptr || (align & (align - 1)) != 0 || size > (std::uint64_t{1} << 32)) {
        return nullptr;
    }
    auto block = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size + align]{});
    if (!block) {
        return nullptr;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(block.get());
    void* at = reinterpret_cast<void*>((start + align - 1) & ~(align - 1));
    std::scoped_lock lock(self->pending_mutex);
    self->uploads.push_back(std::move(block));
    return at;
}

HRESULT __stdcall list_reset_x(CommandListProxy* self, void* allocator, void* first,
                               void* second)
{
    (void)first;
    (void)second;
    static std::atomic<std::uint32_t> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 4) {
        log::info("d3d12: command list ResetX");
    }
    if (allocator == nullptr) {
        allocator = self != nullptr ? self->allocator : nullptr;
    }
    if (allocator == nullptr) {
        return E_INVALIDARG;
    }
    return list_reset(self, allocator, first);
}

void __stdcall list_draw_instanced(CommandListProxy* self,
                                   UINT vertex_count,
                                   UINT instance_count,
                                   UINT start_vertex,
                                   UINT start_instance)
{
    note_list(self, 12);
    if (self != nullptr && self->host != nullptr && pipeline_ready(self)) {
        self->host->DrawInstanced(vertex_count, instance_count, start_vertex, start_instance);
    }
}

void __stdcall list_draw_indexed_instanced(CommandListProxy* self,
                                           UINT index_count,
                                           UINT instance_count,
                                           UINT start_index,
                                           INT base_vertex,
                                           UINT start_instance)
{
    note_list(self, 13);
    if (self != nullptr && self->host != nullptr && pipeline_ready(self)) {
        self->host->DrawIndexedInstanced(index_count,
                                         instance_count,
                                         start_index,
                                         base_vertex,
                                         start_instance);
    }
}

void __stdcall list_dispatch(CommandListProxy* self,
                             UINT thread_group_count_x,
                             UINT thread_group_count_y,
                             UINT thread_group_count_z)
{
    note_list(self, 14);
    if (self != nullptr && self->host != nullptr && pipeline_ready(self)) {
        self->host->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z);
    }
}

void __stdcall list_copy_buffer_region(CommandListProxy* self,
                                       ID3D12Resource* destination,
                                       UINT64 destination_offset,
                                       ID3D12Resource* source,
                                       UINT64 source_offset,
                                       UINT64 bytes)
{
    note_list(self, 15);
    if (self != nullptr && self->host != nullptr) {
        self->host->CopyBufferRegion(static_cast<ID3D12Resource*>(unwrap_any(destination)),
                                     destination_offset,
                                     static_cast<ID3D12Resource*>(unwrap_any(source)),
                                     source_offset,
                                     bytes);
    }
}

void __stdcall list_copy_texture_region(CommandListProxy* self,
                                        const D3D12_TEXTURE_COPY_LOCATION* destination,
                                        UINT destination_x,
                                        UINT destination_y,
                                        UINT destination_z,
                                        const D3D12_TEXTURE_COPY_LOCATION* source,
                                        const D3D12_BOX* source_box)
{
    note_list(self, 16);
    if (self != nullptr && self->host != nullptr) {
        if (destination == nullptr || source == nullptr) {
            return;
        }
        D3D12_TEXTURE_COPY_LOCATION host_destination = *destination;
        D3D12_TEXTURE_COPY_LOCATION host_source = *source;
        host_destination.pResource =
            static_cast<ID3D12Resource*>(unwrap_any(host_destination.pResource));
        host_source.pResource = static_cast<ID3D12Resource*>(unwrap_any(host_source.pResource));
        if (host_destination.pResource == nullptr || host_source.pResource == nullptr) {
            return;
        }
        self->host->CopyTextureRegion(&host_destination,
                                      destination_x,
                                      destination_y,
                                      destination_z,
                                      &host_source,
                                      source_box);
    }
}

void __stdcall list_copy_resource(CommandListProxy* self,
                                  ID3D12Resource* destination,
                                  ID3D12Resource* source)
{
    note_list(self, 17);
    if (self != nullptr && self->host != nullptr) {
        auto* host_destination = static_cast<ID3D12Resource*>(unwrap_any(destination));
        auto* host_source = static_cast<ID3D12Resource*>(unwrap_any(source));
        if (host_destination != nullptr && host_source != nullptr) {
            self->host->CopyResource(host_destination, host_source);
        }
    }
}

void __stdcall list_resolve_subresource(CommandListProxy* self,
                                        ID3D12Resource* destination,
                                        UINT destination_subresource,
                                        ID3D12Resource* source,
                                        UINT source_subresource,
                                        DXGI_FORMAT format)
{
    note_list(self, 19);
    if (self != nullptr && self->host != nullptr) {
        self->host->ResolveSubresource(static_cast<ID3D12Resource*>(unwrap_any(destination)),
                                       destination_subresource,
                                       static_cast<ID3D12Resource*>(unwrap_any(source)),
                                       source_subresource,
                                       format);
    }
}

void __stdcall list_set_primitive_topology(CommandListProxy* self,
                                           D3D12_PRIMITIVE_TOPOLOGY topology)
{
    note_list(self, 20);
    {
        static std::atomic<std::uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 12) {
            log::info("d3d12: topology " + std::to_string(static_cast<int>(topology)));
        }
    }
    switch (static_cast<int>(topology)) {
    case 6:
    case 17:
    case 18:
    case 19:
    case 20:
        topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
    default:
        break;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->IASetPrimitiveTopology(topology);
    }
}

void __stdcall list_set_viewports(CommandListProxy* self,
                                  UINT count,
                                  const D3D12_VIEWPORT* viewports)
{
    note_list(self, 21);
    {
        static std::atomic<std::uint32_t> logged{0};
        if (viewports != nullptr && count != 0
            && logged.fetch_add(1, std::memory_order_relaxed) < 12) {
            char text[200]{};
            std::snprintf(text, sizeof(text),
                          "d3d12: viewports %u first %.1f %.1f %.1f %.1f depth %.3f %.3f", count,
                          viewports[0].TopLeftX, viewports[0].TopLeftY, viewports[0].Width,
                          viewports[0].Height, viewports[0].MinDepth, viewports[0].MaxDepth);
            log::info(text);
        }
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->RSSetViewports(count, viewports);
    }
}

void __stdcall list_set_scissor_rects(CommandListProxy* self,
                                      UINT count,
                                      const D3D12_RECT* rects)
{
    note_list(self, 22);
    {
        static std::atomic<std::uint32_t> logged{0};
        if (rects != nullptr && count != 0
            && logged.fetch_add(1, std::memory_order_relaxed) < 12) {
            char text[160]{};
            std::snprintf(text, sizeof(text), "d3d12: scissors %u first %ld %ld %ld %ld", count,
                          rects[0].left, rects[0].top, rects[0].right, rects[0].bottom);
            log::info(text);
        }
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->RSSetScissorRects(count, rects);
    }
}

void __stdcall list_set_blend_factor(CommandListProxy* self, const FLOAT* factor)
{
    note_list(self, 23);
    if (self != nullptr && self->host != nullptr) {
        self->host->OMSetBlendFactor(factor);
    }
}

void __stdcall list_set_stencil_ref(CommandListProxy* self, UINT reference)
{
    note_list(self, 24);
    if (self != nullptr && self->host != nullptr) {
        self->host->OMSetStencilRef(reference);
    }
}

void __stdcall list_set_pipeline_state(CommandListProxy* self, ID3D12PipelineState* pipeline)
{
    note_list(self, 25);
    if (self == nullptr || self->host == nullptr) {
        return;
    }
    auto* host_pipeline = static_cast<ID3D12PipelineState*>(unwrap_any(pipeline));
    if (host_pipeline != nullptr && !is_host_pipeline(host_pipeline)) {
        host_pipeline = nullptr;
    }
    if (self->draw_state != nullptr) {
        static_cast<DrawState*>(self->draw_state)->pipeline = host_pipeline;
    }
    if (host_pipeline != nullptr) {
        self->host->SetPipelineState(host_pipeline);
    }
}

UINT __stdcall list_resource_barrier(CommandListProxy* self,
                                     UINT count,
                                     const D3D12_RESOURCE_BARRIER* barriers)
{
    note_list(self, 26);
    static const bool skip_barriers = GetEnvironmentVariableW(L"EUPHORIA_SKIP_BARRIERS", nullptr, 0) != 0;
    if (skip_barriers) {
        return 0;
    }
    if (self == nullptr || self->host == nullptr || (barriers == nullptr && count != 0)) {
        return 0;
    }
    std::vector<D3D12_RESOURCE_BARRIER> translated;
    const D3D12_RESOURCE_BARRIER* source = barriers;
    if (barriers != nullptr && count != 0) {
        {
            static std::atomic<std::uint32_t> dumped{0};
            if (dumped.fetch_add(1, std::memory_order_relaxed) < 12) {
                const auto* words = reinterpret_cast<const std::uint32_t*>(barriers);
                std::string text = "d3d12: ResourceBarrier count " + std::to_string(count) + ":";
                for (UINT index = 0; index < std::min<UINT>(count, 3) * 8; ++index) {
                    char word[16]{};
                    std::snprintf(word, sizeof(word), " %08x", words[index]);
                    text += word;
                }
                log::info(text);
            }
        }
        translated.assign(barriers, barriers + count);
        const auto translate = [](ID3D12Resource* resource) -> ID3D12Resource* {
            if (resource != nullptr && is_resource_proxy(resource)) {
                return unwrap_resource(resource);
            }
            return resource;
        };
        std::vector<D3D12_RESOURCE_BARRIER> kept;
        kept.reserve(translated.size());
        for (D3D12_RESOURCE_BARRIER& barrier : translated) {
            if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                continue;
            }
            ID3D12Resource* resource = translate(barrier.Transition.pResource);
            barrier.Transition.pResource = resource;
            constexpr std::uint32_t pc_states = 0x01FFFFFFu;
            const auto after = static_cast<D3D12_RESOURCE_STATES>(
                static_cast<std::uint32_t>(barrier.Transition.StateAfter) & pc_states);
            D3D12_RESOURCE_STATES before{};
            bool fixed = false;
            if (resource == nullptr) {
                barrier.Type = static_cast<D3D12_RESOURCE_BARRIER_TYPE>(-1);
                continue;
            }
            if (!tracked_resource_state(resource, before, fixed)) {
                before = static_cast<D3D12_RESOURCE_STATES>(
                    static_cast<std::uint32_t>(barrier.Transition.StateBefore) & pc_states);
            }
            if (fixed || before == after) {
                barrier.Type = static_cast<D3D12_RESOURCE_BARRIER_TYPE>(-1);
                continue;
            }
            {
                static std::atomic<std::uint32_t> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 80) {
                    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
                    char text[220]{};
                    std::snprintf(text, sizeof(text),
                                  "d3d12: transition %p dim %u %llux%u fmt %u flags 0x%x: "
                                  "title 0x%x->0x%x host 0x%x->0x%x sub %u",
                                  static_cast<void*>(resource), static_cast<unsigned>(desc.Dimension),
                                  static_cast<unsigned long long>(desc.Width), desc.Height,
                                  static_cast<unsigned>(desc.Format),
                                  static_cast<unsigned>(desc.Flags),
                                  static_cast<unsigned>(barrier.Transition.StateBefore),
                                  static_cast<unsigned>(barrier.Transition.StateAfter),
                                  static_cast<unsigned>(before), static_cast<unsigned>(after),
                                  barrier.Transition.Subresource);
                    log::info(text);
                }
            }
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            track_resource_state(resource, after);
        }
        for (D3D12_RESOURCE_BARRIER& barrier : translated) {
            switch (barrier.Type) {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
                kept.push_back(barrier);
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                barrier.Aliasing.pResourceBefore = translate(barrier.Aliasing.pResourceBefore);
                barrier.Aliasing.pResourceAfter = translate(barrier.Aliasing.pResourceAfter);
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                kept.push_back(barrier);
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_UAV:
                barrier.UAV.pResource = translate(barrier.UAV.pResource);
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                kept.push_back(barrier);
                break;
            default:
                break;
            }
        }
        if (kept.empty()) {
            return 0;
        }
        self->host->ResourceBarrier(static_cast<UINT>(kept.size()), kept.data());
        return 0;
    }
    (void)source;
    return 0;
}

void __stdcall list_set_descriptor_heaps(CommandListProxy* self,
                                         UINT count,
                                         ID3D12DescriptorHeap* const* heaps)
{
    note_list(self, 32);
    if (self == nullptr || self->host == nullptr || heaps == nullptr || count == 0) {
        return;
    }
    std::vector<ID3D12DescriptorHeap*> host_heaps(count, nullptr);
    for (UINT index = 0; index < count; ++index) {
        void* heap = heaps[index];
        if (heap != nullptr && is_descriptor_heap_proxy(heap)) {
            auto* proxy = static_cast<DescriptorHeapProxy*>(heap);
            ID3D12DescriptorHeap* mirror =
                console_host_heap(static_cast<std::uint64_t>(proxy->cpu_start.ptr));
            host_heaps[index] = mirror != nullptr ? mirror : proxy->host;
        } else {
            host_heaps[index] = static_cast<ID3D12DescriptorHeap*>(heap);
        }
    }
    self->host->SetDescriptorHeaps(count, host_heaps.data());
}

bool root_bound(const CommandListProxy* self, bool compute)
{
    if (self == nullptr) {
        return false;
    }
    const bool bound = compute ? self->compute_root != nullptr : self->graphics_root != nullptr;
    if (!bound) {
        static std::atomic<std::uint32_t> skipped{0};
        if (skipped.fetch_add(1, std::memory_order_relaxed) < 8) {
            log::warn(std::string("d3d12: root argument without a host ")
                      + (compute ? "compute" : "graphics") + " root signature skipped");
        }
    }
    return bound;
}

void __stdcall list_set_compute_root_signature(CommandListProxy* self,
                                               ID3D12RootSignature* root_signature)
{
    note_list(self, 33);
    if (self != nullptr && self->host != nullptr) {
        auto* host = static_cast<ID3D12RootSignature*>(unwrap_any(root_signature));
        self->compute_root = host;
        if (host != nullptr) {
            self->host->SetComputeRootSignature(host);
        }
    }
}

void __stdcall list_set_graphics_root_signature(CommandListProxy* self,
                                                ID3D12RootSignature* root_signature)
{
    note_list(self, 34);
    if (self != nullptr && self->host != nullptr) {
        auto* host = static_cast<ID3D12RootSignature*>(unwrap_any(root_signature));
        self->graphics_root = host;
        if (host != nullptr) {
            self->host->SetGraphicsRootSignature(host);
        }
    }
}

void __stdcall list_set_compute_root_descriptor_table(CommandListProxy* self,
                                                      UINT root_parameter_index,
                                                      D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
    note_list(self, 35);
    if (!root_bound(self, true)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        if (is_console_handle(handle.ptr)) {
            handle = resolve_console_gpu(handle.ptr);
        }
        self->host->SetComputeRootDescriptorTable(root_parameter_index, handle);
    }
}

void __stdcall list_set_graphics_root_descriptor_table(CommandListProxy* self,
                                                       UINT root_parameter_index,
                                                       D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
    note_list(self, 36);
    if (!root_bound(self, false)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        if (is_console_handle(handle.ptr)) {
            handle = resolve_console_gpu(handle.ptr);
        }
        self->host->SetGraphicsRootDescriptorTable(root_parameter_index, handle);
    }
}

void __stdcall list_set_compute_root_constant(CommandListProxy* self,
                                              UINT root_parameter_index,
                                              UINT value,
                                              UINT offset)
{
    note_list(self, 37);
    if (!root_bound(self, true)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetComputeRoot32BitConstant(root_parameter_index, value, offset);
    }
}

void __stdcall list_set_graphics_root_constant(CommandListProxy* self,
                                               UINT root_parameter_index,
                                               UINT value,
                                               UINT offset)
{
    note_list(self, 38);
    if (!root_bound(self, false)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetGraphicsRoot32BitConstant(root_parameter_index, value, offset);
    }
}

void __stdcall list_set_compute_root_constants(CommandListProxy* self,
                                               UINT root_parameter_index,
                                               UINT count,
                                               const void* values,
                                               UINT offset)
{
    note_list(self, 39);
    if (!root_bound(self, true)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetComputeRoot32BitConstants(root_parameter_index, count, values, offset);
    }
}

void __stdcall list_set_graphics_root_constants(CommandListProxy* self,
                                                UINT root_parameter_index,
                                                UINT count,
                                                const void* values,
                                                UINT offset)
{
    note_list(self, 40);
    if (!root_bound(self, false)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetGraphicsRoot32BitConstants(root_parameter_index, count, values, offset);
    }
}

void __stdcall list_set_compute_root_cbv(CommandListProxy* self,
                                         UINT root_parameter_index,
                                         D3D12_GPU_VIRTUAL_ADDRESS address)
{
    note_list(self, 41);
    if (!root_bound(self, true)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetComputeRootConstantBufferView(root_parameter_index,
                                              host_gpu_address(address, 65536));
    }
}

void __stdcall list_set_graphics_root_cbv(CommandListProxy* self,
                                          UINT root_parameter_index,
                                          D3D12_GPU_VIRTUAL_ADDRESS address)
{
    note_list(self, 42);
    if (!root_bound(self, false)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetGraphicsRootConstantBufferView(root_parameter_index,
                                              host_gpu_address(address, 65536));
    }
}

void __stdcall list_set_compute_root_srv(CommandListProxy* self,
                                         UINT root_parameter_index,
                                         D3D12_GPU_VIRTUAL_ADDRESS address)
{
    note_list(self, 43);
    if (!root_bound(self, true)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetComputeRootShaderResourceView(root_parameter_index,
                                              host_gpu_address(address, 1048576));
    }
}

void __stdcall list_set_graphics_root_srv(CommandListProxy* self,
                                          UINT root_parameter_index,
                                          D3D12_GPU_VIRTUAL_ADDRESS address)
{
    note_list(self, 44);
    if (!root_bound(self, false)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetGraphicsRootShaderResourceView(root_parameter_index,
                                              host_gpu_address(address, 1048576));
    }
}

void __stdcall list_set_compute_root_uav(CommandListProxy* self,
                                         UINT root_parameter_index,
                                         D3D12_GPU_VIRTUAL_ADDRESS address)
{
    note_list(self, 45);
    if (!root_bound(self, true)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetComputeRootUnorderedAccessView(root_parameter_index,
                                              host_gpu_address(address, 1048576));
    }
}

void __stdcall list_set_graphics_root_uav(CommandListProxy* self,
                                          UINT root_parameter_index,
                                          D3D12_GPU_VIRTUAL_ADDRESS address)
{
    note_list(self, 46);
    if (!root_bound(self, false)) {
        return;
    }
    if (self != nullptr && self->host != nullptr) {
        self->host->SetGraphicsRootUnorderedAccessView(root_parameter_index,
                                              host_gpu_address(address, 1048576));
    }
}

D3D12_GPU_VIRTUAL_ADDRESS fallback_buffer_address()
{
    static std::once_flag once;
    static ID3D12Resource* buffer = nullptr;
    std::call_once(once, [] {
        ID3D12Device* device = descriptor_device();
        if (device == nullptr) {
            return;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 65536;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&buffer)))) {
            buffer = nullptr;
            log::warn("d3d12: fallback root buffer creation failed");
        }
    });
    return buffer != nullptr ? buffer->GetGPUVirtualAddress() : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS host_gpu_address(std::uint64_t address, std::size_t size)
{
    if (address == 0) {
        return 0;
    }
    if (D3D12_GPU_VIRTUAL_ADDRESS host = 0; translate_gpu_address(address, host)) {
        return host;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0
        || info.State != MEM_COMMIT) {
        static std::atomic<std::uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            char text[96]{};
            std::snprintf(text, sizeof(text), "d3d12: gpu address 0x%llx is no title memory",
                          static_cast<unsigned long long>(address));
            log::warn(text);
        }
        return fallback_buffer_address();
    }
    const std::uint64_t end =
        reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    const std::size_t usable = static_cast<std::size_t>(
        std::min<std::uint64_t>(size, end - address));
    ID3D12Device* device = descriptor_device();
    if (device == nullptr || usable == 0) {
        return fallback_buffer_address();
    }
    const TitleShadowBuffer shadow = title_memory().create_shadow_buffer(device, address, usable);
    if (shadow.host == nullptr || shadow.gpu_address == 0) {
        return fallback_buffer_address();
    }
    return shadow.gpu_address;
}

void __stdcall list_set_index_buffer(CommandListProxy* self,
                                     const D3D12_INDEX_BUFFER_VIEW* view)
{
    note_list(self, 47);
    if (self != nullptr && self->host != nullptr) {
        if (view == nullptr) {
            self->host->IASetIndexBuffer(nullptr);
            return;
        }
        D3D12_INDEX_BUFFER_VIEW host = *view;
        host.BufferLocation = host_gpu_address(view->BufferLocation, view->SizeInBytes);
        if (host.BufferLocation == 0) {
            return;
        }
        self->host->IASetIndexBuffer(&host);
    }
}

void __stdcall list_set_vertex_buffers(CommandListProxy* self,
                                       UINT start_slot,
                                       UINT count,
                                       const D3D12_VERTEX_BUFFER_VIEW* views)
{
    note_list(self, 48);
    if (self != nullptr && self->host != nullptr) {
        if (views == nullptr || count == 0) {
            self->host->IASetVertexBuffers(start_slot, count, views);
            return;
        }
        std::vector<D3D12_VERTEX_BUFFER_VIEW> host(views, views + count);
        for (D3D12_VERTEX_BUFFER_VIEW& view : host) {
            if (view.BufferLocation != 0) {
                view.BufferLocation = host_gpu_address(view.BufferLocation, view.SizeInBytes);
                if (view.BufferLocation == 0) {
                    view.SizeInBytes = 0;
                }
            }
        }
        self->host->IASetVertexBuffers(start_slot, count, host.data());
    }
}

void require_view_state(CommandListProxy* self, std::uint64_t handle,
                        D3D12_RESOURCE_STATES required)
{
    if (self == nullptr || self->host == nullptr || handle == 0) {
        return;
    }
    ID3D12Resource* resource = console_view_resource(handle);
    if (resource == nullptr) {
        return;
    }
    D3D12_RESOURCE_STATES current{};
    bool fixed = false;
    if (!tracked_resource_state(resource, current, fixed) || fixed) {
        return;
    }
    if (required == D3D12_RESOURCE_STATE_DEPTH_WRITE
        && current == D3D12_RESOURCE_STATE_DEPTH_READ) {
        return;
    }
    if (current == required) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = current;
    barrier.Transition.StateAfter = required;
    self->host->ResourceBarrier(1, &barrier);
    track_resource_state(resource, required);
}

D3D12_CPU_DESCRIPTOR_HANDLE null_render_target()
{
    static std::once_flag once;
    static ID3D12DescriptorHeap* heap = nullptr;
    static D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    std::call_once(once, [] {
        ID3D12Device* device = descriptor_device();
        if (device == nullptr) {
            return;
        }
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) {
            heap = nullptr;
            return;
        }
        handle = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(nullptr, &view, handle);
    });
    return handle;
}

void __stdcall list_set_render_targets(CommandListProxy* self,
                                       UINT count,
                                       const UINT64* handles,
                                       INT one_range,
                                       const UINT64* depth)
{
    note_list(self, 50);
    if (self == nullptr || self->host == nullptr) {
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    const UINT used = std::min(count, k_render_target_limit);
    if (handles != nullptr) {
        const UINT64 first = handles[0];
        for (UINT index = 0; index < used; ++index) {
            const UINT64 handle = one_range != 0 ? first + k_render_target_stride * index
                                                 : handles[index];
            ID3D12Resource* resource =
                is_console_handle(handle) ? console_view_resource(handle) : nullptr;
            if (resource != nullptr
                && (resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) {
                require_view_state(self, handle, D3D12_RESOURCE_STATE_RENDER_TARGET);
                targets[index] = resolve_console_descriptor(handle);
            }
            if (targets[index].ptr == 0) {
                targets[index] = null_render_target();
                static std::atomic<std::uint32_t> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                    char text[96]{};
                    std::snprintf(text, sizeof(text),
                                  "d3d12: render target 0x%llx unusable, null view bound",
                                  static_cast<unsigned long long>(handle));
                    log::warn(text);
                }
            }
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE depth_target{};
    D3D12_CPU_DESCRIPTOR_HANDLE* depth_pointer = nullptr;
    if (depth != nullptr && *depth != 0 && is_console_handle(*depth)) {
        ID3D12Resource* resource = console_view_resource(*depth);
        if (resource != nullptr
            && (resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
            require_view_state(self, *depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            depth_target = resolve_console_descriptor(*depth);
            if (depth_target.ptr != 0) {
                depth_pointer = &depth_target;
            }
        }
    }
    for (UINT index = 0; index < used; ++index) {
        if (targets[index].ptr == 0) {
            return;
        }
    }
    self->host->OMSetRenderTargets(used, targets, FALSE, depth_pointer);
}

void __stdcall list_clear_depth_stencil_view(CommandListProxy* self,
                                             D3D12_CPU_DESCRIPTOR_HANDLE view,
                                             D3D12_CLEAR_FLAGS flags,
                                             FLOAT depth,
                                             UINT8 stencil,
                                             UINT rect_count,
                                             const D3D12_RECT* rects)
{
    note_list(self, 51);
    if (self != nullptr && self->host != nullptr) {
        if (is_console_handle(static_cast<std::uint64_t>(view.ptr))) {
            require_view_state(self, static_cast<std::uint64_t>(view.ptr),
                               D3D12_RESOURCE_STATE_DEPTH_WRITE);
            view = resolve_console_descriptor(static_cast<std::uint64_t>(view.ptr));
        } else {
            return;
        }
        if (view.ptr == 0) {
            return;
        }
        self->host->ClearDepthStencilView(view, flags, depth, stencil, rect_count, rects);
    }
}

void __stdcall list_clear_render_target_view(CommandListProxy* self,
                                             D3D12_CPU_DESCRIPTOR_HANDLE view,
                                             const FLOAT* color,
                                             UINT rect_count,
                                             const D3D12_RECT* rects)
{
    note_list(self, 52);
    static const bool skip_clears = GetEnvironmentVariableW(L"EUPHORIA_SKIP_CLEARS", nullptr, 0) != 0;
    if (skip_clears) {
        return;
    }
    if (self != nullptr && self->host != nullptr && color != nullptr) {
        if (is_console_handle(static_cast<std::uint64_t>(view.ptr))) {
            ID3D12Resource* target = console_view_resource(static_cast<std::uint64_t>(view.ptr));
            if (target != nullptr) {
                const D3D12_RESOURCE_DESC desc = target->GetDesc();
                D3D12_RESOURCE_STATES state{};
                bool fixed = false;
                const bool tracked = tracked_resource_state(target, state, fixed);
                static std::atomic<std::uint32_t> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 24) {
                    char text[200]{};
                    std::snprintf(text, sizeof(text),
                                  "d3d12: clear rtv %p resource %p %llux%u format %u flags 0x%x "
                                  "state 0x%x tracked %d",
                                  reinterpret_cast<void*>(view.ptr), static_cast<void*>(target),
                                  static_cast<unsigned long long>(desc.Width), desc.Height,
                                  static_cast<unsigned>(desc.Format),
                                  static_cast<unsigned>(desc.Flags),
                                  static_cast<unsigned>(state), tracked ? 1 : 0);
                    log::info(text);
                }
                if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
                    return;
                }
            }
            require_view_state(self, static_cast<std::uint64_t>(view.ptr),
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
            view = resolve_console_descriptor(static_cast<std::uint64_t>(view.ptr));
        } else {
            return;
        }
        if (view.ptr == 0) {
            return;
        }
        self->host->ClearRenderTargetView(view, color, rect_count, rects);
    }
}

void __stdcall list_execute_indirect(CommandListProxy* self,
                                     ID3D12CommandSignature* command_signature,
                                     UINT max_command_count,
                                     ID3D12Resource* argument_buffer,
                                     UINT64 argument_buffer_offset,
                                     ID3D12Resource* count_buffer,
                                     UINT64 count_buffer_offset)
{
    note_list(self, 59);
    auto* host_signature = static_cast<ID3D12CommandSignature*>(unwrap_any(command_signature));
    auto* host_arguments = static_cast<ID3D12Resource*>(unwrap_any(argument_buffer));
    if (self != nullptr && self->host != nullptr && host_signature != nullptr
        && host_arguments != nullptr && pipeline_ready(self)) {
        self->host->ExecuteIndirect(host_signature,
                                    max_command_count,
                                    host_arguments,
                                    argument_buffer_offset,
                                    static_cast<ID3D12Resource*>(unwrap_any(count_buffer)),
                                    count_buffer_offset);
    }
}

void __stdcall list_dispatch_mesh(CommandListProxy* self,
                                  UINT thread_group_count_x,
                                  UINT thread_group_count_y,
                                  UINT thread_group_count_z)
{
    note_list(self, 127);
    if (self == nullptr || self->host == nullptr || !pipeline_ready(self)) {
        return;
    }
    ID3D12GraphicsCommandList7* list7 = nullptr;
    if (SUCCEEDED(self->host->QueryInterface(IID_PPV_ARGS(&list7))) && list7 != nullptr) {
        list7->DispatchMesh(thread_group_count_x,
                            thread_group_count_y,
                            thread_group_count_z);
        list7->Release();
        return;
    }
    log::warn("d3d12: DispatchMesh needs an ID3D12GraphicsCommandList7 host");
}

template <typename Function>
void set_slot(void** table, std::size_t slot, Function function)
{
    table[slot] = reinterpret_cast<void*>(function);
}

template <std::size_t Slot>
HRESULT __stdcall queue_missing(CommandQueueProxy* self)
{
    note_queue_slot(self, Slot, false);
    return k_not_implemented;
}

template <std::size_t Slot>
HRESULT __stdcall list_missing(CommandListProxy* self)
{
    note_list_slot(self, Slot, false);
    return k_not_implemented;
}

template <std::size_t... Slots>
void fill_queue_missing(void** table, std::index_sequence<Slots...>)
{
    (set_slot(table, Slots, &queue_missing<Slots>), ...);
}

template <std::size_t... Slots>
void fill_list_missing(void** table, std::index_sequence<Slots...>)
{
    (set_slot(table, Slots, &list_missing<Slots>), ...);
}

void** build_queue_table(CommandLayout layout)
{
    void** table = new void*[command_queue_slot_count]{};
    fill_queue_missing(table, std::make_index_sequence<command_queue_slot_count>{});
    const auto slot = [layout](std::size_t legacy_slot) {
        return queue_slot(layout, legacy_slot);
    };

    set_slot(table, 0, &queue_query_interface);
    set_slot(table, 1, &queue_add_ref);
    set_slot(table, 2, &queue_release);
    set_slot(table, 3, &queue_get_private_data);
    set_slot(table, 4, &queue_set_private_data);
    set_slot(table, 5, &queue_set_private_data_interface);
    set_slot(table, 6, &queue_set_name);
    set_slot(table, 7, &queue_get_device);
    set_slot(table, slot(10), &queue_execute_command_lists);
    set_slot(table, slot(11), &queue_set_marker);
    set_slot(table, slot(12), &queue_begin_event);
    set_slot(table, slot(13), &queue_end_event);
    set_slot(table, slot(14), &queue_signal);
    set_slot(table, slot(15), &queue_wait);
    set_slot(table, slot(16), &queue_get_timestamp_frequency);
    set_slot(table, slot(17), &queue_get_clock_calibration);
    set_slot(table, slot(26), &queue_get_desc);
    set_slot(table, slot(29), &queue_copy_page_mappings);
    set_slot(table, slot(30), &queue_present_x);
    set_slot(table, slot(32), &queue_signal2_x);
    set_slot(table, slot(33), &queue_execute_command_lists_x);
    return table;
}

void** build_list_table(CommandLayout layout)
{
    void** table = new void*[command_list_slot_count]{};
    fill_list_missing(table, std::make_index_sequence<command_list_slot_count>{});
    const auto slot = [layout](std::size_t legacy_slot) {
        return list_slot(layout, legacy_slot);
    };

    set_slot(table, 0, &list_query_interface);
    set_slot(table, 1, &list_add_ref);
    set_slot(table, 2, &list_release);
    if (layout == CommandLayout::Newer && command_list_slot_count > 139) {
        set_slot(table, 139, &list_reset_x);
    }
    set_slot(table, 3, &list_get_private_data);
    set_slot(table, 4, &list_set_private_data);
    set_slot(table, 5, &list_set_private_data_interface);
    set_slot(table, 6, &list_set_name);
    set_slot(table, 7, &list_get_device);
    set_slot(table, 8, &list_get_type);
    set_slot(table, 9, &list_close);
    set_slot(table, 10, &list_reset);
    set_slot(table, 12, &list_draw_instanced);
    set_slot(table, 13, &list_draw_indexed_instanced);
    set_slot(table, 14, &list_dispatch);
    set_slot(table, 15, &list_copy_buffer_region);
    set_slot(table, 16, &list_copy_texture_region);
    set_slot(table, 17, &list_copy_resource);
    set_slot(table, 19, &list_resolve_subresource);
    set_slot(table, 20, &list_set_primitive_topology);
    set_slot(table, 21, &list_set_viewports);
    set_slot(table, 22, &list_set_scissor_rects);
    set_slot(table, 23, &list_set_blend_factor);
    set_slot(table, 24, &list_set_stencil_ref);
    set_slot(table, 25, &list_set_pipeline_state);
    set_slot(table, 26, &list_resource_barrier);
    set_slot(table, 32, &list_set_descriptor_heaps);
    set_slot(table, 33, &list_set_compute_root_signature);
    set_slot(table, 34, &list_set_graphics_root_signature);
    set_slot(table, 35, &list_set_compute_root_descriptor_table);
    set_slot(table, 36, &list_set_graphics_root_descriptor_table);
    set_slot(table, 37, &list_set_compute_root_constant);
    set_slot(table, 38, &list_set_graphics_root_constant);
    set_slot(table, 39, &list_set_compute_root_constants);
    set_slot(table, 40, &list_set_graphics_root_constants);
    set_slot(table, 41, &list_set_compute_root_cbv);
    set_slot(table, 42, &list_set_graphics_root_cbv);
    set_slot(table, 43, &list_set_compute_root_srv);
    set_slot(table, 44, &list_set_graphics_root_srv);
    set_slot(table, 45, &list_set_compute_root_uav);
    set_slot(table, 46, &list_set_graphics_root_uav);
    set_slot(table, 47, &list_set_index_buffer);
    set_slot(table, 48, &list_set_vertex_buffers);
    set_slot(table, 50, &list_set_render_targets);
    set_slot(table, 51, &list_clear_depth_stencil_view);
    set_slot(table, 52, &list_clear_render_target_view);
    set_slot(table, slot(59), &list_execute_indirect);
    set_slot(table, slot(127), &list_dispatch_mesh);
    set_slot(table, 28, &list_ignored_state);
    set_slot(table, 29, &list_ignored_state);
    set_slot(table, slot(68), &list_depth_bounds);
    set_slot(table, slot(69), &list_write32);
    set_slot(table, slot(70), &list_write64);
    set_slot(table, slot(71), &list_wait_value);
    set_slot(table, slot(72), &list_wait_value);
    set_slot(table, slot(73), &list_ignored_state);
    set_slot(table, slot(99), &list_ignored_state);
    set_slot(table, slot(124), &list_ignored_state);
    set_slot(table, slot(129), &list_sync);
    set_slot(table, slot(130), &list_sync);
    if (layout == CommandLayout::Newer) {
        set_slot(table, 142, &list_ignored_state);
        set_slot(table, 146, &list_barrier_groups);
        set_slot(table, 148, &list_allocate_upload);
    } else {
        set_slot(table, 137, &list_reset_x);
    }
    {
        wchar_t buffer[512]{};
        const DWORD length =
            GetEnvironmentVariableW(L"EUPHORIA_SKIP_LIST_SLOTS", buffer, std::size(buffer));
        if (length != 0 && length < std::size(buffer)) {
            std::size_t value = 0;
            bool digits = false;
            for (DWORD index = 0; index <= length; ++index) {
                const wchar_t ch = buffer[index];
                if (ch >= L'0' && ch <= L'9') {
                    value = value * 10 + static_cast<std::size_t>(ch - L'0');
                    digits = true;
                } else {
                    if (digits && value < command_list_slot_count) {
                        set_slot(table, value, &list_ignored_state);
                        log::warn("d3d12: list slot " + std::to_string(value)
                                  + " disabled for diagnosis");
                    }
                    value = 0;
                    digits = false;
                }
            }
        }
    }
    return table;
}

void** queue_table(CommandLayout layout)
{
    static void** legacy = build_queue_table(CommandLayout::Legacy);
    static void** newer = build_queue_table(CommandLayout::Newer);
    return layout == CommandLayout::Newer ? newer : legacy;
}

void** list_table(CommandLayout layout)
{
    static void** legacy = build_list_table(CommandLayout::Legacy);
    static void** newer = build_list_table(CommandLayout::Newer);
    return layout == CommandLayout::Newer ? newer : legacy;
}

}

CommandQueueProxy* create_command_queue(ID3D12CommandQueue* host,
                                        ID3D12Device* device,
                                        void* owner_device,
                                        CommandLayout layout)
{
    auto* proxy = new (std::nothrow) CommandQueueProxy{};
    if (proxy == nullptr) {
        log::warn("d3d12: out of memory for a command queue proxy");
        return nullptr;
    }
    for (std::atomic<std::uint32_t>& counter : proxy->slot_calls) {
        counter.store(0, std::memory_order_relaxed);
    }
    proxy->references.store(1, std::memory_order_relaxed);
    proxy->vtable = queue_table(layout);
    proxy->host = host;
    proxy->device = device;
    proxy->owner_device = owner_device;
    return proxy;
}

CommandListProxy* create_command_list(ID3D12GraphicsCommandList* host,
                                      void* allocator,
                                      void* device,
                                      CommandLayout layout)
{
    auto* proxy = new (std::nothrow) CommandListProxy{};
    if (proxy == nullptr) {
        log::warn("d3d12: out of memory for a command list proxy");
        return nullptr;
    }
    for (std::atomic<std::uint32_t>& counter : proxy->slot_calls) {
        counter.store(0, std::memory_order_relaxed);
    }
    proxy->references.store(1, std::memory_order_relaxed);
    proxy->vtable = list_table(layout);
    proxy->host = host;
    proxy->allocator = allocator;
    proxy->device = device;
    auto* state = new (std::nothrow) DrawState{};
    if (state != nullptr) {
        state->layout = layout;
    }
    proxy->draw_state = state;
    return proxy;
}

bool is_command_queue_proxy(void* object) noexcept
{
    if (object == nullptr) {
        return false;
    }
    void** table = *reinterpret_cast<void***>(object);
    return table == queue_table(CommandLayout::Legacy)
        || table == queue_table(CommandLayout::Newer);
}

bool is_command_list_proxy(void* object) noexcept
{
    if (object == nullptr) {
        return false;
    }
    void** table = *reinterpret_cast<void***>(object);
    return table == list_table(CommandLayout::Legacy)
        || table == list_table(CommandLayout::Newer);
}

ID3D12CommandQueue* unwrap_command_queue(void* object) noexcept
{
    if (object == nullptr) {
        return nullptr;
    }
    if (is_command_queue_proxy(object)) {
        return static_cast<CommandQueueProxy*>(object)->host;
    }
    return static_cast<ID3D12CommandQueue*>(object);
}

ID3D12GraphicsCommandList* unwrap_command_list(void* object) noexcept
{
    if (object == nullptr) {
        return nullptr;
    }
    if (is_command_list_proxy(object)) {
        return static_cast<CommandListProxy*>(object)->host;
    }
    return static_cast<ID3D12GraphicsCommandList*>(object);
}

}
