#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;
using Handle = const void*;

constexpr std::string_view kGamertag = "Player";
constexpr std::uint32_t kModernSuffix = 2;

char g_user = 0;
char g_add_identity = 0;
char g_resolve_identity = 0;
char g_token_identity = 0;

Handle user()
{
    return &g_user;
}

HResult found(bool matched, Handle* out)
{
    return matched ? put(out, user()) : kUserNotFound;
}

HResult duplicate(This, Handle, Handle* out)
{
    return put(out, user());
}

void close(This, Handle)
{
}

std::int32_t compare(This, Handle left, Handle right)
{
    return left == right ? 0 : 1;
}

HResult max_users(This, std::uint32_t* out)
{
    return put(out, 1u);
}

HResult add_provider(std::uint32_t op, const ProviderData* data)
{
    if (op == kOpBegin) {
        async_complete(data->block, kOk, sizeof(Handle));
    } else if (op == kOpGetResult) {
        return put(static_cast<Handle*>(data->buffer), user());
    }
    return kOk;
}

HResult add_async(This, std::uint32_t options, AsyncBlock* block)
{
    log::info("gdk: XUserAddAsync options " + std::to_string(options) + ": offline user signed in");
    return async_begin(block, nullptr, &g_add_identity, nullptr, &add_provider);
}

HResult add_result(This, AsyncBlock* block, Handle* out)
{
    log_first("XUserAddResult");
    return async_get_result(block, &g_add_identity, sizeof(Handle), out, nullptr);
}

HResult get_local_id(This, Handle, std::uint64_t* out)
{
    return put(out, kLocalUserId);
}

HResult find_by_local_id(This, std::uint64_t id, Handle* out)
{
    return found(id == kLocalUserId, out);
}

HResult get_id(This, Handle, std::uint64_t* out)
{
    log_first("XUserGetId");
    return put(out, kXuid);
}

HResult find_by_id(This, std::uint64_t id, Handle* out)
{
    return found(id == kXuid, out);
}

HResult get_is_guest(This, Handle, bool* out)
{
    return put(out, false);
}

HResult get_state(This, Handle, std::uint32_t* out)
{
    return put(out, 0u);
}

HResult get_gamertag(This, Handle, std::uint32_t component, std::size_t size, char* buffer, std::size_t* used)
{
    log_first("XUserGetGamertag");
    return put_string(component == kModernSuffix ? std::string_view{} : kGamertag, size, buffer, used);
}

HResult get_age_group(This, Handle, std::uint32_t* out)
{
    return put(out, 3u);
}

HResult check_privilege(This, Handle, std::uint32_t, std::uint32_t privilege, bool* has, std::uint32_t* reason)
{
    log_first("XUserCheckPrivilege " + std::to_string(privilege));
    if (reason != nullptr) {
        *reason = 0;
    }
    return put(has, true);
}

HResult resolve_privilege_async(This, Handle, std::uint32_t, std::uint32_t, AsyncBlock* block)
{
    return async_finished(block, &g_resolve_identity, kOk);
}

HResult resolve_privilege_result(This, AsyncBlock* block)
{
    return async_get_result(block, &g_resolve_identity, 0, nullptr, nullptr);
}

HResult token_async(This, Handle, std::uint32_t, const char*, const char* url, std::size_t, const void*,
                    std::size_t, const void*, AsyncBlock* block)
{
    log_first("gdk: XUserGetTokenAndSignatureAsync offline (" + text(url) + ")");
    return async_finished(block, &g_token_identity, kFailedToGetToken);
}

HResult token_size(This, AsyncBlock* block, std::size_t* out)
{
    return async_get_result_size(block, out);
}

HResult token_result(This, AsyncBlock* block, std::size_t size, void* buffer, const void**, std::size_t* used)
{
    return async_get_result(block, &g_token_identity, size, buffer, used);
}

HResult register_callback(This, const void*, void*, const void*, std::uint64_t* out)
{
    return put(out, next_token());
}

bool unregister_callback(This, std::uint64_t, std::uint8_t)
{
    return true;
}

HResult find_for_device(This, const void*, Handle*)
{
    return kUserNotFound;
}

const Table<35> kTable = [] {
    auto t = table<35>();
    set(t, 0x18, &duplicate);
    set(t, 0x20, &close);
    set(t, 0x28, &compare);
    set(t, 0x30, &max_users);
    set(t, 0x38, &add_async);
    set(t, 0x40, &add_result);
    set(t, 0x48, &get_local_id);
    set(t, 0x50, &find_by_local_id);
    set(t, 0x58, &get_id);
    set(t, 0x60, &find_by_id);
    set(t, 0x68, &get_is_guest);
    set(t, 0x70, &get_state);
    set(t, 0x78, &get_gamertag);
    set(t, 0x98, &get_age_group);
    set(t, 0xa0, &check_privilege);
    set(t, 0xa8, &resolve_privilege_async);
    set(t, 0xb0, &resolve_privilege_result);
    set(t, 0xb8, &token_async);
    set(t, 0xc0, &token_size);
    set(t, 0xc8, &token_result);
    set(t, 0x108, &register_callback);
    set(t, 0x110, &unregister_callback);
    return t;
}();

const Table<4> kGamertagTable = [] {
    auto t = table<4>();
    set(t, 0x18, &get_gamertag);
    return t;
}();

const Table<6> kDeviceTable = [] {
    auto t = table<6>();
    set(t, 0x18, &find_for_device);
    set(t, 0x20, &register_callback);
    set(t, 0x28, &unregister_callback);
    return t;
}();

const Object kObject{kTable.data(), "user"};
const Object kGamertagObject{kGamertagTable.data(), "user gamertag"};
const Object kDeviceObject{kDeviceTable.data(), "user devices"};

constexpr Guid kProvider{0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
constexpr Guid kIid{0xeb9bf948, 0x18dc, 0x4d82, {0xbb, 0xcc, 0x40, 0xe0, 0xa8, 0x09, 0xc4, 0xc0}};
constexpr Guid kGamertagIid{0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};
constexpr Guid kDeviceProvider{0x7d824997, 0x10dc, 0x45ab, {0x86, 0xb7, 0x27, 0x37, 0x76, 0x7c, 0x0b, 0xf1}};
constexpr Guid kDeviceIid{0x0cc6a956, 0xe7e1, 0x4fdf, {0x93, 0x41, 0x9d, 0x5d, 0xa6, 0x49, 0xeb, 0xc8}};

const std::array<Interface, 4> kInterfaces{{
    {kProvider, kIid, &kObject},
    {kProvider, kProvider, &kObject},
    {kProvider, kGamertagIid, &kGamertagObject},
    {kDeviceProvider, kDeviceIid, &kDeviceObject},
}};

}

std::span<const Interface> user_interfaces()
{
    return kInterfaces;
}

}
