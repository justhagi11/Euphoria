#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>

#include <algorithm>
#include <cstring>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;
using Handle = const void*;

char g_context = 0;
char g_query = 0;
char g_valid = 0;
char g_invalid = 0;
char g_failed_identity = 0;
char g_query_identity = 0;
char g_license_identity = 0;
char g_game_license_identity = 0;

struct GameLicense {
    char sku_store_id[18];
    bool is_active;
    bool is_trial_owned_by_this_user;
    bool is_disc_license;
    bool is_trial;
    std::uint32_t trial_seconds_remaining;
    char trial_unique_id[64];
    std::int64_t expiration;
};

static_assert(sizeof(GameLicense) == 104);

HResult offline(AsyncBlock* block, std::string_view what)
{
    log_first("gdk: store " + std::string(what) + ": offline");
    return async_finished(block, &g_failed_identity, kStoreNetworkError);
}

HResult status(This, AsyncBlock* block)
{
    return async_get_status(block, false);
}

HResult create_context(This, const void*, Handle* out)
{
    log_first("XStoreCreateContext");
    return put(out, static_cast<Handle>(&g_context));
}

void close(This, Handle)
{
}

HResult associated_products_async(This, Handle, std::uint32_t, std::uint32_t, AsyncBlock* block)
{
    return offline(block, "associated products");
}

HResult entitled_products_async(This, Handle, std::uint32_t, std::uint32_t, AsyncBlock* block)
{
    log_first("XStoreQueryEntitledProductsAsync: none");
    return async_finished_with(block, &g_query_identity, bytes_of(static_cast<Handle>(&g_query)));
}

HResult query_result(This, AsyncBlock* block, Handle* out)
{
    return async_result(block, &g_query_identity, out);
}

HResult enumerate_products(This, Handle, void*, const void*)
{
    return kOk;
}

bool has_more_pages(This, Handle)
{
    return false;
}

HResult next_page_async(This, Handle, AsyncBlock* block)
{
    return async_finished(block, &g_query_identity, kIllegalMethodCall);
}

HResult license_for(bool owned, AsyncBlock* block)
{
    const Handle license = owned ? static_cast<Handle>(&g_valid) : static_cast<Handle>(&g_invalid);
    return async_finished_with(block, &g_license_identity, bytes_of(license));
}

HResult license_for_package_async(This, Handle, const char* package, AsyncBlock* block)
{
    log_first("XStoreAcquireLicenseForPackageAsync");
    return license_for(package != nullptr && text(package) == package_identifier(), block);
}

HResult license_for_durables_async(This, Handle, const char*, AsyncBlock* block)
{
    return license_for(false, block);
}

HResult license_result(This, AsyncBlock* block, Handle* out)
{
    return async_result(block, &g_license_identity, out);
}

bool is_license_valid(This, Handle license)
{
    return license == &g_valid;
}

HResult game_license_async(This, Handle, AsyncBlock* block)
{
    log_first("XStoreQueryGameLicenseAsync: full licence");
    GameLicense license{};
    const std::string sku = title().store_id + "/0010";
    std::memcpy(license.sku_store_id, sku.data(), std::min(sku.size(), sizeof(license.sku_store_id) - 1));
    license.is_active = true;
    return async_finished_with(block, &g_game_license_identity, bytes_of(license));
}

HResult game_license_result(This, AsyncBlock* block, GameLicense* out)
{
    return async_result(block, &g_game_license_identity, out);
}

HResult consumable_balance_async(This, Handle, const char*, AsyncBlock* block)
{
    return offline(block, "consumable balance");
}

HResult report_fulfillment_async(This, Handle, const char*, std::uint32_t, const void*, AsyncBlock* block)
{
    return offline(block, "consumable fulfillment");
}

HResult user_id_async(This, Handle, const char*, const char*, AsyncBlock* block)
{
    return offline(block, "user store id");
}

HResult size_status(This, AsyncBlock* block, std::size_t*)
{
    return async_get_status(block, false);
}

HResult string_status(This, AsyncBlock* block, std::size_t, char*)
{
    return async_get_status(block, false);
}

HResult count_status(This, AsyncBlock* block, std::uint32_t*)
{
    return async_get_status(block, false);
}

