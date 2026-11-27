#include <euphoria/shim/dstorage.hpp>

#include <euphoria/formats/bcpack.hpp>
#include <euphoria/formats/inflate.hpp>
#include <euphoria/log/log.hpp>
#include <euphoria/shim/kernel.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

namespace euphoria::shim::dstorage {
namespace {

constexpr std::int32_t kOk = 0;
constexpr std::int32_t kPending = static_cast<std::int32_t>(0x8000000A);
constexpr std::int32_t kNotImplemented = static_cast<std::int32_t>(0x80004001);
constexpr std::int32_t kNoInterface = static_cast<std::int32_t>(0x80004002);
constexpr std::int32_t kInvalidArgument = static_cast<std::int32_t>(0x80070057);
constexpr std::int32_t kOutOfMemory = static_cast<std::int32_t>(0x8007000E);
constexpr std::int32_t kEndOfFile = static_cast<std::int32_t>(0x80070026);
constexpr std::int32_t kInvalidData = static_cast<std::int32_t>(0x8007000D);

constexpr GUID kFactoryIid{0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID kUnknownIid{0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr std::uint64_t kZlib = 1;
constexpr std::uint64_t kBcpack = 0xfull << 1;
constexpr std::uint64_t kPageDestination = 1ull << 21;
constexpr std::uint64_t kMemorySource = 1ull << 22;
constexpr std::uint64_t kKnownOptions = kZlib | kBcpack | kPageDestination | kMemorySource;

constexpr std::size_t kPage = 64 * 1024;
constexpr std::uint64_t kDefaultStaging = 32ull << 20;
constexpr std::array<std::uint64_t, 3> kCost{100, 10, 1};
constexpr std::uint32_t kLoggedRequests = 64;
constexpr std::uint32_t kLoggedSubmits = 32;

struct Request {
    std::uint64_t options;
    void* destination;
    std::uint64_t page_offset;
    std::uint32_t destination_size;
    const void* source;
    std::uint64_t offset;
    std::uint32_t source_size;
    std::uint32_t intermediate_size;
    std::uint64_t tag;
    const char* name;
};

static_assert(sizeof(Request) == 0x48);
static_assert(offsetof(Request, options) == 0x00);
static_assert(offsetof(Request, destination) == 0x08);
static_assert(offsetof(Request, page_offset) == 0x10);
static_assert(offsetof(Request, destination_size) == 0x18);
static_assert(offsetof(Request, source) == 0x20);
static_assert(offsetof(Request, offset) == 0x28);
static_assert(offsetof(Request, source_size) == 0x30);
static_assert(offsetof(Request, intermediate_size) == 0x34);
static_assert(offsetof(Request, tag) == 0x38);
static_assert(offsetof(Request, name) == 0x40);

struct QueueDesc {
    std::uint64_t source_type;
    std::uint16_t capacity;
    std::int8_t priority;
    const char* name;
    std::uint64_t unknown;
};

static_assert(sizeof(QueueDesc) == 0x20);
static_assert(offsetof(QueueDesc, capacity) == 0x08);
static_assert(offsetof(QueueDesc, priority) == 0x0a);
static_assert(offsetof(QueueDesc, name) == 0x10);
static_assert(offsetof(QueueDesc, unknown) == 0x18);

struct FactoryObject {
    void* const* vtable;
};

struct FileObject {
    void* const* vtable;
    std::atomic<std::uint32_t> refs{1};
    HANDLE handle{INVALID_HANDLE_VALUE};
    std::string path;
};

struct StatusArray {
    void* const* vtable;
    std::atomic<std::uint32_t> refs{1};
    std::uint32_t capacity{};
    std::unique_ptr<std::atomic<std::int32_t>[]> entries;
    std::string name;
};

struct QueueObject {
    void* const* vtable;
    std::atomic<std::uint32_t> refs{1};
    std::uint64_t id{};
    std::string name;
};

std::string hresult_text(std::int32_t hr)
{
    return std::format("{:#010x}", static_cast<std::uint32_t>(hr));
}

std::int32_t from_win32(DWORD error)
{
    if (error == 0) {
        return kInvalidArgument;
    }
    return static_cast<std::int32_t>(0x80070000u | (error & 0xffffu));
}

std::string guid_text(const GUID* iid)
{
    if (iid == nullptr) {
        return "null";
    }
    return std::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", iid->Data1,
                       iid->Data2, iid->Data3, iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3],
                       iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);
}

std::string narrow(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(length, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
                        nullptr);
    return result;
}

std::string text_of(const char* name)
{
    return name == nullptr ? std::string{} : std::string{name};
}

enum class Area { Factory, File, Status, Queue };

constexpr std::string_view area_name(Area area)
{
    switch (area) {
    case Area::Factory:
        return "factory";
    case Area::File:
        return "file";
    case Area::Status:
        return "status array";
    default:
        return "queue";
    }
}

template <Area A, std::size_t Slot>
std::int32_t missing(void*)
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        log::warn(std::format("dstorage: {} slot {:#x} not implemented", area_name(A), Slot * 8));
    }
    return kNotImplemented;
}

template <Area A, std::size_t... Slots>
std::array<void*, sizeof...(Slots)> missing_table(std::index_sequence<Slots...>)
{
    return {reinterpret_cast<void*>(&missing<A, Slots>)...};
}

template <Area A, std::size_t N>
std::array<void*, N> missing_table()
{
    return missing_table<A>(std::make_index_sequence<N>{});
}

