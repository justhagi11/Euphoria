#include <euphoria/host/host.hpp>

#include <euphoria/loader/pe.hpp>
#include <euphoria/log/log.hpp>
#include <euphoria/log/trace.hpp>
#include <euphoria/runtime/guest.hpp>
#include <euphoria/shim/kernel.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <mutex>

#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace euphoria::host {
namespace {

std::string hex(std::uint64_t value)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
    return text;
}

bool relaunch_in_conhost()
{
    if (GetConsoleWindow() != nullptr) {
        return false;
    }

    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"EUPHORIA_CONHOST", flag, static_cast<DWORD>(std::size(flag))) > 0) {
        return false;
    }

    if (GetEnvironmentVariableW(L"EUPHORIA_NO_CONHOST", flag, static_cast<DWORD>(std::size(flag))) > 0) {
        AllocConsole();
        return false;
    }

    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        return false;
    }

    std::wstring command = L"conhost.exe \"";
    command += self;
    command += L"\"";

    SetEnvironmentVariableW(L"EUPHORIA_CONHOST", L"1");

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup, &process)
        == 0) {
        AllocConsole();
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

std::optional<std::filesystem::path> find_guest_image()
{
    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        return std::nullopt;
    }

    const std::filesystem::path executable{self};
    const std::filesystem::path directory = executable.parent_path();
    const std::string stem = executable.stem().string();

    const std::vector<std::filesystem::path> candidates{
        directory / (stem + ".guest.exe"),
        directory / (stem + ".xbox.exe"),
        directory / (stem + ".orig.exe"),
        directory / "game_scarlett_master.guest.exe",
        directory / "game_scarlett_master.exe",
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            continue;
        }
        if (std::filesystem::equivalent(candidate, executable, ec)) {
            continue;
        }
        return candidate;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> title_from_environment()
{
    wchar_t value[MAX_PATH * 4]{};
    const DWORD length = GetEnvironmentVariableW(L"EUPHORIA_TITLE", value, static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(value, ec)) {
        return std::nullopt;
    }
    return std::filesystem::path{value};
}

void enable_debug_logging()
{
    wchar_t value[8]{};
    if (GetEnvironmentVariableW(L"EUPHORIA_VERBOSE", value, static_cast<DWORD>(std::size(value))) > 0) {
        log::set_verbose(true);
    }
}

std::filesystem::path cache_directory()
{
    wchar_t value[MAX_PATH * 2]{};
    if (GetEnvironmentVariableW(L"EUPHORIA_CACHE", value, static_cast<DWORD>(std::size(value))) > 0) {
        return std::filesystem::path{value};
    }

    wchar_t local[MAX_PATH * 2]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local))) > 0) {
        return std::filesystem::path{local} / L"Euphoria" / L"images";
    }
    return std::filesystem::temp_directory_path() / L"Euphoria" / L"images";
}

LONG WINAPI unhandled_exception(EXCEPTION_POINTERS* info)
{
    const DWORD code = info != nullptr && info->ExceptionRecord != nullptr
                         ? info->ExceptionRecord->ExceptionCode
                         : 0;
    const auto* address = info != nullptr && info->ExceptionRecord != nullptr
                            ? info->ExceptionRecord->ExceptionAddress
                            : nullptr;
    log::error("host: unhandled exception 0x" + hex(code) + " at 0x"
               + hex(reinterpret_cast<std::uintptr_t>(address)));
    return EXCEPTION_CONTINUE_SEARCH;
}

std::string narrow(const wchar_t* text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

void log_tls_modules()
{
    HMODULE modules[512]{};
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed) == 0) {
        return;
    }
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD index = 0; index < count && index < 512; ++index) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(modules[index]);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            continue;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(data + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            continue;
        }
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (directory.VirtualAddress == 0 || directory.Size < 0x18) {
            continue;
        }
        const auto* tls = data + directory.VirtualAddress;
        const auto index_va = *reinterpret_cast<const std::uint64_t*>(tls + 16);
        std::uint32_t slot = 0;
        if (index_va != 0) {
            slot = *reinterpret_cast<const std::uint32_t*>(index_va);
        }
        wchar_t name[MAX_PATH]{};
        GetModuleBaseNameW(GetCurrentProcess(), modules[index], name, MAX_PATH);
        log::debug("tls: " + narrow(name) + " index " + std::to_string(slot));
    }
}

