#include <euphoria/formats/xma.hpp>
#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <malloc.h>

namespace euphoria::shim {
namespace {

namespace xma = formats::xma;

using HResult = std::int32_t;
using Clock = std::chrono::steady_clock;

constexpr HResult kOk = 0;
constexpr HResult kFail = static_cast<HResult>(0x80004005u);
constexpr HResult kInvalidArg = static_cast<HResult>(0x80070057u);
constexpr HResult kOutOfMemory = static_cast<HResult>(0x8007000Eu);
constexpr HResult kHeapAlreadyAllocated = static_cast<HResult>(0x8ACA0001u);

constexpr std::uint32_t kApuBase = 0x00010000;
constexpr std::uint32_t kApuEnd = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kBigAllocation = std::size_t{32} << 20;
constexpr std::uint32_t kLogLimit = 8;
constexpr std::uint32_t kFailureLogLimit = 32;

bool budget(std::atomic<std::uint32_t>& counter, std::uint32_t limit = kLogLimit) noexcept
{
    return counter.fetch_add(1, std::memory_order_relaxed) < limit;
}

std::string hex(std::uint64_t value)
{
    return std::format("{:#x}", value);
}

std::string pointer(const void* value)
{
    return hex(reinterpret_cast<std::uintptr_t>(value));
}

struct Block {
    std::size_t size;
    std::uint32_t apu;
    std::uint32_t reserved;
};

struct Heap {
    std::mutex mutex;
    std::map<std::uintptr_t, Block> blocks;
    std::map<std::uint32_t, std::uintptr_t> by_apu;
    std::map<std::uint32_t, std::uint32_t> free;
    bool created = false;

    std::optional<std::uint32_t> take(std::uint32_t size, std::uint32_t align)
    {
        if (free.empty() && !created) {
            free.emplace(kApuBase, kApuEnd);
        }
        created = true;
        const std::uint64_t step = align == 0 ? 1 : align;
        for (const auto& [start, end] : free) {
            const std::uint64_t apu = (std::uint64_t{start} + step - 1) / step * step;
            const std::uint64_t limit = apu + size;
            if (apu > kApuEnd || limit > kApuEnd || limit > end) {
                continue;
            }
            const std::uint32_t range_start = start;
            const std::uint32_t range_end = end;
            free.erase(range_start);
            if (range_start < apu) {
                free.emplace(range_start, static_cast<std::uint32_t>(apu));
            }
            if (limit < range_end) {
                free.emplace(static_cast<std::uint32_t>(limit), range_end);
            }
            return static_cast<std::uint32_t>(apu);
        }
        return std::nullopt;
    }

