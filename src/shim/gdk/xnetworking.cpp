#include <euphoria/shim/gdk.hpp>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;

constexpr std::uint16_t kUdpPort = 3074;

char g_port_identity = 0;
char g_security_identity = 0;

struct ConnectivityHint {
    std::uint32_t level;
    std::uint32_t cost;
    std::uint32_t interface_type;
    bool initialized;
    bool approaching_data_limit;
    bool over_data_limit;
    bool roaming;
};

static_assert(sizeof(ConnectivityHint) == 16);

HResult udp_port(This, std::uint16_t* out)
{
    return put(out, kUdpPort);
}

HResult udp_port_async(This, AsyncBlock* block)
{
    return async_finished_with(block, &g_port_identity, bytes_of(kUdpPort));
}

HResult udp_port_result(This, AsyncBlock* block, std::uint16_t* out)
{
    return async_result(block, &g_port_identity, out);
}

HResult security_info_async(This, const char* url, AsyncBlock* block)
{
    log_first("gdk: XNetworkingQuerySecurityInformationForUrlAsync offline (" + text(url) + ")");
    return async_finished(block, &g_security_identity, kNetworkUnreachable);
}

HResult security_info_size(This, AsyncBlock* block, std::size_t* out)
{
    return async_get_result_size(block, out);
}

HResult security_info_result(This, AsyncBlock* block, std::size_t size, std::size_t* used, void* buffer,
                             const void**)
{
    return async_get_result(block, &g_security_identity, size, buffer, used);
}

HResult verify_certificate(This, void*, const void*)
{
    return kNetworkUnreachable;
}

HResult connectivity_hint(This, ConnectivityHint* out)
{
    log_first("XNetworkingGetConnectivityHint");
    return put(out, ConnectivityHint{1, 0, 0, true, false, false, false});
}

HResult register_callback(This, const void*, void*, const void*, std::uint64_t* out)
{
    return put(out, next_token());
}

bool unregister_callback(This, std::uint64_t, std::uint8_t)
{
    return true;
}

const Table<18> kTable = [] {
    auto t = table<18>();
    set(t, 0x18, &udp_port);
    set(t, 0x20, &udp_port_async);
    set(t, 0x28, &udp_port_result);
    set(t, 0x30, &register_callback);
    set(t, 0x38, &unregister_callback);
    set(t, 0x40, &security_info_async);
    set(t, 0x48, &security_info_size);
    set(t, 0x50, &security_info_result);
    set(t, 0x70, &verify_certificate);
    set(t, 0x78, &connectivity_hint);
    set(t, 0x80, &register_callback);
    set(t, 0x88, &unregister_callback);
    return t;
}();

const Object kObject{kTable.data(), "networking"};

constexpr Guid kProvider{0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}};
constexpr Guid kIid2026{0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}};

const std::array<Interface, 2> kInterfaces{{
    {kProvider, kProvider, &kObject},
    {kProvider, kIid2026, &kObject},
}};

}

std::span<const Interface> networking_interfaces()
{
    return kInterfaces;
}

}
