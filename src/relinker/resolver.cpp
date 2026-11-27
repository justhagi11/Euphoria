#include <euphoria/relinker/resolver.hpp>

#include <euphoria/log/log.hpp>
#include <euphoria/shim/kernel.hpp>
#include <euphoria/shim/registry.hpp>
#include <euphoria/shim/xmem.hpp>

#include <array>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <intrin.h>
#include <windows.h>

namespace euphoria::relinker {
namespace {

std::string lower(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (const char ch : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

constexpr std::array<std::string_view, 11> kGuestModules{{
    "xmem.dll",
    "xapu.dll",
    "chad.dll",
    "acphal.dll",
    "d3d12_xs.dll",
    "xg_xs.dll",
    "xcrdapi.dll",
    "bink2scarlett.dll",
    "xboxbvhbuilder.dll",
    "amd_fidelityfx_loader_xbox.dll",
    "libhttpclient.gdk.dll",
}};

constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kShimLibraries{{
    {"xmem.dll", "xapi"},
    {"xapu.dll", "xapi"},
    {"chad.dll", "xaudio2"},
    {"acphal.dll", "acphal"},
    {"d3d12_xs.dll", "d3d12"},
    {"xg_xs.dll", "xg"},
    {"xboxbvhbuilder.dll", "d3d12"},
    {"bink2scarlett.dll", "gdk"},
    {"xcrdapi.dll", "system"},
    {"libhttpclient.gdk.dll", "net"},
}};

std::string_view shim_library_for(std::string_view module)
{
    const std::string name = lower(module);
    for (const auto& [key, library] : kShimLibraries) {
        if (name == key) {
            return library;
        }
    }
    return {};
}

__declspec(noinline) void trace_exit(int code)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    char text[64]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(caller));
    log::error("trace: guest exit(" + std::to_string(code) + ") from " + text);
    ExitProcess(static_cast<UINT>(code));
}

__declspec(noinline) BOOL WINAPI trace_terminate(HANDLE, UINT code)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    char text[64]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(caller));
    log::error("trace: guest TerminateProcess(" + std::to_string(code) + ") from " + text);
    ExitProcess(code);
}

void WINAPI guest_output_debug_string_a(LPCSTR text)
{
    if (text == nullptr) {
        return;
    }
    char line[512]{};
    std::size_t length = 0;
    for (const char* at = text; *at != 0 && length + 1 < sizeof(line); ++at) {
        if (*at != '\n' && *at != '\r') {
            line[length++] = *at;
        }
    }
    if (length != 0) {
        log::warn(std::string("guest debug: ") + line);
    }
}

void WINAPI guest_output_debug_string_w(LPCWSTR text)
{
    if (text == nullptr) {
        return;
    }
    char line[512]{};
    std::size_t length = 0;
    for (const wchar_t* at = text; *at != 0 && length + 1 < sizeof(line); ++at) {
        if (*at != L'\n' && *at != L'\r') {
            line[length++] = *at < 128 ? static_cast<char>(*at) : '?';
        }
    }
    if (length != 0) {
        log::warn(std::string("guest debug: ") + line);
    }
}

struct GuestThreadStart {
    LPTHREAD_START_ROUTINE start;
    LPVOID parameter;
};

void run_guest_tls_callbacks(DWORD reason)
{
    const std::uintptr_t callbacks = shim::kernel::tls_callbacks();
    if (callbacks == 0) {
        return;
    }
    using TlsCallback = void(NTAPI*)(void*, unsigned long, void*);
    auto* list = reinterpret_cast<std::uintptr_t*>(callbacks);
    for (std::size_t index = 0; list[index] != 0; ++index) {
        reinterpret_cast<TlsCallback>(list[index])(
            reinterpret_cast<void*>(shim::kernel::main_image()), reason, nullptr);
    }
}

DWORD WINAPI guest_thread_start(LPVOID parameter)
{
    auto* entry = static_cast<GuestThreadStart*>(parameter);
    run_guest_tls_callbacks(DLL_THREAD_ATTACH);
    const DWORD result = entry->start(entry->parameter);
    run_guest_tls_callbacks(DLL_THREAD_DETACH);
    delete entry;
    return result;
}

