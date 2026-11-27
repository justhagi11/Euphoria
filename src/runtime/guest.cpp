#include <euphoria/runtime/guest.hpp>

#include <euphoria/log/log.hpp>
#include <euphoria/shim/kernel.hpp>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <windows.h>

namespace euphoria::runtime {
namespace {

std::atomic<std::uint32_t> g_validator_calls{0};

void __cdecl log_validator(void* object, std::uint64_t result)
{
    const std::uint32_t call = g_validator_calls.fetch_add(1, std::memory_order_relaxed);
    if (result != 0 && call >= 8) {
        return;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(object);
    char text[160]{};
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    if (bytes != nullptr) {
        std::memcpy(&a, bytes, sizeof(a));
        std::memcpy(&b, bytes + 4, sizeof(b));
    }
    std::snprintf(text, sizeof(text),
                  "allocator validator result %llu object %p first %08x %08x call %u",
                  static_cast<unsigned long long>(result), object, a, b, call);
    log::info(std::string("d3d12: ") + text);
}

void install_validator_hook(void* base, std::uint32_t rva)
{
    auto* site = static_cast<std::uint8_t*>(base) + rva;
    constexpr std::size_t stub_size = 41;
    const auto image_begin = reinterpret_cast<std::uintptr_t>(base);
    const auto image_end = image_begin + 0x20000000;
    std::uint8_t* stub = nullptr;
    for (std::size_t step = 0; step < 256 && stub == nullptr; ++step) {
        const std::uintptr_t hint = (image_end + 0xFFFF + step * 0x10000) & ~0xFFFF;
        stub = static_cast<std::uint8_t*>(
            VirtualAlloc(reinterpret_cast<void*>(hint), 0x1000, MEM_COMMIT | MEM_RESERVE,
                         PAGE_EXECUTE_READWRITE));
    }
    if (stub == nullptr) {
        log::error("guest: validator hook stub allocation failed");
        return;
    }

    const auto logger = reinterpret_cast<std::uint64_t>(&log_validator);
    std::uint8_t code[stub_size]{};
    std::size_t at = 0;
    const std::uint8_t prologue[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0x44, 0x24, 0x20,
                                     0x48, 0x89, 0xC2, 0x48, 0xB8};
    std::memcpy(code + at, prologue, sizeof(prologue));
    at += sizeof(prologue);
    std::memcpy(code + at, &logger, sizeof(logger));
    at += sizeof(logger);
    const std::uint8_t epilogue[] = {0xFF, 0xD0, 0x48, 0x8B, 0x44, 0x24, 0x20,
                                     0x48, 0x83, 0xC4, 0x28, 0xC3};
    std::memcpy(code + at, epilogue, sizeof(epilogue));
    at += sizeof(epilogue);
    std::memcpy(stub, code, at);
    FlushInstructionCache(GetCurrentProcess(), stub, at);

    const auto site_address = reinterpret_cast<std::uintptr_t>(site);
    const auto target = reinterpret_cast<std::uintptr_t>(stub);
    const auto relative = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site_address + 5);
    if (relative < INT32_MIN || relative > INT32_MAX) {
        log::error("guest: validator hook out of range");
        return;
    }
    DWORD previous = 0;
    if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        return;
    }
    const auto rel32 = static_cast<std::int32_t>(relative);
    site[0] = 0xE9;
    std::memcpy(site + 1, &rel32, sizeof(rel32));
    VirtualProtect(site, 5, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log::info("guest: allocator validator hook installed");
}

std::uint8_t* allocate_stub_near(void* base);

std::atomic<std::uint32_t> g_alloc_size_calls{0};

void __cdecl log_alloc_size(std::uint64_t size, std::uint64_t alignment)
{
    const std::uint32_t call = g_alloc_size_calls.fetch_add(1, std::memory_order_relaxed);
    if (call < 12) {
        log::info("guest: alloc size " + std::to_string(size) + " alignment "
                  + std::to_string(alignment) + " call " + std::to_string(call));
    }
}

void install_alloc_size_hook(void* base, std::uint32_t rva)
{
    auto* site = static_cast<std::uint8_t*>(base) + rva;
    std::uint8_t* stub = allocate_stub_near(base);
    if (stub == nullptr) {
        return;
    }
    const auto logger = reinterpret_cast<std::uint64_t>(&log_alloc_size);
    const auto site_address = reinterpret_cast<std::uintptr_t>(site);
    const auto stub_address = reinterpret_cast<std::uintptr_t>(stub);
    std::uint8_t code[96]{};
    std::size_t at = 0;
    const std::uint8_t prologue[] = {0x48, 0x83, 0xEC, 0x38, 0x4C, 0x89, 0x44, 0x24, 0x20,
                                     0x4C, 0x89, 0x4C, 0x24, 0x28, 0x48, 0x89, 0x54, 0x24,
                                     0x30, 0x4C, 0x89, 0xC1, 0x4C, 0x89, 0xCA, 0x48, 0xB8};
    std::memcpy(code + at, prologue, sizeof(prologue));
    at += sizeof(prologue);
    std::memcpy(code + at, &logger, sizeof(logger));
    at += sizeof(logger);
    const std::uint8_t call[] = {0xFF, 0xD0, 0x4C, 0x8B, 0x44, 0x24, 0x20, 0x4C, 0x8B,
                                 0x4C, 0x24, 0x28, 0x48, 0x8B, 0x54, 0x24, 0x30, 0x48,
                                 0x83, 0xC4, 0x38, 0x4D, 0x85, 0xC0};
    std::memcpy(code + at, call, sizeof(call));
    at += sizeof(call);
    const auto zero_target = reinterpret_cast<std::uintptr_t>(base) + 0x24746c2;
    const auto nonzero_target = reinterpret_cast<std::uintptr_t>(base) + 0x2474695;
    const auto jump = [&](std::uintptr_t target) {
        const auto relative = static_cast<std::int64_t>(target)
                            - static_cast<std::int64_t>(stub_address + at + 5);
        const std::uint8_t opcode = 0xE9;
        code[at++] = opcode;
        const auto rel32 = static_cast<std::int32_t>(relative);
        std::memcpy(code + at, &rel32, sizeof(rel32));
        at += sizeof(rel32);
    };
    const auto zero_jump = [&](std::uintptr_t target) {
        const auto relative = static_cast<std::int64_t>(target)
                            - static_cast<std::int64_t>(stub_address + at + 6);
        code[at++] = 0x0F;
        code[at++] = 0x84;
        const auto rel32 = static_cast<std::int32_t>(relative);
        std::memcpy(code + at, &rel32, sizeof(rel32));
        at += sizeof(rel32);
    };
    zero_jump(zero_target);
    jump(nonzero_target);
    std::memcpy(stub, code, at);
    FlushInstructionCache(GetCurrentProcess(), stub, at);

    const auto relative = static_cast<std::int64_t>(stub_address)
                        - static_cast<std::int64_t>(site_address + 5);
    if (relative < INT32_MIN || relative > INT32_MAX) {
        return;
    }
    DWORD previous = 0;
    if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        return;
    }
    const auto rel32 = static_cast<std::int32_t>(relative);
    site[0] = 0xE9;
    std::memcpy(site + 1, &rel32, sizeof(rel32));
    VirtualProtect(site, 5, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log::info("guest: alloc size hook installed");
}

void __cdecl log_branch(std::uint32_t id)
{
    log::info("guest: failure branch " + std::to_string(id) + " taken");
}

void install_branch_log(void* base, std::uint32_t site_rva, std::uint32_t target_rva,
                        std::uint32_t id, const char* name)
{
    auto* site = static_cast<std::uint8_t*>(base) + site_rva;
    if (site[0] != 0x0F || (site[1] & 0xF0) != 0x80) {
        log::warn(std::string("guest: branch log site ") + name + " is not a near jcc");
        return;
    }
    std::uint8_t* stub = allocate_stub_near(base);
    if (stub == nullptr) {
        return;
    }
    const auto logger = reinterpret_cast<std::uint64_t>(&log_branch);
    const auto target = reinterpret_cast<std::uintptr_t>(base) + target_rva;
    const auto stub_address = reinterpret_cast<std::uintptr_t>(stub);
    std::uint8_t code[64]{};
    std::size_t at = 0;
    const std::uint8_t prologue[] = {0x48, 0x83, 0xEC, 0x20, 0xB9};
    std::memcpy(code + at, prologue, sizeof(prologue));
    at += sizeof(prologue);
    std::memcpy(code + at, &id, sizeof(id));
    at += sizeof(id);
    code[at++] = 0x48;
    code[at++] = 0xB8;
    std::memcpy(code + at, &logger, sizeof(logger));
    at += sizeof(logger);
    const std::uint8_t call[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0xE9};
    std::memcpy(code + at, call, sizeof(call));
    at += sizeof(call);
    const auto relative = static_cast<std::int64_t>(target)
                        - static_cast<std::int64_t>(stub_address + at + 4);
    const auto rel32 = static_cast<std::int32_t>(relative);
    std::memcpy(code + at, &rel32, sizeof(rel32));
    at += sizeof(rel32);
    std::memcpy(stub, code, at);
    FlushInstructionCache(GetCurrentProcess(), stub, at);

    const auto site_address = reinterpret_cast<std::uintptr_t>(site);
    const auto branch_relative = static_cast<std::int64_t>(stub_address)
                               - static_cast<std::int64_t>(site_address + 6);
    if (branch_relative < INT32_MIN || branch_relative > INT32_MAX) {
        return;
    }
    DWORD previous = 0;
    if (VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        return;
    }
    const auto branch_rel32 = static_cast<std::int32_t>(branch_relative);
    std::memcpy(site + 2, &branch_rel32, sizeof(branch_rel32));
    VirtualProtect(site, 6, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    log::info(std::string("guest: branch log installed: ") + name);
}

void __cdecl log_device_call(std::uint64_t object)
{
    const auto* address = reinterpret_cast<const std::uint64_t*>(object);
    std::uint64_t vtable = 0;
    std::uint64_t target = 0;
    if (address != nullptr) {
        vtable = address[0];
        if (vtable != 0) {
            target = reinterpret_cast<const std::uint64_t*>(vtable)[0x428 / 8];
        }
    }
    char text[160]{};
    std::snprintf(text, sizeof(text), "object 0x%llx vtable 0x%llx slot133 0x%llx",
                  static_cast<unsigned long long>(object),
                  static_cast<unsigned long long>(vtable),
                  static_cast<unsigned long long>(target));
    log::info(std::string("guest: device slot133 call ") + text);
}

void install_device_call_log(void* base, std::uint32_t rva, std::uint32_t continuation)
{
    auto* site = static_cast<std::uint8_t*>(base) + rva;
    std::uint8_t* stub = allocate_stub_near(base);
    if (stub == nullptr) {
        return;
    }
    const auto logger = reinterpret_cast<std::uint64_t>(&log_device_call);
    const auto stub_address = reinterpret_cast<std::uintptr_t>(stub);
    const auto target = reinterpret_cast<std::uintptr_t>(base) + continuation;
    std::uint8_t code[96]{};
    std::size_t at = 0;
    const std::uint8_t prologue[] = {
        0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
        0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x4C, 0x24, 0x50, 0x48, 0xB8};
    std::memcpy(code + at, prologue, sizeof(prologue));
    at += sizeof(prologue);
    std::memcpy(code + at, &logger, sizeof(logger));
    at += sizeof(logger);
    const std::uint8_t after[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0x41, 0x5B, 0x41,
                                  0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58};
    std::memcpy(code + at, after, sizeof(after));
    at += sizeof(after);
    const std::uint8_t call_slot[] = {0x41, 0xFF, 0x92, 0x28, 0x04, 0x00, 0x00, 0xE9};
    std::memcpy(code + at, call_slot, sizeof(call_slot));
    at += sizeof(call_slot);
    const auto relative = static_cast<std::int64_t>(target)
                        - static_cast<std::int64_t>(stub_address + at + 4);
    const auto rel32 = static_cast<std::int32_t>(relative);
    std::memcpy(code + at, &rel32, sizeof(rel32));
    at += sizeof(rel32);
    std::memcpy(stub, code, at);
    FlushInstructionCache(GetCurrentProcess(), stub, at);

    const auto site_address = reinterpret_cast<std::uintptr_t>(site);
    const auto branch_relative = static_cast<std::int64_t>(stub_address)
                               - static_cast<std::int64_t>(site_address + 5);
    if (branch_relative < INT32_MIN || branch_relative > INT32_MAX) {
        return;
    }
    DWORD previous = 0;
    if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        return;
    }
    const auto branch_rel32 = static_cast<std::int32_t>(branch_relative);
    site[0] = 0xE9;
    std::memcpy(site + 1, &branch_rel32, sizeof(branch_rel32));
    VirtualProtect(site, 5, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log::info("guest: device slot133 call log installed");
}

void __cdecl log_setns(std::uint64_t, std::uint32_t result)
{
    log::info("guest: thread resource create result 0x" + [&] {
        char text[16]{};
        std::snprintf(text, sizeof(text), "%08x", result);
        return std::string(text);
    }() + " (return " + std::to_string(result >= 0 ? 1 : 0) + ")");
}

void install_setns_log(void* base, std::uint32_t rva, std::uint32_t continuation)
{
    auto* site = static_cast<std::uint8_t*>(base) + rva;
    std::uint8_t* stub = allocate_stub_near(base);
    if (stub == nullptr) {
        return;
    }
    const auto logger = reinterpret_cast<std::uint64_t>(&log_setns);
    const auto stub_address = reinterpret_cast<std::uintptr_t>(stub);
    const auto target = reinterpret_cast<std::uintptr_t>(base) + continuation;
    std::uint8_t code[64]{};
    std::size_t at = 0;
    const std::uint8_t prologue[] = {0x50, 0x48, 0x83, 0xEC, 0x28, 0x89, 0xF2, 0x31, 0xC9,
                                     0x48, 0xB8};
    std::memcpy(code + at, prologue, sizeof(prologue));
    at += sizeof(prologue);
    std::memcpy(code + at, &logger, sizeof(logger));
    at += sizeof(logger);
    const std::uint8_t after[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0x58, 0xE9};
    std::memcpy(code + at, after, sizeof(after));
    at += sizeof(after);
    const auto relative = static_cast<std::int64_t>(target)
                        - static_cast<std::int64_t>(stub_address + at + 4);
    const auto rel32 = static_cast<std::int32_t>(relative);
    std::memcpy(code + at, &rel32, sizeof(rel32));
    at += sizeof(rel32);
    std::memcpy(stub, code, at);
    FlushInstructionCache(GetCurrentProcess(), stub, at);

    const auto site_address = reinterpret_cast<std::uintptr_t>(site);
    const auto site_relative = static_cast<std::int64_t>(stub_address)
                             - static_cast<std::int64_t>(site_address + 5);
    if (site_relative < INT32_MIN || site_relative > INT32_MAX) {
        return;
    }
    DWORD previous = 0;
    if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        return;
    }
    const auto site_rel32 = static_cast<std::int32_t>(site_relative);
    site[0] = 0xE9;
    std::memcpy(site + 1, &site_rel32, sizeof(site_rel32));
    VirtualProtect(site, 5, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log::info("guest: thread resource result log installed");
}

std::atomic<std::uint32_t> g_alloc_call_calls{0};

void __cdecl log_alloc_call(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    const std::uint32_t call = g_alloc_call_calls.fetch_add(1, std::memory_order_relaxed);
    if (call < 12) {
        log::info("guest: allocator call a=0x" + [&] {
            char text[32]{};
            std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(a));
            return std::string(text);
        }() + " b=0x" + [&] {
            char text[32]{};
            std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(b));
            return std::string(text);
        }() + " c=0x" + [&] {
            char text[32]{};
            std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(c));
            return std::string(text);
        }() + " d=0x" + [&] {
            char text[32]{};
            std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(d));
            return std::string(text);
        }() + " call " + std::to_string(call));
    }
}