std::uint32_t static_add_ref(void*)
{
    return 1;
}

template <typename T>
std::uint32_t add_ref_object(T* self)
{
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

void destroy(FileObject* file)
{
    if (file->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file->handle);
    }
    delete file;
}

void destroy(StatusArray* array)
{
    delete array;
}

void destroy(QueueObject* queue);

template <typename T>
std::uint32_t release_object(T* self)
{
    const std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        destroy(self);
    }
    return left;
}

const std::array<void*, 8>& file_vtable();
const std::array<void*, 5>& status_vtable();
const std::array<void*, 12>& queue_vtable();

struct Status {
    StatusArray* array{};
    std::uint32_t index{};
};

void release_status(Status& status)
{
    if (status.array != nullptr) {
        release_object(status.array);
        status.array = nullptr;
    }
}

struct Command {
    bool read{};
    Request request{};
    Status status{};
};

void release_command(Command& command)
{
    if (!command.read) {
        release_status(command.status);
    }
}

struct Lane {
    std::size_t klass{};
    std::size_t capacity{};
    std::uint64_t clock{};
    std::string name;
    std::vector<Command> unsubmitted;
    std::vector<Command> held;
    std::deque<std::pair<std::uint64_t, Request>> reads;
    std::deque<std::pair<std::uint64_t, Status>> statuses;
    std::set<std::uint64_t> running;
    std::map<std::uint64_t, std::int32_t> failures;
    bool closed{};
    std::uint64_t submits{};

    std::size_t entries() const noexcept
    {
        return unsubmitted.size() + held.size() + reads.size() + statuses.size() + running.size();
    }

    void flush()
    {
        while (!statuses.empty()) {
            const std::uint64_t seq = statuses.front().first;
            if (!reads.empty() && reads.front().first < seq) {
                return;
            }
            if (!running.empty() && *running.begin() < seq) {
                return;
            }
            Status status = statuses.front().second;
            statuses.pop_front();
            std::int32_t result = kOk;
            if (!failures.empty() && failures.begin()->first < seq) {
                result = failures.begin()->second;
            }
            failures.erase(failures.begin(), failures.lower_bound(seq));
            status.array->entries[status.index].store(result, std::memory_order_release);
            release_status(status);
        }
    }

    void drop_all()
    {
        for (auto& command : unsubmitted) {
            release_command(command);
        }
        for (auto& command : held) {
            release_command(command);
        }
        for (auto& [seq, status] : statuses) {
            release_status(status);
        }
        unsubmitted.clear();
        held.clear();
        reads.clear();
        statuses.clear();
    }
};

struct Job {
    std::uint64_t lane{};
    std::uint64_t seq{};
    Request request{};
    std::uint64_t staging{};
};

std::uint64_t staging_bytes(const Request& request) noexcept
{
    const bool buffered = (request.options & (kZlib | kBcpack | kPageDestination)) != 0;
    if (buffered && (request.options & kMemorySource) == 0) {
        return request.source_size;
    }
    return 0;
}

struct Stats {
    std::uint64_t requests{};
    std::uint64_t bytes{};
    std::uint64_t failures{};
    std::uint64_t bcpack_bytes{};
    std::uint64_t refused{};
    std::chrono::steady_clock::duration busy{};
    std::optional<std::chrono::steady_clock::time_point> since;
};

class Scheduler {
public:
    std::mutex mutex;
    std::condition_variable work;
    std::condition_variable finished;
    std::unordered_map<std::uint64_t, Lane> lanes;
    std::uint64_t next_id{};
    std::uint64_t next_seq{};
    std::uint64_t staged{};
    std::uint64_t staging{kDefaultStaging};
    bool starved{};
    std::array<std::uint64_t, 3> clock{};
    std::uint64_t now{};
    std::array<std::uint64_t, 3> lane_now{};
    Stats stats;

    std::optional<Job> start()
    {
        std::optional<std::pair<std::uint64_t, std::size_t>> best_class;
        for (const auto& [id, lane] : lanes) {
            if (lane.reads.empty()) {
                continue;
            }
            const std::pair<std::uint64_t, std::size_t> key{clock[lane.klass], lane.klass};
            if (!best_class || key < *best_class) {
                best_class = key;
            }
        }
        if (!best_class) {
            return std::nullopt;
        }
        const std::size_t klass = best_class->second;
        std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> best_lane;
        for (const auto& [id, lane] : lanes) {
            if (lane.reads.empty() || lane.klass != klass) {
                continue;
            }
            const std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> key{lane.clock, lane.reads.front().first,
                                                                              id};
            if (!best_lane || key < *best_lane) {
                best_lane = key;
            }
        }
        if (!best_lane) {
            return std::nullopt;
        }
        const std::uint64_t id = std::get<2>(*best_lane);
        Lane& lane = lanes.at(id);
        const auto [seq, request] = lane.reads.front();
        const std::uint64_t need = staging_bytes(request);
        if (staged > 0 && staged + need > staging) {
            starved = true;
            ++stats.refused;
            return std::nullopt;
        }
        lane.reads.pop_front();
        lane.running.insert(seq);
        staged += need;
        const std::uint64_t size =
            std::max<std::uint64_t>(std::max(request.destination_size, request.source_size), 1);
        now = clock[klass];
        clock[klass] += size * kCost[klass];
        lane_now[klass] = lane.clock;
        lane.clock += size;
        return Job{id, seq, request, need};
    }

