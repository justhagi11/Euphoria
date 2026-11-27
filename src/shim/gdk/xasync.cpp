#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace euphoria::shim::gdk {
namespace {

enum class Mode : std::uint32_t {
    Manual,
    ThreadPool,
    Serialized,
    Immediate,
};

constexpr std::uint32_t kInfinite = 0xFFFFFFFFu;
constexpr std::uint32_t kQueueMagic = 0x51554555u;

struct Task {
    void* context;
    TaskCallback callback;

    void run(bool canceled) const { callback(context, canceled); }
};

class Pool {
public:
    void submit(Task task)
    {
        std::unique_lock lock(mutex_);
        tasks_.push_back(task);
        if (tasks_.size() > idle_) {
            std::thread([this] { work(); }).detach();
        } else {
            ready_.notify_one();
        }
    }

private:
    void work()
    {
        std::unique_lock lock(mutex_);
        for (;;) {
            while (tasks_.empty()) {
                ++idle_;
                ready_.wait(lock);
                --idle_;
            }
            const Task task = tasks_.front();
            tasks_.pop_front();
            lock.unlock();
            task.run(false);
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Task> tasks_;
    std::size_t idle_ = 0;
};

Pool& pool()
{
    static Pool* instance = new Pool;
    return *instance;
}

struct Port : std::enable_shared_from_this<Port> {
    explicit Port(Mode value) : mode(value) {}

    Mode mode;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Task> tasks;

    void submit(Task task)
    {
        switch (mode) {
        case Mode::Immediate:
            task.run(false);
            break;
        case Mode::ThreadPool:
            pool().submit(task);
            break;
        case Mode::Manual:
        case Mode::Serialized: {
            std::scoped_lock lock(mutex);
            tasks.push_back(task);
        }
            ready.notify_one();
            break;
        }
    }

    template <class Ended>
    std::optional<Task> take(std::uint32_t timeout_ms, Ended ended)
    {
        std::unique_lock lock(mutex);
        const auto waiting = [&] { return !tasks.empty() || ended(); };
        if (timeout_ms == kInfinite) {
            ready.wait(lock, waiting);
        } else {
            ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), waiting);
        }
        if (tasks.empty()) {
            return std::nullopt;
        }
        const Task task = tasks.front();
        tasks.pop_front();
        return task;
    }
};

std::shared_ptr<Port> make_port(Mode mode)
{
    auto port = std::make_shared<Port>(mode);
    if (mode == Mode::Serialized) {
        std::thread([worker = port]() mutable {
            for (;;) {
                if (const auto task = worker->take(1000, [] { return false; }); task) {
                    task->run(false);
                } else if (worker.use_count() == 1) {
                    break;
                }
            }
        }).detach();
    }
    return port;
}

using MonitorCallback = void (*)(void*, void*, std::uint32_t);

struct Monitor {
    std::uint64_t token;
    void* context;
    MonitorCallback callback;
};

struct Queue {
    std::uint32_t magic = kQueueMagic;
    std::atomic<long> references{1};
    std::array<std::shared_ptr<Port>, 2> ports;
    std::mutex monitor_mutex;
    std::vector<Monitor> monitors;
    std::atomic<bool> terminated{false};
};

void retain(Queue* queue)
{
    queue->references.fetch_add(1, std::memory_order_relaxed);
}

void release(Queue* queue)
{
    if (queue->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        queue->magic = 0;
        delete queue;
    }
}

class QueueRef {
public:
    QueueRef() = default;
    explicit QueueRef(Queue* queue) : queue_(queue) {}
    QueueRef(const QueueRef&) = delete;
    QueueRef& operator=(const QueueRef&) = delete;
    QueueRef(QueueRef&& other) noexcept : queue_(std::exchange(other.queue_, nullptr)) {}
    ~QueueRef()
    {
        if (queue_ != nullptr) {
            release(queue_);
        }
    }