std::string symbolize(std::uintptr_t address)
{
    static std::once_flag initialized;
    static bool ready = false;
    std::call_once(initialized, [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        ready = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    });
    if (!ready) {
        return {};
    }
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol) == FALSE) {
        return {};
    }
    std::string text = std::string(symbol->Name) + "+0x" + hex(displacement);
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD column = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), address, &column, &line) != FALSE
        && line.FileName != nullptr) {
        const std::string file = line.FileName;
        const std::size_t slash = file.find_last_of("\\/");
        text += " (" + (slash == std::string::npos ? file : file.substr(slash + 1)) + ":"
              + std::to_string(line.LineNumber) + ")";
    }
    return text;
}

std::string unwind_frames(const CONTEXT& start)
{
    CONTEXT walk = start;
    std::string frames;
    for (int depth = 0; depth < 16 && walk.Rip != 0; ++depth) {
        const auto rip = static_cast<std::uintptr_t>(walk.Rip);
        HMODULE owner = nullptr;
        wchar_t name[MAX_PATH]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(rip), &owner)
            && owner != nullptr) {
            GetModuleFileNameW(owner, name, MAX_PATH);
            std::wstring path = name;
            const std::size_t slash = path.find_last_of(L"\\/");
            frames += " " + narrow((slash == std::wstring::npos ? path : path.substr(slash + 1)).c_str())
                    + "+0x" + hex(rip - reinterpret_cast<std::uintptr_t>(owner));
        } else {
            frames += " 0x" + hex(rip);
        }
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(walk.Rip, &image_base, nullptr);
        if (function == nullptr) {
            if (walk.Rsp == 0) {
                break;
            }
            std::uint64_t returned = 0;
            SIZE_T read = 0;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(walk.Rsp),
                                  &returned, sizeof(returned), &read)
                == FALSE) {
                break;
            }
            walk.Rip = returned;
            walk.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, walk.Rip, function, &walk,
                         &handler_data, &establisher, nullptr);
    }
    return frames;
}

std::uint64_t g_watch_address = 0;

void arm_watch(DWORD thread_id)
{
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                               FALSE, thread_id);
    if (thread == nullptr) {
        return;
    }
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context) != FALSE) {
            context.Dr0 = g_watch_address;
            context.Dr7 = (context.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);
            SetThreadContext(thread, &context);
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
}

DWORD WINAPI watch_threads(void*)
{
    std::vector<DWORD> armed;
    const DWORD self = GetCurrentThreadId();
    for (;;) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL more = Thread32First(snapshot, &entry); more != FALSE;
                 more = Thread32Next(snapshot, &entry)) {
                if (entry.th32OwnerProcessID != GetCurrentProcessId()
                    || entry.th32ThreadID == self
                    || std::find(armed.begin(), armed.end(), entry.th32ThreadID) != armed.end()) {
                    continue;
                }
                arm_watch(entry.th32ThreadID);
                armed.push_back(entry.th32ThreadID);
            }
            CloseHandle(snapshot);
        }
        Sleep(5);
    }
}

void start_watch()
{
    wchar_t text[32]{};
    if (GetEnvironmentVariableW(L"EUPHORIA_WATCH", text, std::size(text)) == 0) {
        return;
    }
    g_watch_address = std::wcstoull(text, nullptr, 16) & ~7ull;
    if (g_watch_address == 0) {
        return;
    }
    log::warn("host: watching writes to 0x" + hex(g_watch_address));
    CloseHandle(CreateThread(nullptr, 0, &watch_threads, nullptr, 0, nullptr));
}