    void finish(const Job& job, std::int32_t result)
    {
        staged -= job.staging;
        const auto found = lanes.find(job.lane);
        if (found == lanes.end()) {
            return;
        }
        Lane& lane = found->second;
        lane.running.erase(job.seq);
        if (result < 0) {
            lane.failures.emplace(job.seq, result);
        }
        lane.flush();
    }

    void submit_as(std::uint64_t id, bool explicit_submit)
    {
        const auto found = lanes.find(id);
        if (found == lanes.end()) {
            return;
        }
        const std::size_t klass = found->second.klass;
        const bool class_busy = std::any_of(lanes.begin(), lanes.end(), [&](const auto& entry) {
            return entry.second.klass == klass && !entry.second.reads.empty();
        });
        if (!class_busy) {
            clock[klass] = std::max(clock[klass], now);
        }
        Lane& lane = found->second;
        if (lane.reads.empty()) {
            lane.clock = std::max(lane.clock, lane_now[klass]);
        }
        std::vector<Command> commands;
        if (explicit_submit) {
            commands = std::move(lane.held);
            lane.held.clear();
        }
        for (auto& command : lane.unsubmitted) {
            commands.push_back(command);
        }
        lane.unsubmitted.clear();
        for (auto& command : commands) {
            bool hold = false;
            if (!explicit_submit) {
                hold = command.read ? (command.request.options & kPageDestination) != 0 : !lane.held.empty();
            }
            if (hold) {
                lane.held.push_back(command);
                continue;
            }
            const std::uint64_t seq = next_seq++;
            if (command.read) {
                lane.reads.emplace_back(seq, command.request);
            } else {
                lane.statuses.emplace_back(seq, command.status);
            }
        }
        lane.flush();
    }

    void count(const Job& job, std::int32_t result, std::chrono::steady_clock::time_point started)
    {
        const auto now_time = std::chrono::steady_clock::now();
        ++stats.requests;
        stats.bytes += job.request.destination_size;
        stats.busy += now_time - started;
        if (result < 0) {
            ++stats.failures;
        }
        if ((job.request.options & kBcpack) != 0) {
            stats.bcpack_bytes += job.request.destination_size;
        }
        if (!stats.since) {
            stats.since = started;
        }
        const double elapsed = std::chrono::duration<double>(now_time - *stats.since).count();
        if (elapsed < 5.0) {
            return;
        }
        std::size_t waiting = 0;
        for (const auto& [id, lane] : lanes) {
            waiting += lane.reads.size();
        }
        log::info(std::format("dstorage: last {:.1f} s: {} requests, {:.1f} MB/s, {} failed, busy workers {:.2f}, "
                              "bcpack {} MB, staging waits {}, waiting {}",
                              elapsed, stats.requests, static_cast<double>(stats.bytes) / elapsed / 1e6,
                              stats.failures, std::chrono::duration<double>(stats.busy).count() / elapsed,
                              stats.bcpack_bytes / 1000000, stats.refused, waiting));
        stats = Stats{};
    }
};

std::int32_t execute(const Request& request);

void worker(Scheduler* scheduler)
{
    std::unique_lock lock(scheduler->mutex);
    for (;;) {
        std::optional<Job> job = scheduler->start();
        if (!job) {
            scheduler->work.wait(lock);
            continue;
        }
        lock.unlock();
        const auto started = std::chrono::steady_clock::now();
        const std::int32_t result = execute(job->request);
        lock.lock();
        scheduler->finish(*job, result);
        scheduler->count(*job, result, started);
        if (std::exchange(scheduler->starved, false)) {
            scheduler->work.notify_all();
        }
        scheduler->finished.notify_all();
    }
}

Scheduler& scheduler()
{
    static Scheduler* instance = [] {
        auto* created = new Scheduler{};
        const unsigned count = std::max(1u, std::thread::hardware_concurrency());
        unsigned started = 0;
        for (unsigned index = 0; index < count; ++index) {
            try {
                std::thread thread(worker, created);
                SetThreadDescription(thread.native_handle(), std::format(L"dstorage {}", index).c_str());
                thread.detach();
                ++started;
            } catch (...) {
                break;
            }
        }
        log::info(std::format("dstorage: {} workers", started));
        return created;
    }();
    return *instance;
}

std::mutex g_files_mutex;
std::unordered_map<const void*, std::string> g_files;

std::string file_name(const Request& request)
{
    if ((request.options & kMemorySource) != 0) {
        return "memory";
    }
    std::lock_guard lock(g_files_mutex);
    const auto found = g_files.find(request.source);
    return found != g_files.end() ? found->second : std::string{"?"};
}

std::string compression_text(std::uint64_t options)
{
    const auto bc = static_cast<unsigned>((options & kBcpack) >> 1);
    const bool zlib = (options & kZlib) != 0;
    if (zlib && bc != 0) {
        return std::format("zlib+bcpack bc{}", bc);
    }
    if (zlib) {
        return "zlib";
    }
    if (bc != 0) {
        return std::format("bcpack bc{}", bc);
    }
    return "none";
}

std::string describe(const Request& request)
{
    const bool pages = (request.options & kPageDestination) != 0;
    return std::format("file {} offset {:#x} size {} -> {} {:p} size {} ({}, options {:#x}, intermediate {}, name "
                       "'{}')",
                       file_name(request), request.offset, request.source_size, pages ? "pages" : "address",
                       request.destination, request.destination_size, compression_text(request.options),
                       request.options, request.intermediate_size, text_of(request.name));
}