HANDLE WINAPI guest_create_thread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack_size,
                                  LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags,
                                  LPDWORD thread_id)
{
    if (start == nullptr) {
        return CreateThread(attributes, stack_size, start, parameter, flags, thread_id);
    }
    auto* entry = new (std::nothrow) GuestThreadStart{start, parameter};
    if (entry == nullptr) {
        return CreateThread(attributes, stack_size, start, parameter, flags, thread_id);
    }
    HANDLE thread = CreateThread(attributes, stack_size, &guest_thread_start, entry, flags,
                                 thread_id);
    if (thread == nullptr) {
        const DWORD error = GetLastError();
        log::warn("thread: CreateThread failed stack " + std::to_string(stack_size)
                  + ", error " + std::to_string(error));
        delete entry;
        return nullptr;
    }
    log::info("thread: created stack " + std::to_string(stack_size) + " id "
              + std::to_string(thread_id != nullptr ? *thread_id : 0));
    return thread;
}

LPVOID WINAPI guest_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
{
    LPVOID result = nullptr;
    if (shim::xmem::guest_virtual_alloc(address, size, type, protect, result)) {
        return result;
    }
    result = VirtualAlloc(address, size, type, protect);
    if (result == nullptr) {
        log::warn("alloc: VirtualAlloc failed address " + std::to_string(reinterpret_cast<std::uintptr_t>(address))
                  + " size " + std::to_string(size) + " type " + std::to_string(type)
                  + " protect " + std::to_string(protect) + " error " + std::to_string(GetLastError()));
    }
    return result;
}

BOOL WINAPI guest_virtual_free(void* address, SIZE_T size, DWORD type)
{
    if (int handled = FALSE; shim::xmem::guest_virtual_free(address, size, type, handled)) {
        return handled;
    }
    if (VirtualFree(address, size, type) != 0) {
        return TRUE;
    }
    if ((type & MEM_RELEASE) != 0 && size != 0) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) != 0) {
            if (info.State == MEM_RESERVE) {
                return TRUE;
            }
            if (VirtualFree(address, size, MEM_DECOMMIT) != 0) {
                return TRUE;
            }
        }
        return TRUE;
    }
    return FALSE;
}

bool is_exit_entry(std::string_view name)
{
    return name == "exit" || name == "_exit" || name == "_c_exit" || name == "quick_exit";
}

std::mutex g_stub_mutex;
std::vector<std::string> g_stub_names;
std::vector<unsigned> g_stub_counts;

std::uintptr_t __stdcall stub_called(std::uintptr_t id)
{
    std::string name;
    {
        std::scoped_lock lock(g_stub_mutex);
        if (id < g_stub_names.size()) {
            unsigned& count = g_stub_counts[id];
            ++count;
            if (count > 5) {
                return 0;
            }
            name = g_stub_names[id];
            if (count > 1) {
                name += " (call " + std::to_string(count) + ")";
            }
        }
    }
    log::warn("stub: " + name);
    return 0;
}

}

Resolver::Resolver() = default;

Resolver::~Resolver()
{
    for (const auto& [name, handle] : modules_) {
        (void)name;
        if (handle != nullptr) {
            FreeLibrary(static_cast<HMODULE>(handle));
        }
    }
}

std::uintptr_t Resolver::stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    return 0;
}

bool Resolver::is_guest_module(std::string_view module)
{
    const std::string name = lower(module);
    for (const auto candidate : kGuestModules) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

void* Resolver::make_stub(std::string_view module, std::string_view name)
{
    std::uintptr_t id = 0;
    {
        std::scoped_lock lock(g_stub_mutex);
        id = g_stub_names.size();
        std::string label(module);
        label += "!";
        label += name;
        g_stub_names.push_back(std::move(label));
        g_stub_counts.push_back(0);
    }

    auto* code = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (code == nullptr) {
        return reinterpret_cast<void*>(&Resolver::stub);
    }

    const std::uint64_t handler = reinterpret_cast<std::uint64_t>(&stub_called);
    std::uint8_t bytes[23]{
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xE0,
    };
    std::memcpy(bytes + 2, &id, sizeof(id));
    std::memcpy(bytes + 12, &handler, sizeof(handler));
    std::memcpy(code, bytes, sizeof(bytes));
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(bytes));
    return code;
}

