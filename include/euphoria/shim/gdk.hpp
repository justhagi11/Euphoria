#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace euphoria::shim::gdk {

using HResult = std::int32_t;

inline constexpr HResult kOk = 0;
inline constexpr HResult kFalse = 1;
inline constexpr HResult kPending = static_cast<HResult>(0x8000000Au);
inline constexpr HResult kAbort = static_cast<HResult>(0x80004004u);
inline constexpr HResult kNotImplemented = static_cast<HResult>(0x80004001u);
inline constexpr HResult kNoInterface = static_cast<HResult>(0x80004002u);
inline constexpr HResult kFail = static_cast<HResult>(0x80004005u);
inline constexpr HResult kInvalidArgument = static_cast<HResult>(0x80070057u);
inline constexpr HResult kNotSufficientBuffer = static_cast<HResult>(0x8007007Au);
inline constexpr HResult kIllegalMethodCall = static_cast<HResult>(0x8000000Eu);
inline constexpr HResult kNotSupported = static_cast<HResult>(0x80070032u);
inline constexpr HResult kNetworkUnreachable = static_cast<HResult>(0x800704CFu);
inline constexpr HResult kUserNotFound = static_cast<HResult>(0x89245104u);
inline constexpr HResult kFailedToGetToken = static_cast<HResult>(0x89245111u);
inline constexpr HResult kNoPackage = static_cast<HResult>(0x89245208u);
inline constexpr HResult kStoreNetworkError = static_cast<HResult>(0x89245301u);

struct Guid {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t data4[8];

    friend bool operator==(const Guid&, const Guid&) = default;
};

std::string to_string(const Guid& guid);

struct AsyncBlock;
using CompletionRoutine = void (*)(AsyncBlock*);

struct AsyncBlock {
    void* queue;
    void* context;
    CompletionRoutine callback;
    std::uintptr_t internal[4];
};

struct ProviderData {
    AsyncBlock* block;
    std::size_t buffer_size;
    void* buffer;
    void* context;
};

using AsyncProvider = HResult (*)(std::uint32_t, const ProviderData*);
using AsyncWork = HResult (*)(AsyncBlock*);
using TaskCallback = void (*)(void*, bool);

inline constexpr std::uint32_t kOpBegin = 0;
inline constexpr std::uint32_t kOpDoWork = 1;
inline constexpr std::uint32_t kOpGetResult = 2;
inline constexpr std::uint32_t kOpCancel = 3;
inline constexpr std::uint32_t kOpCleanup = 4;

inline constexpr std::uint32_t kPortWork = 0;
inline constexpr std::uint32_t kPortCompletion = 1;

struct Object;
using Method = HResult (*)(const Object*);

struct Object {
    const Method* vtable;
    const char* area;
};

struct Interface {
    Guid provider;
    Guid iid;
    const Object* object;
};

template <std::size_t N>
using Table = std::array<Method, N>;

HResult missing_slot(const Object* self, std::size_t slot);
std::uint32_t add_ref(const Object* self);

template <std::size_t Slot>
HResult missing(const Object* self)
{
    return missing_slot(self, Slot);
}

template <std::size_t... Slots>
constexpr Table<sizeof...(Slots)> missing_table(std::index_sequence<Slots...>)
{
    return Table<sizeof...(Slots)>{&missing<Slots>...};
}

template <std::size_t N, class Function>
void set(Table<N>& table, std::size_t offset, Function function)
{
    table[offset / 8] = reinterpret_cast<Method>(function);
}

template <std::size_t N>
Table<N> table()
{
    Table<N> result = missing_table(std::make_index_sequence<N>{});
    set(result, 0x08, &add_ref);
    set(result, 0x10, &add_ref);
    return result;
}

template <class T>
HResult put(T* out, T value)
{
    if (out == nullptr) {
        return kInvalidArgument;
    }
    *out = value;
    return kOk;
}

HResult put_string(std::string_view text, std::size_t size, char* buffer, std::size_t* used);
std::string text(const char* value);
std::uint64_t next_token();
void log_first(std::string_view api);

template <class T>
std::vector<std::uint8_t> bytes_of(const T& value)
{
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    return std::vector<std::uint8_t>(begin, begin + sizeof(T));
}

struct Title {
    std::uint32_t id = 0;
    std::string store_id;
    std::filesystem::path data;
    std::filesystem::path package;
    std::uint64_t storage_bytes = 0;
};

const Title& title();
std::string package_identifier();
std::uint64_t disk_free(const std::filesystem::path& directory);

std::span<const Interface> task_queue_interfaces();
std::span<const Interface> user_interfaces();
std::span<const Interface> system_interfaces();
std::span<const Interface> game_interfaces();
std::span<const Interface> storage_interfaces();
std::span<const Interface> networking_interfaces();
std::span<const Interface> package_interfaces();
std::span<const Interface> display_interfaces();
std::span<const Interface> game_save_interfaces();
std::span<const Interface> store_interfaces();
std::span<const Interface> game_ui_interfaces();
std::span<const Interface> speech_interfaces();

HResult query_api_impl(const Guid* provider, const Guid* iid, void** out);
void* pseudo_module(std::string_view name);
bool is_pseudo_module(const void* handle) noexcept;
void* pseudo_export(const void* handle, const char* name);

HResult task_queue_create(std::uint32_t work, std::uint32_t completion, void** out);
HResult task_queue_get_port(void* queue, std::uint32_t port, void** out);
HResult task_queue_create_composite(void* work, void* completion, void** out);
HResult task_queue_duplicate(void* queue, void** out);
void task_queue_close(void* queue);
bool task_queue_dispatch(void* queue, std::uint32_t port, std::uint32_t timeout_ms);
HResult task_queue_submit(void* queue, std::uint32_t port, std::uint32_t delay_ms, void* context,
                          TaskCallback callback);
HResult task_queue_terminate(void* queue, bool wait, void* context, void (*callback)(void*));
HResult task_queue_register_monitor(void* queue, void* context,
                                    void (*callback)(void*, void*, std::uint32_t),
                                    std::uint64_t* token);
void task_queue_unregister_monitor(void* queue, std::uint64_t token);
bool task_queue_get_process_queue(void** out);
void task_queue_set_process_queue(void* queue);

HResult async_begin(AsyncBlock* block, void* context, const void* identity, const char* name,
                    AsyncProvider provider);
HResult async_schedule(AsyncBlock* block, std::uint32_t delay_ms);
void async_complete(AsyncBlock* block, HResult result, std::size_t required_size);
HResult async_get_status(AsyncBlock* block, bool wait);
HResult async_get_result_size(AsyncBlock* block, std::size_t* out);
void async_cancel(AsyncBlock* block);
HResult async_get_result(AsyncBlock* block, const void* identity, std::size_t size, void* buffer,
                         std::size_t* used);
HResult async_run(AsyncBlock* block, AsyncWork work);
HResult async_finished(AsyncBlock* block, const void* identity, HResult result);
HResult async_finished_with(AsyncBlock* block, const void* identity, std::vector<std::uint8_t> data);

template <class T>
HResult async_result(AsyncBlock* block, const void* identity, T* out)
{
    return async_get_result(block, identity, sizeof(T), out, nullptr);
}

inline constexpr std::uint64_t kXuid = 0x0009000000000001ull;
inline constexpr std::uint64_t kLocalUserId = 1;

HResult game_input_create(void** out);

int self_test(std::string& report);

}