std::int32_t fail(const Request& request, std::int32_t hr, std::string_view why)
{
    log::warn(std::format("dstorage: request failed hr {}: {}: {}", hresult_text(hr), why, describe(request)));
    return hr;
}

HANDLE read_event()
{
    thread_local HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return event;
}

std::int32_t read_file(HANDLE handle, std::uint64_t offset, std::uint8_t* out, std::size_t size)
{
    std::size_t done = 0;
    while (done < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, 1u << 30));
        OVERLAPPED overlapped{};
        const std::uint64_t at = offset + done;
        overlapped.Offset = static_cast<DWORD>(at);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        overlapped.hEvent = read_event();
        DWORD got = 0;
        if (!ReadFile(handle, out + done, chunk, nullptr, &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                return kEndOfFile;
            }
            if (error != ERROR_IO_PENDING) {
                return from_win32(error);
            }
        }
        if (!GetOverlappedResult(handle, &overlapped, &got, TRUE)) {
            const DWORD error = GetLastError();
            return error == ERROR_HANDLE_EOF ? kEndOfFile : from_win32(error);
        }
        if (got == 0) {
            return kEndOfFile;
        }
        done += got;
    }
    return kOk;
}

std::int32_t read_into(const Request& request, std::span<std::uint8_t> out)
{
    if ((request.options & kMemorySource) != 0) {
        std::memcpy(out.data(), request.source, out.size());
        return kOk;
    }
    const auto* file = static_cast<const FileObject*>(request.source);
    if (file->vtable != file_vtable().data()) {
        return kInvalidArgument;
    }
    return read_file(file->handle, request.offset, out.data(), out.size());
}

template <typename Fill>
std::int32_t into_destination(const Request& request, std::size_t length, Fill&& fill)
{
    if ((request.options & kPageDestination) == 0) {
        auto* target = static_cast<std::uint8_t*>(request.destination);
        return fill(std::span<std::uint8_t>{target, length});
    }
    static std::atomic<bool> noted{false};
    if (!noted.exchange(true)) {
        log::info("dstorage: first request into physical pages; written to the pages' backing memory");
    }
    const std::size_t offset = request.page_offset & 0xffff;
    const std::size_t count = (offset + length + kPage - 1) / kPage;
    const auto* pages = static_cast<const std::size_t*>(request.destination);
    bool contiguous = true;
    for (std::size_t index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<std::uint8_t*>(pages[index] << 12);
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0
            || reinterpret_cast<std::uint8_t*>(info.BaseAddress) + info.RegionSize < address + kPage) {
            return kInvalidArgument;
        }
        if (index > 0 && (pages[index] << 12) != (pages[index - 1] << 12) + kPage) {
            contiguous = false;
        }
    }
    if (count == 0) {
        return fill(std::span<std::uint8_t>{});
    }
    if (contiguous) {
        auto* target = reinterpret_cast<std::uint8_t*>(pages[0] << 12) + offset;
        return fill(std::span<std::uint8_t>{target, length});
    }
    std::vector<std::uint8_t> buffer(length);
    const std::int32_t result = fill(std::span<std::uint8_t>{buffer});
    std::size_t done = 0;
    std::size_t inner = offset;
    for (std::size_t index = 0; index < count && done < length; ++index) {
        const std::size_t part = std::min(kPage - inner, length - done);
        std::memcpy(reinterpret_cast<std::uint8_t*>(pages[index] << 12) + inner, buffer.data() + done, part);
        done += part;
        inner = 0;
    }
    return result;
}

std::int32_t execute(const Request& request)
{
    const std::uint64_t options = request.options;
    if ((options & ~kKnownOptions) != 0) {
        return fail(request, kNotImplemented, "unsupported options");
    }
    if (request.source == nullptr || request.destination == nullptr) {
        return fail(request, kInvalidArgument, "no source or destination");
    }
    const auto bc = static_cast<std::uint8_t>((options & kBcpack) >> 1);
    const std::size_t size = request.destination_size;
    if ((options & (kZlib | kBcpack)) == 0) {
        if (request.source_size > request.destination_size) {
            return fail(request, kInvalidArgument, "source larger than destination");
        }
        const std::int32_t result = into_destination(request, request.source_size,
                                                     [&](std::span<std::uint8_t> out) { return read_into(request, out); });
        return result < 0 ? fail(request, result, "reading") : kOk;
    }
    const formats::bcpack::Tables* tables = nullptr;
    if (bc != 0) {
        tables = formats::bcpack::builtin_tables();
        if (tables == nullptr) {
            return fail(request, kNotImplemented,
                        std::format("bcpack tables unavailable: {}", formats::bcpack::builtin_tables_error()));
        }
    }
    std::vector<std::uint8_t> owned;
    std::span<const std::uint8_t> source;
    if ((options & kMemorySource) != 0) {
        source = {static_cast<const std::uint8_t*>(request.source), request.source_size};
    } else {
        owned.resize(request.source_size);
        if (const std::int32_t result = read_into(request, owned); result < 0) {
            return fail(request, result, "reading the source");
        }
        source = owned;
    }
    std::vector<std::uint8_t> packed;
    if (bc != 0 && (options & kZlib) != 0) {
        packed.resize(request.intermediate_size);
        const auto inflated = formats::zlib_inflate(source, packed);
        if (!inflated) {
            return fail(request, kInvalidData, std::format("zlib: {}", inflated.error()));
        }
        packed.resize(*inflated);
        source = packed;
    }
    std::string why;
    const std::int32_t result = into_destination(request, size, [&](std::span<std::uint8_t> out) -> std::int32_t {
        if (tables != nullptr) {
            const auto decoded = formats::bcpack::decode(bc, source, out, *tables);
            if (!decoded) {
                why = std::format("bcpack: {}", decoded.error());
                return kInvalidData;
            }
            return kOk;
        }
        const auto inflated = formats::zlib_inflate(source, out);
        if (!inflated) {
            why = std::format("zlib: {}", inflated.error());
            return kInvalidData;
        }
        return kOk;
    });
    if (result < 0) {
        return fail(request, result, why.empty() ? std::string_view{"destination"} : std::string_view{why});
    }
    return kOk;
}

