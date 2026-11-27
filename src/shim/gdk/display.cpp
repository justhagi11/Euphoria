#include <euphoria/shim/gdk.hpp>

#include <euphoria/log/log.hpp>

#include <windows.h>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;

std::uint32_t try_enable_hdr(This, std::uint32_t preference, void*)
{
    log_first("gdk: XDisplayTryEnableHdrMode preference " + std::to_string(preference) + ": disabled (SDR)");
    return 2;
}

HResult launch_uri(This, const void*, const char* uri)
{
    log::info("gdk: XLaunchUri " + text(uri) + " not launched");
    return kNotSupported;
}

HResult acquire_deferral(This, void** out)
{
    log_first("XDisplayAcquireTimeoutDeferral");
    if (out == nullptr) {
        return kInvalidArgument;
    }
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    wchar_t reason[] = L"Title display deferral";
    context.Reason.SimpleReasonString = reason;
    HANDLE request = PowerCreateRequest(&context);
    if (request == INVALID_HANDLE_VALUE) {
        return static_cast<HResult>(0x80070000u | (GetLastError() & 0xFFFFu));
    }
    if (PowerSetRequest(request, PowerRequestDisplayRequired) == 0) {
        const DWORD error = GetLastError();
        CloseHandle(request);
        return static_cast<HResult>(0x80070000u | (error & 0xFFFFu));
    }
    *out = request;
    return kOk;
}

void close_deferral(This, void* handle)
{
    if (handle != nullptr) {
        PowerClearRequest(handle, PowerRequestDisplayRequired);
        CloseHandle(handle);
    }
}

HResult take_screenshot(This, const void*, void*)
{
    log_first("gdk: XAppCaptureTakeScreenshot not supported");
    return kNotSupported;
}

const Table<5> kHdrTable = [] {
    auto t = table<5>();
    set(t, 0x20, &try_enable_hdr);
    return t;
}();

const Table<6> kShellTable = [] {
    auto t = table<6>();
    set(t, 0x18, &launch_uri);
    set(t, 0x20, &acquire_deferral);
    set(t, 0x28, &close_deferral);
    return t;
}();

const Table<6> kCaptureTable = [] {
    auto t = table<6>();
    set(t, 0x28, &take_screenshot);
    return t;
}();

const Object kHdrObject{kHdrTable.data(), "hdr"};
const Object kShellObject{kShellTable.data(), "shell"};
const Object kCaptureObject{kCaptureTable.data(), "capture"};

constexpr Guid kHdrProvider{0x03f0fe74, 0xfdd9, 0x4e5c, {0xb6, 0x30, 0xf9, 0x33, 0x9c, 0x47, 0xac, 0xc5}};
constexpr Guid kHdrIid{0x35f07670, 0x706e, 0x4bfb, {0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3}};
constexpr Guid kShell{0x1b339674, 0x328d, 0x4283, {0xa2, 0x00, 0x31, 0x71, 0xf1, 0x8d, 0x36, 0x39}};
constexpr Guid kCaptureProvider{0xa4f1aee2, 0x4bf1, 0x4485, {0xb0, 0x08, 0xa7, 0xc2, 0x6d, 0x52, 0xac, 0x27}};
constexpr Guid kCaptureIid2026{0x22e672d7, 0xb4e3, 0x406c, {0xbd, 0x50, 0x8f, 0x0d, 0x25, 0x23, 0x6f, 0x9e}};

const std::array<Interface, 3> kInterfaces{{
    {kHdrProvider, kHdrIid, &kHdrObject},
    {kShell, kShell, &kShellObject},
    {kCaptureProvider, kCaptureIid2026, &kCaptureObject},
}};

}

std::span<const Interface> display_interfaces()
{
    return kInterfaces;
}

}
