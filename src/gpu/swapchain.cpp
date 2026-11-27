#include <euphoria/gpu/swapchain.hpp>
#include <euphoria/log/log.hpp>

#include <array>
#include <cstdint>
#include <new>
#include <string>

#include <windows.h>

#include <dxgi1_6.h>

namespace euphoria::gpu {
namespace {

constexpr std::int32_t k_ok = 0;
constexpr std::int32_t k_no_interface = static_cast<std::int32_t>(0x80004002u);
constexpr std::int32_t k_not_implemented = static_cast<std::int32_t>(0x80004001u);
constexpr std::int32_t k_pointer = static_cast<std::int32_t>(0x80004003u);
constexpr std::size_t k_total_slots = 64;

void log_unsupported(std::uint32_t slot)
{
    log::warn("dxgi: swap chain vtable slot " + std::to_string(slot)
              + " is not implemented");
}

template <std::uint32_t Slot>
std::int32_t __stdcall unsupported(SwapChainProxy*, ...)
{
    log_unsupported(Slot);
    return k_not_implemented;
}

std::int32_t __stdcall query_interface(SwapChainProxy* self, const IID& iid, void** out)
{
    if (out == nullptr) {
        return k_pointer;
    }
    const bool known = IsEqualGUID(iid, __uuidof(IUnknown)) != 0
        || IsEqualGUID(iid, __uuidof(IDXGIObject)) != 0
        || IsEqualGUID(iid, __uuidof(IDXGIDeviceSubObject)) != 0
        || IsEqualGUID(iid, __uuidof(IDXGISwapChain)) != 0
        || IsEqualGUID(iid, __uuidof(IDXGISwapChain1)) != 0
        || IsEqualGUID(iid, __uuidof(IDXGISwapChain2)) != 0
        || IsEqualGUID(iid, __uuidof(IDXGISwapChain3)) != 0
        || IsEqualGUID(iid, __uuidof(IDXGISwapChain4)) != 0;
    if (!known) {
        *out = nullptr;
        return k_no_interface;
    }
    *out = self;
    return k_ok;
}

std::uint32_t __stdcall add_ref(SwapChainProxy*)
{
    return 2;
}

std::uint32_t __stdcall release(SwapChainProxy*)
{
    return 1;
}

std::int32_t __stdcall set_private_data(SwapChainProxy* self, const GUID& name,
                                        std::uint32_t size, const void* data)
{
    return self->host->SetPrivateData(name, size, data);
}

std::int32_t __stdcall set_private_data_interface(SwapChainProxy* self, const GUID& name,
                                                  const IUnknown* unknown)
{
    return self->host->SetPrivateDataInterface(name, unknown);
}

std::int32_t __stdcall get_private_data(SwapChainProxy* self, const GUID& name,
                                        std::uint32_t* size, void* data)
{
    return self->host->GetPrivateData(name, size, data);
}

std::int32_t __stdcall get_parent(SwapChainProxy* self, const GUID& iid, void** out)
{
    return self->host->GetParent(iid, out);
}

std::int32_t __stdcall get_device(SwapChainProxy* self, void** out)
{
    if (out == nullptr) {
        return k_pointer;
    }
    *out = self->device;
    return k_ok;
}

std::int32_t __stdcall present(SwapChainProxy* self, std::uint32_t sync_interval,
                               std::uint32_t flags)
{
    log::debug("dxgi: swap chain present frame " + std::to_string(self->frame));
    ++self->frame;
    return self->host->Present(sync_interval, flags);
}

std::int32_t __stdcall get_buffer(SwapChainProxy* self, std::uint32_t index, const GUID& iid,
                                  void** out)
{
    return self->host->GetBuffer(index, iid, out);
}

std::int32_t __stdcall set_fullscreen_state(SwapChainProxy* self, BOOL fullscreen,
                                            IDXGIOutput* target)
{
    return self->host->SetFullscreenState(fullscreen, target);
}

std::int32_t __stdcall get_fullscreen_state(SwapChainProxy* self, BOOL* fullscreen,
                                            IDXGIOutput** target)
{
    return self->host->GetFullscreenState(fullscreen, target);
}

std::int32_t __stdcall get_desc(SwapChainProxy* self, DXGI_SWAP_CHAIN_DESC* desc)
{
    return self->host->GetDesc(desc);
}

std::int32_t __stdcall resize_buffers(SwapChainProxy* self, std::uint32_t count,
                                      std::uint32_t width, std::uint32_t height,
                                      DXGI_FORMAT format, std::uint32_t flags)
{
    return self->host->ResizeBuffers(count, width, height, format, flags);
}

std::int32_t __stdcall resize_target(SwapChainProxy* self, const DXGI_MODE_DESC* target)
{
    return self->host->ResizeTarget(target);
}

std::int32_t __stdcall get_containing_output(SwapChainProxy* self, IDXGIOutput** output)
{
    return self->host->GetContainingOutput(output);
}

std::int32_t __stdcall get_frame_statistics(SwapChainProxy* self, DXGI_FRAME_STATISTICS* stats)
{
    return self->host->GetFrameStatistics(stats);
}

std::int32_t __stdcall get_last_present_count(SwapChainProxy* self, std::uint32_t* count)
{
    return self->host->GetLastPresentCount(count);
}

std::int32_t __stdcall get_desc1(SwapChainProxy* self, DXGI_SWAP_CHAIN_DESC1* desc)
{
    if (self->host1 == nullptr) {
        log_unsupported(18);
        return k_not_implemented;
    }
    return self->host1->GetDesc1(desc);
}

std::int32_t __stdcall get_fullscreen_desc(SwapChainProxy* self,
                                           DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc)
{
    if (self->host1 == nullptr) {
        log_unsupported(19);
        return k_not_implemented;
    }
    return self->host1->GetFullscreenDesc(desc);
}

std::int32_t __stdcall get_hwnd(SwapChainProxy* self, HWND* hwnd)
{
    if (self->host1 == nullptr) {
        log_unsupported(20);
        return k_not_implemented;
    }
    return self->host1->GetHwnd(hwnd);
}

std::int32_t __stdcall get_core_window(SwapChainProxy* self, const GUID& iid, void** out)
{
    if (self->host1 == nullptr) {
        log_unsupported(21);
        return k_not_implemented;
    }
    return self->host1->GetCoreWindow(iid, out);
}

std::int32_t __stdcall present1(SwapChainProxy* self, std::uint32_t sync_interval,
                                std::uint32_t flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    if (self->host1 == nullptr) {
        log_unsupported(22);
        return k_not_implemented;
    }
    log::debug("dxgi: swap chain present1 frame " + std::to_string(self->frame));
    ++self->frame;
    return self->host1->Present1(sync_interval, flags, parameters);
}

BOOL __stdcall is_temporary_mono_supported(SwapChainProxy* self)
{
    if (self->host1 == nullptr) {
        log_unsupported(23);
        return FALSE;
    }
    return self->host1->IsTemporaryMonoSupported();
}

std::int32_t __stdcall get_restrict_to_output(SwapChainProxy* self, IDXGIOutput** output)
{
    if (self->host1 == nullptr) {
        log_unsupported(24);
        return k_not_implemented;
    }
    return self->host1->GetRestrictToOutput(output);
}

std::int32_t __stdcall set_background_color(SwapChainProxy* self, const DXGI_RGBA* color)
{
    if (self->host1 == nullptr) {
        log_unsupported(25);
        return k_not_implemented;
    }
    return self->host1->SetBackgroundColor(color);
}

std::int32_t __stdcall get_background_color(SwapChainProxy* self, DXGI_RGBA* color)
{
    if (self->host1 == nullptr) {
        log_unsupported(26);
        return k_not_implemented;
    }
    return self->host1->GetBackgroundColor(color);
}

std::int32_t __stdcall set_rotation(SwapChainProxy* self, DXGI_MODE_ROTATION rotation)
{
    if (self->host1 == nullptr) {
        log_unsupported(27);
        return k_not_implemented;
    }
    return self->host1->SetRotation(rotation);
}

std::int32_t __stdcall get_rotation(SwapChainProxy* self, DXGI_MODE_ROTATION* rotation)
{
    if (self->host1 == nullptr) {
        log_unsupported(28);
        return k_not_implemented;
    }
    return self->host1->GetRotation(rotation);
}

std::int32_t __stdcall set_source_size(SwapChainProxy* self, std::uint32_t width,
                                       std::uint32_t height)
{
    if (self->host2 == nullptr) {
        log_unsupported(29);
        return k_not_implemented;
    }
    return self->host2->SetSourceSize(width, height);
}

std::int32_t __stdcall get_source_size(SwapChainProxy* self, std::uint32_t* width,
                                       std::uint32_t* height)
{
    if (self->host2 == nullptr) {
        log_unsupported(30);
        return k_not_implemented;
    }
    return self->host2->GetSourceSize(width, height);
}

std::int32_t __stdcall set_maximum_frame_latency(SwapChainProxy* self, std::uint32_t latency)
{
    if (self->host2 == nullptr) {
        log_unsupported(31);
        return k_not_implemented;
    }
    return self->host2->SetMaximumFrameLatency(latency);
}

std::int32_t __stdcall get_maximum_frame_latency(SwapChainProxy* self, std::uint32_t* latency)
{
    if (self->host2 == nullptr) {
        log_unsupported(32);
        return k_not_implemented;
    }
    return self->host2->GetMaximumFrameLatency(latency);
}

HANDLE __stdcall get_frame_latency_waitable_object(SwapChainProxy* self)
{
    if (self->host2 == nullptr) {
        log_unsupported(33);
        return nullptr;
    }
    return self->host2->GetFrameLatencyWaitableObject();
}

std::int32_t __stdcall set_matrix_transform(SwapChainProxy* self,
                                            const DXGI_MATRIX_3X2_F* matrix)
{
    if (self->host2 == nullptr) {
        log_unsupported(34);
        return k_not_implemented;
    }
    return self->host2->SetMatrixTransform(matrix);
}

std::int32_t __stdcall get_matrix_transform(SwapChainProxy* self, DXGI_MATRIX_3X2_F* matrix)
{
    if (self->host2 == nullptr) {
        log_unsupported(35);
        return k_not_implemented;
    }
    return self->host2->GetMatrixTransform(matrix);
}

UINT __stdcall get_current_back_buffer_index(SwapChainProxy* self)
{
    if (self->host3 == nullptr) {
        log_unsupported(36);
        return 0;
    }
    return self->host3->GetCurrentBackBufferIndex();
}

std::int32_t __stdcall check_color_space_support(SwapChainProxy* self,
                                                 DXGI_COLOR_SPACE_TYPE color_space,
                                                 std::uint32_t* support)
{
    if (self->host3 == nullptr) {
        log_unsupported(37);
        return k_not_implemented;
    }
    return self->host3->CheckColorSpaceSupport(color_space, support);
}

std::int32_t __stdcall set_color_space1(SwapChainProxy* self, DXGI_COLOR_SPACE_TYPE color_space)
{
    if (self->host3 == nullptr) {
        log_unsupported(38);
        return k_not_implemented;
    }
    return self->host3->SetColorSpace1(color_space);
}

std::int32_t __stdcall resize_buffers1(SwapChainProxy* self, std::uint32_t count,
                                       std::uint32_t width, std::uint32_t height,
                                       DXGI_FORMAT format, std::uint32_t flags,
                                       const UINT* node_mask, IUnknown* const* present_queue)
{
    if (self->host3 == nullptr) {
        log_unsupported(39);
        return k_not_implemented;
    }
    return self->host3->ResizeBuffers1(count, width, height, format, flags, node_mask,
                                       present_queue);
}

std::int32_t __stdcall set_hdr_metadata(SwapChainProxy* self, DXGI_HDR_METADATA_TYPE type,
                                        std::uint32_t size, void* data)
{
    if (self->host4 == nullptr) {
        log_unsupported(40);
        return k_not_implemented;
    }
    return self->host4->SetHDRMetaData(type, size, data);
}

template <typename Function>
void* entry(Function function) noexcept
{
    return reinterpret_cast<void*>(function);
}

std::array<void*, k_total_slots>& vtable_storage()
{
    static std::array<void*, k_total_slots> table = [] {
        std::array<void*, k_total_slots> built{};
        built[0] = entry(&query_interface);
        built[1] = entry(&add_ref);
        built[2] = entry(&release);
        built[3] = entry(&set_private_data);
        built[4] = entry(&set_private_data_interface);
        built[5] = entry(&get_private_data);
        built[6] = entry(&get_parent);
        built[7] = entry(&get_device);
        built[8] = entry(&present);
        built[9] = entry(&get_buffer);
        built[10] = entry(&set_fullscreen_state);
        built[11] = entry(&get_fullscreen_state);
        built[12] = entry(&get_desc);
        built[13] = entry(&resize_buffers);
        built[14] = entry(&resize_target);
        built[15] = entry(&get_containing_output);
        built[16] = entry(&get_frame_statistics);
        built[17] = entry(&get_last_present_count);
        built[18] = entry(&get_desc1);
        built[19] = entry(&get_fullscreen_desc);
        built[20] = entry(&get_hwnd);
        built[21] = entry(&get_core_window);
        built[22] = entry(&present1);
        built[23] = entry(&is_temporary_mono_supported);
        built[24] = entry(&get_restrict_to_output);
        built[25] = entry(&set_background_color);
        built[26] = entry(&get_background_color);
        built[27] = entry(&set_rotation);
        built[28] = entry(&get_rotation);
        built[29] = entry(&set_source_size);
        built[30] = entry(&get_source_size);
        built[31] = entry(&set_maximum_frame_latency);
        built[32] = entry(&get_maximum_frame_latency);
        built[33] = entry(&get_frame_latency_waitable_object);
        built[34] = entry(&set_matrix_transform);
        built[35] = entry(&get_matrix_transform);
        built[36] = entry(&get_current_back_buffer_index);
        built[37] = entry(&check_color_space_support);
        built[38] = entry(&set_color_space1);
        built[39] = entry(&resize_buffers1);
        built[40] = entry(&set_hdr_metadata);
        built[41] = entry(&unsupported<41>);
        built[42] = entry(&unsupported<42>);
        built[43] = entry(&unsupported<43>);
        built[44] = entry(&unsupported<44>);
        built[45] = entry(&unsupported<45>);
        built[46] = entry(&unsupported<46>);
        built[47] = entry(&unsupported<47>);
        built[48] = entry(&unsupported<48>);
        built[49] = entry(&unsupported<49>);
        built[50] = entry(&unsupported<50>);
        built[51] = entry(&unsupported<51>);
        built[52] = entry(&unsupported<52>);
        built[53] = entry(&unsupported<53>);
        built[54] = entry(&unsupported<54>);
        built[55] = entry(&unsupported<55>);
        built[56] = entry(&unsupported<56>);
        built[57] = entry(&unsupported<57>);
        built[58] = entry(&unsupported<58>);
        built[59] = entry(&unsupported<59>);
        built[60] = entry(&unsupported<60>);
        built[61] = entry(&unsupported<61>);
        built[62] = entry(&unsupported<62>);
        built[63] = entry(&unsupported<63>);
        return built;
    }();
    return table;
}

template <typename Interface>
Interface* query_host(IDXGISwapChain* host) noexcept
{
    Interface* result = nullptr;
    const HRESULT status = host->QueryInterface(__uuidof(Interface),
                                                reinterpret_cast<void**>(&result));
    if (FAILED(status)) {
        return nullptr;
    }
    return result;
}

}

void** swap_chain_vtable() noexcept
{
    return vtable_storage().data();
}

bool is_swap_chain_proxy(const void* object) noexcept
{
    return object != nullptr && *static_cast<void* const*>(object) == swap_chain_vtable();
}

SwapChainProxy* swap_chain_proxy(void* object) noexcept
{
    return is_swap_chain_proxy(object) ? static_cast<SwapChainProxy*>(object) : nullptr;
}

const SwapChainProxy* swap_chain_proxy(const void* object) noexcept
{
    return is_swap_chain_proxy(object) ? static_cast<const SwapChainProxy*>(object) : nullptr;
}

IDXGISwapChain* swap_chain_host(void* object) noexcept
{
    const SwapChainProxy* proxy = swap_chain_proxy(object);
    return proxy == nullptr ? nullptr : proxy->host;
}

const IDXGISwapChain* swap_chain_host(const void* object) noexcept
{
    const SwapChainProxy* proxy = swap_chain_proxy(object);
    return proxy == nullptr ? nullptr : proxy->host;
}

SwapChainProxy* create_swap_chain_proxy(IDXGISwapChain* host, IUnknown* device) noexcept
{
    if (host == nullptr) {
        return nullptr;
    }
    auto* proxy = new (std::nothrow) SwapChainProxy{};
    if (proxy == nullptr) {
        return nullptr;
    }
    proxy->vtable = swap_chain_vtable();
    proxy->host = host;
    proxy->device = device;

    if ((proxy->host4 = query_host<IDXGISwapChain4>(host)) != nullptr) {
        proxy->host3 = proxy->host4;
        proxy->host2 = proxy->host4;
        proxy->host1 = proxy->host4;
        proxy->max_slot = 40;
    } else if ((proxy->host3 = query_host<IDXGISwapChain3>(host)) != nullptr) {
        proxy->host2 = proxy->host3;
        proxy->host1 = proxy->host3;
        proxy->max_slot = 39;
    } else if ((proxy->host2 = query_host<IDXGISwapChain2>(host)) != nullptr) {
        proxy->host1 = proxy->host2;
        proxy->max_slot = 35;
    } else if ((proxy->host1 = query_host<IDXGISwapChain1>(host)) != nullptr) {
        proxy->max_slot = 28;
    } else {
        proxy->max_slot = 17;
    }
    return proxy;
}

SwapChainProxy* wrap_swap_chain(IDXGISwapChain* host, IUnknown* device) noexcept
{
    return create_swap_chain_proxy(host, device);
}

}
