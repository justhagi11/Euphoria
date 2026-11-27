#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>

namespace euphoria::shim::gdk {
namespace {

namespace fs = std::filesystem;

using This = const Object*;

constexpr HResult kInvalidContainerName = static_cast<HResult>(0x80830001u);
constexpr HResult kNoAccess = static_cast<HResult>(0x80830002u);
constexpr HResult kOutOfLocalStorage = static_cast<HResult>(0x80830003u);
constexpr HResult kUpdateTooBig = static_cast<HResult>(0x80830005u);
constexpr HResult kQuotaExceeded = static_cast<HResult>(0x80830006u);
constexpr HResult kBufferTooSmall = static_cast<HResult>(0x80830007u);
constexpr HResult kBlobNotFound = static_cast<HResult>(0x80830008u);

constexpr std::uint64_t kQuota = 256ull << 20;
constexpr std::size_t kMaxUpdate = 16u << 20;
constexpr std::size_t kBlobEntry = 24;

char g_init_identity = 0;
char g_quota_identity = 0;
char g_delete_identity = 0;
char g_read_identity = 0;
char g_submit_identity = 0;

std::mutex& saves_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::string escape(std::string_view name)
{
    std::string result;
    for (const char ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
            || byte == '-' || byte == '_') {
            result.push_back(ch);
        } else {
            char code[4]{};
            std::snprintf(code, sizeof(code), "%%%02X", byte);
            result += code;
        }
    }
    return result;
}

fs::path container_dir(const fs::path& root, std::string_view name)
{
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const char ch : name) {
        hash = (hash ^ static_cast<unsigned char>(ch)) * 0x100000001b3ull;
    }
    std::string readable = escape(name);
    if (readable.size() > 100) {
        readable.resize(100);
    }
    char suffix[24]{};
    std::snprintf(suffix, sizeof(suffix), "~%016llx", static_cast<unsigned long long>(hash));
    return root / (readable + suffix);
}

bool valid_container(std::string_view name)
{
    if (name.size() > 256) {
        return false;
    }
    const std::size_t slash = name.rfind('/');
    const std::string_view path = slash == std::string_view::npos ? std::string_view{} : name.substr(0, slash);
    const std::string_view file = slash == std::string_view::npos ? name : name.substr(slash + 1);
    const auto alnum = [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    };
    for (const char ch : path) {
        if (!alnum(ch) && ch != '_' && ch != '/') {
            return false;
        }
    }
    if (file.empty() || file.back() == '.' || file.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char ch : file) {
        if (!alnum(ch) && ch != '_' && ch != '.' && ch != '-') {
            return false;
        }
    }
    return true;
}

struct Version {
    fs::path dir;
    std::uint64_t number = 0;
    std::string name;
    std::string display;
    std::vector<std::string> blobs;

    fs::path blob(std::size_t index) const { return dir / std::to_string(index); }

    std::uint64_t size(std::size_t index) const
    {
        std::error_code error;
        const auto bytes = fs::file_size(blob(index), error);
        return error ? 0 : bytes;
    }

    std::uint64_t total() const
    {
        std::uint64_t sum = 0;
        for (std::size_t index = 0; index < blobs.size(); ++index) {
            sum += size(index);
        }
        return sum;
    }
};

std::vector<std::uint8_t> read_file(const fs::path& path, bool& ok)
{
    std::ifstream file(path, std::ios::binary);
    ok = static_cast<bool>(file);
    if (!ok) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& path, const std::uint8_t* data, std::size_t size)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    file.flush();
    return static_cast<bool>(file);
}

std::vector<std::uint8_t> manifest(const std::vector<std::string>& strings)
{
    std::vector<std::uint8_t> out;
    for (const auto& value : strings) {
        const auto length = static_cast<std::uint32_t>(value.size());
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&length);
        out.insert(out.end(), bytes, bytes + 4);
        out.insert(out.end(), value.begin(), value.end());
    }
    return out;
}