    void give_back(std::uint32_t start, std::uint32_t end)
    {
        auto before = free.lower_bound(start);
        if (before != free.begin()) {
            --before;
            if (before->second == start) {
                start = before->first;
                free.erase(before);
            }
        }
        if (const auto after = free.find(end); after != free.end()) {
            end = after->second;
            free.erase(after);
        }
        free.emplace(start, end);
    }
};

Heap& heap()
{
    static Heap* instance = new Heap;
    return *instance;
}

std::optional<std::pair<std::uint8_t*, std::uint32_t>> heap_alloc(std::size_t size, std::size_t align)
{
    const std::size_t bytes = std::max<std::size_t>(size, 1);
    const std::size_t host_align = std::max<std::size_t>(align, 16);
    if (bytes > kApuEnd || align > kApuEnd) {
        return std::nullopt;
    }
    auto* memory = static_cast<std::uint8_t*>(_aligned_malloc(bytes, host_align));
    if (memory == nullptr) {
        return std::nullopt;
    }
    std::memset(memory, 0, bytes);
    std::uint32_t apu = 0;
    {
        Heap& h = heap();
        std::scoped_lock lock(h.mutex);
        const std::optional<std::uint32_t> address =
            h.take(static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(align));
        if (!address) {
            _aligned_free(memory);
            return std::nullopt;
        }
        apu = *address;
        const auto host = reinterpret_cast<std::uintptr_t>(memory);
        h.blocks.emplace(host, Block{size, apu, static_cast<std::uint32_t>(bytes)});
        h.by_apu.emplace(apu, host);
    }
    if (size >= kBigAllocation) {
        log::info(std::format("acphal: {} MiB of audio memory at {} (device {:#x})", size >> 20, pointer(memory), apu));
    }
    return std::pair{memory, apu};
}

void heap_free(void* memory)
{
    if (memory == nullptr) {
        return;
    }
    {
        Heap& h = heap();
        std::scoped_lock lock(h.mutex);
        const auto found = h.blocks.find(reinterpret_cast<std::uintptr_t>(memory));
        if (found == h.blocks.end()) {
            return;
        }
        const Block block = found->second;
        h.blocks.erase(found);
        h.by_apu.erase(block.apu);
        h.give_back(block.apu, block.apu + block.reserved);
    }
    _aligned_free(memory);
}

std::uint32_t heap_map(std::uintptr_t address)
{
    Heap& h = heap();
    std::scoped_lock lock(h.mutex);
    auto it = h.blocks.upper_bound(address);
    if (it == h.blocks.begin()) {
        return 0;
    }
    --it;
    const std::uintptr_t start = it->first;
    const Block& block = it->second;
    if (address < start + std::max<std::size_t>(block.size, 1)) {
        return block.apu + static_cast<std::uint32_t>(address - start);
    }
    return 0;
}

std::uint8_t* heap_unmap(std::uint32_t apu)
{
    Heap& h = heap();
    std::scoped_lock lock(h.mutex);
    auto it = h.by_apu.upper_bound(apu);
    if (it == h.by_apu.begin()) {
        return nullptr;
    }
    --it;
    const std::uint32_t start = it->first;
    const std::uintptr_t host = it->second;
    const Block& block = h.blocks.at(host);
    if (std::uint64_t{apu} < std::uint64_t{start} + std::max<std::size_t>(block.size, 1)) {
        return reinterpret_cast<std::uint8_t*>(host + (apu - start));
    }
    return nullptr;
}

std::uint8_t* heap_bytes(std::uint32_t apu, std::size_t length)
{
    Heap& h = heap();
    std::scoped_lock lock(h.mutex);
    auto it = h.by_apu.upper_bound(apu);
    if (it == h.by_apu.begin()) {
        return nullptr;
    }
    --it;
    const std::size_t offset = apu - it->first;
    const std::uintptr_t host = it->second;
    if (offset + length <= h.blocks.at(host).size) {
        return reinterpret_cast<std::uint8_t*>(host + offset);
    }
    return nullptr;
}

struct ShapeContexts {
    std::uint32_t counts[6];
    void* arrays[6];
    std::uint32_t apu[6];
};
static_assert(sizeof(ShapeContexts) == 0x60);
static_assert(offsetof(ShapeContexts, arrays) == 0x18);
static_assert(offsetof(ShapeContexts, apu) == 0x48);

constexpr std::array<std::size_t, 6> kContextBytes{32, 256, 256, 16, 64, 256};
constexpr std::size_t kXmaKind = 4;

struct Contexts {
    std::mutex mutex;
    std::optional<std::array<void*, 6>> arrays;
};

Contexts& contexts()
{
    static Contexts* instance = new Contexts;
    return *instance;
}

constexpr std::uint32_t kEnableXma = 5;
constexpr std::uint32_t kEnableXmas = 6;
constexpr std::uint32_t kDisableXma = 7;
constexpr std::uint32_t kDisableXmas = 8;
constexpr std::uint32_t kAdvanceXmaRead = 18;

constexpr std::size_t kBlockBytes = 256;
constexpr std::size_t kSamplesPerBlock = kBlockBytes / 2;
constexpr std::uint32_t kInputInvalid = 4;
constexpr std::array<std::uint32_t, 4> kRates{24000, 32000, 44100, 48000};

struct Trace {
    Clock::time_point since;
    std::size_t produced = 0;
    std::size_t taken = 0;
    std::uint32_t takes = 0;
    std::uint32_t partial_takes = 0;
    std::uint32_t frames = 0;
    std::uint32_t bad_frames = 0;
    std::int32_t peak = 0;
    std::optional<long long> ended_ms;
};

struct Voice {
    std::optional<xma::Decoder> decoder;
    std::deque<std::int16_t> pcm;
    std::size_t skip = 0;
    bool ended = false;
    Trace trace;
};

struct Slot {
    std::mutex mutex;
    std::optional<Voice> voice;
};

class Context {
public:
    explicit Context(std::uintptr_t address) noexcept : words_(reinterpret_cast<volatile std::uint32_t*>(address)) {}

    std::uint32_t get(std::size_t word) const noexcept
    {
        return words_[word];
    }

    void set(std::size_t word, std::uint32_t value) const noexcept
    {
        words_[word] = value;
    }