void close_lane(std::uint64_t id)
{
    Scheduler& s = scheduler();
    std::unique_lock lock(s.mutex);
    const auto found = s.lanes.find(id);
    if (found == s.lanes.end()) {
        return;
    }
    found->second.closed = true;
    found->second.drop_all();
    s.finished.wait(lock, [&] {
        const auto lane = s.lanes.find(id);
        return lane == s.lanes.end() || lane->second.running.empty();
    });
    s.lanes.erase(id);
    s.finished.notify_all();
}

void destroy(QueueObject* queue)
{
    close_lane(queue->id);
    delete queue;
}

std::atomic<std::uint32_t> g_logged_requests{0};
std::atomic<std::uint32_t> g_auto_submits{0};

void enqueue(QueueObject* queue, Command command)
{
    Scheduler& s = scheduler();
    std::unique_lock lock(s.mutex);
    for (;;) {
        const auto found = s.lanes.find(queue->id);
        if (found == s.lanes.end() || found->second.closed) {
            release_command(command);
            return;
        }
        const Lane& lane = found->second;
        if (lane.entries() + 1 < std::max<std::size_t>(lane.capacity, 2)) {
            break;
        }
        if (!lane.held.empty()) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                log::warn("dstorage: a full queue starts its page reads before the title's submit");
            }
            s.submit_as(queue->id, true);
            s.work.notify_all();
        }
        s.finished.wait(lock);
    }
    Lane& lane = s.lanes.at(queue->id);
    lane.unsubmitted.push_back(command);
    if (lane.unsubmitted.size() >= std::max<std::size_t>(lane.capacity / 2, 1)) {
        s.submit_as(queue->id, false);
        s.work.notify_all();
        const std::uint32_t n = g_auto_submits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (std::has_single_bit(n)) {
            log::info(std::format("dstorage: queue '{}' half full submitted itself ({} times, capacity {}, held {})",
                                  lane.name, n, lane.capacity, lane.held.size()));
        }
    }
}

std::int32_t factory_query_interface(FactoryObject* self, const GUID* iid, void** out)
{
    if (out == nullptr) {
        return kInvalidArgument;
    }
    if (iid != nullptr
        && (std::memcmp(iid, &kFactoryIid, sizeof(GUID)) == 0 || std::memcmp(iid, &kUnknownIid, sizeof(GUID)) == 0)) {
        *out = self;
        return kOk;
    }
    *out = nullptr;
    log::warn("dstorage: factory QueryInterface for unknown " + guid_text(iid));
    return kNoInterface;
}