std::optional<std::vector<std::string>> parse_manifest(const std::vector<std::uint8_t>& bytes)
{
    std::vector<std::string> strings;
    std::size_t at = 0;
    while (at < bytes.size()) {
        if (bytes.size() - at < 4) {
            return std::nullopt;
        }
        std::uint32_t length = 0;
        std::memcpy(&length, bytes.data() + at, 4);
        at += 4;
        if (bytes.size() - at < length) {
            return std::nullopt;
        }
        strings.emplace_back(reinterpret_cast<const char*>(bytes.data() + at), length);
        at += length;
    }
    if (strings.size() < 2) {
        return std::nullopt;
    }
    return strings;
}

std::optional<Version> current(const fs::path& dir)
{
    std::error_code error;
    if (!fs::is_directory(dir, error)) {
        return std::nullopt;
    }
    std::vector<std::uint64_t> committed;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        const std::string file = entry.path().filename().string();
        if (file.ends_with(".new")) {
            fs::remove_all(entry.path(), error);
        } else if (file.size() > 1 && file[0] == 'v'
                   && file.find_first_not_of("0123456789", 1) == std::string::npos) {
            committed.push_back(std::strtoull(file.c_str() + 1, nullptr, 10));
        }
    }
    if (committed.empty()) {
        return std::nullopt;
    }
    std::sort(committed.begin(), committed.end());
    for (std::size_t index = 0; index + 1 < committed.size(); ++index) {
        fs::remove_all(dir / ("v" + std::to_string(committed[index])), error);
    }
    Version version;
    version.number = committed.back();
    version.dir = dir / ("v" + std::to_string(version.number));
    bool ok = false;
    const auto strings = parse_manifest(read_file(version.dir / "manifest", ok));
    if (!ok || !strings) {
        return std::nullopt;
    }
    version.name = (*strings)[0];
    version.display = (*strings)[1];
    version.blobs.assign(strings->begin() + 2, strings->end());
    return version;
}

std::optional<Version> current_named(const fs::path& dir, std::string_view name)
{
    auto version = current(dir);
    if (version && version->name != name) {
        return std::nullopt;
    }
    return version;
}

