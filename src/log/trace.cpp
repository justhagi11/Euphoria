#include <euphoria/log/trace.hpp>

#include <euphoria/log/log.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

namespace euphoria::trace {
namespace {

std::uint8_t g_saved[12]{};
std::uintptr_t g_guest_base = 0;
std::uint32_t g_guest_size = 0;

std::string frame_name(void* address)
{
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    char text[64]{};
    if (g_guest_base != 0 && value >= g_guest_base && value < g_guest_base + g_guest_size) {
        std::snprintf(text, sizeof(text), "guest+0x%llx", static_cast<unsigned long long>(value - g_guest_base));
    } else {
        std::snprintf(text, sizeof(text), "host+0x%llx", static_cast<unsigned long long>(value));
    }
    return text;
}

__declspec(noinline) void hooked_exit(unsigned int code)
{
    log::error("trace: ExitProcess(" + std::to_string(code) + ")");

    void* frames[24]{};
    const USHORT count = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    for (USHORT index = 0; index < count; ++index) {
        log::error("  frame " + std::to_string(index) + ": " + frame_name(frames[index]));
    }

    TerminateProcess(GetCurrentProcess(), code);
}

void patch_exit_function(const char* module_name, const char* function_name)
{
    HMODULE module = GetModuleHandleA(module_name);
    if (module == nullptr) {
        return;
    }
    auto* target = reinterpret_cast<std::uint8_t*>(GetProcAddress(module, function_name));
    if (target == nullptr) {
        return;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(target, sizeof(g_saved), PAGE_EXECUTE_READWRITE, &old_protection)) {
        return;
    }
    std::memcpy(g_saved, target, sizeof(g_saved));

    const std::uint64_t handler = reinterpret_cast<std::uint64_t>(&hooked_exit);
    std::uint8_t jump[12]{0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    std::memcpy(jump + 2, &handler, sizeof(handler));
    std::memcpy(target, jump, sizeof(jump));
    VirtualProtect(target, sizeof(g_saved), old_protection, &old_protection);
    log::debug(std::string("trace: hooked ") + module_name + "!" + function_name);
}

}

void install_exit_hook(std::uintptr_t guest_base, std::uint32_t guest_size)
{
    g_guest_base = guest_base;
    g_guest_size = guest_size;
    patch_exit_function("kernelbase.dll", "ExitProcess");
    patch_exit_function("ntdll.dll", "RtlExitUserProcess");
}

}