void install_alloc_call_hook(void* base, std::uint32_t rva, std::uint32_t continuation)
{
    auto* site = static_cast<std::uint8_t*>(base) + rva;
    std::uint8_t* stub = allocate_stub_near(base);
    if (stub == nullptr) {
        return;
    }
    const auto logger = reinterpret_cast<std::uint64_t>(&log_alloc_call);
    const auto stub_address = reinterpret_cast<std::uintptr_t>(stub);
    const auto target = reinterpret_cast<std::uintptr_t>(base) + continuation;
    std::uint8_t code[128]{};
    std::size_t at = 0;
    const std::uint8_t prologue[] = {0x48, 0x83, 0xEC, 0x48, 0x48, 0x89, 0x4C, 0x24, 0x20,
                                     0x48, 0x89, 0x54, 0x24, 0x28, 0x4C, 0x89, 0x44, 0x24,
                                     0x30, 0x4C, 0x89, 0x4C, 0x24, 0x38, 0x48, 0xB8};
    std::memcpy(code + at, prologue, sizeof(prologue));
    at += sizeof(prologue);
    std::memcpy(code + at, &logger, sizeof(logger));
    at += sizeof(logger);
    const std::uint8_t call[] = {0xFF, 0xD0, 0x48, 0x8B, 0x4C, 0x24, 0x20, 0x48, 0x8B,
                                 0x54, 0x24, 0x28, 0x4C, 0x8B, 0x44, 0x24, 0x30, 0x4C,
                                 0x8B, 0x4C, 0x24, 0x38, 0x48, 0x83, 0xC4, 0x48, 0xE9};
    std::memcpy(code + at, call, sizeof(call));
    at += sizeof(call);
    const auto relative = static_cast<std::int64_t>(target)
                        - static_cast<std::int64_t>(stub_address + at + 4);
    const auto rel32 = static_cast<std::int32_t>(relative);
    std::memcpy(code + at, &rel32, sizeof(rel32));
    at += sizeof(rel32);
    std::memcpy(stub, code, at);
    FlushInstructionCache(GetCurrentProcess(), stub, at);

    const auto site_relative = static_cast<std::int64_t>(stub_address)
                             - static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site) + 5);
    if (site_relative < INT32_MIN || site_relative > INT32_MAX) {
        return;
    }
    DWORD previous = 0;
    if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        return;
    }
    const auto site_rel32 = static_cast<std::int32_t>(site_relative);
    site[0] = 0xE9;
    std::memcpy(site + 1, &site_rel32, sizeof(site_rel32));
    VirtualProtect(site, 5, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log::info("guest: allocator call hook installed");
}