void Resolver::add_title_module(std::string_view module, void* handle)
{
    title_modules_[lower(module)] = handle;
}

void* Resolver::resolve(std::string_view module, std::string_view name, std::uint32_t ordinal, bool by_ordinal)
{
    if (const auto title = title_modules_.find(lower(module)); title != title_modules_.end()) {
        FARPROC function = by_ordinal
            ? GetProcAddress(static_cast<HMODULE>(title->second),
                             reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(ordinal)))
            : GetProcAddress(static_cast<HMODULE>(title->second), std::string(name).c_str());
        if (function != nullptr) {
            ++stats_.host;
            return reinterpret_cast<void*>(function);
        }
        log::warn("resolver: title module " + std::string(module) + " lacks "
                  + (by_ordinal ? "#" + std::to_string(ordinal) : std::string(name)));
    }
    if (!by_ordinal && is_exit_entry(name)) {
        return reinterpret_cast<void*>(&trace_exit);
    }
    if (!by_ordinal && name == "TerminateProcess") {
        return reinterpret_cast<void*>(&trace_terminate);
    }
    if (!by_ordinal && name == "VirtualAlloc") {
        return reinterpret_cast<void*>(&guest_virtual_alloc);
    }
    if (!by_ordinal && name == "VirtualFree") {
        return reinterpret_cast<void*>(&guest_virtual_free);
    }
    if (!by_ordinal && name == "OutputDebugStringA") {
        return reinterpret_cast<void*>(&guest_output_debug_string_a);
    }
    if (!by_ordinal && name == "OutputDebugStringW") {
        return reinterpret_cast<void*>(&guest_output_debug_string_w);
    }
    if (!by_ordinal && name == "CreateThread") {
        return reinterpret_cast<void*>(&guest_create_thread);
    }
    if (!by_ordinal && name == "GetModuleHandleW") {
        return reinterpret_cast<void*>(&shim::kernel::get_module_handle_w);
    }
    if (!by_ordinal && name == "GetModuleHandleA") {
        return reinterpret_cast<void*>(&shim::kernel::get_module_handle_a);
    }
    if (!by_ordinal && name == "GetProcAddress") {
        return reinterpret_cast<void*>(&shim::kernel::get_proc_address);
    }
    if (!by_ordinal && name == "GetModuleFileNameW") {
        return reinterpret_cast<void*>(&shim::kernel::get_module_file_name_w);
    }
    if (!by_ordinal && name == "GetModuleFileNameA") {
        return reinterpret_cast<void*>(&shim::kernel::get_module_file_name_a);
    }
    if (!by_ordinal && name == "CreateFileW") {
        return reinterpret_cast<void*>(&shim::kernel::create_file_w);
    }
    if (!by_ordinal && name == "CreateFileA") {
        return reinterpret_cast<void*>(&shim::kernel::create_file_a);
    }
    if (!by_ordinal && name == "GetFileAttributesW") {
        return reinterpret_cast<void*>(&shim::kernel::get_file_attributes_w);
    }
    if (!by_ordinal && name == "GetFileAttributesA") {
        return reinterpret_cast<void*>(&shim::kernel::get_file_attributes_a);
    }
    if (!by_ordinal && name == "CreateDirectoryW") {
        return reinterpret_cast<void*>(&shim::kernel::create_directory_w);
    }
    if (!by_ordinal && name == "CreateDirectoryA") {
        return reinterpret_cast<void*>(&shim::kernel::create_directory_a);
    }
    if (!by_ordinal && name == "DeleteFileW") {
        return reinterpret_cast<void*>(&shim::kernel::delete_file_w);
    }
    if (!by_ordinal && name == "DeleteFileA") {
        return reinterpret_cast<void*>(&shim::kernel::delete_file_a);
    }
    if (!by_ordinal && name == "MoveFileExW") {
        return reinterpret_cast<void*>(&shim::kernel::move_file_ex_w);
    }
    if (!by_ordinal && name == "MoveFileExA") {
        return reinterpret_cast<void*>(&shim::kernel::move_file_ex_a);
    }
    if (!by_ordinal && name == "FindFirstFileW") {
        return reinterpret_cast<void*>(&shim::kernel::find_first_file_w);
    }
    if (!by_ordinal && name == "FindFirstFileA") {
        return reinterpret_cast<void*>(&shim::kernel::find_first_file_a);
    }
    if (!by_ordinal && name == "RegisterClassExA") {
        return reinterpret_cast<void*>(&shim::kernel::register_class_ex_a);
    }
    if (!by_ordinal && name == "RegisterClassExW") {
        return reinterpret_cast<void*>(&shim::kernel::register_class_ex_w);
    }
    if (!by_ordinal && name == "CreateWindowExA") {
        return reinterpret_cast<void*>(&shim::kernel::create_window_ex_a);
    }
    if (!by_ordinal && name == "CreateWindowExW") {
        return reinterpret_cast<void*>(&shim::kernel::create_window_ex_w);
    }

    if (!by_ordinal && lower(module) == "d3d12_xs.dll") {
        if (void* shim = resolve_shim(module, name, ordinal); shim != nullptr) {
            ++stats_.shim;
            return shim;
        }
        if (void* host = resolve_host("d3d12.dll", std::string(name), ordinal, false); host != nullptr) {
            ++stats_.host;
            return host;
        }
    }

    if (is_guest_module(module)) {
        void* function = resolve_shim(module, name, ordinal);
        if (function != nullptr) {
            ++stats_.shim;
            return function;
        }
        log::debug("resolver: no shim for " + std::string(module) + "!" + std::string(name));
        ++stats_.stub;
        return make_stub(module, by_ordinal ? "#" + std::to_string(ordinal) : name);
    }

    void* function = resolve_host(module, std::string(name), ordinal, by_ordinal);
    if (function != nullptr) {
        ++stats_.host;
        return function;
    }

    log::warn("resolver: host lookup failed for " + std::string(module) + "!"
              + (by_ordinal ? "#" + std::to_string(ordinal) : std::string(name)));
    ++stats_.failed;
    return make_stub(module, "missing host function");
}

