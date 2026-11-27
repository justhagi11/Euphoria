#include <euphoria/log/log.hpp>
#include <euphoria/shim/kernel.hpp>
#include <euphoria/shim/libraries.hpp>
#include <euphoria/shim/xmem.hpp>

#include <atomic>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <malloc.h>
#include <intrin.h>

#include <windows.h>

#include <psapi.h>

namespace euphoria::shim {
namespace {

constexpr std::size_t kPage = 64 * 1024;
constexpr std::uint64_t kAlloc32Bit = 1ULL << 33;
constexpr std::uint64_t kAlignmentShift = 24;

struct AllocationHeader {
    std::size_t size;
    std::size_t alignment;
};

std::size_t alignment_of(std::uint64_t attributes) noexcept
{
    std::size_t alignment = static_cast<std::size_t>(1) << ((attributes >> kAlignmentShift) & 0x1F);
    if (alignment < 16) {
        alignment = 16;
    }
    if (alignment > (1 << 20)) {
        alignment = 1 << 20;
    }
    return alignment;
}

void* xmem_alloc_default(std::size_t size, std::uint64_t attributes)
{
    if (size == 0) {
        size = 1;
    }

    if ((attributes & kAlloc32Bit) != 0) {
        return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    const std::size_t alignment = alignment_of(attributes);
    auto* raw = static_cast<std::uint8_t*>(_aligned_malloc(size + alignment, alignment));
    if (raw == nullptr) {
        return nullptr;
    }

    auto* header = reinterpret_cast<AllocationHeader*>(raw);
    header->size = size;
    header->alignment = alignment;

    return raw + alignment;
}

void xmem_free_default(void* address, std::uint64_t attributes)
{
    if (address == nullptr) {
        return;
    }

    if ((attributes & kAlloc32Bit) != 0) {
        VirtualFree(address, 0, MEM_RELEASE);
        return;
    }

    auto* raw = static_cast<std::uint8_t*>(address) - alignment_of(attributes);
    _aligned_free(raw);
}

using AllocRoutine = void*(__stdcall*)(std::size_t, std::uint64_t);
using FreeRoutine = void(__stdcall*)(void*, std::uint64_t);

struct Hooks {
    AllocRoutine alloc;
    FreeRoutine free;
};

Hooks title_hooks()
{
    static std::atomic<bool> resolved{false};
    static Hooks hooks{};
    if (resolved.load(std::memory_order_acquire)) {
        return hooks;
    }
    const std::uintptr_t base = kernel::main_image();
    if (base == 0) {
        return {};
    }
    const auto* table = reinterpret_cast<const std::uintptr_t*>(
        GetProcAddress(reinterpret_cast<HMODULE>(base), "__XMemAllocationHooks__"));
    if (table != nullptr) {
        hooks.alloc = reinterpret_cast<AllocRoutine>(table[0]);
        hooks.free = reinterpret_cast<FreeRoutine>(table[1]);
    }
    resolved.store(true, std::memory_order_release);
    log::info(std::string("xmem: title allocation hooks ")
              + (table != nullptr ? "found" : "absent"));
    return hooks;
}

void* xmem_alloc(std::size_t size, std::uint64_t attributes)
{
    if (const Hooks hooks = title_hooks(); hooks.alloc != nullptr) {
        return hooks.alloc(size, attributes);
    }
    return xmem_alloc_default(size, attributes);
}

void xmem_free(void* address, std::uint64_t attributes)
{
    if (const Hooks hooks = title_hooks(); hooks.free != nullptr) {
        hooks.free(address, attributes);
        return;
    }
    xmem_free_default(address, attributes);
}

int xmem_is_tools_mem_available()
{
    return 0;
}

using VirtualAlloc2Function = void*(WINAPI*)(HANDLE, void*, SIZE_T, ULONG, ULONG,
                                             MEM_EXTENDED_PARAMETER*, ULONG);

VirtualAlloc2Function virtual_alloc2()
{
    static VirtualAlloc2Function function = [] {
        HMODULE module = GetModuleHandleW(L"kernelbase.dll");
        if (module == nullptr) {
            module = GetModuleHandleW(L"kernel32.dll");
        }
        if (module == nullptr) {
            return static_cast<VirtualAlloc2Function>(nullptr);
        }
        return reinterpret_cast<VirtualAlloc2Function>(
            GetProcAddress(module, "VirtualAlloc2"));
    }();
    return function;
}

using MapViewOfFile3Function = void*(WINAPI*)(HANDLE, HANDLE, void*, ULONG64, SIZE_T, ULONG, ULONG,
                                              MEM_EXTENDED_PARAMETER*, ULONG);
using UnmapViewOfFile2Function = BOOL(WINAPI*)(HANDLE, void*, ULONG);

template <typename Function>
Function kernelbase_function(const char* name)
{
    HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    if (module == nullptr) {
        module = GetModuleHandleW(L"kernel32.dll");
    }
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

MapViewOfFile3Function map_view_of_file3()
{
    static MapViewOfFile3Function function = kernelbase_function<MapViewOfFile3Function>("MapViewOfFile3");
    return function;
}

UnmapViewOfFile2Function unmap_view_of_file2()
{
    static UnmapViewOfFile2Function function = kernelbase_function<UnmapViewOfFile2Function>("UnmapViewOfFile2");
    return function;
}

constexpr DWORD kPages2M = 0x20000000;
constexpr DWORD kPages64KBit = 0x400000;
constexpr DWORD kPages64K = kPages2M | kPages64KBit;
constexpr std::uintptr_t kPageNumberLimit = 1ULL << 37;

void memory_warning(std::string_view what, const void* address, std::size_t count, DWORD error)
{
    static std::atomic<std::uint32_t> logged{0};
    const std::uint32_t index = logged.fetch_add(1, std::memory_order_relaxed);
    if (index >= 256) {
        return;
    }
    log::warn(std::format("xmem: {} at {} count {} error {}{}", what, address, count, error,
                          index == 255 ? " (further failures not logged)" : ""));
}

void address_range(std::uint64_t flags, std::uintptr_t& lowest, std::uintptr_t& highest)
{
    lowest = 1ULL << 32;
    highest = (1ULL << 40) - 1;
    if ((flags & 4) != 0) {
        lowest = 0;
        highest = 0xFFFFFFFFULL;
    } else {
        const std::uint64_t field = (flags >> 12) & 7;
        if (field != 0) {
            highest = (1ULL << (47 - field)) - 1;
        }
    }
}

struct Section {
    HANDLE handle = nullptr;
    std::uint8_t* backing = nullptr;
    std::size_t pages = 0;
    std::size_t live = 0;
    std::vector<bool> alive;
    bool lazy = false;
    bool executable = false;

    Section() = default;
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;

    ~Section()
    {
        if (backing != nullptr) {
            UnmapViewOfFile(backing);
        }
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }
};

struct Piece {
    std::size_t size = 0;
    std::shared_ptr<Section> section;
    std::size_t index = 0;
    DWORD protect = 0;
};

struct Reservation {
    std::size_t size = 0;
    std::map<std::uintptr_t, Piece> pieces;
};

struct Physical {
    std::mutex mutex;
    std::map<std::uintptr_t, Reservation> reservations;
    std::map<std::uintptr_t, std::shared_ptr<Section>> sections;
};

Physical& physical()
{
    static auto* state = new Physical;
    return *state;
}

Reservation* reservation_at(Physical& state, std::uintptr_t address, std::size_t size)
{
    auto it = state.reservations.upper_bound(address);
    if (it == state.reservations.begin()) {
        return nullptr;
    }
    --it;
    const std::uintptr_t end = it->first + it->second.size;
    if (address >= end || size > end - address) {
        return nullptr;
    }
    return &it->second;
}

std::uintptr_t reservation_base(const Reservation& reservation)
{
    return reservation.pieces.begin()->first;
}

bool split_placeholder(Reservation& reservation, std::uintptr_t at)
{
    auto it = reservation.pieces.upper_bound(at);
    if (it == reservation.pieces.begin()) {
        return true;
    }
    --it;
    Piece& piece = it->second;
    const std::uintptr_t end = it->first + piece.size;
    if (it->first == at || at >= end || piece.section != nullptr) {
        return true;
    }
    if (VirtualFree(reinterpret_cast<void*>(at), end - at, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
        memory_warning("splitting a placeholder failed", reinterpret_cast<void*>(at), (end - at) / kPage,
                       GetLastError());
        return false;
    }
    piece.size = at - it->first;
    reservation.pieces.emplace(at, Piece{end - at, nullptr, 0, 0});
    return true;
}

bool map_view(std::uintptr_t base, std::size_t size, const std::shared_ptr<Section>& section, std::size_t index,
              DWORD protect)
{
    const MapViewOfFile3Function map = map_view_of_file3();
    if (map == nullptr) {
        memory_warning("MapViewOfFile3 unavailable", reinterpret_cast<void*>(base), size / kPage, 0);
        return false;
    }
    const DWORD view_protect = section->executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    void* view = map(section->handle, GetCurrentProcess(), reinterpret_cast<void*>(base),
                     static_cast<ULONG64>(index) * kPage, size, MEM_REPLACE_PLACEHOLDER, view_protect, nullptr, 0);
    if (view != reinterpret_cast<void*>(base)) {
        memory_warning("mapping a view over a placeholder failed", reinterpret_cast<void*>(base), size / kPage,
                       GetLastError());
        if (view != nullptr) {
            unmap_view_of_file2()(GetCurrentProcess(), view, MEM_PRESERVE_PLACEHOLDER);
        }
        return false;
    }
    if (section->lazy && VirtualAlloc(view, size, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        memory_warning("committing mapped physical pages failed", view, size / kPage, GetLastError());
        unmap_view_of_file2()(GetCurrentProcess(), view, MEM_PRESERVE_PLACEHOLDER);
        return false;
    }
    if (protect != 0 && protect != view_protect) {
        DWORD previous = 0;
        if (VirtualProtect(view, size, protect, &previous) == 0) {
            memory_warning("protecting a committed view failed", view, size / kPage, GetLastError());
        }
    }
    return true;
}

bool map_pieces(Reservation& reservation, std::uintptr_t begin, std::uintptr_t end,
                const std::shared_ptr<Section>& section, std::size_t index, DWORD protect)
{
    auto first = reservation.pieces.find(begin);
    if (first == reservation.pieces.end()) {
        memory_warning("no placeholder starts at the mapping", reinterpret_cast<void*>(begin),
                       (end - begin) / kPage, 0);
        return false;
    }
    const auto last = reservation.pieces.lower_bound(end);
    if (std::next(first) != last) {
        if (VirtualFree(reinterpret_cast<void*>(begin), end - begin,
                        MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0) {
            first->second.size = end - begin;
            reservation.pieces.erase(std::next(first), last);
        } else {
            memory_warning("coalescing placeholders failed", reinterpret_cast<void*>(begin),
                           (end - begin) / kPage, GetLastError());
        }
    }
    for (auto it = reservation.pieces.find(begin); it != reservation.pieces.end() && it->first < end; ++it) {
        const std::size_t at = index + (it->first - begin) / kPage;
        if (!map_view(it->first, it->second.size, section, at, protect)) {
            return false;
        }
        it->second.section = section;
        it->second.index = at;
        it->second.protect = protect;
    }
    return true;
}

bool release_views(Reservation& reservation, std::uintptr_t begin, std::uintptr_t end)
{
    struct Remainder {
        std::uintptr_t base;
        std::size_t size;
        std::shared_ptr<Section> section;
        std::size_t index;
        DWORD protect;
    };
    std::vector<Remainder> remainders;
    auto it = reservation.pieces.upper_bound(begin);
    if (it != reservation.pieces.begin()) {
        --it;
    }
    for (; it != reservation.pieces.end() && it->first < end; ++it) {
        Piece& piece = it->second;
        const std::uintptr_t start = it->first;
        const std::uintptr_t finish = start + piece.size;
        if (piece.section == nullptr || finish <= begin) {
            continue;
        }
        if (unmap_view_of_file2()(GetCurrentProcess(), reinterpret_cast<void*>(start), MEM_PRESERVE_PLACEHOLDER)
            == 0) {
            memory_warning("unmapping a view failed", reinterpret_cast<void*>(start), piece.size / kPage,
                           GetLastError());
            return false;
        }
        if (start < begin) {
            remainders.push_back({start, begin - start, piece.section, piece.index, piece.protect});
        }
        if (finish > end) {
            remainders.push_back({end, finish - end, piece.section, piece.index + (end - start) / kPage,
                                  piece.protect});
        }
        piece.section.reset();
        piece.index = 0;
        piece.protect = 0;
    }
    if (!split_placeholder(reservation, begin) || !split_placeholder(reservation, end)) {
        return false;
    }
    bool ok = true;
    for (const Remainder& remainder : remainders) {
        if (!map_pieces(reservation, remainder.base, remainder.base + remainder.size, remainder.section,
                        remainder.index, remainder.protect)) {
            ok = false;
        }
    }
    return ok;
}

std::shared_ptr<Section> anonymous_section(std::size_t size, bool executable)
{
    auto section = std::make_shared<Section>();
    section->executable = executable;
    section->handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                         (executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) | SEC_COMMIT,
                                         static_cast<DWORD>(static_cast<std::uint64_t>(size) >> 32),
                                         static_cast<DWORD>(size), nullptr);
    if (section->handle == nullptr) {
        return nullptr;
    }
    return section;
}

void* commit_range(Reservation& reservation, std::uintptr_t address, std::size_t size, DWORD protect)
{
    const std::uintptr_t limit = reservation_base(reservation) + reservation.size;
    const std::uintptr_t begin = address & ~static_cast<std::uintptr_t>(kPage - 1);
    std::uintptr_t end = (address + (size == 0 ? 1 : size) + kPage - 1) & ~static_cast<std::uintptr_t>(kPage - 1);
    if (end > limit) {
        end = limit;
    }
    if (protect == 0) {
        protect = PAGE_READWRITE;
    }
    if (!split_placeholder(reservation, begin) || !split_placeholder(reservation, end)) {
        return nullptr;
    }
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> gaps;
    for (auto it = reservation.pieces.lower_bound(begin); it != reservation.pieces.end() && it->first < end; ++it) {
        if (it->second.section != nullptr) {
            continue;
        }
        if (!gaps.empty() && gaps.back().second == it->first) {
            gaps.back().second = it->first + it->second.size;
        } else {
            gaps.emplace_back(it->first, it->first + it->second.size);
        }
    }
    const bool executable = (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                        | PAGE_EXECUTE_WRITECOPY)) != 0;
    for (const auto& [gap_begin, gap_end] : gaps) {
        const std::shared_ptr<Section> section = anonymous_section(gap_end - gap_begin, executable);
        if (section == nullptr) {
            memory_warning("creating a section to commit a placeholder failed", reinterpret_cast<void*>(gap_begin),
                           (gap_end - gap_begin) / kPage, GetLastError());
            return nullptr;
        }
        if (!map_pieces(reservation, gap_begin, gap_end, section, 0, protect)) {
            return nullptr;
        }
    }
    return reinterpret_cast<void*>(address & ~static_cast<std::uintptr_t>(0xFFF));
}

bool release_reservation(Physical& state, Reservation& reservation)
{
    const std::uintptr_t base = reservation_base(reservation);
    bool ok = true;
    for (const auto& [at, piece] : reservation.pieces) {
        if (piece.section != nullptr) {
            if (unmap_view_of_file2()(GetCurrentProcess(), reinterpret_cast<void*>(at), 0) == 0) {
                memory_warning("unmapping a view to release a reservation failed", reinterpret_cast<void*>(at),
                               piece.size / kPage, GetLastError());
                ok = false;
            }
        } else if (VirtualFree(reinterpret_cast<void*>(at), 0, MEM_RELEASE) == 0) {
            memory_warning("releasing a placeholder failed", reinterpret_cast<void*>(at), piece.size / kPage,
                           GetLastError());
            ok = false;
        }
    }
    state.reservations.erase(base);
    return ok;
}

void* reserve_placeholder(void* address, std::size_t size, std::uint64_t flags, bool constrained)
{
    const VirtualAlloc2Function allocate2 = virtual_alloc2();
    if (allocate2 == nullptr || map_view_of_file3() == nullptr || unmap_view_of_file2() == nullptr) {
        memory_warning("placeholder functions unavailable", address, size / kPage, 0);
        return nullptr;
    }
    size = (size + kPage - 1) & ~(kPage - 1);
    if (size == 0) {
        size = kPage;
    }
    std::uintptr_t lowest = 0;
    std::uintptr_t highest = 0;
    address_range(flags, lowest, highest);
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.LowestStartingAddress = reinterpret_cast<void*>(lowest);
    requirements.HighestEndingAddress = reinterpret_cast<void*>(highest);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    const bool required = constrained && address == nullptr;
    void* result = allocate2(GetCurrentProcess(), address, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                             required ? &parameter : nullptr, required ? 1 : 0);
    if (result == nullptr && required) {
        result = allocate2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                           PAGE_NOACCESS, nullptr, 0);
    }
    if (result == nullptr) {
        memory_warning("reserving a placeholder failed", address, size / kPage, GetLastError());
        return nullptr;
    }
    Physical& state = physical();
    std::scoped_lock lock(state.mutex);
    Reservation& reservation = state.reservations[reinterpret_cast<std::uintptr_t>(result)];
    reservation.size = size;
    reservation.pieces.clear();
    reservation.pieces.emplace(reinterpret_cast<std::uintptr_t>(result), Piece{size, nullptr, 0, 0});
    return result;
}

bool reservation_alloc(void* address, std::size_t size, std::uint32_t kind, std::uint32_t protect, void*& result)
{
    Physical& state = physical();
    std::scoped_lock lock(state.mutex);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    Reservation* reservation = reservation_at(state, at, 0);
    if (reservation == nullptr) {
        return false;
    }
    if ((kind & MEM_RELEASE) != 0) {
        result = reservation_base(*reservation) == at && release_reservation(state, *reservation) ? address
                                                                                                  : nullptr;
        return true;
    }
    if ((kind & MEM_COMMIT) != 0) {
        result = commit_range(*reservation, at, size, protect & 0x1FF);
        if (result == nullptr) {
            memory_warning("committing inside a placeholder reservation failed (count in bytes)", address, size,
                           GetLastError());
        }
        return true;
    }
    result = address;
    return true;
}

void* xmem_virtual_alloc(void* address, std::size_t size, std::uint32_t kind, std::uint64_t flags, std::uint32_t protect)
{
    const bool placeholder = (kind & kPages64K) == kPages64K && (kind & MEM_COMMIT) == 0;
    void* handled = nullptr;
    const bool in_reservation = address != nullptr && reservation_alloc(address, size, kind, protect, handled);
    if (in_reservation || placeholder) {
        void* result = handled;
        if (!in_reservation) {
            result = (kind & MEM_REPLACE_PLACEHOLDER) != 0 ? address
                                                           : reserve_placeholder(address, size, flags, true);
        }
        char text[224]{};
        std::snprintf(text, sizeof(text), "%p size %zu kind %08x flags %llx protect %x -> %p (%s) caller %p",
                      address, size, kind, static_cast<unsigned long long>(flags), protect, result,
                      in_reservation ? "in placeholder reservation" : "placeholder", _ReturnAddress());
        log::info(std::string("xmem: VirtualAlloc ") + text);
        return result;
    }
    DWORD host_kind = kind & (MEM_COMMIT | MEM_RESERVE | MEM_RESET);
    if ((kind & MEM_RELEASE) != 0) {
        host_kind = MEM_RELEASE;
    } else if (host_kind == 0) {
        host_kind = MEM_COMMIT;
    }
    bool commit_in_place = false;
    if (address != nullptr && (kind & MEM_RELEASE) == 0) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) != 0 && info.State == MEM_RESERVE) {
            host_kind |= MEM_COMMIT;
            host_kind &= ~MEM_RESERVE;
            commit_in_place = true;
        }
    }
    if (address == nullptr && (host_kind & MEM_RESERVE) == 0) {
        host_kind |= MEM_RESERVE;
    }
    DWORD host_protect = protect & 0x1FF;
    if (host_protect == 0) {
        host_protect = (commit_in_place || (host_kind & MEM_COMMIT) != 0) ? PAGE_READWRITE
                                                                         : PAGE_NOACCESS;
    }
    void* result = nullptr;
    const VirtualAlloc2Function allocate2 = virtual_alloc2();
    if (address == nullptr && allocate2 != nullptr) {
        std::uintptr_t lowest = 1ULL << 32;
        std::uintptr_t highest = (1ULL << 40) - 1;
        if ((flags & 4) != 0) {
            lowest = 0;
            highest = 0xFFFFFFFFULL;
        } else {
            const std::uint64_t field = (flags >> 12) & 7;
            if (field != 0) {
                highest = (1ULL << (47 - field)) - 1;
            }
        }
        MEM_ADDRESS_REQUIREMENTS requirements{};
        requirements.LowestStartingAddress = reinterpret_cast<void*>(lowest);
        requirements.HighestEndingAddress = reinterpret_cast<void*>(highest);
        MEM_EXTENDED_PARAMETER parameter{};
        parameter.Type = MemExtendedParameterAddressRequirements;
        parameter.Pointer = &requirements;
        result = allocate2(GetCurrentProcess(), nullptr, size, host_kind, host_protect,
                           &parameter, 1);
    }
    if (result == nullptr) {
        result = VirtualAlloc(address, size, host_kind, host_protect);
    }
    char text[192]{};
    std::snprintf(text, sizeof(text), "%p size %zu kind %08x flags %llx protect %x -> %p (error %lu) caller %p",
                  address, size, kind, static_cast<unsigned long long>(flags), protect, result,
                  GetLastError(), _ReturnAddress());
    log::info(std::string("xmem: VirtualAlloc ") + text);
    return result;
}

struct XmemBasicInformation {
    MEMORY_BASIC_INFORMATION host;
    std::uint64_t flags;
    std::uint32_t tag_code;
    std::uint8_t tag[4];
};

int xmem_virtual_query(HANDLE process, const void* address, void* output)
{
    if (output == nullptr) {
        return 0;
    }

    auto* info = static_cast<XmemBasicInformation*>(output);
    std::memset(info, 0, sizeof(*info));
    if (VirtualQueryEx(process, address, &info->host, sizeof(info->host)) == 0) {
        return 0;
    }
    if (info->host.State == MEM_COMMIT) {
        info->flags = 3;
    }
    return 1;
}

struct XmemWorkingSet {
    std::size_t title_limit;
    std::size_t title_used;
    std::size_t tools_limit;
    std::size_t tools_used;
    std::uint32_t stack_used;
    std::uint32_t physical_pages;
    std::size_t gpu_optimal_limit;
    std::size_t gpu_optimal_used;
    std::size_t tools_gpu_optimal_limit;
    std::size_t tools_gpu_optimal_used;
    std::uint32_t kernel_pool_used;
    std::uint32_t kernel_pool_limit;
};

int xmem_get_working_set_statistics(std::uint32_t, void* output)
{
    if (output == nullptr) {
        return 0;
    }

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));

    constexpr std::size_t gib = static_cast<std::size_t>(1) << 30;
    XmemWorkingSet statistics{};
    statistics.title_limit = 10 * gib + 7 * gib / 2;
    statistics.title_used = static_cast<std::size_t>(counters.PagefileUsage);
    statistics.gpu_optimal_limit = 10 * gib;
    static std::atomic<std::uint64_t> asked{0};
    if ((asked.fetch_add(1, std::memory_order_relaxed) & 1023) == 0) {
        log::info("xmem: working set asked, limit " + std::to_string(statistics.title_limit >> 20)
                  + " MB used " + std::to_string(statistics.title_used >> 20) + " MB");
    }
    std::memcpy(output, &statistics, sizeof(statistics));
    return 1;
}