std::int32_t factory_open_file(FactoryObject*, const wchar_t* path, const GUID* iid, void** out)
{
    if (path == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    std::wstring mapped;
    const wchar_t* use = kernel::remap_guest_path(path, mapped) ? mapped.c_str() : path;
    const std::string name = narrow(path);
    HANDLE handle = CreateFileW(use, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const std::int32_t hr = from_win32(GetLastError());
        log::warn(std::format("dstorage: OpenFile {} ({}) failed hr {}", name, narrow(use), hresult_text(hr)));
        *out = nullptr;
        return hr;
    }
    auto* file = new FileObject{file_vtable().data()};
    file->handle = handle;
    file->path = name;
    {
        std::lock_guard lock(g_files_mutex);
        g_files[file] = name;
    }
    log::info(std::format("dstorage: OpenFile {} iid {} -> {:p}", name, guid_text(iid), static_cast<void*>(file)));
    *out = file;
    return kOk;
}

std::int32_t factory_create_status_array(FactoryObject*, std::uint32_t capacity, const char* name, const GUID* iid,
                                         void** out)
{
    if (out == nullptr) {
        return kInvalidArgument;
    }
    auto* array = new (std::nothrow) StatusArray{status_vtable().data()};
    if (array == nullptr) {
        return kOutOfMemory;
    }
    array->capacity = capacity;
    array->entries = std::make_unique<std::atomic<std::int32_t>[]>(capacity);
    for (std::uint32_t index = 0; index < capacity; ++index) {
        array->entries[index].store(kOk, std::memory_order_relaxed);
    }
    array->name = text_of(name);
    log::info(std::format("dstorage: CreateStatusArray capacity {} name '{}' iid {} -> {:p}", capacity, array->name,
                          guid_text(iid), static_cast<void*>(array)));
    *out = array;
    return kOk;
}

std::int32_t factory_set_staging_buffer_size(FactoryObject*, std::uint32_t size)
{
    log::info(std::format("dstorage: SetStagingBufferSize {}", size));
    Scheduler& s = scheduler();
    std::lock_guard lock(s.mutex);
    s.staging = size;
    return kOk;
}

std::int32_t factory_create_queue(FactoryObject*, const QueueDesc* desc, const GUID* iid, void** out)
{
    if (desc == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    const std::string name = text_of(desc->name);
    Scheduler& s = scheduler();
    auto* queue = new QueueObject{queue_vtable().data()};
    queue->name = name;
    {
        std::lock_guard lock(s.mutex);
        queue->id = s.next_id++;
        Lane lane;
        lane.klass = static_cast<std::size_t>(std::clamp<int>(desc->priority, -1, 1) + 1);
        lane.capacity = desc->capacity;
        lane.name = name;
        s.lanes.emplace(queue->id, std::move(lane));
    }
    log::info(std::format("dstorage: CreateQueue '{}' source {} capacity {} priority {} unknown {} iid {} -> {:p}",
                          name, desc->source_type, desc->capacity, static_cast<int>(desc->priority), desc->unknown,
                          guid_text(iid), static_cast<void*>(queue)));
    *out = queue;
    return kOk;
}

bool status_is_complete(StatusArray* self, std::uint32_t index)
{
    if (self == nullptr || index >= self->capacity) {
        return true;
    }
    return self->entries[index].load(std::memory_order_acquire) != kPending;
}

std::int32_t status_get_hresult(StatusArray* self, std::uint32_t index)
{
    if (self == nullptr || index >= self->capacity) {
        return kInvalidArgument;
    }
    return self->entries[index].load(std::memory_order_acquire);
}

void queue_enqueue_request(QueueObject* self, const Request* request)
{
    if (self == nullptr || request == nullptr) {
        return;
    }
    if (g_logged_requests.fetch_add(1, std::memory_order_relaxed) < kLoggedRequests) {
        log::info(std::format("dstorage: request on '{}': {}", self->name, describe(*request)));
    }
    Command command;
    command.read = true;
    command.request = *request;
    enqueue(self, command);
}

void queue_enqueue_status(QueueObject* self, StatusArray* array, std::uint32_t index)
{
    if (self == nullptr || array == nullptr) {
        return;
    }
    if (index >= array->capacity) {
        log::warn(std::format("dstorage: status index {} outside array of {}", index, array->capacity));
        return;
    }
    array->entries[index].store(kPending, std::memory_order_release);
    add_ref_object(array);
    Command command;
    command.status = {array, index};
    enqueue(self, command);
}

void queue_submit(QueueObject* self)
{
    if (self == nullptr) {
        return;
    }
    Scheduler& s = scheduler();
    std::lock_guard lock(s.mutex);
    const auto found = s.lanes.find(self->id);
    if (found == s.lanes.end()) {
        return;
    }
    Lane& lane = found->second;
    std::size_t reads = 0;
    std::size_t statuses = 0;
    for (const auto* list : {&lane.unsubmitted, &lane.held}) {
        for (const auto& command : *list) {
            (command.read ? reads : statuses) += 1;
        }
    }
    ++lane.submits;
    const std::string message = std::format("dstorage: submit '{}' #{}: {} reads, {} statuses, {} queued, {} running",
                                            lane.name, lane.submits, reads, statuses, lane.reads.size(),
                                            lane.running.size());
    if (lane.submits <= kLoggedSubmits) {
        log::info(message);
    } else {
        log::debug(message);
    }
    s.submit_as(self->id, true);
    s.work.notify_all();
}

void queue_close(QueueObject* self)
{
    if (self == nullptr) {
        return;
    }
    log::info(std::format("dstorage: close queue '{}'", self->name));
    close_lane(self->id);
}

const std::array<void*, 16>& factory_vtable()
{
    static const std::array<void*, 16> table = [] {
        auto result = missing_table<Area::Factory, 16>();
        result[0] = reinterpret_cast<void*>(&factory_query_interface);
        result[1] = reinterpret_cast<void*>(&static_add_ref);
        result[2] = reinterpret_cast<void*>(&static_add_ref);
        result[4] = reinterpret_cast<void*>(&factory_open_file);
        result[5] = reinterpret_cast<void*>(&factory_create_status_array);
        result[8] = reinterpret_cast<void*>(&factory_set_staging_buffer_size);
        result[10] = reinterpret_cast<void*>(&factory_create_queue);
        return result;
    }();
    return table;
}

const std::array<void*, 8>& file_vtable()
{
    static const std::array<void*, 8> table = [] {
        auto result = missing_table<Area::File, 8>();
        result[1] = reinterpret_cast<void*>(&add_ref_object<FileObject>);
        result[2] = reinterpret_cast<void*>(&release_object<FileObject>);
        return result;
    }();
    return table;
}

const std::array<void*, 5>& status_vtable()
{
    static const std::array<void*, 5> table = [] {
        auto result = missing_table<Area::Status, 5>();
        result[1] = reinterpret_cast<void*>(&add_ref_object<StatusArray>);
        result[2] = reinterpret_cast<void*>(&release_object<StatusArray>);
        result[3] = reinterpret_cast<void*>(&status_is_complete);
        result[4] = reinterpret_cast<void*>(&status_get_hresult);
        return result;
    }();
    return table;
}

const std::array<void*, 12>& queue_vtable()
{
    static const std::array<void*, 12> table = [] {
        auto result = missing_table<Area::Queue, 12>();
        result[1] = reinterpret_cast<void*>(&add_ref_object<QueueObject>);
        result[2] = reinterpret_cast<void*>(&release_object<QueueObject>);
        result[3] = reinterpret_cast<void*>(&queue_enqueue_request);
        result[4] = reinterpret_cast<void*>(&queue_enqueue_status);
        result[6] = reinterpret_cast<void*>(&queue_submit);
        result[8] = reinterpret_cast<void*>(&queue_close);
        return result;
    }();
    return table;
}

FactoryObject& factory_object()
{
    static FactoryObject object{factory_vtable().data()};
    return object;
}

}

std::int32_t get_factory(const void* iid, void** factory)
{
    const auto* guid = static_cast<const GUID*>(iid);
    if (guid == nullptr || std::memcmp(guid, &kFactoryIid, sizeof(GUID)) != 0) {
        log::warn("dstorage: DStorageGetFactory for unknown " + guid_text(guid));
        if (factory != nullptr) {
            *factory = nullptr;
        }
        return kNoInterface;
    }
    if (factory == nullptr) {
        return kInvalidArgument;
    }
    *factory = &factory_object();
    log::info("dstorage: DStorageGetFactory -> console factory");
    return kOk;
}

namespace {

template <typename R, typename... Args>
R call(void* object, std::size_t offset, Args... args)
{
    void* const* table = *static_cast<void* const* const*>(object);
    using Fn = R (*)(void*, Args...);
    return reinterpret_cast<Fn>(table[offset / 8])(object, args...);
}

std::vector<std::uint8_t> pattern(std::size_t n)
{
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(i * 7 + i / 251);
    }
    return out;
}

std::vector<std::uint8_t> stored_zlib(std::span<const std::uint8_t> plain)
{
    std::vector<std::uint8_t> out{0x78, 0x01};
    std::size_t at = 0;
    do {
        const std::size_t part = std::min<std::size_t>(plain.size() - at, 65535);
        const bool last = at + part == plain.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(part));
        out.push_back(static_cast<std::uint8_t>(part >> 8));
        out.push_back(static_cast<std::uint8_t>(~part));
        out.push_back(static_cast<std::uint8_t>(~part >> 8));
        out.insert(out.end(), plain.begin() + static_cast<std::ptrdiff_t>(at),
                   plain.begin() + static_cast<std::ptrdiff_t>(at + part));
        at += part;
    } while (at < plain.size());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : plain) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; --k) {
        out.push_back(static_cast<std::uint8_t>(adler >> (8 * k)));
    }
    return out;
}