    Queue* get() const { return queue_; }
    Queue* operator->() const { return queue_; }
    explicit operator bool() const { return queue_ != nullptr; }
    Queue* detach() { return std::exchange(queue_, nullptr); }

private:
    Queue* queue_ = nullptr;
};

Queue* make_queue(std::shared_ptr<Port> work, std::shared_ptr<Port> completion)
{
    auto* queue = new Queue;
    queue->ports[0] = std::move(work);
    queue->ports[1] = std::move(completion);
    return queue;
}

std::mutex& process_mutex()
{
    static std::mutex mutex;
    return mutex;
}

bool g_process_set = false;
Queue* g_process_queue = nullptr;

QueueRef process_queue()
{
    std::scoped_lock lock(process_mutex());
    if (!g_process_set) {
        g_process_set = true;
        g_process_queue = make_queue(make_port(Mode::ThreadPool), make_port(Mode::ThreadPool));
    }
    if (g_process_queue == nullptr) {
        return {};
    }
    retain(g_process_queue);
    return QueueRef(g_process_queue);
}

QueueRef acquire(void* handle)
{
    if (handle == nullptr) {
        return process_queue();
    }
    auto* queue = static_cast<Queue*>(handle);
    if (queue->magic != kQueueMagic) {
        log::warn("xasync: unknown task queue handle");
        return {};
    }
    retain(queue);
    return QueueRef(queue);
}

void deliver(Queue* queue, std::uint32_t port, Task task)
{
    queue->ports[port]->submit(task);
    std::vector<Monitor> monitors;
    {
        std::scoped_lock lock(queue->monitor_mutex);
        monitors = queue->monitors;
    }
    for (const auto& monitor : monitors) {
        monitor.callback(monitor.context, queue, port);
    }
}

HResult submit(Queue* queue, std::uint32_t port, std::uint32_t delay_ms, Task task)
{
    if (port > kPortCompletion) {
        return kInvalidArgument;
    }
    if (queue->terminated.load(std::memory_order_acquire)) {
        return kAbort;
    }
    if (delay_ms == 0) {
        deliver(queue, port, task);
        return kOk;
    }
    retain(queue);
    std::thread([queue, port, delay_ms, task] {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        deliver(queue, port, task);
        release(queue);
    }).detach();
    return kOk;
}

bool valid_mode(std::uint32_t mode)
{
    return mode <= static_cast<std::uint32_t>(Mode::Immediate);
}

std::atomic<std::uint64_t> g_next_monitor{1};

struct Call {
    AsyncProvider provider;
    void* context;
    const void* identity;
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;