int xmem_transfer_memory(std::uint32_t kind, std::size_t size)
{
    if (size == 0) {
        return 1;
    }
    return kind == 0 ? 1 : 0;
}

int xmem_allocate_physical_pages(std::uint64_t flags, std::size_t* count, std::size_t* pages)
{
    if (count == nullptr || pages == nullptr || *count == 0) {
        return 0;
    }

    const std::size_t wanted = *count;
    if (wanted > kPageNumberLimit / kPage) {
        memory_warning("AllocatePhysicalPages count too large", nullptr, wanted, 0);
        *count = 0;
        return 0;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(wanted) * kPage;
    const MapViewOfFile3Function map = map_view_of_file3();
    if (map == nullptr) {
        memory_warning("AllocatePhysicalPages without MapViewOfFile3", nullptr, wanted, 0);
        *count = 0;
        return 0;
    }
    auto section = std::make_shared<Section>();
    section->handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                         static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), nullptr);
    if (section->handle == nullptr) {
        memory_warning("AllocatePhysicalPages committed section failed, reserving instead", nullptr, wanted,
                       GetLastError());
        section->lazy = true;
        section->handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                             static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), nullptr);
    }
    if (section->handle == nullptr) {
        memory_warning("AllocatePhysicalPages section failed", nullptr, wanted, GetLastError());
        *count = 0;
        return 0;
    }
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.LowestStartingAddress = reinterpret_cast<void*>(1ULL << 32);
    requirements.HighestEndingAddress = reinterpret_cast<void*>(kPageNumberLimit - 1);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    section->backing = static_cast<std::uint8_t*>(
        map(section->handle, GetCurrentProcess(), nullptr, 0, static_cast<SIZE_T>(size), 0, PAGE_READWRITE,
            &parameter, 1));
    if (section->backing == nullptr) {
        memory_warning("AllocatePhysicalPages backing view failed", nullptr, wanted, GetLastError());
        *count = 0;
        return 0;
    }
    section->pages = wanted;
    section->live = wanted;
    section->alive.assign(wanted, true);
    const auto base = reinterpret_cast<std::uintptr_t>(section->backing);
    {
        Physical& state = physical();
        std::scoped_lock lock(state.mutex);
        state.sections[base] = section;
    }
    for (std::size_t index = 0; index < wanted; ++index) {
        pages[index] = (base + index * kPage) >> 12;
    }
    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        log::info(std::format("xmem: AllocatePhysicalPages {} pages flags {:x} backing {} {}", wanted, flags,
                              static_cast<void*>(section->backing), section->lazy ? "reserved" : "committed"));
    }
    return 1;
}

