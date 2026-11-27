#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>
#include <euphoria/shim/kernel.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>

#include <windows.h>

namespace euphoria::shim::gdk {
namespace {

std::string hex(std::uint64_t value)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::string tag(std::string_view text, std::string_view name)
{
    const std::string open = "<" + std::string(name) + ">";
    const std::string close = "</" + std::string(name) + ">";
    const std::size_t begin = text.find(open);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t start = begin + open.size();
    const std::size_t end = text.find(close, start);
    if (end == std::string_view::npos) {
        return {};
    }
    std::string value(text.substr(start, end - start));
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    return value;
}

std::filesystem::path local_data_root()
{
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length != 0 && length < MAX_PATH) {
        return std::filesystem::path(buffer) / L"Euphoria";
    }
    return std::filesystem::temp_directory_path() / L"Euphoria";
}

Title load_title()
{
    Title result;
    result.storage_bytes = 2048ull << 20;
    wchar_t path[MAX_PATH * 2]{};
    const std::uint32_t length = kernel::get_module_file_name_w(nullptr, path, MAX_PATH * 2);
    if (length != 0) {
        result.package = std::filesystem::path(std::wstring(path, length)).parent_path();
    }
    if (!result.package.empty()) {
        std::ifstream file(result.package / L"MicrosoftGame.config", std::ios::binary);
        if (file) {
            std::stringstream contents;
            contents << file.rdbuf();
            const std::string text = contents.str();
            const std::string id = tag(text, "TitleId");
            if (!id.empty()) {
                result.id = static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 16));
            }
            result.store_id = tag(text, "StoreId");
            const std::string storage = tag(tag(text, "PersistentLocalStorage"), "SizeMB");
            if (!storage.empty()) {
                result.storage_bytes = std::strtoull(storage.c_str(), nullptr, 10) << 20;
            }
        }
    }
    char folder[16]{};
    std::snprintf(folder, sizeof(folder), "%08X", result.id);
    result.data = local_data_root() / folder;
    std::error_code error;
    std::filesystem::create_directories(result.data, error);
    log::info("gdk: title " + std::string(folder) + " store " + (result.store_id.empty() ? "?" : result.store_id)
              + " data " + result.data.string());
    return result;
}

using Area = std::span<const Interface> (*)();

constexpr std::array<Area, 12> kAreas{{
    &task_queue_interfaces,
    &user_interfaces,
    &system_interfaces,
    &game_interfaces,
    &storage_interfaces,
    &networking_interfaces,
    &package_interfaces,
    &display_interfaces,
    &game_save_interfaces,
    &store_interfaces,
    &game_ui_interfaces,
    &speech_interfaces,
}};

HResult xgame_runtime_initialize()
{
    log::info("gdk: XGameRuntimeInitialize");
    return kOk;
}

void xgame_runtime_uninitialize()
{
    log::info("gdk: XGameRuntimeUninitialize");
}

HResult initialize_api_impl(std::uint64_t version, std::uint64_t build)
{
    log::info("gdk: InitializeApiImpl version " + hex(version) + " build " + hex(build));
    return kOk;
}

HResult initialize_api_impl_ex(std::uint64_t version, std::uint64_t build, std::uint64_t flags)
{
    log::info("gdk: InitializeApiImplEx version " + hex(version) + " build " + hex(build) + " flags "
              + hex(flags));
    return kOk;
}

HResult initialize_api_impl_ex2(std::uint64_t version, std::uint64_t build, std::uint64_t flags,
                                const void* options)
{
    log::info("gdk: InitializeApiImplEx2 version " + hex(version) + " build " + hex(build) + " flags "
              + hex(flags) + " options " + hex(reinterpret_cast<std::uintptr_t>(options)));
    return kOk;
}

void uninitialize_api_impl()
{
    log::info("gdk: UninitializeApiImpl");
}

void xerror_report(HResult result, const char* message)
{
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08x", static_cast<unsigned>(result));
    log::warn(std::string("gdk: XErrorReport ") + code + " " + text(message));
}

struct Export {
    std::string_view name;
    std::uint32_t ordinal;
    void* address;
};

struct PseudoModule {
    std::string_view name;
    std::span<const Export> exports;
};

const std::array<Export, 8> kRuntimeExports{{
    {"XGameRuntimeInitialize", 0, reinterpret_cast<void*>(&xgame_runtime_initialize)},
    {"XGameRuntimeUninitialize", 0, reinterpret_cast<void*>(&xgame_runtime_uninitialize)},
    {"InitializeApiImpl", 0, reinterpret_cast<void*>(&initialize_api_impl)},
    {"InitializeApiImplEx", 0, reinterpret_cast<void*>(&initialize_api_impl_ex)},
    {"InitializeApiImplEx2", 0, reinterpret_cast<void*>(&initialize_api_impl_ex2)},
    {"UninitializeApiImpl", 0, reinterpret_cast<void*>(&uninitialize_api_impl)},
    {"QueryApiImpl", 0, reinterpret_cast<void*>(&query_api_impl)},
    {"XErrorReport", 0, reinterpret_cast<void*>(&xerror_report)},
}};