std::atomic<std::uint32_t> g_thread_fallbacks{0};

void* __cdecl fake_thread_block()
{
    const std::uint32_t count = g_thread_fallbacks.fetch_add(1, std::memory_order_relaxed);
    void* block = VirtualAlloc(nullptr, 0xA8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (count < 8) {
        log::warn("guest: thread fallback block " + std::to_string(reinterpret_cast<std::uintptr_t>(block))
                  + " call " + std::to_string(count));
    }
    return block;
}

std::uint8_t* allocate_stub_near(void* base)
{
    const auto image_begin = reinterpret_cast<std::uintptr_t>(base);
    const auto image_end = image_begin + 0x20000000;
    for (std::size_t step = 0; step < 256; ++step) {
        const std::uintptr_t hint = (image_end + 0xFFFF + step * 0x10000) & ~0xFFFF;
        if (auto* page = static_cast<std::uint8_t*>(
                VirtualAlloc(reinterpret_cast<void*>(hint), 0x1000, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE))) {
            return page;
        }
    }
    return nullptr;
}

void install_thread_fallback(void* base, std::uint32_t rva)
{
    auto* site = static_cast<std::uint8_t*>(base) + rva;
    std::uint8_t* stub = allocate_stub_near(base);
    if (stub == nullptr) {
        log::error("guest: thread fallback stub allocation failed");
        return;
    }
    const auto allocator = reinterpret_cast<std::uint64_t>(&fake_thread_block);
    std::uint8_t code[64]{};
    std::size_t at = 0;
    const std::uint8_t prologue[] = {0x48, 0x83, 0xEC, 0x20, 0x48, 0xB8};
    std::memcpy(code + at, prologue, sizeof(prologue));
    at += sizeof(prologue);
    std::memcpy(code + at, &allocator, sizeof(allocator));
    at += sizeof(allocator);
    const std::uint8_t epilogue[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x48, 0x83,
                                     0xC4, 0x60, 0x5B, 0x5F, 0x5E, 0xC3};
    std::memcpy(code + at, epilogue, sizeof(epilogue));
    at += sizeof(epilogue);
    std::memcpy(stub, code, at);
    FlushInstructionCache(GetCurrentProcess(), stub, at);

    const auto site_address = reinterpret_cast<std::uintptr_t>(site);
    const auto target = reinterpret_cast<std::uintptr_t>(stub);
    const auto relative = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site_address + 5);
    if (relative < INT32_MIN || relative > INT32_MAX) {
        log::error("guest: thread fallback out of range");
        return;
    }
    DWORD previous = 0;
    if (VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        return;
    }
    const auto rel32 = static_cast<std::int32_t>(relative);
    site[0] = 0xE9;
    std::memcpy(site + 1, &rel32, sizeof(rel32));
    site[5] = 0x90;
    site[6] = 0x90;
    site[7] = 0x90;
    VirtualProtect(site, 8, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    log::info("guest: thread fallback installed");
}

struct BootContext {
    std::uintptr_t entry{};
    std::uintptr_t tls_callbacks{};
    std::uintptr_t module_base{};
    int result{};
    unsigned long exception_code{};
    std::uintptr_t exception_address{};
    std::uintptr_t fault_rip{};
    std::uintptr_t fault_rsp{};
    std::uintptr_t registers[4]{};
    void* frames[16]{};
    unsigned short frame_count{};
};

int exception_filter(BootContext* context, EXCEPTION_POINTERS* info)
{
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        context->exception_code = info->ExceptionRecord->ExceptionCode;
        context->exception_address =
            reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    }
    if (info != nullptr && info->ContextRecord != nullptr) {
        context->fault_rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
        context->fault_rsp = static_cast<std::uintptr_t>(info->ContextRecord->Rsp);
        context->registers[0] = static_cast<std::uintptr_t>(info->ContextRecord->Rcx);
        context->registers[1] = static_cast<std::uintptr_t>(info->ContextRecord->Rdx);
        context->registers[2] = static_cast<std::uintptr_t>(info->ContextRecord->R8);
        context->registers[3] = static_cast<std::uintptr_t>(info->ContextRecord->R9);
    }
    context->frame_count = 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

int guarded_boot(BootContext* context)
{
    using TlsCallback = void(NTAPI*)(void*, unsigned long, void*);
    __try {
        if (context->tls_callbacks != 0) {
            auto* callbacks = reinterpret_cast<std::uintptr_t*>(context->tls_callbacks);
            for (std::size_t index = 0; callbacks[index] != 0; ++index) {
                reinterpret_cast<TlsCallback>(callbacks[index])(
                    reinterpret_cast<void*>(context->module_base), DLL_PROCESS_ATTACH, nullptr);
            }
        }
        return reinterpret_cast<int (*)()>(context->entry)();
    }
    __except (exception_filter(context, GetExceptionInformation())) {
        return 0;
    }
}

DWORD WINAPI boot_thread(void* parameter)
{
    auto* context = static_cast<BootContext*>(parameter);
    context->result = guarded_boot(context);
    return 0;
}

}