std::shared_ptr<Section> page_section(Physical& state, std::size_t page, std::size_t& index)
{
    const std::uintptr_t address = static_cast<std::uintptr_t>(page) << 12;
    auto it = state.sections.upper_bound(address);
    if (it == state.sections.begin()) {
        return nullptr;
    }
    --it;
    const std::uintptr_t offset = address - it->first;
    if (offset % kPage != 0 || offset / kPage >= it->second->pages || !it->second->alive[offset / kPage]) {
        return nullptr;
    }
    index = offset / kPage;
    return it->second;
}

int xmem_free_physical_pages(std::size_t count, const std::size_t* pages)
{
    if (pages == nullptr) {
        return 0;
    }
    Physical& state = physical();
    std::scoped_lock lock(state.mutex);
    for (std::size_t index = 0; index < count; ++index) {
        std::size_t at = 0;
        const std::shared_ptr<Section> section = page_section(state, pages[index], at);
        if (section == nullptr) {
            memory_warning("FreePhysicalPages unknown page",
                           reinterpret_cast<void*>(static_cast<std::uintptr_t>(pages[index]) << 12), 1, 0);
            continue;
        }
        section->alive[at] = false;
        if (--section->live == 0) {
            state.sections.erase(reinterpret_cast<std::uintptr_t>(section->backing));
        }
    }
    return 1;
}