LONG WINAPI first_chance_exception(EXCEPTION_POINTERS* info)
{
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && g_watch_address != 0 && info->ContextRecord != nullptr
        && (info->ContextRecord->Dr6 & 1) != 0) {
        static std::atomic<std::uint32_t> hits{0};
        if (hits.fetch_add(1, std::memory_order_relaxed) < 400) {
            const std::uint64_t value = *reinterpret_cast<const volatile std::uint64_t*>(g_watch_address);
            log::warn("host: watch write value 0x" + hex(value) + " thread "
                      + std::to_string(GetCurrentThreadId()) + ":"
                      + unwind_frames(*info->ContextRecord));
        }
        info->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == 0xE06D7363 && !log::in_write()) {
        const EXCEPTION_RECORD* record = info->ExceptionRecord;
        std::string type = "?";
        if (record->NumberParameters >= 4) {
            const auto base = static_cast<std::uintptr_t>(record->ExceptionInformation[3]);
            const auto* throw_info =
                reinterpret_cast<const std::int32_t*>(record->ExceptionInformation[2]);
            if (base != 0 && throw_info != nullptr && throw_info[3] != 0) {
                const auto* array = reinterpret_cast<const std::int32_t*>(base + throw_info[3]);
                if (array[0] > 0 && array[1] != 0) {
                    const auto* catchable = reinterpret_cast<const std::int32_t*>(base + array[1]);
                    if (catchable[1] != 0) {
                        type = reinterpret_cast<const char*>(base + catchable[1] + 16);
                    }
                }
            }
        }
        std::string trace;
        void* frames[40]{};
        const USHORT captured = RtlCaptureStackBackTrace(0, 40, frames, nullptr);
        for (USHORT index = 0; index < captured; ++index) {
            HMODULE owner = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(frames[index]), &owner)
                && owner != nullptr) {
                wchar_t name[MAX_PATH]{};
                GetModuleBaseNameW(GetCurrentProcess(), owner, name, MAX_PATH);
                trace += " " + narrow(name) + "+0x"
                       + hex(reinterpret_cast<std::uintptr_t>(frames[index])
                             - reinterpret_cast<std::uintptr_t>(owner));
            } else {
                trace += " 0x" + hex(reinterpret_cast<std::uintptr_t>(frames[index]));
            }
        }
        log::error("host: C++ exception " + type + " stack:" + trace);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == 0xE06D7363 || code == 0x40010006 || code == 0x4001000A || code == 0xE0000001
        || code == 0x406D1388 || log::in_write()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);

    wchar_t module_name[MAX_PATH]{};
    HMODULE faulting = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &faulting)
        && faulting != nullptr) {
        GetModuleBaseNameW(GetCurrentProcess(), faulting, module_name, MAX_PATH);
    }

    const std::uintptr_t module_base = reinterpret_cast<std::uintptr_t>(faulting);
    const std::uintptr_t guest = shim::kernel::main_image();

    if (guest != 0 && address == guest) {
        log::error("host: first-chance exception 0x" + hex(code) + " at guest+0x0");
    } else if (faulting != nullptr && address >= module_base) {
        std::string where = narrow(module_name) + "+0x" + hex(address - module_base);
        if (faulting == GetModuleHandleW(nullptr)) {
            const std::string symbol = symbolize(address);
            if (!symbol.empty()) {
                where += " [" + symbol + "]";
            }
        }
        log::error("host: first-chance exception 0x" + hex(code) + " at " + where);
    } else {
        log::error("host: first-chance exception 0x" + hex(code) + " at 0x" + hex(address));
    }

    if ((code == 0xC0000005 || code == 0x80000003) && info->ContextRecord != nullptr) {
        const CONTEXT* context = info->ContextRecord;
        log::error("host:   rax=0x" + hex(static_cast<std::uint64_t>(context->Rax))
                   + " rbx=0x" + hex(static_cast<std::uint64_t>(context->Rbx))
                   + " rcx=0x" + hex(static_cast<std::uint64_t>(context->Rcx))
                   + " rdx=0x" + hex(static_cast<std::uint64_t>(context->Rdx))
                   + " rsi=0x" + hex(static_cast<std::uint64_t>(context->Rsi))
                   + " rdi=0x" + hex(static_cast<std::uint64_t>(context->Rdi))
                   + " r15=0x" + hex(static_cast<std::uint64_t>(context->R15))
                   + " r12=0x" + hex(static_cast<std::uint64_t>(context->R12)));
        const auto* stack = reinterpret_cast<const std::uint64_t*>(context->Rsp);
        std::string trace;
        if (stack != nullptr) {
            for (int i = 0; i < 16; ++i) {
                const std::uint64_t value = stack[i];
                if (value >= guest && value < guest + 0x8000000) {
                    trace += " 0x" + hex(value - guest);
                }
            }
        }
        if (!trace.empty()) {
            log::error("host:   guest stack:" + trace);
        }
        if (code == 0xC0000005 || code == 0x80000003) {
            log::error("host:   unwound:" + unwind_frames(*context));
        }
        if (code == 0xC0000005 && stack != nullptr) {
            const auto self = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
            MODULEINFO own{};
            GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &own, sizeof(own));
            std::string host_trace;
            int found = 0;
            for (int i = 0; i < 1024 && found < 10; ++i) {
                std::uint64_t value = 0;
                SIZE_T read = 0;
                if (ReadProcessMemory(GetCurrentProcess(), stack + i, &value, sizeof(value), &read)
                    == FALSE) {
                    break;
                }
                if (value > self && value < self + own.SizeOfImage) {
                    const std::string symbol = symbolize(value);
                    if (!symbol.empty()) {
                        host_trace += "\n    " + symbol;
                        ++found;
                    }
                }
            }
            if (!host_trace.empty()) {
                log::error("host:   euphoria frames on stack:" + host_trace);
            }
        }
        if (code == 0x80000003) {
            const auto try_string = [](std::uint64_t pointer) -> std::string {
                if (pointer < 0x10000) {
                    return {};
                }
                char buffer[200]{};
                SIZE_T read = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(pointer),
                                      buffer, sizeof(buffer) - 1, &read)
                    == FALSE) {
                    return {};
                }
                std::string text;
                for (SIZE_T index = 0; index < read && buffer[index] != 0; ++index) {
                    const unsigned char c = static_cast<unsigned char>(buffer[index]);
                    if (c < 0x20 || c > 0x7E) {
                        if (c != '\n' && c != '\t') {
                            return text.size() >= 6 ? text : std::string{};
                        }
                        text.push_back(' ');
                        continue;
                    }
                    text.push_back(static_cast<char>(c));
                }
                return text.size() >= 6 ? text : std::string{};
            };
            for (const std::uint64_t object : {context->Rcx, context->Rbx}) {
                std::uint64_t table = 0;
                std::uint64_t first = 0;
                SIZE_T read = 0;
                if (object > 0x10000
                    && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(object),
                                         &table, sizeof(table), &read)
                    && table > 0x10000
                    && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(table),
                                         &first, sizeof(first), &read)
                    && guest != 0 && table >= guest && table < guest + 0x10000000) {
                    log::error("host:   object 0x" + hex(object) + " vtable guest+0x"
                               + hex(table - guest) + " slot0 guest+0x"
                               + hex(first >= guest ? first - guest : first));
                    std::uint64_t words[16]{};
                    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(object),
                                          words, sizeof(words), &read)) {
                        std::string dump;
                        for (std::uint64_t word : words) {
                            dump += " " + hex(word);
                        }
                        log::error("host:   object words:" + dump);
                    }
                }
            }
            const std::uint64_t registers[] = {context->Rcx, context->Rdx, context->R8,
                                               context->R9, context->Rsi, context->Rdi};
            const char* names[] = {"rcx", "rdx", "r8", "r9", "rsi", "rdi"};
            for (std::size_t index = 0; index < std::size(registers); ++index) {
                const std::string text = try_string(registers[index]);
                if (!text.empty()) {
                    log::error(std::string("host:   ") + names[index] + " -> \"" + text + "\"");
                }
            }
        }
        if (code == 0x80000003 && guest != 0) {
            const auto* data = reinterpret_cast<const std::uint8_t*>(guest);
            const auto read32 = [data](std::uint32_t rva) {
                return *reinterpret_cast<const std::uint32_t*>(data + rva);
            };
            log::error("host:   config a=" + std::to_string(read32(0xA9438CC))
                       + " b=" + std::to_string(read32(0xA9438D0))
                       + " copied_a=" + std::to_string(read32(0x75E6FE0))
                       + " copied_b=" + std::to_string(read32(0x75E6FE4)));
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}