std::vector<std::pair<std::string, fs::path>> containers(const fs::path& root)
{
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(root, error)) {
        if (!entry.is_directory(error)) {
            continue;
        }
        const std::string file = entry.path().filename().string();
        if (file.ends_with(".gone")) {
            fs::remove_all(entry.path(), error);
        } else if (file.find('~') != std::string::npos) {
            if (auto version = current(entry.path()); version) {
                out.emplace_back(version->name, entry.path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uint64_t used_bytes(const fs::path& root)
{
    std::uint64_t total = 0;
    for (const auto& [name, dir] : containers(root)) {
        if (auto version = current_named(dir, name); version) {
            total += version->total();
        }
    }
    return total;
}

struct Provider {
    fs::path root;
};

struct Container {
    std::string name;
    fs::path dir;
};

struct Change {
    std::string name;
    std::optional<std::vector<std::uint8_t>> data;
};

struct Update {
    std::string name;
    fs::path dir;
    std::optional<std::string> display;
    std::vector<Change> changes;
};

struct Source {
    std::string name;
    const std::vector<std::uint8_t>* data;
    fs::path kept;
};

HResult submit_update(const Update& update)
{
    std::scoped_lock lock(saves_mutex());
    const auto previous = current(update.dir);
    if (previous && previous->name != update.name) {
        return kNoAccess;
    }
    std::vector<Source> blobs;
    if (previous) {
        for (std::size_t index = 0; index < previous->blobs.size(); ++index) {
            blobs.push_back(Source{previous->blobs[index], nullptr, previous->blob(index)});
        }
    }
    std::uint64_t replaced = 0;
    for (const auto& change : update.changes) {
        for (auto it = blobs.begin(); it != blobs.end();) {
            if (it->name == change.name) {
                if (it->data == nullptr) {
                    std::error_code error;
                    const auto bytes = fs::file_size(it->kept, error);
                    replaced += error ? 0 : bytes;
                }
                it = blobs.erase(it);
            } else {
                ++it;
            }
        }
        if (change.data) {
            blobs.push_back(Source{change.name, &*change.data, {}});
        }
    }
    std::sort(blobs.begin(), blobs.end(), [](const Source& a, const Source& b) { return a.name < b.name; });
    std::size_t written = 0;
    for (const auto& blob : blobs) {
        written += blob.data != nullptr ? blob.data->size() : 0;
    }
    if (written > kMaxUpdate) {
        return kUpdateTooBig;
    }
    const fs::path root = update.dir.parent_path();
    const std::uint64_t used = used_bytes(root);
    if (written > (kQuota > used ? kQuota - used : 0) + replaced) {
        return kQuotaExceeded;
    }
    if (written > disk_free(root)) {
        return kOutOfLocalStorage;
    }
    const std::uint64_t number = previous ? previous->number + 1 : 1;
    const fs::path staged = update.dir / ("v" + std::to_string(number) + ".new");
    std::error_code error;
    fs::remove_all(staged, error);
    fs::create_directories(staged, error);
    if (error) {
        log::warn("gdk: game save cannot create " + staged.string());
        return kNoAccess;
    }
    std::vector<std::string> strings{update.name,
                                     update.display ? *update.display : (previous ? previous->display : "")};
    for (std::size_t index = 0; index < blobs.size(); ++index) {
        const fs::path target = staged / std::to_string(index);
        strings.push_back(blobs[index].name);
        if (blobs[index].data != nullptr) {
            if (!write_file(target, blobs[index].data->data(), blobs[index].data->size())) {
                return kOutOfLocalStorage;
            }
        } else {
            fs::create_hard_link(blobs[index].kept, target, error);
            if (error) {
                error.clear();
                fs::copy_file(blobs[index].kept, target, fs::copy_options::overwrite_existing, error);
                if (error) {
                    return kNoAccess;
                }
            }
        }
    }
    const auto bytes = manifest(strings);
    if (!write_file(staged / "manifest", bytes.data(), bytes.size())) {
        return kOutOfLocalStorage;
    }
    fs::rename(staged, update.dir / ("v" + std::to_string(number)), error);
    if (error) {
        log::warn("gdk: game save cannot commit " + staged.string());
        return kNoAccess;
    }
    if (previous) {
        fs::remove_all(previous->dir, error);
    }
    return kOk;
}

HResult delete_container(const fs::path& root, std::string_view name)
{
    std::scoped_lock lock(saves_mutex());
    const fs::path dir = container_dir(root, name);
    if (!current_named(dir, name)) {
        return kOk;
    }
    fs::path gone = dir;
    gone += ".gone";
    std::error_code error;
    fs::remove_all(gone, error);
    fs::rename(dir, gone, error);
    if (error) {
        return kNoAccess;
    }
    fs::remove_all(gone, error);
    return kOk;
}

using Blobs = std::vector<std::pair<std::string, std::vector<std::uint8_t>>>;

HResult read_blobs_of(const Container& container, const std::optional<std::vector<std::string>>& names, Blobs& out)
{
    std::scoped_lock lock(saves_mutex());
    const auto version = current_named(container.dir, container.name);
    if (!version) {
        return kBlobNotFound;
    }
    const std::vector<std::string> wanted = names ? *names : version->blobs;
    for (const auto& name : wanted) {
        const auto found = std::find(version->blobs.begin(), version->blobs.end(), name);
        if (found == version->blobs.end()) {
            return kBlobNotFound;
        }
        bool ok = false;
        auto data = read_file(version->blob(static_cast<std::size_t>(found - version->blobs.begin())), ok);
        if (!ok) {
            return kNoAccess;
        }
        out.emplace_back(name, std::move(data));
    }
    return kOk;
}

std::vector<std::uint8_t> layout(const Blobs& blobs, std::uintptr_t base)
{
    std::vector<std::uint8_t> out(blobs.size() * kBlobEntry, 0);
    for (std::size_t index = 0; index < blobs.size(); ++index) {
        const auto& [name, data] = blobs[index];
        const std::uintptr_t name_at = base + out.size();
        out.insert(out.end(), name.begin(), name.end());
        out.push_back(0);
        const std::uintptr_t data_at = base + out.size();
        out.insert(out.end(), data.begin(), data.end());
        const auto size = static_cast<std::uint32_t>(data.size());
        std::memcpy(out.data() + index * kBlobEntry, &name_at, 8);
        std::memcpy(out.data() + index * kBlobEntry + 8, &size, 4);
        std::memcpy(out.data() + index * kBlobEntry + 16, &data_at, 8);
    }
    return out;
}

std::optional<std::vector<std::string>> names_of(const char* const* names, std::uint32_t count)
{
    if (names == nullptr) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    for (std::uint32_t index = 0; index < count; ++index) {
        out.push_back(text(names[index]));
    }
    return out;
}

struct ContainerInfo {
    const char* name;
    const char* display_name;
    std::uint32_t blob_count;
    std::uint64_t total_size;
    std::int64_t last_modified;
    bool needs_sync;
};

struct BlobInfo {
    const char* name;
    std::uint32_t size;
};

using ContainerCallback = bool (*)(const ContainerInfo*, void*);
using BlobCallback = bool (*)(const BlobInfo*, void*);

struct Report {
    std::string name;
    std::string display;
    std::uint32_t count;
    std::uint64_t size;
    std::int64_t modified;
};

void report(const fs::path& root, std::string_view only, bool filter, void* context, ContainerCallback callback)
{
    std::vector<Report> reports;
    {
        std::scoped_lock lock(saves_mutex());
        for (const auto& [name, dir] : containers(root)) {
            if (filter && name != only) {
                continue;
            }
            const auto version = current_named(dir, name);
            if (!version) {
                continue;
            }
            std::error_code error;
            const auto time = fs::last_write_time(version->dir / "manifest", error);
            std::int64_t modified = 0;
            if (!error) {
                const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
                modified = std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
            }
            reports.push_back(Report{version->name, version->display, static_cast<std::uint32_t>(version->blobs.size()),
                                     version->total(), modified});
        }
    }
    for (const auto& entry : reports) {
        const ContainerInfo info{entry.name.c_str(), entry.display.c_str(), entry.count, entry.size, entry.modified, false};
        if (!callback(&info, context)) {
            break;
        }
    }
}

HResult initialize_async(This, const void*, const char* config, std::uint8_t, AsyncBlock* block)
{
    std::string name = text(config);
    if (name.empty()) {
        name = "default";
    }
    log::info("gdk: XGameSaveInitializeProviderAsync config '" + name + "'");
    char user[24]{};
    std::snprintf(user, sizeof(user), "%016llX", static_cast<unsigned long long>(kXuid));
    const fs::path root = title().data / "GameSaves" / user / escape(name);
    std::error_code error;
    fs::create_directories(root, error);
    if (error) {
        return async_finished(block, &g_init_identity, kNoAccess);
    }
    {
        std::scoped_lock lock(saves_mutex());
        containers(root);
    }
    auto* provider = new Provider{root};
    const HResult result = async_finished_with(block, &g_init_identity, bytes_of(provider));
    if (result < 0) {
        delete provider;
    }
    return result;
}

HResult initialize_result(This, AsyncBlock* block, Provider** out)
{
    return async_result(block, &g_init_identity, out);
}

void close_provider(This, Provider* provider)
{
    delete provider;
}

HResult quota_async(This, const Provider* provider, AsyncBlock* block)
{
    if (provider == nullptr) {
        return async_finished(block, &g_quota_identity, kInvalidArgument);
    }
    std::int64_t remaining = 0;
    {
        std::scoped_lock lock(saves_mutex());
        const std::uint64_t used = used_bytes(provider->root);
        remaining = static_cast<std::int64_t>(std::min(kQuota > used ? kQuota - used : 0, disk_free(provider->root)));
    }
    return async_finished_with(block, &g_quota_identity, bytes_of(remaining));
}

HResult quota_result(This, AsyncBlock* block, std::int64_t* out)
{
    return async_result(block, &g_quota_identity, out);
}

HResult delete_async(This, const Provider* provider, const char* name, AsyncBlock* block)
{
    HResult result = kInvalidArgument;
    if (provider != nullptr && name != nullptr) {
        result = valid_container(name) ? delete_container(provider->root, name) : kInvalidContainerName;
    }
    return async_finished(block, &g_delete_identity, result);
}

HResult delete_result(This, AsyncBlock* block)
{
    return async_get_result(block, &g_delete_identity, 0, nullptr, nullptr);
}

HResult container_info(This, const Provider* provider, const char* name, void* context, ContainerCallback callback)
{
    if (provider == nullptr || name == nullptr || callback == nullptr) {
        return kInvalidArgument;
    }
    if (!valid_container(name)) {
        return kInvalidContainerName;
    }
    report(provider->root, name, true, context, callback);
    return kOk;
}

HResult enumerate_containers(This, const Provider* provider, void* context, ContainerCallback callback)
{
    if (provider == nullptr || callback == nullptr) {
        return kInvalidArgument;
    }
    report(provider->root, {}, false, context, callback);
    return kOk;
}

HResult create_container(This, const Provider* provider, const char* name, Container** out)
{
    if (provider == nullptr || name == nullptr) {
        return kInvalidArgument;
    }
    if (!valid_container(name)) {
        return kInvalidContainerName;
    }
    if (out == nullptr) {
        return kInvalidArgument;
    }
    *out = new Container{name, container_dir(provider->root, name)};
    return kOk;
}

void close_container(This, Container* container)
{
    delete container;
}

HResult enumerate_blobs(This, const Container* container, const char* prefix, void* context, BlobCallback callback)
{
    if (container == nullptr || callback == nullptr) {
        return kInvalidArgument;
    }
    const std::string start = text(prefix);
    std::vector<std::pair<std::string, std::uint32_t>> blobs;
    {
        std::scoped_lock lock(saves_mutex());
        if (const auto version = current_named(container->dir, container->name); version) {
            for (std::size_t index = 0; index < version->blobs.size(); ++index) {
                blobs.emplace_back(version->blobs[index], static_cast<std::uint32_t>(version->size(index)));
            }
        }
    }
    for (const auto& [name, size] : blobs) {
        if (!name.starts_with(start)) {
            continue;
        }
        const BlobInfo info{name.c_str(), size};
        if (!callback(&info, context)) {
            break;
        }
    }
    return kOk;
}

HResult read_blobs(This, const Container* container, const char* const* names, std::uint32_t* count,
                   std::size_t size, std::uint8_t* out)
{
    if (container == nullptr) {
        return kInvalidArgument;
    }
    Blobs blobs;
    if (const HResult result = read_blobs_of(*container, names_of(names, count != nullptr ? *count : 0), blobs);
        result < 0) {
        return result;
    }
    const auto bytes = layout(blobs, reinterpret_cast<std::uintptr_t>(out));
    if (out == nullptr || size < bytes.size()) {
        return kBufferTooSmall;
    }
    std::memcpy(out, bytes.data(), bytes.size());
    return put(count, static_cast<std::uint32_t>(blobs.size()));
}

HResult read_async(This, const Container* container, const char* const* names, std::uint32_t count, AsyncBlock* block)
{
    if (container == nullptr) {
        return async_finished(block, &g_read_identity, kInvalidArgument);
    }
    Blobs blobs;
    if (const HResult result = read_blobs_of(*container, names_of(names, count), blobs); result < 0) {
        return async_finished(block, &g_read_identity, result);
    }
    return async_finished_with(block, &g_read_identity, layout(blobs, 0));
}

HResult read_result(This, AsyncBlock* block, std::size_t size, std::uint8_t* out, std::uint32_t* count)
{
    std::size_t used = 0;
    const HResult result = async_get_result(block, &g_read_identity, size, out, &used);
    if (result == kNotSufficientBuffer) {
        return kBufferTooSmall;
    }
    if (result < 0) {
        return result;
    }
    std::size_t blobs = 0;
    if (used != 0) {
        std::uintptr_t first = 0;
        std::memcpy(&first, out, 8);
        blobs = first / kBlobEntry;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(out);
    for (std::size_t index = 0; index < blobs; ++index) {
        for (const std::size_t field : {std::size_t{0}, std::size_t{16}}) {
            std::uintptr_t value = 0;
            std::memcpy(&value, out + index * kBlobEntry + field, 8);
            value += base;
            std::memcpy(out + index * kBlobEntry + field, &value, 8);
        }
    }
    return put(count, static_cast<std::uint32_t>(blobs));
}

HResult create_update(This, const Container* container, const char* display, Update** out)
{
    if (container == nullptr || out == nullptr) {
        return kInvalidArgument;
    }
    auto* update = new Update{container->name, container->dir, std::nullopt, {}};
    if (display != nullptr) {
        update->display = display;
    }
    *out = update;
    return kOk;
}

void close_update(This, Update* update)
{
    delete update;
}

HResult write_blob(This, Update* update, const char* name, const std::uint8_t* data, std::size_t size)
{
    if (update == nullptr || name == nullptr || *name == 0 || (data == nullptr && size > 0)) {
        return kInvalidArgument;
    }
    if (size > kMaxUpdate) {
        return kUpdateTooBig;
    }
    update->changes.push_back(Change{name, std::vector<std::uint8_t>(data, data + size)});
    return kOk;
}

HResult delete_blob(This, Update* update, const char* name)
{
    if (update == nullptr || name == nullptr || *name == 0) {
        return kInvalidArgument;
    }
    update->changes.push_back(Change{name, std::nullopt});
    return kOk;
}

HResult submit_async(This, const Update* update, AsyncBlock* block)
{
    log_first("XGameSaveSubmitUpdateAsync");
    return async_finished(block, &g_submit_identity, update == nullptr ? kInvalidArgument : submit_update(*update));
}

HResult submit_result(This, AsyncBlock* block)
{
    return async_get_result(block, &g_submit_identity, 0, nullptr, nullptr);
}

const Table<30> kTable = [] {
    auto t = table<30>();
    set(t, 0x20, &initialize_async);
    set(t, 0x28, &initialize_result);
    set(t, 0x30, &close_provider);
    set(t, 0x40, &quota_async);
    set(t, 0x48, &quota_result);
    set(t, 0x58, &delete_async);
    set(t, 0x60, &delete_result);
    set(t, 0x68, &container_info);
    set(t, 0x70, &enumerate_containers);
    set(t, 0x80, &create_container);
    set(t, 0x88, &close_container);
    set(t, 0x98, &enumerate_blobs);
    set(t, 0xa0, &read_blobs);
    set(t, 0xa8, &read_async);
    set(t, 0xb0, &read_result);
    set(t, 0xb8, &create_update);
    set(t, 0xc0, &close_update);
    set(t, 0xc8, &write_blob);
    set(t, 0xd0, &delete_blob);
    set(t, 0xe0, &submit_async);
    set(t, 0xe8, &submit_result);
    return t;
}();

const Object kObject{kTable.data(), "game save"};

constexpr Guid kProvider{0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f, 0xe2}};
constexpr Guid kIid2026{0x1bfff3af, 0xf14a, 0x40a3, {0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9}};

const std::array<Interface, 2> kInterfaces{{
    {kProvider, kProvider, &kObject},
    {kProvider, kIid2026, &kObject},
}};

}

std::span<const Interface> game_save_interfaces()
{
    return kInterfaces;
}

}
