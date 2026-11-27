#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>

#include <atomic>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;

char g_monitor = 0;
char g_install_identity = 0;
char g_mount_identity = 0;

struct Progress {
    std::uint64_t total_bytes;
    std::uint64_t installed_bytes;
    std::uint64_t launch_bytes;
    bool launchable;
    bool completed;
};

bool is_ours(const char* id)
{
    return id != nullptr && text(id) == package_identifier();
}

std::uint64_t package_bytes()
{
    static const std::uint64_t bytes = [] {
        std::uint64_t total = 0;
        if (title().package.empty()) {
            return total;
        }
        std::error_code error;
        for (auto it = std::filesystem::recursive_directory_iterator(title().package, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_regular_file(error)) {
                total += it->file_size(error);
            }
        }
        return total;
    }();
    return bytes;
}

const void* monitor()
{
    return &g_monitor;
}

HResult current_identifier(This, std::size_t size, char* buffer)
{
    log_first("XPackageGetCurrentProcessPackageIdentifier");
    return put_string(package_identifier(), size, buffer, nullptr);
}

bool is_packaged(This)
{
    log_first("XPackageIsPackagedProcess");
    return true;
}

HResult create_monitor(This, const char* id, std::uint32_t, const void*, std::uint32_t, const void*,
                       const void** out)
{
    log_first("XPackageCreateInstallationMonitor");
    if (!is_ours(id)) {
        return kNoPackage;
    }
    return put(out, monitor());
}

void close_monitor(This, const void*)
{
}

void progress(This, const void*, Progress* out)
{
    const std::uint64_t bytes = package_bytes();
    put(out, Progress{bytes, bytes, bytes, true, true});
}

void update_monitor(This, const void*)
{
}

HResult register_progress(This, const void*, void*, const void*, std::uint64_t* out)
{
    return put(out, next_token());
}

bool unregister_progress(This, const void*, std::uint64_t, std::uint8_t)
{
    return true;
}

HResult find_availability(This, const char* id, std::uint32_t, const void*, std::uint32_t* out)
{
    log_first("XPackageFindChunkAvailability");
    if (!is_ours(id)) {
        return kNoPackage;
    }
    return put(out, 0u);
}

HResult change_order(This, const char* id, std::uint32_t, const void*)
{
    return is_ours(id) ? kOk : kNoPackage;
}

HResult install_chunks_async(This, const char* id, std::uint32_t, const void*, std::uint32_t, std::uint8_t,
                             AsyncBlock* block)
{
    if (!is_ours(id)) {
        return async_finished(block, &g_install_identity, kNoPackage);
    }
    return async_finished_with(block, &g_install_identity, bytes_of(monitor()));
}

HResult install_chunks_result(This, AsyncBlock* block, const void** out)
{
    return async_result(block, &g_install_identity, out);
}

HResult estimate_download(This, const char* id, std::uint32_t, const void*, std::uint64_t* bytes, bool* confirm)
{
    if (!is_ours(id)) {
        return kNoPackage;
    }
    if (confirm != nullptr) {
        *confirm = false;
    }
    return put(bytes, std::uint64_t{0});
}

HResult uninstall_chunks(This, const char* id, std::uint32_t, const void*)
{
    return is_ours(id) ? kOk : kNoPackage;
}

HResult mount_path(This, const void*, std::size_t, char*)
{
    return kInvalidArgument;
}

void close_mount(This, const void*)
{
}

bool uninstall_package(This, const char* id)
{
    log::info("gdk: XPackageUninstallPackage " + text(id) + " refused");
    return false;
}

HResult mount_async(This, const char* id, AsyncBlock* block)
{
    log::info("gdk: XPackageMountWithUiAsync " + text(id) + ": not installed");
    return async_finished(block, &g_mount_identity, kNoPackage);
}

HResult mount_result(This, AsyncBlock* block, const void** out)
{
    return async_result(block, &g_mount_identity, out);
}

HResult enumerate(This, std::uint32_t kind, std::uint32_t, void*, const void*)
{
    log_first("gdk: XPackageEnumeratePackages kind " + std::to_string(kind) + ": none");
    return kOk;
}

const Table<40> kTable = [] {
    auto t = table<40>();
    set(t, 0x18, &current_identifier);
    set(t, 0x20, &is_packaged);
    set(t, 0x28, &create_monitor);
    set(t, 0x30, &close_monitor);
    set(t, 0x38, &progress);
    set(t, 0x40, &update_monitor);
    set(t, 0x48, &register_progress);
    set(t, 0x50, &unregister_progress);
    set(t, 0x60, &find_availability);
    set(t, 0x70, &change_order);
    set(t, 0x80, &install_chunks_async);
    set(t, 0x88, &install_chunks_result);
    set(t, 0x90, &estimate_download);
    set(t, 0x98, &uninstall_chunks);
    set(t, 0xc8, &mount_path);
    set(t, 0xd0, &close_mount);
    set(t, 0x110, &uninstall_package);
    set(t, 0x128, &mount_async);
    set(t, 0x130, &mount_result);
    set(t, 0x138, &enumerate);
    return t;
}();

const Object kObject{kTable.data(), "package"};

constexpr Guid kProvider{0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}};
constexpr Guid kIid2026{0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};

const std::array<Interface, 1> kInterfaces{{
    {kProvider, kIid2026, &kObject},
}};

}

std::span<const Interface> package_interfaces()
{
    return kInterfaces;
}

}
