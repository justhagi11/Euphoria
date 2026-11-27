#include <euphoria/shim/kernel.hpp>

#include <euphoria/log/log.hpp>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <string_view>

#include <windows.h>

namespace euphoria::shim::kernel {
namespace {

std::uintptr_t g_main_image = 0;
std::uintptr_t g_tls_callbacks = 0;
std::wstring g_main_path;
std::wstring g_main_filename;
std::wstring g_host_filename;
std::wstring g_console_root;
std::wstring g_temp_root;

bool same_name(const std::wstring& left, const std::wstring& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) {
            return false;
        }
    }
    return true;
}

std::uintptr_t find_export(std::uintptr_t base, const char* name)
{
    if (base == 0 || name == nullptr) {
        return 0;
    }

    const auto* base_bytes = reinterpret_cast<const std::uint8_t*>(base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        return 0;
    }

    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base_bytes + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base_bytes + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base_bytes + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base_bytes + exports->AddressOfFunctions);

    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const char* candidate = reinterpret_cast<const char*>(base_bytes + names[index]);
        if (std::strcmp(candidate, name) == 0) {
            const DWORD rva = functions[ordinals[index]];
            return base + rva;
        }
    }
    return 0;
}

std::wstring basename(const wchar_t* path)
{
    if (path == nullptr) {
        return {};
    }
    const wchar_t* last = path;
    for (const wchar_t* cursor = path; *cursor != 0; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            last = cursor + 1;
        }
    }
    return last;
}

bool is_main_module(void* module)
{
    return module == nullptr || reinterpret_cast<std::uintptr_t>(module) == g_main_image;
}

std::uint32_t copy_path(const std::wstring& path, wchar_t* buffer, std::uint32_t size)
{
    if (buffer == nullptr || size == 0) {
        return static_cast<std::uint32_t>(path.size());
    }
    const std::size_t length = std::min<std::size_t>(path.size(), size - 1);
    std::memcpy(buffer, path.data(), length * sizeof(wchar_t));
    buffer[length] = L'\0';
    return static_cast<std::uint32_t>(length);
}

}

void set_main_image(std::uintptr_t base, const wchar_t* path)
{
    g_main_image = base;
    g_main_path = path != nullptr ? path : L"";
    g_main_filename = basename(path);

    wchar_t host[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, host, MAX_PATH) != 0) {
        g_host_filename = basename(host);
    }

    const std::size_t slash = g_main_path.find_last_of(L"\\/");
    g_console_root = slash == std::wstring::npos ? std::wstring{} : g_main_path.substr(0, slash);

    wchar_t temporary[MAX_PATH * 2]{};
    const DWORD length = GetTempPathW(MAX_PATH * 2, temporary);
    g_temp_root = length > 0 ? std::wstring(temporary, length) + L"Euphoria" : std::wstring{};
    if (!g_temp_root.empty()) {
        CreateDirectoryW(g_temp_root.c_str(), nullptr);
    }
}

bool is_main_image(std::uintptr_t base) noexcept
{
    return base != 0 && base == g_main_image;
}

std::uintptr_t main_image() noexcept
{
    return g_main_image;
}

void set_tls_callbacks(std::uintptr_t callbacks) noexcept
{
    g_tls_callbacks = callbacks;
}

std::uintptr_t tls_callbacks() noexcept
{
    return g_tls_callbacks;
}

void* __stdcall get_module_handle_w(const wchar_t* name)
{
    if (name == nullptr || same_name(name, g_main_filename) || same_name(name, g_host_filename)) {
        return reinterpret_cast<void*>(g_main_image);
    }
    return GetModuleHandleW(name);
}

void* __stdcall get_module_handle_a(const char* name)
{
    if (name == nullptr) {
        return reinterpret_cast<void*>(g_main_image);
    }

    const int length = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), length);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }

    if (same_name(wide, g_main_filename) || same_name(wide, g_host_filename)) {
        return reinterpret_cast<void*>(g_main_image);
    }
    return GetModuleHandleA(name);
}