bool Guest::initialize(const GuestConfig& config)
{
    config_ = config;

    if (config_.title.empty()) {
        log::error("guest: no title image provided");
        return false;
    }

    log::info("guest: loading " + config_.title.string());
    std::optional<loader::Image> image = loader::Image::load_file(config_.title);
    if (!image) {
        return false;
    }
    image_ = std::move(*image);

    if (!image_.load_as_module(config_.cache_directory)) {
        return false;
    }

    if (!load_title_dlls()) {
        return false;
    }

    for (const auto& import : image_.imports()) {
        void* address = resolver_.resolve(import.module, import.name, import.ordinal, import.by_ordinal);
        auto* slot = reinterpret_cast<std::uint64_t*>(image_.at(import.iat_rva));
        DWORD previous = 0;
        if (VirtualProtect(slot, sizeof(std::uint64_t), PAGE_READWRITE, &previous)) {
            *slot = reinterpret_cast<std::uint64_t>(address);
            VirtualProtect(slot, sizeof(std::uint64_t), previous, &previous);
        } else {
            *slot = reinterpret_cast<std::uint64_t>(address);
        }
    }

    shim::kernel::set_main_image(reinterpret_cast<std::uintptr_t>(image_.base()), config_.title.c_str());
    shim::kernel::set_tls_callbacks(image_.tls_callbacks_original());
    if (!image_.redirect_tls()) {
        return false;
    }

    const auto& stats = resolver_.stats();
    log::info("guest: imports resolved, host " + std::to_string(stats.host)
              + ", shim " + std::to_string(stats.shim)
              + ", stub " + std::to_string(stats.stub)
              + ", failed " + std::to_string(stats.failed)
              + ", host modules " + std::to_string(resolver_.host_module_count()));
    log::info("guest: tls index " + std::to_string(image_.tls_index()));
    log::info("guest: entry at rva 0x" + [&] {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(image_.entry_rva()));
        return std::string(text);
    }() + ", stack reserve " + std::to_string(image_.stack_reserve()));

    initialized_ = true;
    return true;
}