int run()
{
    enable_debug_logging();
    if (relaunch_in_conhost()) {
        return 0;
    }
    if (GetConsoleWindow() == nullptr) {
        AllocConsole();
    }

    log::info("host: starting");
    SetUnhandledExceptionFilter(&unhandled_exception);
    AddVectoredExceptionHandler(1, &first_chance_exception);
    start_watch();

    std::optional<std::filesystem::path> title = title_from_environment();
    if (title) {
        log::info("using guest image from EUPHORIA_TITLE");
    } else {
        title = find_guest_image();
        if (title) {
            log::info("drop-in: using guest image " + title->string());
        }
    }

    if (!title) {
        log::error("no guest image found next to the executable");
        log::error("place the original Xbox executable next to Euphoria as <name>.guest.exe");
        return 1;
    }

    std::error_code cache_error;
    const std::filesystem::path cache = cache_directory();
    log::info("host: cache " + cache.string());
    std::filesystem::create_directories(cache, cache_error);

    runtime::Guest guest;
    log_tls_modules();
    const runtime::GuestConfig config{.title = *title, .cache_directory = cache};

    if (!guest.initialize(config)) {
        log::error("failed to load the guest image; see euphoria.log");
        return 1;
    }

    trace::install_exit_hook(reinterpret_cast<std::uintptr_t>(guest.image().base()),
                             guest.image().size_of_image());

    const int result = guest.run();
    guest.shutdown();
    return result;
}

}