void* __stdcall get_proc_address(void* module, const char* name)
{
    if (module != nullptr && reinterpret_cast<std::uintptr_t>(module) == g_main_image && name != nullptr) {
        if (const std::uintptr_t found = find_export(g_main_image, name); found != 0) {
            return reinterpret_cast<void*>(found);
        }
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
}

std::uint32_t __stdcall get_module_file_name_w(void* module, wchar_t* buffer, std::uint32_t size)
{
    if (is_main_module(module)) {
        return copy_path(g_main_path, buffer, size);
    }
    return GetModuleFileNameW(static_cast<HMODULE>(module), buffer, size);
}

std::uint32_t __stdcall get_module_file_name_a(void* module, char* buffer, std::uint32_t size)
{
    if (is_main_module(module)) {
        if (buffer == nullptr || size == 0) {
            return static_cast<std::uint32_t>(g_main_path.size());
        }
        const int length = WideCharToMultiByte(CP_ACP, 0, g_main_path.c_str(), -1, buffer,
                                              static_cast<int>(size), nullptr, nullptr);
        return length > 0 ? static_cast<std::uint32_t>(length - 1) : 0;
    }
    return GetModuleFileNameA(static_cast<HMODULE>(module), buffer, size);
}

namespace {

int g_remap_logs = 0;

std::wstring parent_directory(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring widen(const char* text)
{
    if (text == nullptr) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), length);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }
    return wide;
}

std::string narrow_ansi(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, narrow.data(), length, nullptr, nullptr);
    if (!narrow.empty() && narrow.back() == '\0') {
        narrow.pop_back();
    }
    return narrow;
}

bool remap_path(std::wstring_view input, std::wstring& output)
{
    if (input.size() < 2 || input[1] != L':') {
        return false;
    }

    const wchar_t drive = std::towupper(input[0]);
    const std::wstring& root = drive == L'G' ? g_console_root
                             : drive == L'T' ? g_temp_root
                                             : std::wstring{};
    if (root.empty()) {
        return false;
    }

    output = root;
    for (std::size_t index = 2; index < input.size(); ++index) {
        output.push_back(input[index] == L'/' ? L'\\' : input[index]);
    }
    while (output.size() > root.size() && output.back() == L'\\') {
        output.pop_back();
    }

    if (g_remap_logs < 24) {
        ++g_remap_logs;
        log::info("path: " + narrow_ansi(std::wstring(input)) + " -> " + narrow_ansi(output));
    }
    return true;
}

const wchar_t* remapped_w(const wchar_t* path, std::wstring& storage)
{
    if (path != nullptr && remap_path(path, storage)) {
        return storage.c_str();
    }
    return path;
}

const char* remapped_a(const char* path, std::wstring& wide_storage, std::string& narrow_storage)
{
    if (path == nullptr) {
        return path;
    }
    const std::wstring wide = widen(path);
    if (!remap_path(wide, wide_storage)) {
        return path;
    }
    narrow_storage = narrow_ansi(wide_storage);
    return narrow_storage.c_str();
}

}

void* __stdcall create_file_w(const wchar_t* path, std::uint32_t access, std::uint32_t share,
                              void* attributes, std::uint32_t disposition, std::uint32_t flags,
                              void* template_file)
{
    std::wstring storage;
    return CreateFileW(remapped_w(path, storage), access, share,
                       static_cast<LPSECURITY_ATTRIBUTES>(attributes), disposition, flags,
                       static_cast<HANDLE>(template_file));
}

void* __stdcall create_file_a(const char* path, std::uint32_t access, std::uint32_t share,
                              void* attributes, std::uint32_t disposition, std::uint32_t flags,
                              void* template_file)
{
    std::wstring wide_storage;
    std::string narrow_storage;
    return CreateFileA(remapped_a(path, wide_storage, narrow_storage), access, share,
                       static_cast<LPSECURITY_ATTRIBUTES>(attributes), disposition, flags,
                       static_cast<HANDLE>(template_file));
}

std::uint32_t __stdcall get_file_attributes_w(const wchar_t* path)
{
    std::wstring storage;
    return GetFileAttributesW(remapped_w(path, storage));
}

std::uint32_t __stdcall get_file_attributes_a(const char* path)
{
    std::wstring wide_storage;
    std::string narrow_storage;
    return GetFileAttributesA(remapped_a(path, wide_storage, narrow_storage));
}

int __stdcall create_directory_w(const wchar_t* path, void* attributes)
{
    std::wstring storage;
    return CreateDirectoryW(remapped_w(path, storage), static_cast<LPSECURITY_ATTRIBUTES>(attributes));
}

int __stdcall create_directory_a(const char* path, void* attributes)
{
    std::wstring wide_storage;
    std::string narrow_storage;
    return CreateDirectoryA(remapped_a(path, wide_storage, narrow_storage),
                            static_cast<LPSECURITY_ATTRIBUTES>(attributes));
}

