#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>

#include <algorithm>

#include <windows.h>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;

constexpr std::array<std::uint32_t, 20> kAvailable{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20};

HResult title_id(This, std::uint32_t* out)
{
    log_first("XGameGetXboxTitleId");
    return put(out, title().id);
}

void launch_new_game(This, const char* executable, const char* arguments, const void*)
{
    log::error("gdk: XLaunchNewGame " + text(executable) + " " + text(arguments) + ": exiting");
    ExitProcess(0);
}

bool is_feature_available(This, std::uint32_t feature)
{
    log_first("XGameRuntimeIsFeatureAvailable " + std::to_string(feature));
    return std::find(kAvailable.begin(), kAvailable.end(), feature) != kAvailable.end();
}

HResult register_callback(This, const void*, void*, const void*, std::uint64_t* out)
{
    return put(out, next_token());
}

bool unregister_callback(This, std::uint64_t, std::uint8_t)
{
    return true;
}

HResult write_event(This, const void*, const char*, const char*, const char* name, const char*, const char*)
{
    log_first("gdk: XGameEventWrite " + text(name) + " dropped");
    return kOk;
}

const Table<5> kTitleTable = [] {
    auto t = table<5>();
    set(t, 0x18, &title_id);
    set(t, 0x20, &launch_new_game);
    return t;
}();

const Table<4> kFeaturesTable = [] {
    auto t = table<4>();
    set(t, 0x18, &is_feature_available);
    return t;
}();

const Table<5> kInvitesTable = [] {
    auto t = table<5>();
    set(t, 0x18, &register_callback);
    set(t, 0x20, &unregister_callback);
    return t;
}();

const Table<4> kEventsTable = [] {
    auto t = table<4>();
    set(t, 0x18, &write_event);
    return t;
}();

const Object kTitleObject{kTitleTable.data(), "title"};
const Object kFeaturesObject{kFeaturesTable.data(), "features"};
const Object kInvitesObject{kInvitesTable.data(), "invites"};
const Object kEventsObject{kEventsTable.data(), "events"};

constexpr Guid kTitleProvider{0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};
constexpr Guid kTitleIid{0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}};
constexpr Guid kFeatures{0x8836fe87, 0xedb9, 0x4fe3, {0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40}};
constexpr Guid kInvites{0x0651aae2, 0x4012, 0x4077, {0xbf, 0x84, 0x8b, 0x90, 0x97, 0x09, 0x0e, 0x2c}};
constexpr Guid kEvents{0xbbfbdcc7, 0xbfe7, 0x409b, {0xa5, 0xca, 0xed, 0xf0, 0x54, 0x96, 0x0b, 0x4d}};

const std::array<Interface, 4> kInterfaces{{
    {kTitleProvider, kTitleIid, &kTitleObject},
    {kFeatures, kFeatures, &kFeaturesObject},
    {kInvites, kInvites, &kInvitesObject},
    {kEvents, kEvents, &kEventsObject},
}};

}

std::span<const Interface> game_interfaces()
{
    return kInterfaces;
}

}