void* map_by_copy(void* address, std::size_t count, const std::size_t* pages)
{
    memory_warning("MapPhysicalPages outside a placeholder reservation, copying", address, count, 0);
    auto* destination = static_cast<std::uint8_t*>(address);
    if (pages == nullptr) {
        VirtualFree(address, count * kPage, MEM_DECOMMIT);
        return address;
    }
    for (std::size_t index = 0; index < count; ++index) {
        std::uint8_t* target = destination + index * kPage;
        if (VirtualAlloc(target, kPage, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
            memory_warning("MapPhysicalPages commit failed", target, count - index, GetLastError());
            return nullptr;
        }
        std::memcpy(target, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(pages[index]) << 12), kPage);
    }
    return address;
}

void* xmem_map_physical_pages(void* address, std::size_t count, const std::size_t* pages)
{
    if (address == nullptr) {
        return nullptr;
    }
    if (count == 0) {
        return address;
    }

    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
        log::info(std::format("xmem: MapPhysicalPages {} count {}{}", address, count,
                              pages == nullptr ? " (unmap)" : ""));
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin % kPage != 0 || count > (~static_cast<std::uintptr_t>(0) - begin) / kPage) {
        memory_warning("MapPhysicalPages misaligned or oversized", address, count, 0);
        return nullptr;
    }
    const std::uintptr_t end = begin + count * kPage;
    Physical& state = physical();
    std::unique_lock lock(state.mutex);
    Reservation* reservation = reservation_at(state, begin, end - begin);
    if (reservation == nullptr) {
        lock.unlock();
        return map_by_copy(address, count, pages);
    }
    std::vector<std::pair<std::shared_ptr<Section>, std::size_t>> slices;
    if (pages != nullptr) {
        slices.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            std::size_t at = 0;
            std::shared_ptr<Section> section = page_section(state, pages[index], at);
            if (section == nullptr) {
                memory_warning("MapPhysicalPages unknown page",
                               reinterpret_cast<void*>(static_cast<std::uintptr_t>(pages[index]) << 12), index, 0);
                return nullptr;
            }
            slices.emplace_back(std::move(section), at);
        }
    }
    if (!release_views(*reservation, begin, end)) {
        memory_warning("MapPhysicalPages could not clear the range", address, count, GetLastError());
        return nullptr;
    }
    std::size_t index = 0;
    while (index < slices.size()) {
        std::size_t run = 1;
        while (index + run < slices.size() && slices[index + run].first == slices[index].first
               && slices[index + run].second == slices[index].second + run) {
            ++run;
        }
        const std::uintptr_t run_begin = begin + index * kPage;
        const std::uintptr_t run_end = run_begin + run * kPage;
        if (!split_placeholder(*reservation, run_end)
            || !map_pieces(*reservation, run_begin, run_end, slices[index].first, slices[index].second, 0)) {
            memory_warning("MapPhysicalPages mapping a run failed", reinterpret_cast<void*>(run_begin), run,
                           GetLastError());
            return nullptr;
        }
        index += run;
    }
    return address;
}