    std::uint32_t field(std::size_t word, std::uint32_t shift, std::uint32_t bits) const noexcept
    {
        return (get(word) >> shift) & ((1u << bits) - 1);
    }

    void set_field(std::size_t word, std::uint32_t shift, std::uint32_t bits, std::uint32_t value) const noexcept
    {
        const std::uint32_t mask = ((1u << bits) - 1) << shift;
        set(word, (get(word) & ~mask) | ((value << shift) & mask));
    }

    std::size_t channels() const noexcept
    {
        return 1 + field(1, 29, 1);
    }

private:
    volatile std::uint32_t* words_;
};

struct Xma {
    std::shared_mutex mutex;
    std::uintptr_t array = 0;
    std::vector<std::unique_ptr<Slot>> voices;

    std::optional<Context> context(std::uint32_t index) const noexcept
    {
        if (index >= voices.size() || array == 0) {
            return std::nullopt;
        }
        return Context(array + std::size_t{index} * 64);
    }
};

Xma& xma_state()
{
    static Xma* instance = new Xma;
    return *instance;
}

void wake_decoder();

long long elapsed_ms(Clock::time_point since)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

void xma_enable(Xma& x, std::uint32_t index)
{
    const std::optional<Context> context = x.context(index);
    if (!context) {
        return;
    }
    {
        std::scoped_lock lock(x.voices[index]->mutex);
        context->set_field(2, 26, 6, 0);
        if (log::verbose()) {
            std::string words;
            for (std::size_t w = 0; w < 10; ++w) {
                words += std::format("{}{:08x}", w == 0 ? "" : " ", context->get(w));
            }
            log::debug(std::format("acphal: XMA voice {} enabled, words {}", index, words));
        }
        Voice voice;
        voice.skip = std::size_t{context->field(1, 24, 3)} * xma::kSubframeSamples;
        voice.trace.since = Clock::now();
        x.voices[index]->voice = std::move(voice);
    }
    wake_decoder();
}

void xma_disable(Xma& x, std::uint32_t index)
{
    const std::optional<Context> context = x.context(index);
    if (!context) {
        return;
    }
    std::optional<Voice> voice;
    {
        std::scoped_lock lock(x.voices[index]->mutex);
        voice = std::move(x.voices[index]->voice);
        x.voices[index]->voice.reset();
    }
    if (!voice || !log::verbose()) {
        return;
    }
    const Trace& t = voice->trace;
    log::debug(std::format("acphal: XMA voice {} disabled after {} ms, produced {} taken {} takes {} partial {} "
                           "frames {} bad {} peak {} input ended {} read bit {} input bits {}",
                           index, elapsed_ms(t.since), t.produced, t.taken * kSamplesPerBlock / context->channels(),
                           t.takes, t.partial_takes, t.frames, t.bad_frames, t.peak,
                           t.ended_ms ? std::format("{} ms", *t.ended_ms) : std::string("never"),
                           context->field(2, 0, 26), context->field(0, 0, 12) * xma::kPacketBytes * 8));
}

void xma_advance_read(Xma& x, std::uint32_t index, std::uint32_t blocks)
{
    const std::optional<Context> context = x.context(index);
    if (!context) {
        return;
    }
    {
        std::scoped_lock lock(x.voices[index]->mutex);
        const std::uint32_t size = std::max<std::uint32_t>(context->field(0, 22, 5), 1);
        const std::uint32_t read = (context->field(9, 0, 5) + blocks % size) % size;
        context->set_field(9, 0, 5, read);
        if (blocks > 0) {
            context->set_field(1, 31, 1, 1);
        }
        if (std::optional<Voice>& voice = x.voices[index]->voice; voice) {
            Trace& t = voice->trace;
            t.taken += blocks;
            ++t.takes;
            t.partial_takes += blocks < size ? 1u : 0u;
        }
    }
    wake_decoder();
}

bool xma_decode(const Context& context, Voice& voice)
{
    if (voice.ended) {
        return false;
    }
    const std::size_t channels = context.channels();
    std::uint32_t current = context.field(4, 31, 1);
    const std::uint32_t valid = context.field(0, 20, 2);
    if ((valid & (1u << current)) == 0) {
        if ((valid & (1u << (1 - current))) == 0) {
            if (voice.decoder) {
                std::vector<std::int16_t> tail;
                voice.decoder->flush(tail);
                voice.pcm.insert(voice.pcm.end(), tail.begin(), tail.end());
            }
            voice.ended = true;
            voice.trace.ended_ms = elapsed_ms(voice.trace.since);
            context.set_field(2, 26, 5, kInputInvalid);
            context.set_field(2, 31, 1, 1);
            return !voice.pcm.empty();
        }
        current = 1 - current;
        context.set_field(4, 31, 1, current);
        context.set_field(2, 0, 26, 0);
    }
    const std::uint32_t address = current == 0 ? context.get(5) : context.get(6);
    const std::uint32_t packets = current == 0 ? context.field(0, 0, 12) : context.field(1, 0, 12);
    const std::size_t length = std::size_t{packets} * xma::kPacketBytes;
    const std::uint8_t* bytes = heap_bytes(address & ~0x7ffu, length);
    if (bytes == nullptr) {
        context.set_field(0, 20 + current, 1, 0);
        return true;
    }
    const std::span<const std::uint8_t> data(bytes, length);
    std::size_t at = context.field(2, 0, 26);
    if (at < 32) {
        const std::optional<xma::Packet> first = xma::packet(data, 0);
        at = 32 + (first ? first->first_frame_bit : 0u);
    }
    const std::optional<xma::Frame> frame = xma::read_frame(data, at);
    if (!frame) {
        context.set_field(0, 20 + current, 1, 0);
        return true;
    }
    if (!voice.decoder) {
        voice.decoder.emplace(channels, kRates[context.field(1, 27, 2)]);
    }
    std::vector<std::int16_t> pcm;
    pcm.reserve(xma::kFrameSamples * channels);
    ++voice.trace.frames;
    const auto decoded = voice.decoder->decode(frame->data, frame->bits, pcm);
    if (!decoded) {
        ++voice.trace.bad_frames;
        if (voice.trace.bad_frames <= 2) {
            std::string head;
            for (std::size_t i = 0; i < std::min<std::size_t>(frame->data.size(), 8); ++i) {
                head += std::format("{:02x}", frame->data[i]);
            }
            log::debug(std::format("acphal: XMA frame at bit {} ({} bits, {}) did not decode: {}", at, frame->bits,
                                   head, decoded.error()));
        }
    }
    std::size_t next = frame->next.value_or(0);
    const std::uint32_t loops = context.field(0, 12, 8);
    const std::size_t loop_end = context.field(4, 0, 26);
    const bool ends_loop = loop_end != 0
        && (frame->start == loop_end
            || (frame->start < loop_end && (!frame->next || loop_end < *frame->next)));
    if (loops > 0 && ends_loop) {
        const std::size_t keep = (std::size_t{context.field(1, 12, 2)} + 1) * xma::kSubframeSamples * channels;
        if (pcm.size() > keep) {
            pcm.resize(keep);
        }
        next = context.field(3, 0, 26);
        voice.skip = std::size_t{context.field(1, 17, 3)} * xma::kSubframeSamples;
        if (loops != 255) {
            context.set_field(0, 12, 8, loops - 1);
        }
    }
    const std::size_t skip = std::min(voice.skip * channels, pcm.size());
    voice.skip -= skip / channels;
    voice.pcm.insert(voice.pcm.end(), pcm.begin() + static_cast<std::ptrdiff_t>(skip), pcm.end());
    if (next == 0) {
        context.set_field(0, 20 + current, 1, 0);
    }
    context.set_field(2, 0, 26, static_cast<std::uint32_t>(next));
    return true;
}

void xma_pump_voice(const Context& context, Voice& voice)
{
    const std::uint32_t size = context.field(0, 22, 5);
    if (size == 0) {
        return;
    }
    while (context.field(1, 31, 1) == 1) {
        if (voice.pcm.size() < kSamplesPerBlock && !xma_decode(context, voice)) {
            return;
        }
        const std::uint32_t write = context.field(0, 27, 5);
        const std::uint64_t address = std::uint64_t{context.get(7)} + std::uint64_t{write} * kBlockBytes;
        if (address > kApuEnd) {
            return;
        }
        std::uint8_t* out = heap_bytes(static_cast<std::uint32_t>(address), kBlockBytes);
        if (out == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < kSamplesPerBlock; ++i) {
            std::int16_t sample = 0;
            if (!voice.pcm.empty()) {
                sample = voice.pcm.front();
                voice.pcm.pop_front();
            }
            const std::int32_t magnitude = sample == std::numeric_limits<std::int16_t>::min() ? 32767 : std::abs(sample);
            voice.trace.peak = std::max(voice.trace.peak, magnitude);
            std::memcpy(out + i * 2, &sample, sizeof(sample));
        }
        voice.trace.produced += kSamplesPerBlock / context.channels();
        const std::uint32_t next = (write + 1) % size;
        context.set_field(0, 27, 5, next);
        if (next == context.field(9, 0, 5)) {
            context.set_field(1, 31, 1, 0);
        }
    }
}

void xma_pump(Xma& x)
{
    const Clock::time_point start = Clock::now();
    std::size_t active = 0;
    for (std::uint32_t index = 0; index < x.voices.size(); ++index) {
        const std::optional<Context> context = x.context(index);
        if (!context) {
            continue;
        }
        std::scoped_lock lock(x.voices[index]->mutex);
        if (std::optional<Voice>& voice = x.voices[index]->voice; voice) {
            xma_pump_voice(*context, *voice);
            ++active;
        }
    }
    const long long took = elapsed_ms(start);
    if (took >= 4) {
        log::debug(std::format("acphal: slow XMA decoding pass, {} voices in {} ms", active, took));
    }
}

struct Waker {
    std::mutex mutex;
    std::condition_variable condition;
    bool woken = false;
    std::once_flag started;
};

Waker& waker()
{
    static Waker* instance = new Waker;
    return *instance;
}

void decoder_thread()
{
    Waker& w = waker();
    Xma& x = xma_state();
    for (;;) {
        {
            std::shared_lock lock(x.mutex);
            xma_pump(x);
        }
        std::unique_lock lock(w.mutex);
        w.condition.wait_for(lock, std::chrono::milliseconds(2), [&w] { return w.woken; });
        w.woken = false;
    }
}

void wake_decoder()
{
    Waker& w = waker();
    std::call_once(w.started, [] {
        std::thread(&decoder_thread).detach();
        log::info("acphal: XMA decoder thread started");
    });
    {
        std::scoped_lock lock(w.mutex);
        w.woken = true;
    }
    w.condition.notify_one();
}

template <typename Word>
bool xma_command(std::uint32_t command, Word word)
{
    Xma& x = xma_state();
    std::shared_lock lock(x.mutex);
    switch (command) {
    case kEnableXma:
        xma_enable(x, word(0).value_or(kApuEnd));
        return true;
    case kDisableXma:
        xma_disable(x, word(0).value_or(kApuEnd));
        return true;
    case kEnableXmas:
    case kDisableXmas:
        for (std::uint32_t i = 0; i < x.voices.size(); ++i) {
            if ((word(i / 32).value_or(0) & (1u << (i % 32))) != 0) {
                if (command == kEnableXmas) {
                    xma_enable(x, i);
                } else {
                    xma_disable(x, i);
                }
            }
        }
        return true;
    case kAdvanceXmaRead:
        xma_advance_read(x, word(0).value_or(kApuEnd), word(1).value_or(0));
        return true;
    default:
        return false;
    }
}

constexpr std::uint32_t kLoadFlowgraph = 1;
constexpr std::uint32_t kRegisterMessages = 2;
constexpr std::uint32_t kUnregisterMessages = 3;

constexpr std::uint32_t kFlowgraphCompleted = 0x02;
constexpr std::uint32_t kCommandCompleted = 0x10;
constexpr std::uint32_t kDisconnected = 0x80;

constexpr std::uint64_t kFramesPerSecond = 375;

struct Message {
    std::uint32_t kind;
    std::uint32_t dropped;
    std::uint32_t usec;
    std::uint8_t data[16];
};
static_assert(sizeof(Message) == 28);
static_assert(offsetof(Message, data) == 12);

struct AcpState {
    bool connected = false;
    std::uint32_t mask = 0;
    std::deque<Message> messages;
    std::size_t capacity = 0;
    std::uint32_t dropped = 0;
    Clock::time_point since = Clock::now();