    HResult invoke(std::uint32_t op, AsyncBlock* block, std::size_t size, void* buffer) const
    {
        const ProviderData data{block, size, buffer, context};
        return provider(op, &data);
    }
};

std::mutex& calls_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<AsyncBlock*, std::shared_ptr<Call>>& calls()
{
    static auto* map = new std::unordered_map<AsyncBlock*, std::shared_ptr<Call>>;
    return *map;
}

std::shared_ptr<Call> find_call(AsyncBlock* block)
{
    std::scoped_lock lock(calls_mutex());
    const auto found = calls().find(block);
    return found == calls().end() ? nullptr : found->second;
}

std::atomic_ref<std::uintptr_t> status_word(AsyncBlock* block)
{
    return std::atomic_ref<std::uintptr_t>(block->internal[1]);
}

std::atomic_ref<std::uintptr_t> size_word(AsyncBlock* block)
{
    return std::atomic_ref<std::uintptr_t>(block->internal[2]);
}

HResult status_of(AsyncBlock* block)
{
    return static_cast<HResult>(static_cast<std::uint32_t>(status_word(block).load(std::memory_order_acquire)));
}

void release_call(AsyncBlock* block)
{
    std::shared_ptr<Call> removed;
    {
        std::scoped_lock lock(calls_mutex());
        const auto found = calls().find(block);
        if (found != calls().end()) {
            removed = std::move(found->second);
            calls().erase(found);
        }
    }
    if (removed) {
        removed->invoke(kOpCleanup, block, 0, nullptr);
    }
}

void run_completion(void* context, bool)
{
    auto* block = static_cast<AsyncBlock*>(context);
    if (block->callback != nullptr) {
        block->callback(block);
    }
}

void run_work(void* context, bool canceled)
{
    auto* block = static_cast<AsyncBlock*>(context);
    const auto call = find_call(block);
    if (!call) {
        return;
    }
    const HResult result = canceled ? kAbort : call->invoke(kOpDoWork, block, 0, nullptr);
    if (result != kPending) {
        async_complete(block, result, 0);
    }
}

HResult finished_provider(std::uint32_t op, const ProviderData* data)
{
    auto* result = static_cast<std::pair<HResult, std::vector<std::uint8_t>>*>(data->context);
    switch (op) {
    case kOpCleanup:
        delete result;
        break;
    case kOpBegin: {
        const HResult hr = result->first;
        const std::size_t size = hr < 0 ? 0 : result->second.size();
        async_complete(data->block, hr, size);
        break;
    }
    case kOpGetResult:
        if (result->first >= 0 && !result->second.empty()) {
            std::memcpy(data->buffer, result->second.data(), result->second.size());
        }
        break;
    default:
        break;
    }
    return kOk;
}

HResult run_provider(std::uint32_t op, const ProviderData* data)
{
    switch (op) {
    case kOpBegin:
        return async_schedule(data->block, 0);
    case kOpDoWork:
        return reinterpret_cast<AsyncWork>(data->context)(data->block);
    default:
        return kOk;
    }
}

char g_run_identity = 0;

}

HResult task_queue_create(std::uint32_t work, std::uint32_t completion, void** out)
{
    if (!valid_mode(work) || !valid_mode(completion) || out == nullptr) {
        return kInvalidArgument;
    }
    *out = make_queue(make_port(static_cast<Mode>(work)), make_port(static_cast<Mode>(completion)));
    return kOk;
}

HResult task_queue_get_port(void* handle, std::uint32_t port, void** out)
{
    const QueueRef queue = acquire(handle);
    if (!queue || port > kPortCompletion || out == nullptr) {
        return kInvalidArgument;
    }
    *out = queue->ports[port].get();
    return kOk;
}

HResult task_queue_create_composite(void* work, void* completion, void** out)
{
    if (work == nullptr || completion == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    *out = make_queue(static_cast<Port*>(work)->shared_from_this(),
                      static_cast<Port*>(completion)->shared_from_this());
    return kOk;
}

HResult task_queue_duplicate(void* handle, void** out)
{
    QueueRef queue = acquire(handle);
    if (!queue || out == nullptr) {
        return kInvalidArgument;
    }
    *out = queue.detach();
    return kOk;
}

void task_queue_close(void* handle)
{
    if (handle == nullptr) {
        return;
    }
    auto* queue = static_cast<Queue*>(handle);
    if (queue->magic != kQueueMagic) {
        log::warn("xasync: closing unknown task queue handle");
        return;
    }
    release(queue);
}

bool task_queue_dispatch(void* handle, std::uint32_t port, std::uint32_t timeout_ms)
{
    const QueueRef queue = acquire(handle);
    if (!queue || port > kPortCompletion) {
        return false;
    }
    Port& target = *queue->ports[port];
    if (target.mode != Mode::Manual) {
        return false;
    }
    Queue* raw = queue.get();
    const auto task = target.take(timeout_ms, [raw] { return raw->terminated.load(std::memory_order_acquire); });
    if (!task) {
        return false;
    }
    task->run(false);
    return true;
}

HResult task_queue_submit(void* handle, std::uint32_t port, std::uint32_t delay_ms, void* context,
                          TaskCallback callback)
{
    const QueueRef queue = acquire(handle);
    if (!queue || callback == nullptr) {
        return kInvalidArgument;
    }
    return submit(queue.get(), port, delay_ms, Task{context, callback});
}

HResult task_queue_terminate(void* handle, bool, void* context, void (*callback)(void*))
{
    const QueueRef queue = acquire(handle);
    if (!queue) {
        return kInvalidArgument;
    }
    queue->terminated.store(true, std::memory_order_release);
    for (const auto& port : queue->ports) {
        std::deque<Task> pending;
        {
            std::scoped_lock lock(port->mutex);
            pending.swap(port->tasks);
        }
        port->ready.notify_all();
        for (const auto& task : pending) {
            task.run(true);
        }
    }
    if (callback != nullptr) {
        callback(context);
    }
    return kOk;
}

HResult task_queue_register_monitor(void* handle, void* context,
                                    void (*callback)(void*, void*, std::uint32_t),
                                    std::uint64_t* token)
{
    const QueueRef queue = acquire(handle);
    if (!queue || callback == nullptr || token == nullptr) {
        return kInvalidArgument;
    }
    const std::uint64_t id = g_next_monitor.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock(queue->monitor_mutex);
        queue->monitors.push_back(Monitor{id, context, callback});
    }
    *token = id;
    return kOk;
}

