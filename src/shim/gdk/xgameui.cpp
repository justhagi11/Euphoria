#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;

char g_profile_identity = 0;
char g_text_identity = 0;
char g_message_identity = 0;

HResult message_dialog_async(This, AsyncBlock* block, const char* title, const char* content, const char*,
                             const char*, const char*, std::uint32_t, std::uint32_t)
{
    log::info("gdk: XGameUiShowMessageDialogAsync '" + text(title) + "' '" + text(content)
              + "': first button");
    return async_finished_with(block, &g_message_identity, bytes_of(std::uint32_t{0}));
}

HResult message_dialog_result(This, AsyncBlock* block, std::uint32_t* out)
{
    return async_result(block, &g_message_identity, out);
}

HResult profile_card_async(This, AsyncBlock* block, const void*, std::uint64_t xuid)
{
    log::info("gdk: XGameUiShowPlayerProfileCardAsync " + std::to_string(xuid) + ": closed");
    return async_finished(block, &g_profile_identity, kOk);
}

HResult profile_card_result(This, AsyncBlock* block)
{
    return async_get_result(block, &g_profile_identity, 0, nullptr, nullptr);
}

HResult text_entry_async(This, AsyncBlock* block, const char* title, const char* description, const char*,
                         std::uint32_t, std::uint32_t max_length)
{
    log::info("gdk: XGameUiShowTextEntryAsync '" + text(title) + "' '" + text(description) + "' max "
              + std::to_string(max_length) + ": cancelled");
    return async_finished(block, &g_text_identity, kAbort);
}

HResult text_entry_size(This, AsyncBlock* block, std::uint32_t* out)
{
    std::size_t size = 0;
    const HResult result = async_get_result_size(block, &size);
    if (result < 0) {
        return result;
    }
    return put(out, static_cast<std::uint32_t>(size));
}

HResult text_entry_result(This, AsyncBlock* block, std::uint32_t size, char* buffer, std::uint32_t* used)
{
    std::size_t written = 0;
    const HResult result = async_get_result(block, &g_text_identity, size, buffer, &written);
    if (result < 0 || used == nullptr) {
        return result;
    }
    return put(used, static_cast<std::uint32_t>(written));
}

const Table<20> kTable = [] {
    auto t = table<20>();
    set(t, 0x18, &message_dialog_async);
    set(t, 0x20, &message_dialog_result);
    set(t, 0x38, &profile_card_async);
    set(t, 0x40, &profile_card_result);
    set(t, 0x88, &text_entry_async);
    set(t, 0x90, &text_entry_size);
    set(t, 0x98, &text_entry_result);
    return t;
}();

const Object kObject{kTable.data(), "game ui"};

constexpr Guid kProvider{0xdfcd4649, 0x4ff8, 0x4043, {0xba, 0x07, 0x35, 0xd6, 0x07, 0xdf, 0x98, 0xb0}};
constexpr Guid kIid2026{0xeaf669df, 0x5542, 0x4590, {0x99, 0xa3, 0x8d, 0xc0, 0x61, 0xf8, 0x37, 0xcc}};

const std::array<Interface, 1> kInterfaces{{
    {kProvider, kIid2026, &kObject},
}};

}

std::span<const Interface> game_ui_interfaces()
{
    return kInterfaces;
}

}