const std::array<Export, 1> kGameInputExports{{
    {"GameInputCreate", 1, reinterpret_cast<void*>(&game_input_create)},
}};

const std::array<PseudoModule, 2> kModules{{
    {"xgameruntime.dll", kRuntimeExports},
    {"gameinput.dll", kGameInputExports},
}};

std::string module_file(std::string_view name)
{
    const std::size_t slash = name.find_last_of("\\/");
    if (slash != std::string_view::npos) {
        name = name.substr(slash + 1);
    }
    std::string file;
    file.reserve(name.size() + 4);
    for (const char ch : name) {
        file.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (file.find('.') == std::string::npos) {
        file += ".dll";
    }
    return file;
}

std::mutex& first_mutex()
{
    static std::mutex mutex;
    return mutex;
}

}

std::string to_string(const Guid& guid)
{
    char text[40]{};
    std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid.data1,
                  guid.data2, guid.data3, guid.data4[0], guid.data4[1], guid.data4[2], guid.data4[3],
                  guid.data4[4], guid.data4[5], guid.data4[6], guid.data4[7]);
    return text;
}

HResult missing_slot(const Object* self, std::size_t slot)
{
    const std::string area = self != nullptr && self->area != nullptr ? self->area : "?";
    char offset[16]{};
    std::snprintf(offset, sizeof(offset), "0x%zx", slot * 8);
    log_first("gdk: runtime method not implemented: " + area + " slot " + offset);
    return kNotImplemented;
}

std::uint32_t add_ref(const Object*)
{
    return 1;
}

HResult put_string(std::string_view value, std::size_t size, char* buffer, std::size_t* used)
{
    const std::size_t length = value.size() + 1;
    if (buffer == nullptr || size < length) {
        return kNotSufficientBuffer;
    }
    std::memcpy(buffer, value.data(), value.size());
    buffer[value.size()] = 0;
    if (used != nullptr) {
        *used = length;
    }
    return kOk;
}

std::string text(const char* value)
{
    return value == nullptr ? std::string{} : std::string(value);
}

std::uint64_t next_token()
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

void log_first(std::string_view api)
{
    {
        std::scoped_lock lock(first_mutex());
        static auto* seen = new std::unordered_set<std::string>;
        if (!seen->emplace(api).second) {
            return;
        }
    }
    if (api.starts_with("gdk: runtime method not implemented")) {
        log::warn(api);
    } else if (api.find(':') != std::string_view::npos) {
        log::info(api);
    } else {
        log::info("gdk: " + std::string(api));
    }
}

const Title& title()
{
    static const Title value = load_title();
    return value;
}

std::string package_identifier()
{
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08X", title().id);
    return text;
}

std::uint64_t disk_free(const std::filesystem::path& directory)
{
    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(directory.c_str(), &free, nullptr, nullptr) != 0) {
        return free.QuadPart;
    }
    return ~0ull;
}

HResult query_api_impl(const Guid* provider, const Guid* iid, void** out)
{
    if (provider == nullptr || iid == nullptr) {
        return kNotSupported;
    }
    for (const Area area : kAreas) {
        for (const auto& entry : area()) {
            if (entry.provider == *provider && entry.iid == *iid) {
                if (out == nullptr) {
                    return kInvalidArgument;
                }
                *out = const_cast<Object*>(entry.object);
                log_first("gdk: QueryApiImpl " + std::string(entry.object->area) + " " + to_string(*provider)
                          + " / " + to_string(*iid));
                return kOk;
            }
        }
    }
    static std::mutex mutex;
    static std::set<std::string> reported;
    const std::string key = to_string(*provider) + " / " + to_string(*iid);
    {
        std::scoped_lock lock(mutex);
        if (!reported.insert(key).second) {
            return kNotSupported;
        }
    }
    log::warn("gdk: QueryApiImpl area not implemented: provider " + key);
    return kNotSupported;
}

void* pseudo_module(std::string_view name)
{
    const std::string file = module_file(name);
    for (const auto& module : kModules) {
        if (module.name == file) {
            return const_cast<PseudoModule*>(&module);
        }
    }
    return nullptr;
}

bool is_pseudo_module(const void* handle) noexcept
{
    for (const auto& module : kModules) {
        if (handle == &module) {
            return true;
        }
    }
    return false;
}

void* pseudo_export(const void* handle, const char* name)
{
    const auto* module = static_cast<const PseudoModule*>(handle);
    const auto value = reinterpret_cast<std::uintptr_t>(name);
    if (value < 0x10000) {
        for (const auto& entry : module->exports) {
            if (entry.ordinal != 0 && entry.ordinal == value) {
                log::info("gdk: GetProcAddress " + std::string(module->name) + "!#" + std::to_string(value));
                return entry.address;
            }
        }
        log::warn("gdk: GetProcAddress " + std::string(module->name) + "!#" + std::to_string(value)
                  + " not implemented");
        return nullptr;
    }
    for (const auto& entry : module->exports) {
        if (entry.name == name) {
            log::info("gdk: GetProcAddress " + std::string(module->name) + "!" + name);
            return entry.address;
        }
    }
    log::warn("gdk: GetProcAddress " + std::string(module->name) + "!" + name + " not implemented");
    return nullptr;
}

}