bool Guest::load_title_dlls()
{
    const std::filesystem::path directory = config_.title.parent_path();
    const auto lowercase = [](std::string text) {
        for (char& ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return text;
    };
    const auto wanted = [&](const std::string& name) {
        static constexpr std::string_view kHostOwned[] = {
            "msvcp", "vcruntime", "concrt", "vccorlib", "vcomp", "ucrtbase", "api-ms-",
            "ext-ms-", "dstorage", "d3d12", "xg_xs"};
        for (const std::string_view prefix : kHostOwned) {
            if (name.starts_with(prefix)) {
                return false;
            }
        }
        std::error_code error;
        return std::filesystem::is_regular_file(directory / name, error);
    };
    std::unordered_set<std::string> seen;
    std::vector<std::string> names;
    std::function<bool(const std::vector<std::string>&)> map_modules;
    const auto map_dll = [&](const std::string& module) -> bool {
        const std::string name = lowercase(module);
        if (!seen.insert(name).second || !wanted(name)) {
            return true;
        }
        const std::filesystem::path path = directory / name;
        std::optional<loader::Image> loaded = loader::Image::load_file(path);
        if (!loaded) {
            log::error("guest: cannot read title dll " + path.string());
            return false;
        }
        auto dll = std::make_unique<loader::Image>(std::move(*loaded));
        if (!dll->load_as_module(config_.cache_directory)) {
            log::error("guest: cannot map title dll " + path.string());
            return false;
        }
        std::vector<std::string> modules;
        for (const auto& import : dll->imports()) {
            modules.emplace_back(import.module);
        }
        if (!map_modules(modules)) {
            return false;
        }
        names.push_back(name);
        title_dlls_.push_back(std::move(dll));
        return true;
    };
    map_modules = [&](const std::vector<std::string>& modules) {
        for (const std::string& module : modules) {
            if (!map_dll(module)) {
                return false;
            }
        }
        return true;
    };
    std::vector<std::string> main_modules;
    for (const auto& import : image_.imports()) {
        main_modules.emplace_back(import.module);
    }
    if (!map_modules(main_modules) || !map_dll("XboxBvhBuilder.dll")) {
        return false;
    }
    for (std::size_t index = 0; index < title_dlls_.size(); ++index) {
        resolver_.add_title_module(names[index], title_dlls_[index]->base());
    }
    for (std::size_t index = 0; index < title_dlls_.size(); ++index) {
        auto& dll = title_dlls_[index];
        const std::filesystem::path path = directory / names[index];
        for (const auto& import : dll->imports()) {
            void* address =
                resolver_.resolve(import.module, import.name, import.ordinal, import.by_ordinal);
            auto* slot = reinterpret_cast<std::uint64_t*>(dll->at(import.iat_rva));
            DWORD previous = 0;
            if (VirtualProtect(slot, sizeof(std::uint64_t), PAGE_READWRITE, &previous)) {
                *slot = reinterpret_cast<std::uint64_t>(address);
                VirtualProtect(slot, sizeof(std::uint64_t), previous, &previous);
            } else {
                *slot = reinterpret_cast<std::uint64_t>(address);
            }
        }
        char text[64]{};
        std::snprintf(text, sizeof(text), "0x%llx",
                      static_cast<unsigned long long>(
                          reinterpret_cast<std::uintptr_t>(dll->base())));
        log::info("guest: title dll " + path.filename().string() + " at " + text + ", "
                  + std::to_string(dll->imports().size()) + " imports bound");
    }
    title_dll_names_ = names;
    return true;
}

void Guest::attach_title_dlls()
{
    using DllMain = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
    for (std::size_t index = 0; index < title_dlls_.size(); ++index) {
        const auto& dll = title_dlls_[index];
        log::info("guest: attaching title dll "
                  + (index < title_dll_names_.size() ? title_dll_names_[index] : std::string("?")));
        const std::uintptr_t entry = dll->entry_point();
        if (entry == 0 || entry == reinterpret_cast<std::uintptr_t>(dll->base())) {
            continue;
        }
        const BOOL result = reinterpret_cast<DllMain>(entry)(
            reinterpret_cast<HINSTANCE>(dll->base()), DLL_PROCESS_ATTACH, nullptr);
        log::info("guest: title dll " + (index < title_dll_names_.size() ? title_dll_names_[index] : std::string("?")) + " attached, DllMain returned "
                  + (result != FALSE ? "TRUE" : "FALSE"));
    }
}

int Guest::run()
{
    if (!initialized_) {
        log::error("guest: run requested before initialize");
        return 1;
    }

    BootContext context;
    context.entry = image_.entry_point();
    context.module_base = reinterpret_cast<std::uintptr_t>(image_.base());
    context.tls_callbacks = image_.tls_callbacks_original();

    attach_title_dlls();

    install_validator_hook(image_.base(), 0x247819b);
    install_alloc_size_hook(image_.base(), 0x2474690);
    install_alloc_call_hook(image_.base(), 0x5204e60, 0x34beb0);
    install_branch_log(image_.base(), 0x34bed6, 0x34c0c8, 1, "validator false");
    install_branch_log(image_.base(), 0x34c003, 0x34c0c0, 2, "thread init false");
    install_branch_log(image_.base(), 0x193e24b, 0x193e310, 3, "thread alloc null");
    install_branch_log(image_.base(), 0x193e009, 0x193e2e8, 4, "thread init early success");
    install_device_call_log(image_.base(), 0x193e2d4, 0x193e2db);
    install_setns_log(image_.base(), 0x193e1c8, 0x193e2ea);
    if (GetEnvironmentVariableW(L"EUPHORIA_THREAD_FALLBACK", nullptr, 0) != 0) {
        install_thread_fallback(image_.base(), 0xcc2834);
    }

    log::warn("guest: entering title entry point");

    const std::size_t stack = image_.stack_reserve() >= (1U << 20)
                                ? image_.stack_reserve()
                                : (1U << 20);
    HANDLE thread = CreateThread(nullptr, stack, boot_thread, &context, 0, nullptr);
    if (thread == nullptr) {
        log::error("guest: CreateThread failed, error " + std::to_string(GetLastError()));
        return 1;
    }

    WaitForSingleObject(thread, INFINITE);
    DWORD thread_code = 0;
    GetExitCodeThread(thread, &thread_code);
    CloseHandle(thread);

    if (context.exception_code != 0) {
        const auto base = reinterpret_cast<std::uintptr_t>(image_.base());
        const auto rva = [base](std::uintptr_t address) {
            char text[48]{};
            if (address >= base) {
                std::snprintf(text, sizeof(text), "0x%llx",
                              static_cast<unsigned long long>(address - base));
            } else {
                std::snprintf(text, sizeof(text), "0x%llx",
                              static_cast<unsigned long long>(address));
            }
            return std::string(text);
        };

        log::error("guest: unhandled guest exception 0x"
                   + std::to_string(context.exception_code)
                   + " at rva " + rva(context.fault_rip)
                   + " (record rva " + rva(context.exception_address) + ")");
        for (unsigned short index = 0; index < context.frame_count; ++index) {
            log::error("  frame " + std::to_string(index) + ": rva "
                       + rva(reinterpret_cast<std::uintptr_t>(context.frames[index])));
        }
        log::error("  rcx " + rva(context.registers[0])
                   + " rdx " + rva(context.registers[1])
                   + " r8 " + rva(context.registers[2])
                   + " r9 " + rva(context.registers[3]));
        return static_cast<int>(context.exception_code);
    }

    log::info("guest: title returned " + std::to_string(context.result));
    return context.result;
}

void Guest::shutdown() noexcept
{
    if (!initialized_) {
        return;
    }

    image_.abandon();
    log::info("guest: shutdown");
    initialized_ = false;
}

}