Request make_request(std::uint64_t options, void* destination, std::size_t destination_size, const void* source,
                     std::uint64_t offset, std::size_t source_size)
{
    Request request{};
    request.options = options;
    request.destination = destination;
    request.destination_size = static_cast<std::uint32_t>(destination_size);
    request.source = source;
    request.offset = offset;
    request.source_size = static_cast<std::uint32_t>(source_size);
    return request;
}

struct Harness {
    void* factory{};

    void* open(const std::wstring& path)
    {
        void* file = nullptr;
        return call<std::int32_t>(factory, 0x20, path.c_str(), &kFactoryIid, &file) == kOk ? file : nullptr;
    }

    void* status(std::uint32_t capacity)
    {
        void* array = nullptr;
        call<std::int32_t>(factory, 0x28, capacity, "selftest", &kFactoryIid, &array);
        return array;
    }

    void* queue(std::uint16_t capacity, std::int8_t priority)
    {
        QueueDesc desc{};
        desc.capacity = capacity;
        desc.priority = priority;
        desc.name = "selftest";
        desc.unknown = 1;
        void* created = nullptr;
        call<std::int32_t>(factory, 0x50, &desc, &kFactoryIid, &created);
        return created;
    }

    static void enqueue(void* queue, const Request& request) { call<void>(queue, 0x18, &request); }
    static void enqueue_status(void* queue, void* array, std::uint32_t index)
    {
        call<void>(queue, 0x20, array, index);
    }
    static void submit(void* queue) { call<void>(queue, 0x30); }
    static void close(void* queue) { call<void>(queue, 0x40); }
    static std::int32_t result(void* array, std::uint32_t index)
    {
        return call<std::int32_t>(array, 0x20, index);
    }

    static std::int32_t wait(void* array, std::uint32_t index)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!call<bool>(array, 0x18, index)) {
            if (std::chrono::steady_clock::now() > deadline) {
                return kPending;
            }
            Sleep(1);
        }
        return result(array, index);
    }
};

}

