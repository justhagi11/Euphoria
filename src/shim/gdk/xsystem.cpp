#include <euphoria/shim/gdk.hpp>

#include <cstring>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;

constexpr std::string_view kConsoleId = "00000000000000000000000000000000000001";
constexpr std::string_view kSandbox = "RETAIL";
constexpr std::string_view kDeviceId = "RXVwaG9yaWEgb2ZmbGluZSBkZXZpY2UgaWRlbnRpdHk=";

struct Version {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t build;
    std::uint16_t revision;
};

struct AnalyticsInfo {
    Version os;
    Version hosting_os;
    char family[64];
    char form[64];
};

static_assert(sizeof(AnalyticsInfo) == 0x90);

HResult console_id(This, std::size_t size, char* buffer, std::size_t* used)
{
    log_first("XSystemGetConsoleId");
    return put_string(kConsoleId, size, buffer, used);
}

HResult sandbox_id(This, std::size_t size, char* buffer, std::size_t* used)
{
    log_first("XSystemGetXboxLiveSandboxId");
    return put_string(kSandbox, size, buffer, used);
}

HResult device_id(This, std::size_t size, char* buffer, std::size_t* used)
{
    log_first("XSystemGetAppSpecificDeviceId");
    return put_string(kDeviceId, size, buffer, used);
}

AnalyticsInfo* analytics_info(This, AnalyticsInfo* out)
{
    log_first("XSystemGetAnalyticsInfo");
    AnalyticsInfo info{};
    info.os = Version{10, 0, 22000, 0};
    info.hosting_os = info.os;
    std::memcpy(info.family, "Xbox", 5);
    std::memcpy(info.form, "Xbox Series X", 14);
    *out = info;
    return out;
}

const Table<6> kTable = [] {
    auto t = table<6>();
    set(t, 0x18, &console_id);
    set(t, 0x20, &sandbox_id);
    set(t, 0x28, &device_id);
    return t;
}();

const Table<4> kAnalyticsTable = [] {
    auto t = table<4>();
    set(t, 0x18, &analytics_info);
    return t;
}();

const Object kObject{kTable.data(), "system"};
const Object kAnalyticsObject{kAnalyticsTable.data(), "analytics"};

constexpr Guid kProvider{0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};
constexpr Guid kIid{0x6fd71f09, 0x7513, 0x49f0, {0x89, 0xbc, 0xbf, 0xaf, 0x5d, 0xf6, 0xf8, 0x52}};
constexpr Guid kIid2026{0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}};
constexpr Guid kAnalytics{0xb884675d, 0xb738, 0x4a9c, {0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b}};

const std::array<Interface, 3> kInterfaces{{
    {kProvider, kIid, &kObject},
    {kProvider, kIid2026, &kObject},
    {kAnalytics, kAnalytics, &kAnalyticsObject},
}};

}

std::span<const Interface> system_interfaces()
{
    return kInterfaces;
}

}