void task_queue_unregister_monitor(void* handle, std::uint64_t token)
{
    const QueueRef queue = acquire(handle);
    if (!queue) {
        return;
    }
    std::scoped_lock lock(queue->monitor_mutex);
    std::erase_if(queue->monitors, [token](const Monitor& monitor) { return monitor.token == token; });
}

bool task_queue_get_process_queue(void** out)
{
    QueueRef queue = process_queue();
    const bool found = static_cast<bool>(queue);
    if (out != nullptr) {
        *out = queue.detach();
    }
    return found;
}

void task_queue_set_process_queue(void* handle)
{
    Queue* replacement = nullptr;
    if (handle != nullptr) {
        replacement = acquire(handle).detach();
    }
    Queue* previous = nullptr;
    {
        std::scoped_lock lock(process_mutex());
        previous = g_process_set ? g_process_queue : nullptr;
        g_process_set = true;
        g_process_queue = replacement;
    }
    if (previous != nullptr) {
        release(previous);
    }
}

HResult async_begin(AsyncBlock* block, void* context, const void* identity, const char*,
                    AsyncProvider provider)
{
    if (provider == nullptr || block == nullptr) {
        return kInvalidArgument;
    }
    auto call = std::make_shared<Call>();
    call->provider = provider;
    call->context = context;
    call->identity = identity;
    {
        std::scoped_lock lock(calls_mutex());
        if (calls().contains(block)) {
            return kInvalidArgument;
        }
        status_word(block).store(static_cast<std::uint32_t>(kPending), std::memory_order_release);
        size_word(block).store(0, std::memory_order_release);
        calls().emplace(block, call);
    }
    const HResult result = call->invoke(kOpBegin, block, 0, nullptr);
    if (result < 0) {
        async_complete(block, result, 0);
    }
    return kOk;
}

HResult async_schedule(AsyncBlock* block, std::uint32_t delay_ms)
{
    if (block == nullptr || !find_call(block)) {
        return kInvalidArgument;
    }
    const QueueRef queue = acquire(block->queue);
    if (!queue) {
        return kInvalidArgument;
    }
    return submit(queue.get(), kPortWork, delay_ms, Task{block, &run_work});
}

