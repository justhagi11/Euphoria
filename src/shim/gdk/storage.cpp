#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>

#include <algorithm>

#include <windows.h>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;

char g_prompt_identity = 0;

struct SpaceInfo {
    std::uint64_t available_free;
    std::uint64_t total_free;
    std::uint64_t used;
    std::uint64_t total;
};

const std::filesystem::path& folder()
{
    static const std::filesystem::path path = [] {
        std::filesystem::path result = title().data / L"PersistentLocalStorage";
        std::error_code error;
        std::filesystem::create_directories(result, error);
        log::info("gdk: persistent local storage " + result.string());
        return result;
    }();
    return path;
}

std::string folder_text()
{
    const std::wstring wide = folder().wstring();
    const int length = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(std::max(length, 1)), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, narrow.data(), length, nullptr, nullptr);
    if (!narrow.empty() && narrow.back() == '\0') {
        narrow.pop_back();
    }
    if (!narrow.empty() && narrow.back() != '\\') {
        narrow.push_back('\\');
    }
    return narrow;
}

std::uint64_t used_bytes(const std::filesystem::path& directory)
{
    std::uint64_t total = 0;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error)) {
            total += it->file_size(error);
        }
    }
    return total;
}

SpaceInfo space()
{
    const std::uint64_t used = used_bytes(folder());
    const std::uint64_t quota = title().storage_bytes;
    const std::uint64_t free = quota > used ? quota - used : 0;
    return SpaceInfo{std::min(free, disk_free(folder())), free, used, quota};
}

HResult path_size(This, std::size_t* out)
{
    log_first("XPersistentLocalStorageGetPathSize");
    return put(out, folder_text().size() + 1);
}

HResult path(This, std::size_t size, char* buffer, std::size_t* used)
{
    log_first("XPersistentLocalStorageGetPath");
    const std::string value = folder_text();
    if (size <= value.size()) {
        return kFail;
    }
    return put_string(value, size, buffer, used);
}

HResult space_info(This, SpaceInfo* out)
{
    log_first("XPersistentLocalStorageGetSpaceInfo");
    return put(out, space());
}

HResult prompt_for_space(This, std::uint64_t bytes, AsyncBlock* block)
{
    return async_finished(block, &g_prompt_identity, bytes <= space().total_free ? kOk : kAbort);
}

HResult prompt_result(This, AsyncBlock* block)
{
    return async_get_result(block, &g_prompt_identity, 0, nullptr, nullptr);
}

const Table<8> kTable = [] {
    auto t = table<8>();
    set(t, 0x18, &path_size);
    set(t, 0x20, &path);
    set(t, 0x28, &space_info);
    set(t, 0x30, &prompt_for_space);
    set(t, 0x38, &prompt_result);
    return t;
}();

const Object kObject{kTable.data(), "persistent local storage"};

constexpr Guid kProvider{0xf4faf4d4, 0x2d04, 0x4fce, {0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e, 0x84}};
constexpr Guid kIid{0xd29411df, 0x0794, 0x4553, {0x8b, 0x27, 0x95, 0xfc, 0x02, 0xd0, 0xf7, 0x5d}};
constexpr Guid kIid2026{0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}};

const std::array<Interface, 2> kInterfaces{{
    {kProvider, kIid, &kObject},
    {kProvider, kIid2026, &kObject},
}};

}

std::span<const Interface> storage_interfaces()
{
    return kInterfaces;
}

}