int xmem_map_physical_pages_scatter(void** addresses, std::size_t count, const std::size_t* pages)
{
    if (addresses == nullptr) {
        return 0;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t* page = pages != nullptr && pages[index] != 0 ? pages + index : nullptr;
        if (xmem_map_physical_pages(addresses[index], 1, page) == nullptr) {
            return 0;
        }
    }
    return 1;
}

void* xapi_stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("xapi: unimplemented shim called");
    return nullptr;
}

const std::array<Entry, 18> kEntries{{
    {"XMemAlloc", 0, reinterpret_cast<Thunk>(&xmem_alloc)},
    {"XMemAllocDefault", 0, reinterpret_cast<Thunk>(&xmem_alloc_default)},
    {"XMemAllocatePhysicalPages", 0, reinterpret_cast<Thunk>(&xmem_allocate_physical_pages)},
    {"XMemFree", 0, reinterpret_cast<Thunk>(&xmem_free)},
    {"XMemFreeDefault", 0, reinterpret_cast<Thunk>(&xmem_free_default)},
    {"XMemFreePhysicalPages", 0, reinterpret_cast<Thunk>(&xmem_free_physical_pages)},
    {"XMemGetWorkingSetStatistics", 0, reinterpret_cast<Thunk>(&xmem_get_working_set_statistics)},
    {"XMemIsToolsMemAvailable", 0, reinterpret_cast<Thunk>(&xmem_is_tools_mem_available)},
    {"XMemMapPhysicalPages", 0, reinterpret_cast<Thunk>(&xmem_map_physical_pages)},
    {"XMemMapPhysicalPagesScatter", 0, reinterpret_cast<Thunk>(&xmem_map_physical_pages_scatter)},
    {"XMemTransferMemory", 0, reinterpret_cast<Thunk>(&xmem_transfer_memory)},
    {"XMemVirtualAlloc", 0, reinterpret_cast<Thunk>(&xmem_virtual_alloc)},
    {"XMemVirtualQuery", 0, reinterpret_cast<Thunk>(&xmem_virtual_query)},
    {"XThreadGetId", 0, reinterpret_cast<Thunk>(&xapi_stub)},
    {"XTimeGetFileTime", 0, reinterpret_cast<Thunk>(&xapi_stub)},
    {"XMemGetBase", 0, reinterpret_cast<Thunk>(&xapi_stub)},
    {"XMemGetTitleMemoryUsage", 0, reinterpret_cast<Thunk>(&xapi_stub)},
    {"XMemSetAllocationHooks", 0, reinterpret_cast<Thunk>(&xapi_stub)},
}};