int __stdcall delete_file_w(const wchar_t* path)
{
    std::wstring storage;
    return DeleteFileW(remapped_w(path, storage));
}

int __stdcall delete_file_a(const char* path)
{
    std::wstring wide_storage;
    std::string narrow_storage;
    return DeleteFileA(remapped_a(path, wide_storage, narrow_storage));
}

int __stdcall move_file_ex_w(const wchar_t* from, const wchar_t* to, std::uint32_t flags)
{
    std::wstring from_storage;
    std::wstring to_storage;
    return MoveFileExW(remapped_w(from, from_storage), remapped_w(to, to_storage), flags);
}

int __stdcall move_file_ex_a(const char* from, const char* to, std::uint32_t flags)
{
    std::wstring from_wide;
    std::wstring to_wide;
    std::string from_narrow;
    std::string to_narrow;
    return MoveFileExA(remapped_a(from, from_wide, from_narrow), remapped_a(to, to_wide, to_narrow), flags);
}

void* __stdcall find_first_file_w(const wchar_t* pattern, void* data)
{
    std::wstring storage;
    return FindFirstFileW(remapped_w(pattern, storage), static_cast<WIN32_FIND_DATAW*>(data));
}

void* __stdcall find_first_file_a(const char* pattern, void* data)
{
    std::wstring wide_storage;
    std::string narrow_storage;
    return FindFirstFileA(remapped_a(pattern, wide_storage, narrow_storage),
                          static_cast<WIN32_FIND_DATAA*>(data));
}

bool remap_guest_path(const wchar_t* path, std::wstring& output)
{
    return path != nullptr && remap_path(path, output);
}

std::int16_t __stdcall register_class_ex_a(const void* description)
{
    const auto* window_class = static_cast<const WNDCLASSEXA*>(description);
    const ATOM result = RegisterClassExA(window_class);
    char text[160]{};
    std::snprintf(text, sizeof(text), "class %s instance %p -> %u (error %lu)",
                  window_class != nullptr && window_class->lpszClassName != nullptr
                      ? window_class->lpszClassName
                      : "?",
                  window_class != nullptr ? reinterpret_cast<void*>(window_class->hInstance) : nullptr,
                  static_cast<unsigned>(result), GetLastError());
    log::info(std::string("user32: RegisterClassExA ") + text);
    return static_cast<std::int16_t>(result);
}

std::int16_t __stdcall register_class_ex_w(const void* description)
{
    const auto* window_class = static_cast<const WNDCLASSEXW*>(description);
    const ATOM result = RegisterClassExW(window_class);
    char name[128]{};
    if (window_class != nullptr && window_class->lpszClassName != nullptr) {
        WideCharToMultiByte(CP_ACP, 0, window_class->lpszClassName, -1, name, sizeof(name), nullptr,
                            nullptr);
    }
    log::info("user32: RegisterClassExW " + std::string(name) + " -> "
              + std::to_string(static_cast<unsigned>(result)) + " (error "
              + std::to_string(GetLastError()) + ")");
    return static_cast<std::int16_t>(result);
}

void* __stdcall create_window_ex_a(std::uint32_t ex_style, const char* class_name,
                                   const char* window_name, std::uint32_t style, int x, int y,
                                   int width, int height, void* parent, void* menu,
                                   void* instance, void* parameter)
{
    void* window = CreateWindowExA(ex_style, class_name, window_name, style, x, y, width, height,
                                   static_cast<HWND>(parent), static_cast<HMENU>(menu),
                                   static_cast<HINSTANCE>(instance), parameter);
    log::info("user32: CreateWindowExA " + std::string(class_name != nullptr ? class_name : "?")
              + " -> " + std::to_string(reinterpret_cast<std::uintptr_t>(window)) + " (error "
              + std::to_string(GetLastError()) + ")");
    return window;
}

void* __stdcall create_window_ex_w(std::uint32_t ex_style, const wchar_t* class_name,
                                   const wchar_t* window_name, std::uint32_t style, int x, int y,
                                   int width, int height, void* parent, void* menu,
                                   void* instance, void* parameter)
{
    void* window = CreateWindowExW(ex_style, class_name, window_name, style, x, y, width, height,
                                   static_cast<HWND>(parent), static_cast<HMENU>(menu),
                                   static_cast<HINSTANCE>(instance), parameter);
    log::info("user32: CreateWindowExW -> " + std::to_string(reinterpret_cast<std::uintptr_t>(window))
              + " (error " + std::to_string(GetLastError()) + ")");
    return window;
}

}
