#include <euphoria/shader/gfx10/emit/bitcode_census.hpp>

#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace euphoria::shader::gfx10::emit {
namespace {

struct Written {
    std::size_t len{0};
    std::uint64_t hash{0};
    Census counts;
};

thread_local std::vector<Written> g_written;

std::atomic<int> g_enabled{-1};

std::uint64_t fnv(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const std::uint8_t b : bytes) {
        h = (h ^ b) * 0x00000100000001b3ull;
    }
    return h;
}

std::size_t index_of(std::string_view s)
{
    for (std::size_t i = 0; i < census::KINDS.size(); ++i) {
        if (census::KINDS[i] == s) {
            return i;
        }
    }
    throw std::logic_error("census kind");
}

}

namespace census {

bool enabled()
{
    int on = g_enabled.load(std::memory_order_relaxed);
    if (on < 0) {
        on = std::getenv("EUPHORIA_SHADER_CENSUS") != nullptr ? 1 : 0;
        g_enabled.store(on, std::memory_order_relaxed);
    }
    return on != 0;
}

void set_enabled(bool on)
{
    g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
}

std::optional<Census> take(std::span<const std::uint8_t> dxil)
{
    std::vector<Written> written = std::move(g_written);
    g_written.clear();
    std::size_t at = dxil.size();
    for (std::size_t i = 0; i + 4 <= dxil.size(); ++i) {
        if (dxil[i] == 'B' && dxil[i + 1] == 'C' && dxil[i + 2] == 0xC0 && dxil[i + 3] == 0xDE) {
            at = i;
            break;
        }
    }
    if (at == dxil.size()) {
        return std::nullopt;
    }
    for (auto it = written.rbegin(); it != written.rend(); ++it) {
        if (at + it->len <= dxil.size() && fnv(dxil.subspan(at, it->len)) == it->hash) {
            return it->counts;
        }
    }
    return std::nullopt;
}

void note(const Module& m, std::span<const std::uint8_t> bitcode)
{
    if (!enabled()) {
        return;
    }
    g_written.push_back(Written{bitcode.size(), fnv(bitcode), m.census()});
}

std::size_t kind(std::string_view name)
{
    std::string_view n = name;
    if (n.starts_with("dx.op.")) {
        n.remove_prefix(6);
    }
    if (const auto dot = n.find('.'); dot != std::string_view::npos) {
        n = n.substr(0, dot);
    }
    if (n == "unary" || n == "binary" || n == "tertiary" || n == "unaryBits" || n == "legacyF16ToF32"
        || n == "legacyF32ToF16" || n == "isSpecialFloat" || n == "dot2" || n == "dot3" || n == "dot4"
        || n == "quaternary") {
        return index_of("math_call");
    }
    if (n == "bufferLoad" || n == "rawBufferLoad") {
        return index_of("buffer_load");
    }
    if (n == "cbufferLoadLegacy" || n == "cbufferLoad") {
        return index_of("cbuffer_load");
    }
    if (n == "bufferStore" || n == "rawBufferStore" || n == "textureStore") {
        return index_of("buffer_store");
    }
    if (n == "atomicBinOp" || n == "atomicCompareExchange") {
        return index_of("atomic");
    }
    if (n == "createHandle" || n == "createHandleFromBinding" || n == "createHandleFromHeap") {
        return index_of("handle");
    }
    if (n == "annotateHandle") {
        return index_of("annotate");
    }
    if (n == "getDimensions") {
        return index_of("dimensions");
    }
    if (n == "barrier") {
        return index_of("barrier");
    }
    if (n.starts_with("wave") || n.starts_with("quad")) {
        return index_of("wave_op");
    }
    if (n.starts_with("textureLoad") || n.starts_with("sample") || n.starts_with("textureGather")
        || n.starts_with("calculateLOD")) {
        return index_of("texture_read");
    }
    return index_of("io_other");
}

}

Census Module::census() const
{
    std::vector<std::size_t> call_kind;
    call_kind.reserve(funcs.size());
    for (const Func& f : funcs) {
        call_kind.push_back(census::kind(f.name));
    }
    const std::vector<bool> alive = live();
    std::vector<bool> seen(blocks.size(), false);
    std::vector<std::size_t> stack{0};
    if (!blocks.empty()) {
        seen[0] = true;
    } else {
        stack.clear();
    }
    while (!stack.empty()) {
        const std::size_t b = stack.back();
        stack.pop_back();
        for (const std::size_t s : successors(blocks[b].term)) {
            if (!seen[s]) {
                seen[s] = true;
                stack.push_back(s);
            }
        }
    }
    const std::size_t arith = index_of("arith");
    const std::size_t bitcast = index_of("bitcast");
    const std::size_t cast_kind = index_of("cast");
    const std::size_t select_kind = index_of("select");
    const std::size_t extract_kind = index_of("extract");
    const std::size_t phi_kind = index_of("phi");
    const std::size_t gep = index_of("gep");
    const std::size_t lds_load = index_of("lds_load");
    const std::size_t lds_store = index_of("lds_store");
    const std::size_t atomic_kind = index_of("atomic");
    const std::size_t guard = index_of("guard_select");
    const std::size_t clamp = index_of("clamp");
    const std::size_t counter = index_of("loop_counter");
    const std::size_t block_kind = index_of("blocks");
    Census c;
    const auto count = [&](std::uint32_t i) {
        c.counts[0] += 1;
        const Op& op = insts[i].op;
        std::size_t at = arith;
        switch (op.kind) {
        case Op::Kind::Bin:
        case Op::Kind::Cmp:
            at = arith;
            break;
        case Op::Kind::Cast:
            at = op.cast() == Cast::Bitcast ? bitcast : cast_kind;
            break;
        case Op::Kind::Select:
            at = select_kind;
            break;
        case Op::Kind::Extract:
            at = extract_kind;
            break;
        case Op::Kind::Phi:
            at = phi_kind;
            break;
        case Op::Kind::Gep:
            at = gep;
            break;
        case Op::Kind::Load:
            at = lds_load;
            break;
        case Op::Kind::Store:
            at = lds_store;
            break;
        case Op::Kind::AtomicRmw:
        case Op::Kind::CmpXchg:
            at = atomic_kind;
            break;
        case Op::Kind::Call:
            at = call_kind[op.index];
            break;
        }
        c.counts[at] += 1;
        if (const auto t = tags.find(i); t != tags.end()) {
            switch (t->second) {
            case Tag::Guard:
                c.counts[guard] += 1;
                break;
            case Tag::Clamp:
                c.counts[clamp] += 1;
                break;
            case Tag::Counter:
                c.counts[counter] += 1;
                break;
            }
        }
    };
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        if (!seen[b]) {
            continue;
        }
        c.counts[block_kind] += 1;
        for (const std::uint32_t i : blocks[b].phis) {
            if (alive[i]) {
                count(i);
            }
        }
        for (const std::uint32_t i : blocks[b].body) {
            count(i);
        }
    }
    return c;
}

}