    std::uint64_t usec() const
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - since).count());
    }

    std::uint32_t frame() const
    {
        return static_cast<std::uint32_t>(usec() * kFramesPerSecond / 1000000);
    }

    void post(std::uint32_t kind, std::span<const std::uint8_t> data)
    {
        if ((mask & kind) == 0) {
            return;
        }
        if (messages.size() >= capacity) {
            if (dropped != std::numeric_limits<std::uint32_t>::max()) {
                ++dropped;
            }
            static std::atomic<std::uint32_t> logged{0};
            if (budget(logged, kFailureLogLimit)) {
                log::debug(std::format("acphal: coprocessor message {:#x} dropped ({} dropped)", kind, dropped));
            }
            return;
        }
        Message message{};
        message.kind = kind;
        message.dropped = std::exchange(dropped, 0u);
        message.usec = static_cast<std::uint32_t>(usec());
        std::memcpy(message.data, data.data(), std::min(data.size(), sizeof(message.data)));
        messages.push_back(message);
    }
};

struct Acp {
    const void* const* vtable;
    std::mutex mutex;
    AcpState state;
};

HResult acp_connect(Acp* self, std::uint32_t commands, std::uint32_t messages)
{
    log::info(std::format("acphal: coprocessor {} connect commands {:#x} messages {:#x}", pointer(self), commands,
                          messages));
    if (commands == 0) {
        log::warn("acphal: coprocessor connect with no command queue");
        return kInvalidArg;
    }
    std::scoped_lock lock(self->mutex);
    self->state.connected = true;
    self->state.capacity = messages;
    return kOk;
}