const Library kLibrary{"xapi", kEntries};

}

const Library& xapi_library() noexcept
{
    return kLibrary;
}

void* title_allocate(std::size_t size, std::uint64_t attributes)
{
    return xmem_alloc(size, attributes);
}

void title_release(void* address, std::uint64_t attributes)
{
    xmem_free(address, attributes);
}

namespace xmem {

bool guest_virtual_alloc(void* address, std::size_t size, std::uint32_t type, std::uint32_t protect, void*& result)
{
    if (address != nullptr && reservation_alloc(address, size, type, protect, result)) {
        return true;
    }
    if ((type & kPages64K) == kPages64K && (type & MEM_COMMIT) == 0) {
        result = (type & MEM_REPLACE_PLACEHOLDER) != 0 ? address : reserve_placeholder(address, size, 0, false);
        log::info(std::format("xmem: guest VirtualAlloc {} size {} type {:08x} -> {} (placeholder)", address, size,
                              type, result));
        return true;
    }
    if ((type & kPages64K) != 0) {
        result = VirtualAlloc(address, size, type & ~kPages64K, protect);
        if (result == nullptr) {
            memory_warning("guest VirtualAlloc without console page flags failed", address, size, GetLastError());
        }
        return true;
    }
    return false;
}

bool guest_virtual_free(void* address, std::size_t size, std::uint32_t type, int& result)
{
    Physical& state = physical();
    std::scoped_lock lock(state.mutex);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    Reservation* reservation = reservation_at(state, at, 0);
    if (reservation == nullptr) {
        return false;
    }
    if ((type & MEM_RELEASE) != 0 && size == 0 && reservation_base(*reservation) == at) {
        result = release_reservation(state, *reservation) ? TRUE : FALSE;
        return true;
    }
    const std::uintptr_t limit = reservation_base(*reservation) + reservation->size;
    const std::uintptr_t begin = (at + kPage - 1) & ~static_cast<std::uintptr_t>(kPage - 1);
    std::uintptr_t end = size == 0 ? limit : (at + size) & ~static_cast<std::uintptr_t>(kPage - 1);
    if (end > limit) {
        end = limit;
    }
    result = begin >= end || release_views(*reservation, begin, end) ? TRUE : FALSE;
    if (result == FALSE) {
        memory_warning("guest VirtualFree inside a placeholder reservation failed", address, size, GetLastError());
    }
    return true;
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
    const auto committed = [](const void* address) {
        MEMORY_BASIC_INFORMATION info{};
        return VirtualQuery(address, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT
               && (info.Protect & PAGE_READWRITE) != 0;
    };
    const auto reserved = [](const void* address) {
        MEMORY_BASIC_INFORMATION info{};
        return VirtualQuery(address, &info, sizeof(info)) != 0 && info.State == MEM_RESERVE;
    };
    const auto byte = [](void* base, std::size_t page, std::size_t offset = 0) {
        return static_cast<volatile std::uint8_t*>(base) + page * kPage + offset;
    };

    auto* a = static_cast<std::uint8_t*>(xmem_virtual_alloc(nullptr, 8 * kPage, MEM_RESERVE | kPages64K, 0,
                                                            PAGE_READWRITE));
    auto* b = static_cast<std::uint8_t*>(xmem_virtual_alloc(nullptr, 8 * kPage, MEM_RESERVE | kPages64K, 0,
                                                            PAGE_READWRITE));
    check("64 KiB reservations become placeholders", a != nullptr && b != nullptr && reserved(a) && reserved(b));
    std::array<std::size_t, 4> pages{};
    std::size_t count = pages.size();
    check("allocate four physical pages", xmem_allocate_physical_pages(0, &count, pages.data()) == 1 && count == 4);
    bool numbers = true;
    for (const std::size_t page : pages) {
        numbers = numbers && page % 16 == 0 && ((page >> 4) & 0x1FFFFF) << 4 == page;
    }
    check("page numbers fit 21 bits of 64 KiB frames", numbers);
    if (a == nullptr || b == nullptr || count != 4) {
        return failures;
    }

    check("map page 0 at two addresses",
          xmem_map_physical_pages(a, 1, pages.data()) == a && xmem_map_physical_pages(b + kPage, 1, pages.data())
              == b + kPage);
    *byte(a, 0, 7) = 0x5a;
    check("a write through one view shows through the other", *byte(b, 1, 7) == 0x5a);
    check("and through the page's backing address",
          *reinterpret_cast<volatile std::uint8_t*>((static_cast<std::uintptr_t>(pages[0]) << 12) + 7) == 0x5a);
    check("mapped pages are committed read-write", committed(a) && committed(b + kPage) && reserved(b));

    check("map three pages over the first", xmem_map_physical_pages(a, 3, pages.data() + 1) == a);
    *byte(a, 0) = 1;
    *byte(a, 1) = 2;
    *byte(a, 2) = 3;
    check("remapping replaces the old page", *byte(b, 1, 7) == 0x5a && *byte(a, 0, 7) == 0);
    check("unmap the middle page", xmem_map_physical_pages(a + kPage, 1, nullptr) == a + kPage);
    check("its neighbours stay mapped with their contents",
          reserved(a + kPage) && committed(a) && committed(a + 2 * kPage) && *byte(a, 0) == 1 && *byte(a, 2) == 3);
    void* hole = a + kPage;
    check("scatter page 0 into the hole", xmem_map_physical_pages_scatter(&hole, 1, pages.data()) == 1
                                              && *byte(a, 1, 7) == 0x5a);
    const std::size_t zero = 0;
    check("scatter page number 0 unmaps", xmem_map_physical_pages_scatter(&hole, 1, &zero) == 1 && reserved(hole));
    bool repeated = true;
    for (int round = 0; round < 200; ++round) {
        repeated = repeated && xmem_map_physical_pages(a, 3, pages.data() + 1) == a;
    }
    check("remapping the same pages keeps their contents", repeated && *byte(a, 0) == 1 && *byte(a, 1) == 2);

    void* commit = xmem_virtual_alloc(a + 4 * kPage, 2 * kPage, MEM_COMMIT, 0, PAGE_READWRITE);
    check("commit inside a placeholder reservation", commit == a + 4 * kPage && committed(a + 5 * kPage));
    *byte(a, 5, 3) = 9;
    void* guest = nullptr;
    check("guest VirtualAlloc commits there too",
          guest_virtual_alloc(a + 6 * kPage + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE, guest)
              && guest == a + 6 * kPage + 0x1000 && committed(a + 6 * kPage));
    check("map over committed memory", xmem_map_physical_pages(a + 5 * kPage, 1, pages.data() + 3) == a + 5 * kPage
                                           && *byte(a, 5, 3) == 0 && *byte(a, 4) == 0 && committed(a + 4 * kPage));
    int freed = 0;
    check("guest decommit returns the range to a placeholder",
          guest_virtual_free(a + 4 * kPage, 3 * kPage, MEM_DECOMMIT, freed) && freed == TRUE
              && reserved(a + 4 * kPage) && reserved(a + 5 * kPage) && reserved(a + 6 * kPage));
    check("and physical pages map there again", xmem_map_physical_pages(a + 4 * kPage, 4, pages.data()) == a + 4 * kPage
                                                    && *byte(a, 4, 7) == 0x5a && *byte(a, 5) == 1);

    std::array<std::size_t, 512> many{};
    std::size_t many_count = many.size();
    auto* pool = static_cast<std::uint8_t*>(xmem_virtual_alloc(nullptr, many.size() * kPage,
                                                               MEM_RESERVE | kPages64K, 0, PAGE_READWRITE));
    bool large = pool != nullptr && xmem_allocate_physical_pages(0, &many_count, many.data()) == 1
                 && xmem_map_physical_pages(pool, many.size(), many.data()) == pool;
    if (large) {
        for (std::size_t index = 0; index < many.size(); ++index) {
            *byte(pool, index) = static_cast<std::uint8_t>(index);
        }
        for (std::size_t index = 10; index < many.size(); index += 37) {
            large = large && xmem_map_physical_pages(pool + index * kPage, 1, many.data() + index - 1)
                                 == pool + index * kPage;
        }
        for (std::size_t index = 0; index < many.size(); ++index) {
            const std::size_t expected = (index >= 10 && (index - 10) % 37 == 0) ? index - 1 : index;
            large = large && *byte(pool, index) == static_cast<std::uint8_t>(expected);
        }
    }
    check("a long run remapped page by page keeps the rest", large);

    xmem_free_physical_pages(many_count, many.data());
    xmem_free_physical_pages(pages.size(), pages.data());
    check("pages stay readable through their views after freeing", *byte(a, 4, 7) == 0x5a);
    int released = 0;
    check("release the reservations", guest_virtual_free(a, 0, MEM_RELEASE, released) && released == TRUE
                                          && guest_virtual_free(b, 0, MEM_RELEASE, released) && released == TRUE
                                          && (pool == nullptr
                                              || (guest_virtual_free(pool, 0, MEM_RELEASE, released) && released)));
    MEMORY_BASIC_INFORMATION info{};
    check("released ranges are free", VirtualQuery(a, &info, sizeof(info)) != 0 && info.State == MEM_FREE);
    return failures;
}

}

}