void async_complete(AsyncBlock* block, HResult result, std::size_t required_size)
{
    if (result == kPending || block == nullptr) {
        return;
    }
    const auto call = find_call(block);
    if (!call) {
        return;
    }
    std::unique_lock lock(call->mutex);
    if (call->done) {
        return;
    }
    void* const queue_handle = block->queue;
    const CompletionRoutine routine = block->callback;
    if (result < 0 || required_size == 0) {
        release_call(block);
    }
    size_word(block).store(required_size, std::memory_order_release);
    status_word(block).store(static_cast<std::uint32_t>(result), std::memory_order_release);
    call->done = true;
    call->finished.notify_all();
    lock.unlock();
    if (routine != nullptr) {
        const Task task{block, &run_completion};
        const QueueRef queue = acquire(queue_handle);
        if (!queue || submit(queue.get(), kPortCompletion, 0, task) != kOk) {
            task.run(true);
        }
    }
}

HResult async_get_status(AsyncBlock* block, bool wait)
{
    if (block == nullptr) {
        return kInvalidArgument;
    }
    if (wait) {
        if (const auto call = find_call(block); call) {
            std::unique_lock lock(call->mutex);
            call->finished.wait(lock, [&] { return call->done; });
        }
    }
    return status_of(block);
}

HResult async_get_result_size(AsyncBlock* block, std::size_t* out)
{
    if (block == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    const HResult result = status_of(block);
    if (result >= 0) {
        *out = size_word(block).load(std::memory_order_acquire);
    }
    return result;
}

void async_cancel(AsyncBlock* block)
{
    if (const auto call = find_call(block); call) {
        call->invoke(kOpCancel, block, 0, nullptr);
        async_complete(block, kAbort, 0);
    }
}

HResult async_get_result(AsyncBlock* block, const void* identity, std::size_t size, void* buffer,
                         std::size_t* used)
{
    if (block == nullptr) {
        return kInvalidArgument;
    }
    const HResult result = status_of(block);
    const std::size_t required = size_word(block).load(std::memory_order_acquire);
    if (result < 0 || required == 0) {
        if (result >= 0 && used != nullptr) {
            *used = 0;
        }
        return result;
    }
    const auto call = find_call(block);
    if (!call) {
        return kIllegalMethodCall;
    }
    if (call->identity != identity) {
        return kInvalidArgument;
    }
    if (size < required || buffer == nullptr) {
        return kNotSufficientBuffer;
    }
    const HResult provided = call->invoke(kOpGetResult, block, size, buffer);
    if (used != nullptr) {
        *used = required;
    }
    release_call(block);
    return provided;
}

HResult async_run(AsyncBlock* block, AsyncWork work)
{
    if (work == nullptr) {
        return kInvalidArgument;
    }
    return async_begin(block, reinterpret_cast<void*>(work), &g_run_identity, nullptr, &run_provider);
}

HResult async_finished(AsyncBlock* block, const void* identity, HResult result)
{
    auto* data = new std::pair<HResult, std::vector<std::uint8_t>>(result, std::vector<std::uint8_t>{});
    const HResult begun = async_begin(block, data, identity, nullptr, &finished_provider);
    if (begun < 0) {
        delete data;
    }
    return begun;
}

HResult async_finished_with(AsyncBlock* block, const void* identity, std::vector<std::uint8_t> bytes)
{
    auto* data = new std::pair<HResult, std::vector<std::uint8_t>>(kOk, std::move(bytes));
    const HResult begun = async_begin(block, data, identity, nullptr, &finished_provider);
    if (begun < 0) {
        delete data;
    }
    return begun;
}

namespace {

using This = const Object*;

HResult m_get_status(This, AsyncBlock* block, std::uint8_t wait)
{
    return async_get_status(block, wait != 0);
}

HResult m_get_result_size(This, AsyncBlock* block, std::size_t* out)
{
    return async_get_result_size(block, out);
}

void m_cancel(This, AsyncBlock* block)
{
    async_cancel(block);
}

HResult m_run(This, AsyncBlock* block, AsyncWork work)
{
    log_first("XAsyncRun");
    return async_run(block, work);
}

HResult m_begin(This, AsyncBlock* block, void* context, const void* identity, const char* name,
                AsyncProvider provider)
{
    return async_begin(block, context, identity, name, provider);
}

HResult m_schedule(This, AsyncBlock* block, std::uint32_t delay)
{
    return async_schedule(block, delay);
}

void m_complete(This, AsyncBlock* block, HResult result, std::size_t size)
{
    async_complete(block, result, size);
}

HResult m_get_result(This, AsyncBlock* block, const void* identity, std::size_t size, void* buffer,
                     std::size_t* used)
{
    return async_get_result(block, identity, size, buffer, used);
}

HResult m_create(This, std::uint32_t work, std::uint32_t completion, void** out)
{
    log_first("XTaskQueueCreate");
    return task_queue_create(work, completion, out);
}

HResult m_create_composite(This, void* work, void* completion, void** out)
{
    log_first("XTaskQueueCreateComposite");
    return task_queue_create_composite(work, completion, out);
}

HResult m_get_port(This, void* queue, std::uint32_t port, void** out)
{
    return task_queue_get_port(queue, port, out);
}

HResult m_duplicate(This, void* queue, void** out)
{
    return task_queue_duplicate(queue, out);
}

bool m_dispatch(This, void* queue, std::uint32_t port, std::uint32_t timeout)
{
    return task_queue_dispatch(queue, port, timeout);
}

void m_close(This, void* queue)
{
    task_queue_close(queue);
}

HResult m_submit(This, void* queue, std::uint32_t port, void* context, TaskCallback callback)
{
    return task_queue_submit(queue, port, 0, context, callback);
}

HResult m_submit_delayed(This, void* queue, std::uint32_t port, std::uint32_t delay, void* context,
                         TaskCallback callback)
{
    return task_queue_submit(queue, port, delay, context, callback);
}

HResult m_terminate(This, void* queue, std::uint8_t wait, void* context, void (*callback)(void*))
{
    log_first("XTaskQueueTerminate");
    return task_queue_terminate(queue, wait != 0, context, callback);
}

HResult m_register_monitor(This, void* queue, void* context, void (*callback)(void*, void*, std::uint32_t),
                           std::uint64_t* token)
{
    return task_queue_register_monitor(queue, context, callback, token);
}

void m_unregister_monitor(This, void* queue, std::uint64_t token)
{
    task_queue_unregister_monitor(queue, token);
}

bool m_get_process_queue(This, void** out)
{
    return task_queue_get_process_queue(out);
}

void m_set_process_queue(This, void* queue)
{
    log_first("XTaskQueueSetCurrentProcessTaskQueue");
    task_queue_set_process_queue(queue);
}

void m_check_thread(This)
{
}

const Table<31> kTable = [] {
    auto t = table<31>();
    set(t, 0x18, &m_get_status);
    set(t, 0x20, &m_get_result_size);
    set(t, 0x28, &m_cancel);
    set(t, 0x30, &m_run);
    set(t, 0x38, &m_begin);
    set(t, 0x48, &m_schedule);
    set(t, 0x50, &m_complete);
    set(t, 0x58, &m_get_result);
    set(t, 0x60, &m_create);
    set(t, 0x68, &m_create_composite);
    set(t, 0x70, &m_get_port);
    set(t, 0x78, &m_duplicate);
    set(t, 0x80, &m_dispatch);
    set(t, 0x88, &m_close);
    set(t, 0x90, &m_submit);
    set(t, 0x98, &m_submit_delayed);
    set(t, 0xb0, &m_terminate);
    set(t, 0xb8, &m_register_monitor);
    set(t, 0xc0, &m_unregister_monitor);
    set(t, 0xc8, &m_get_process_queue);
    set(t, 0xd0, &m_set_process_queue);
    set(t, 0xe8, &m_check_thread);
    return t;
}();

const Object kObject{kTable.data(), "task queue"};

constexpr Guid kProvider{0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

const std::array<Interface, 1> kInterfaces{{
    {kProvider, kProvider, &kObject},
}};

}

std::span<const Interface> task_queue_interfaces()
{
    return kInterfaces;
}

}