HResult acp_disconnect(Acp* self)
{
    log::info(std::format("acphal: coprocessor {} disconnect", pointer(self)));
    std::scoped_lock lock(self->mutex);
    if (!self->state.connected) {
        return kFail;
    }
    self->state.post(kDisconnected, {});
    self->state.connected = false;
    return kOk;
}

HResult acp_submit(Acp* self, std::uint32_t command, std::uint64_t id, std::uint32_t frame, const void* data,
                   std::uint32_t notification)
{
    static std::atomic<std::uint64_t> seen{0};
    if (command < 64) {
        const std::uint64_t bit = std::uint64_t{1} << command;
        if ((seen.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
            log::info(std::format("acphal: first coprocessor command {} id {:#x} frame {:#x} data {} notification {:#x}",
                                  command, id, frame, pointer(data), notification));
        }
    } else {
        static std::atomic<std::uint32_t> unknown{0};
        if (budget(unknown, kFailureLogLimit)) {
            log::warn(std::format("acphal: coprocessor command {} out of range", command));
        }
    }
    const auto word = [data](std::size_t at) -> std::optional<std::uint32_t> {
        if (data == nullptr) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        std::memcpy(&value, static_cast<const std::uint8_t*>(data) + at * sizeof(value), sizeof(value));
        return value;
    };
    std::scoped_lock lock(self->mutex);
    AcpState& state = self->state;
    switch (command) {
    case kRegisterMessages:
        state.mask |= word(0).value_or(0);
        break;
    case kUnregisterMessages:
        state.mask &= ~word(0).value_or(0);
        break;
    case kLoadFlowgraph: {
        const std::uint32_t flowgraph = word(1).value_or(0);
        std::array<std::uint8_t, 4> bytes{};
        std::memcpy(bytes.data(), &flowgraph, sizeof(flowgraph));
        state.post(kFlowgraphCompleted, bytes);
        break;
    }
    default:
        xma_command(command, word);
        break;
    }
    if (id != 0) {
        std::array<std::uint8_t, 16> done{};
        const std::uint32_t at = state.frame();
        std::memcpy(done.data(), &command, 4);
        std::memcpy(done.data() + 4, &id, 8);
        std::memcpy(done.data() + 12, &at, 4);
        state.post(kCommandCompleted, done);
    }
    if (notification != 0) {
        if (std::uint8_t* target = heap_unmap(notification); target != nullptr) {
            *reinterpret_cast<volatile std::uint32_t*>(target) = 1;
        } else {
            static std::atomic<std::uint32_t> missing{0};
            if (budget(missing, kFailureLogLimit)) {
                log::warn(std::format("acphal: notification {:#x} of command {} is outside the APU heap", notification,
                                      command));
            }
        }
    }
    return kOk;
}

std::int32_t acp_pop_message(Acp* self, Message* out)
{
    static std::atomic<std::uint32_t> logged{0};
    std::optional<Message> message;
    {
        std::scoped_lock lock(self->mutex);
        if (!self->state.messages.empty()) {
            message = self->state.messages.front();
            self->state.messages.pop_front();
        }
    }
    if (budget(logged)) {
        log::info(std::format("acphal: coprocessor pop message -> {}",
                              message ? std::format("kind {:#x} dropped {}", message->kind, message->dropped)
                                      : std::string("none")));
    }
    if (!message) {
        return 0;
    }
    if (out != nullptr) {
        std::memcpy(out, &*message, sizeof(Message));
    }
    return 1;
}

std::uint32_t acp_message_count(Acp* self)
{
    static std::atomic<std::uint32_t> logged{0};
    std::uint32_t count = 0;
    {
        std::scoped_lock lock(self->mutex);
        count = static_cast<std::uint32_t>(self->state.messages.size());
    }
    if (budget(logged)) {
        log::info(std::format("acphal: coprocessor message count -> {}", count));
    }
    return count;
}

void acp_release(Acp* self)
{
    log::info(std::format("acphal: coprocessor {} released", pointer(self)));
    delete self;
}

template <typename Function>
const void* slot(Function function) noexcept
{
    return reinterpret_cast<const void*>(function);
}

const std::array<const void*, 6> kAcpVtable{
    slot(&acp_connect),
    slot(&acp_disconnect),
    slot(&acp_submit),
    slot(&acp_pop_message),
    slot(&acp_message_count),
    slot(&acp_release),
};

HResult apu_create_heap(std::uint32_t cached, std::uint32_t non_cached)
{
    log::info(std::format("acphal: ApuCreateHeap cached {:#x} non-cached {:#x}", cached, non_cached));
    if (cached == 0) {
        return kInvalidArg;
    }
    Heap& h = heap();
    std::scoped_lock lock(h.mutex);
    if (h.created) {
        log::warn("acphal: ApuCreateHeap after the heap was created");
        return kHeapAlreadyAllocated;
    }
    h.created = true;
    h.free.emplace(kApuBase, kApuEnd);
    return kOk;
}

HResult apu_alloc(void** out, std::uint32_t* apu, std::uint32_t size, std::uint32_t align, std::uint32_t flags)
{
    static std::atomic<std::uint32_t> logged{0};
    static std::atomic<std::uint32_t> failures{0};
    if (out == nullptr || size == 0 || align < 4 || (align & (align - 1)) != 0) {
        if (budget(failures, kFailureLogLimit)) {
            log::warn(std::format("acphal: ApuAlloc rejected size {:#x} align {:#x} flags {:#x}", size, align, flags));
        }
        return kInvalidArg;
    }
    const auto allocated = heap_alloc(size, align);
    if (!allocated) {
        if (budget(failures, kFailureLogLimit)) {
            log::warn(std::format("acphal: ApuAlloc out of memory size {:#x} align {:#x} flags {:#x}", size, align,
                                  flags));
        }
        return kOutOfMemory;
    }
    *out = allocated->first;
    if (apu != nullptr) {
        *apu = allocated->second;
    }
    if (budget(logged)) {
        log::info(std::format("acphal: ApuAlloc size {:#x} align {:#x} flags {:#x} -> {} device {:#x}", size, align,
                              flags, pointer(allocated->first), allocated->second));
    }
    return kOk;
}

void apu_free(void* memory)
{
    static std::atomic<std::uint32_t> logged{0};
    if (budget(logged)) {
        log::info("acphal: ApuFree " + pointer(memory));
    }
    heap_free(memory);
}

void* apu_map_apu_address(std::uint32_t apu)
{
    static std::atomic<std::uint32_t> logged{0};
    std::uint8_t* host = heap_unmap(apu);
    if (budget(logged)) {
        log::info(std::format("acphal: ApuMapApuAddress {:#x} -> {}", apu, pointer(host)));
    }
    return host;
}

std::uint32_t apu_map_virtual_address(const void* address)
{
    static std::atomic<std::uint32_t> logged{0};
    const std::uint32_t apu = heap_map(reinterpret_cast<std::uintptr_t>(address));
    if (budget(logged)) {
        log::info(std::format("acphal: ApuMapVirtualAddress {} -> {:#x}", pointer(address), apu));
    }
    return apu;
}

std::int32_t apu_is_virtual_address_valid(const void* address, std::uint32_t align)
{
    static std::atomic<std::uint32_t> logged{0};
    const std::uint32_t apu = heap_map(reinterpret_cast<std::uintptr_t>(address));
    const bool valid = apu != 0 && (align == 0 || apu % align == 0);
    if (budget(logged)) {
        log::info(std::format("acphal: ApuIsVirtualAddressValid {} align {:#x} -> {}", pointer(address), align, valid));
    }
    return valid ? 1 : 0;
}

HResult acp_hal_allocate_shape_contexts(ShapeContexts* request)
{
    if (request == nullptr) {
        log::warn("acphal: AcpHalAllocateShapeContexts without a request");
        return kInvalidArg;
    }
    Contexts& c = contexts();
    std::scoped_lock lock(c.mutex);
    std::array<std::uint32_t, 6> counts{};
    std::memcpy(counts.data(), reinterpret_cast<const std::uint8_t*>(request) + offsetof(ShapeContexts, counts),
                sizeof(counts));
    log::info(std::format("acphal: AcpHalAllocateShapeContexts counts {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", counts[0],
                          counts[1], counts[2], counts[3], counts[4], counts[5]));
    if (c.arrays) {
        log::warn("acphal: SHAPE contexts are already allocated");
        return kFail;
    }
    std::array<void*, 6> arrays{};
    std::array<std::uint32_t, 6> addresses{};
    for (std::size_t i = 0; i < 6; ++i) {
        if (counts[i] == 0) {
            continue;
        }
        const auto allocated = heap_alloc(std::size_t{counts[i]} * kContextBytes[i], 256);
        if (!allocated) {
            for (void* array : arrays) {
                heap_free(array);
            }
            log::warn(std::format("acphal: no memory for {} SHAPE contexts of kind {}", counts[i], i));
            return kOutOfMemory;
        }
        arrays[i] = allocated->first;
        addresses[i] = allocated->second;
    }
    auto* base = reinterpret_cast<std::uint8_t*>(request);
    std::memcpy(base + offsetof(ShapeContexts, arrays), arrays.data(), sizeof(arrays));
    std::memcpy(base + offsetof(ShapeContexts, apu), addresses.data(), sizeof(addresses));
    c.arrays = arrays;
    {
        Xma& x = xma_state();
        std::unique_lock xma_lock(x.mutex);
        x.array = reinterpret_cast<std::uintptr_t>(arrays[kXmaKind]);
        x.voices.clear();
        x.voices.reserve(counts[kXmaKind]);
        for (std::uint32_t i = 0; i < counts[kXmaKind]; ++i) {
            x.voices.push_back(std::make_unique<Slot>());
        }
    }
    for (std::size_t i = 0; i < 6; ++i) {
        if (arrays[i] != nullptr) {
            log::info(std::format("acphal: SHAPE kind {} array {} device {:#x}", i, pointer(arrays[i]), addresses[i]));
        }
    }
    return kOk;
}

HResult acp_hal_release_shape_contexts()
{
    log::info("acphal: AcpHalReleaseShapeContexts");
    Contexts& c = contexts();
    std::scoped_lock lock(c.mutex);
    if (!c.arrays) {
        return kFail;
    }
    {
        Xma& x = xma_state();
        std::unique_lock xma_lock(x.mutex);
        x.array = 0;
        x.voices.clear();
    }
    for (void* array : *c.arrays) {
        heap_free(array);
    }
    c.arrays.reset();
    return kOk;
}

HResult acp_hal_create(Acp** out)
{
    if (out == nullptr) {
        log::warn("acphal: AcpHalCreate without an out pointer");
        return kInvalidArg;
    }
    auto* acp = new Acp{};
    acp->vtable = kAcpVtable.data();
    *out = acp;
    log::info("acphal: AcpHalCreate -> " + pointer(acp));
    return kOk;
}

const std::array<Entry, 9> kEntries{{
    {"AcpHalAllocateShapeContexts", 0, reinterpret_cast<Thunk>(&acp_hal_allocate_shape_contexts)},
    {"AcpHalCreate", 0, reinterpret_cast<Thunk>(&acp_hal_create)},
    {"AcpHalReleaseShapeContexts", 0, reinterpret_cast<Thunk>(&acp_hal_release_shape_contexts)},
    {"ApuAlloc", 0, reinterpret_cast<Thunk>(&apu_alloc)},
    {"ApuCreateHeap", 0, reinterpret_cast<Thunk>(&apu_create_heap)},
    {"ApuFree", 0, reinterpret_cast<Thunk>(&apu_free)},
    {"ApuIsVirtualAddressValid", 0, reinterpret_cast<Thunk>(&apu_is_virtual_address_valid)},
    {"ApuMapApuAddress", 0, reinterpret_cast<Thunk>(&apu_map_apu_address)},
    {"ApuMapVirtualAddress", 0, reinterpret_cast<Thunk>(&apu_map_virtual_address)},
}};

const Library kLibrary{"acphal", kEntries};

}

const Library& acphal_library() noexcept
{
    return kLibrary;
}

}