void* Resolver::resolve_host(std::string_view module, const std::string& name, std::uint32_t ordinal, bool by_ordinal)
{
    const std::string key = lower(module);
    void* handle = nullptr;
    const auto existing = modules_.find(key);
    if (existing != modules_.end()) {
        handle = existing->second;
    } else {
        HMODULE loaded = LoadLibraryA(key.c_str());
        if (loaded == nullptr) {
            log::warn("resolver: LoadLibrary failed for " + key + ", error "
                      + std::to_string(GetLastError()));
            modules_[key] = nullptr;
            return nullptr;
        }
        handle = loaded;
        modules_[key] = handle;
        log::debug("resolver: loaded host module " + key);
    }

    if (handle == nullptr) {
        return nullptr;
    }

    FARPROC function = nullptr;
    if (by_ordinal) {
        function = GetProcAddress(static_cast<HMODULE>(handle), reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(ordinal)));
    } else {
        function = GetProcAddress(static_cast<HMODULE>(handle), name.c_str());
    }
    return reinterpret_cast<void*>(function);
}

void* Resolver::resolve_shim(std::string_view module, std::string_view name, std::uint32_t ordinal)
{
    const std::string_view library = shim_library_for(module);
    if (library.empty()) {
        return nullptr;
    }

    const shim::Entry* entry = shim::resolve(library, name, ordinal);
    if (entry == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(entry->thunk);
}

}