int self_test(std::string& report)
{
    int failures = 0;
    const auto check = [&](std::string_view name, bool ok) {
        report += std::format("{} {}\n", ok ? "ok  " : "FAIL", name);
        if (!ok) {
            ++failures;
        }
    };

    Harness h;
    check("factory for the console iid", get_factory(&kFactoryIid, &h.factory) == kOk && h.factory != nullptr);
    if (h.factory == nullptr) {
        return failures;
    }
    void* same = nullptr;
    check("factory query interface",
          call<std::int32_t>(h.factory, 0x00, &kFactoryIid, &same) == kOk && same == h.factory);

    wchar_t directory[MAX_PATH]{};
    GetTempPathW(MAX_PATH, directory);
    const std::wstring path = std::wstring(directory) + std::format(L"euphoria-dstorage-{}.bin", GetCurrentProcessId());
    const auto plain = pattern(300000);
    const auto packed = stored_zlib(plain);
    {
        std::vector<std::uint8_t> contents = plain;
        contents.insert(contents.end(), packed.begin(), packed.end());
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written = 0;
        const bool ok = handle != INVALID_HANDLE_VALUE
                        && WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr)
                        && written == contents.size();
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        check("temporary source file", ok);
    }

    void* file = h.open(path);
    check("open file", file != nullptr);
    if (file != nullptr) {
        void* queue = h.queue(0x80, 0);
        void* array = h.status(1);
        std::vector<std::uint8_t> raw(1000);
        std::vector<std::uint8_t> inflated(plain.size());
        Harness::enqueue(queue, make_request(0, raw.data(), raw.size(), file, 5000, raw.size()));
        Harness::enqueue(queue, make_request(kZlib, inflated.data(), inflated.size(), file, plain.size(), packed.size()));
        Harness::enqueue_status(queue, array, 0);
        const bool pending = Harness::result(array, 0) == kPending;
        Harness::submit(queue);
        const bool done = Harness::wait(array, 0) == kOk;
        check("raw and zlib reads from a file",
              pending && done && std::equal(raw.begin(), raw.end(), plain.begin() + 5000) && inflated == plain);
        Harness::close(queue);
        call<std::uint32_t>(queue, 0x10);
        call<std::uint32_t>(array, 0x10);
    }

    {
        const auto small = pattern(70000);
        const auto source = stored_zlib(small);
        void* queue = h.queue(0x80, 1);
        void* array = h.status(1);
        std::vector<std::uint8_t> out(small.size());
        Harness::enqueue(queue, make_request(kZlib | kMemorySource, out.data(), out.size(), source.data(), 0,
                                             source.size()));
        Harness::enqueue_status(queue, array, 0);
        Harness::submit(queue);
        check("memory sources inflate", Harness::wait(array, 0) == kOk && out == small);
        call<std::uint32_t>(queue, 0x10);
        call<std::uint32_t>(array, 0x10);
    }

    if (file != nullptr) {
        const std::size_t size = plain.size() + packed.size();
        void* queue = h.queue(0x80, 0);
        void* array = h.status(3);
        std::array<std::array<std::uint8_t, 16>, 5> buffers{};
        Harness::enqueue(queue, make_request(0, buffers[0].data(), 16, file, size - 6, 16));
        Harness::enqueue(queue, make_request(kZlib, buffers[1].data(), 16, file, 0, 16));
        Harness::enqueue_status(queue, array, 0);
        Harness::enqueue(queue, make_request(0, buffers[3].data(), 16, file, 0, 16));
        Harness::enqueue(queue, make_request(0, buffers[2].data(), 16, file, 100, 16));
        Harness::enqueue_status(queue, array, 1);
        Harness::submit(queue);
        Harness::enqueue(queue, make_request(7ull << 1, buffers[4].data(), 16, file, 0, 0));
        Harness::enqueue_status(queue, array, 2);
        Harness::submit(queue);
        const std::int32_t third = Harness::wait(array, 2);
        check("statuses complete in order with the first failure",
              third == kInvalidData && Harness::result(array, 0) == kEndOfFile && Harness::result(array, 1) == kOk
                  && std::equal(buffers[2].begin(), buffers[2].end(), plain.begin() + 100)
                  && std::equal(buffers[3].begin(), buffers[3].end(), plain.begin()));
        call<std::uint32_t>(queue, 0x10);
        call<std::uint32_t>(array, 0x10);
    }

    if (file != nullptr) {
        void* queue = h.queue(4, 1);
        void* array = h.status(1);
        std::array<std::uint8_t, 16> out{};
        Harness::enqueue(queue, make_request(0, out.data(), 16, file, 8, 16));
        Harness::enqueue_status(queue, array, 0);
        check("half a queue submits itself",
              Harness::wait(array, 0) == kOk && std::equal(out.begin(), out.end(), plain.begin() + 8));
        call<std::uint32_t>(queue, 0x10);
        call<std::uint32_t>(array, 0x10);
    }

    if (file != nullptr) {
        void* queue = h.queue(0x80, 0);
        void* array = h.status(1);
        std::array<std::uint8_t, 16> out{};
        Harness::enqueue(queue, make_request(0, out.data(), 16, file, 0, 16));
        Harness::enqueue_status(queue, array, 0);
        Harness::close(queue);
        Harness::submit(queue);
        Sleep(50);
        check("a closed queue completes nothing",
              Harness::result(array, 0) == kPending && out == std::array<std::uint8_t, 16>{});
        call<std::uint32_t>(queue, 0x10);
        call<std::uint32_t>(array, 0x10);
    }

    {
        auto* first = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        auto* second = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        std::array<std::size_t, 2> pages{reinterpret_cast<std::size_t>(first) >> 12,
                                         reinterpret_cast<std::size_t>(second) >> 12};
        const auto small = pattern(100);
        void* queue = h.queue(0x80, 1);
        void* array = h.status(1);
        Request request = make_request(kPageDestination | kMemorySource, pages.data(), small.size(), small.data(), 0,
                                       small.size());
        request.page_offset = kPage - 40;
        Harness::enqueue(queue, request);
        Harness::enqueue_status(queue, array, 0);
        Harness::submit(queue);
        const bool ok = Harness::wait(array, 0) == kOk && first != nullptr && second != nullptr
                        && std::equal(small.begin(), small.begin() + 40, first + kPage - 40)
                        && std::equal(small.begin() + 40, small.end(), second);
        check("physical page destinations", ok);
        call<std::uint32_t>(queue, 0x10);
        call<std::uint32_t>(array, 0x10);
        VirtualFree(first, 0, MEM_RELEASE);
        VirtualFree(second, 0, MEM_RELEASE);
    }

    if (file != nullptr) {
        call<std::uint32_t>(file, 0x10);
    }
    DeleteFileW(path.c_str());
    return failures;
}

}