HResult array_status(This, AsyncBlock* block, std::uint32_t, void*)
{
    return async_get_status(block, false);
}

HResult purchase_ui_async(This, Handle, const char*, const char*, const char*, AsyncBlock* block)
{
    return offline(block, "purchase page");
}

HResult product_page_ui_async(This, Handle, const char*, AsyncBlock* block)
{
    return offline(block, "product page");
}

HResult redeem_ui_async(This, Handle, const char*, const char* const*, std::size_t, std::uint8_t, AsyncBlock* block)
{
    return offline(block, "redeem page");
}

HResult updates_async(This, Handle, AsyncBlock* block)
{
    return offline(block, "update check");
}

HResult install_async(This, Handle, const char* const*, std::size_t, AsyncBlock* block)
{
    return offline(block, "package download");
}

HResult package_identifier_for(This, const char* store_id, std::size_t size, char* buffer)
{
    if (store_id != nullptr && !title().store_id.empty() && text(store_id) == title().store_id) {
        return put_string(package_identifier(), size, buffer, nullptr);
    }
    return kNoPackage;
}

HResult register_callback(This, Handle, const void*, void*, const void*, std::uint64_t* out)
{
    return put(out, next_token());
}

bool unregister_callback(This, Handle, std::uint64_t, std::uint8_t)
{
    return true;
}

const Table<76> kTable = [] {
    auto t = table<76>();
    set(t, 0x18, &create_context);
    set(t, 0x20, &close);
    set(t, 0x28, &associated_products_async);
    set(t, 0x30, &query_result);
    set(t, 0x48, &entitled_products_async);
    set(t, 0x50, &query_result);
    set(t, 0x78, &enumerate_products);
    set(t, 0x80, &has_more_pages);
    set(t, 0x88, &next_page_async);
    set(t, 0x90, &query_result);
    set(t, 0x98, &close);
    set(t, 0xa0, &license_for_package_async);
    set(t, 0xa8, &license_result);
    set(t, 0xb0, &is_license_valid);
    set(t, 0xb8, &close);
    set(t, 0xe0, &game_license_async);
    set(t, 0xe8, &game_license_result);
    set(t, 0x108, &consumable_balance_async);
    set(t, 0x110, &status);
    set(t, 0x118, &report_fulfillment_async);
    set(t, 0x128, &user_id_async);
    set(t, 0x130, &size_status);
    set(t, 0x138, &string_status);
    set(t, 0x140, &user_id_async);
    set(t, 0x148, &size_status);
    set(t, 0x150, &string_status);
    set(t, 0x188, &purchase_ui_async);
    set(t, 0x1a8, &redeem_ui_async);
    set(t, 0x1b8, &updates_async);
    set(t, 0x1c0, &count_status);
    set(t, 0x1c8, &array_status);
    set(t, 0x1e0, &install_async);
    set(t, 0x1e8, &status);
    set(t, 0x1f0, &install_async);
    set(t, 0x1f8, &count_status);
    set(t, 0x200, &array_status);
    set(t, 0x208, &package_identifier_for);
    set(t, 0x210, &register_callback);
    set(t, 0x218, &unregister_callback);
    set(t, 0x220, &register_callback);
    set(t, 0x228, &unregister_callback);
    set(t, 0x238, &license_for_durables_async);
    set(t, 0x240, &license_result);
    set(t, 0x258, &product_page_ui_async);
    return t;
}();

const Object kObject{kTable.data(), "store"};

constexpr Guid kProvider{0x0dd112ac, 0x7c24, 0x448c, {0xb9, 0x2b, 0x39, 0x60, 0xfb, 0x5b, 0xd3, 0x0c}};
constexpr Guid kIid2026{0xb09d803c, 0x2414, 0x4a05, {0x82, 0xc6, 0x66, 0xdf, 0xdc, 0x9e, 0x9a, 0x44}};
constexpr Guid kDurablesIid{0x2d42fea5, 0xe71d, 0x4b76, {0x97, 0xcd, 0xc5, 0x0a, 0xfb, 0xb3, 0xae, 0x5d}};

const std::array<Interface, 2> kInterfaces{{
    {kProvider, kIid2026, &kObject},
    {kProvider, kDurablesIid, &kObject},
}};

}

std::span<const Interface> store_interfaces()
{
    return kInterfaces;
}

}
