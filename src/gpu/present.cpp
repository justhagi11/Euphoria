#include <euphoria/gpu/present.hpp>

#include <euphoria/gpu/command.hpp>
#include <euphoria/gpu/descriptor_host.hpp>
#include <euphoria/gpu/resources.hpp>
#include <euphoria/log/log.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace euphoria::gpu {
namespace {

constexpr UINT k_buffer_count = 2;
constexpr std::uint64_t k_log_interval = 60;
constexpr DWORD k_fence_timeout_milliseconds = 5000;
constexpr std::uint32_t k_default_frame_interval = 16667;
constexpr std::uint32_t k_origin_event = 0;
constexpr std::uint32_t k_token_on_timeout = 2;
constexpr std::size_t k_event_count = 3;
constexpr wchar_t k_window_class[] = L"EuphoriaPresenterWindow";

std::string hex_value(std::uint32_t value)
{
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<unsigned>(value));
    return buffer;
}

std::string hr_text(HRESULT status)
{
    return hex_value(static_cast<std::uint32_t>(status));
}

std::string window_text(HWND window)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%p", reinterpret_cast<void*>(window));
    return buffer;
}

DXGI_FORMAT swap_format(DXGI_FORMAT format) noexcept
{
    const auto value = static_cast<std::uint32_t>(format);
    if (value >= 27 && value <= 32) {
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (value == 87 || value == 90 || value == 91) {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    if (value >= 23 && value <= 25) {
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    if (value >= 9 && value <= 14) {
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
                                   D3D12_RESOURCE_STATES before,
                                   D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

ATOM register_window_class() noexcept
{
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.style = CS_HREDRAW | CS_VREDRAW;
    description.lpfnWndProc = DefWindowProcW;
    description.hInstance = GetModuleHandleW(nullptr);
    description.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    description.lpszClassName = k_window_class;
    const ATOM atom = RegisterClassExW(&description);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log::warn("gpu: presenter window class registration failed, error "
                  + std::to_string(GetLastError()));
    }
    return atom;
}

HWND create_own_window(std::uint32_t width, std::uint32_t height) noexcept
{
    static const ATOM atom = register_window_class();
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND window = CreateWindowExW(0,
                                  k_window_class,
                                  L"Euphoria",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  static_cast<int>(width),
                                  static_cast<int>(height),
                                  nullptr,
                                  nullptr,
                                  instance,
                                  nullptr);
    if (window == nullptr) {
        log::warn("gpu: presenter window creation failed, error "
                  + std::to_string(GetLastError()));
        return nullptr;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return window;
}

struct WindowSearch {
    DWORD process;
    HWND found;
};

BOOL CALLBACK visit_window(HWND window, LPARAM parameter)
{
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process == search->process && IsWindowVisible(window) != FALSE
        && GetWindow(window, GW_OWNER) == nullptr) {
        search->found = window;
        return FALSE;
    }
    return TRUE;
}

}

struct Presenter::State {
    std::mutex mutex;
    HWND window = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* swap_chain = nullptr;
    IDXGISwapChain3* swap_chain3 = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    std::uint64_t fence_value = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t frame_index = 0;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> states;
};

struct FramePacer::State {
    std::mutex mutex;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::microseconds interval{k_default_frame_interval};
    std::uint64_t first = 0;
    std::uint64_t last[k_event_count]{};
    std::atomic<std::uint64_t> frame{0};
};

HWND title_window() noexcept
{
    WindowSearch search{GetCurrentProcessId(), nullptr};
    EnumWindows(&visit_window, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

FramePacer& frame_pacer() noexcept
{
    return FramePacer::instance();
}

FramePacer& FramePacer::instance() noexcept
{
    static FramePacer pacer;
    return pacer;
}

FramePacer::FramePacer() : state_(new (std::nothrow) State{})
{
}

FramePacer::~FramePacer()
{
    delete state_;
}

HRESULT FramePacer::SetFrameIntervalX(std::uint32_t length_microseconds,
                                      std::uint32_t period,
                                      std::uint32_t flags) noexcept
{
    (void)period;
    (void)flags;
    if (state_ == nullptr) {
        log::error("gpu: frame pacer state is unavailable");
        return E_OUTOFMEMORY;
    }
    const std::uint32_t microseconds =
        length_microseconds == 0 ? k_default_frame_interval : length_microseconds;
    std::scoped_lock lock(state_->mutex);
    std::uint64_t latest = 0;
    for (const std::uint64_t value : state_->last) {
        latest = value > latest ? value : latest;
    }
    state_->start = std::chrono::steady_clock::now();
    state_->interval = std::chrono::microseconds(microseconds);
    state_->first = latest;
    log::info("gpu: frame interval " + std::to_string(microseconds) + " us");
    return S_OK;
}

HRESULT FramePacer::ScheduleFrameEventX(std::uint32_t kind,
                                        std::uint32_t offset,
                                        const void* list,
                                        std::uint32_t flags) noexcept
{
    (void)list;
    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 3) {
        log::debug("gpu: frame event kind " + std::to_string(kind) + " at offset "
                   + std::to_string(offset) + " us, flags " + std::to_string(flags));
    }
    return S_OK;
}

HRESULT FramePacer::WaitForFrameEventX(std::uint32_t kind,
                                       std::uint32_t timeout_milliseconds,
                                       const void* list,
                                       std::uint32_t flags,
                                       std::uint64_t* token) noexcept
{
    (void)list;
    if (state_ == nullptr) {
        log::error("gpu: frame pacer state is unavailable");
        return E_OUTOFMEMORY;
    }
    const std::uint64_t limit_nanoseconds =
        static_cast<std::uint64_t>(timeout_milliseconds) * 1000000ull;
    std::uint64_t wait_nanoseconds = 0;
    std::uint64_t tick = 0;
    {
        std::scoped_lock lock(state_->mutex);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - state_->start)
                                 .count();
        const std::uint64_t elapsed_nanoseconds =
            elapsed < 0 ? 0 : static_cast<std::uint64_t>(elapsed);
        std::uint64_t interval_nanoseconds =
            static_cast<std::uint64_t>(state_->interval.count()) * 1000ull;
        if (interval_nanoseconds == 0) {
            interval_nanoseconds = 1;
        }
        std::uint64_t& last = state_->last[kind < k_event_count ? kind : k_event_count - 1];
        tick = state_->first + elapsed_nanoseconds / interval_nanoseconds;
        if (tick <= last) {
            tick = last + 1;
        }
        const std::uint64_t offset_nanoseconds =
            (tick - state_->first) * interval_nanoseconds;
        wait_nanoseconds = offset_nanoseconds > elapsed_nanoseconds
            ? offset_nanoseconds - elapsed_nanoseconds
            : 0;
        if (wait_nanoseconds <= limit_nanoseconds) {
            last = tick;
        }
    }
    HRESULT status = S_OK;
    if (wait_nanoseconds > limit_nanoseconds) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<long long>(timeout_milliseconds)));
        status = S_FALSE;
    } else if (wait_nanoseconds > 0) {
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(static_cast<long long>(wait_nanoseconds)));
    }
    if (kind == k_origin_event && (status == S_OK || (flags & k_token_on_timeout) != 0)) {
        const std::uint64_t frame =
            state_->frame.fetch_add(1, std::memory_order_relaxed) + 1;
        if (token != nullptr) {
            *token = frame;
        }
    }
    log::debug("gpu: frame wait kind " + std::to_string(kind) + " hr " + hr_text(status)
               + " tick " + std::to_string(tick) + " wait "
               + std::to_string(wait_nanoseconds / 1000) + " us");
    return status;
}

HRESULT FramePacer::GetFrameStatisticsX(std::uint64_t token,
                                        std::uint32_t kinds,
                                        std::uint32_t* count,
                                        void* out) noexcept
{
    (void)out;
    if (count == nullptr) {
        log::warn("gpu: frame statistics without a count");
        return E_INVALIDARG;
    }
    *count = 0;
    if (log::verbose()) {
        log::debug("gpu: frame statistics for token " + std::to_string(token) + " kinds "
                   + std::to_string(kinds));
    }
    return S_OK;
}

std::uint64_t FramePacer::current_frame() const noexcept
{
    if (state_ == nullptr) {
        return 0;
    }
    return state_->frame.load(std::memory_order_relaxed);
}

std::uint64_t FramePacer::interval_microseconds() const noexcept
{
    if (state_ == nullptr) {
        return k_default_frame_interval;
    }
    std::scoped_lock lock(state_->mutex);
    const auto value = state_->interval.count();
    return value < 0 ? 0 : static_cast<std::uint64_t>(value);
}

Presenter& Presenter::instance() noexcept
{
    static Presenter presenter;
    return presenter;
}

Presenter::Presenter() : state_(new (std::nothrow) State{})
{
}

Presenter::~Presenter()
{
    shutdown();
    delete state_;
}

void Presenter::set_window(HWND window) noexcept
{
    if (state_ == nullptr) {
        return;
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->swap_chain != nullptr) {
        if (window != state_->window) {
            log::warn("gpu: window override ignored after the swap chain was created");
        }
        return;
    }
    state_->window = window;
    if (window != nullptr) {
        log::info("gpu: window override " + window_text(window));
    }
}

HWND Presenter::window() const noexcept
{
    if (state_ == nullptr) {
        return nullptr;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->window;
}

HRESULT Presenter::create_swap_chain(void* queue_pointer,
                                     std::uint32_t width,
                                     std::uint32_t height,
                                     std::uint32_t format_value) noexcept
{
    ID3D12CommandQueue* queue = unwrap_command_queue(queue_pointer);
    if (queue == nullptr) {
        queue = static_cast<ID3D12CommandQueue*>(queue_pointer);
    }
    if (queue == nullptr) {
        log::warn("gpu: swap chain creation without a command queue");
        return E_INVALIDARG;
    }
    if (state_->window == nullptr) {
        HWND found = title_window();
        if (found == nullptr) {
            found = create_own_window(width, height);
            if (found == nullptr) {
                log::warn("gpu: no title window and no presenter window could be created");
                return E_FAIL;
            }
            log::info("gpu: no title window; created a presenter window "
                      + window_text(found));
        }
        state_->window = found;
    }
    log::info("gpu: presenting to window " + window_text(state_->window));

    HRESULT status = S_OK;
    state_->device = descriptor_device();
    if (state_->device != nullptr) {
        state_->device->AddRef();
    } else {
        status = queue->GetDevice(IID_PPV_ARGS(&state_->device));
    }
    if (FAILED(status) || state_->device == nullptr) {
        log::warn("gpu: swap chain queue has no device, hr " + hr_text(status));
        release();
        return FAILED(status) ? status : E_FAIL;
    }
    {
        D3D12_COMMAND_QUEUE_DESC own{};
        own.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        own.NodeMask = 0;
        status = state_->device->CreateCommandQueue(&own, IID_PPV_ARGS(&state_->queue));
        if (FAILED(status) || state_->queue == nullptr) {
            log::warn("gpu: present queue creation failed, hr " + hr_text(status));
            release();
            return FAILED(status) ? status : E_FAIL;
        }
        log::info("gpu: presenter owns a direct queue");
    }

    status = state_->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&state_->allocator));
    if (FAILED(status)) {
        log::warn("gpu: present allocator creation failed, hr " + hr_text(status));
        release();
        return status;
    }
    status = state_->device->CreateCommandList(0,
                                               D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               state_->allocator,
                                               nullptr,
                                               IID_PPV_ARGS(&state_->list));
    if (SUCCEEDED(status)) {
        status = state_->list->Close();
    }
    if (FAILED(status)) {
        log::warn("gpu: present command list creation failed, hr " + hr_text(status));
        release();
        return status;
    }
    status = state_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(&state_->fence));
    if (FAILED(status)) {
        log::warn("gpu: present fence creation failed, hr " + hr_text(status));
        release();
        return status;
    }
    state_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (state_->fence_event == nullptr) {
        const DWORD error = GetLastError();
        log::warn("gpu: present event creation failed, error " + std::to_string(error));
        release();
        return HRESULT_FROM_WIN32(error);
    }
    status = CreateDXGIFactory2(0, IID_PPV_ARGS(&state_->factory));
    if (FAILED(status) || state_->factory == nullptr) {
        log::warn("gpu: DXGI factory creation failed, hr " + hr_text(status));
        release();
        return FAILED(status) ? status : E_FAIL;
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = static_cast<DXGI_FORMAT>(format_value);
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = k_buffer_count;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = 0;

    status = state_->factory->CreateSwapChainForHwnd(state_->queue,
                                                    state_->window,
                                                    &description,
                                                    nullptr,
                                                    nullptr,
                                                    &state_->swap_chain);
    if (FAILED(status) || state_->swap_chain == nullptr) {
        log::warn("gpu: swap chain creation for " + std::to_string(width) + "x"
                  + std::to_string(height) + " failed, hr " + hr_text(status));
        release();
        return FAILED(status) ? status : E_FAIL;
    }
    if (FAILED(state_->swap_chain->QueryInterface(IID_PPV_ARGS(&state_->swap_chain3)))) {
        state_->swap_chain3 = nullptr;
    }
    state_->factory->MakeWindowAssociation(state_->window, DXGI_MWA_NO_ALT_ENTER);
    state_->width = width;
    state_->height = height;
    state_->format = static_cast<DXGI_FORMAT>(format_value);
    state_->frame_index = 0;
    log::info("gpu: swap chain " + std::to_string(width) + "x" + std::to_string(height)
              + " format " + std::to_string(static_cast<unsigned>(state_->format))
              + " on " + window_text(state_->window));
    return S_OK;
}

void Presenter::release() noexcept
{
    if (state_ == nullptr) {
        return;
    }
    if (state_->list != nullptr) {
        state_->list->Release();
        state_->list = nullptr;
    }
    if (state_->allocator != nullptr) {
        state_->allocator->Release();
        state_->allocator = nullptr;
    }
    if (state_->fence_event != nullptr) {
        CloseHandle(state_->fence_event);
        state_->fence_event = nullptr;
    }
    if (state_->fence != nullptr) {
        state_->fence->Release();
        state_->fence = nullptr;
    }
    if (state_->swap_chain3 != nullptr) {
        state_->swap_chain3->Release();
        state_->swap_chain3 = nullptr;
    }
    if (state_->swap_chain != nullptr) {
        state_->swap_chain->Release();
        state_->swap_chain = nullptr;
    }
    if (state_->factory != nullptr) {
        state_->factory->Release();
        state_->factory = nullptr;
    }
    if (state_->device != nullptr) {
        state_->device->Release();
        state_->device = nullptr;
    }
    if (state_->queue != nullptr) {
        state_->queue->Release();
        state_->queue = nullptr;
    }
    state_->states.clear();
    state_->fence_value = 0;
    state_->width = 0;
    state_->height = 0;
    state_->format = DXGI_FORMAT_UNKNOWN;
    state_->frame_index = 0;
}

void Presenter::shutdown() noexcept
{
    if (state_ == nullptr) {
        return;
    }
    std::scoped_lock lock(state_->mutex);
    release();
}

HRESULT Presenter::present(void* queue_pointer,
                           std::uint32_t planes,
                           const Plane* plane_parameters,
                           const void* parameters) noexcept
{
    (void)parameters;
    static const bool no_present = GetEnvironmentVariableW(L"EUPHORIA_NO_PRESENT", nullptr, 0) != 0;
    if (no_present) {
        return S_OK;
    }
    if (state_ == nullptr) {
        log::error("gpu: presenter state is unavailable");
        return E_OUTOFMEMORY;
    }
    if (queue_pointer == nullptr) {
        log::warn("gpu: present without a command queue");
        return E_INVALIDARG;
    }
    if (planes == 0 || plane_parameters == nullptr) {
        log::warn("gpu: present without a plane");
        return E_INVALIDARG;
    }
    const Plane& plane = plane_parameters[0];
    if (plane.count == 0 || plane.resources == nullptr || plane.resources[0] == nullptr) {
        log::warn("gpu: present without a resource");
        return E_INVALIDARG;
    }
    if (planes > 1) {
        log::debug("gpu: present named " + std::to_string(planes)
                   + " planes; the first is shown");
    }
    auto* texture = static_cast<ID3D12Resource*>(unwrap_any(plane.resources[0]));
    if (texture == nullptr) {
        log::warn("gpu: present plane has no host texture");
        return E_INVALIDARG;
    }
    ID3D12CommandQueue* queue = unwrap_command_queue(queue_pointer);
    if (queue == nullptr) {
        log::warn("gpu: present with a null host command queue");
        return E_INVALIDARG;
    }

    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        log::warn("gpu: cannot present a resource that is not a texture2d");
        return E_INVALIDARG;
    }
    const DXGI_FORMAT format = swap_format(description.Format);
    if (format == DXGI_FORMAT_UNKNOWN) {
        log::warn("gpu: cannot present format "
                  + std::to_string(static_cast<unsigned>(description.Format)));
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (description.Width == 0 || description.Height == 0) {
        log::warn("gpu: cannot present an empty texture");
        return E_INVALIDARG;
    }
    const auto width = static_cast<std::uint32_t>(description.Width);
    const std::uint32_t height = description.Height;

    std::scoped_lock lock(state_->mutex);

    if (state_->swap_chain == nullptr) {
        const HRESULT created = create_swap_chain(queue_pointer, width, height,
                                                  static_cast<std::uint32_t>(format));
        if (FAILED(created)) {
            return created;
        }
    } else if (queue != state_->queue) {
        ID3D12Device* device = nullptr;
        const HRESULT queried = queue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(queried) || device == nullptr) {
            log::warn("gpu: present queue has no device, hr " + hr_text(queried));
            return FAILED(queried) ? queried : E_FAIL;
        }
        const bool same_device = device == state_->device;
        device->Release();
        if (!same_device) {
            log::warn("gpu: present queue is on another device");
            return E_INVALIDARG;
        }
    }

    if (width != state_->width || height != state_->height || format != state_->format) {
        const HRESULT resized = state_->swap_chain->ResizeBuffers(k_buffer_count,
                                                                  width,
                                                                  height,
                                                                  format,
                                                                  0);
        if (FAILED(resized)) {
            log::warn("gpu: swap chain resize to " + std::to_string(width) + "x"
                      + std::to_string(height) + " failed, hr " + hr_text(resized));
            return resized;
        }
        state_->width = width;
        state_->height = height;
        state_->format = format;
        log::info("gpu: swap chain resized to " + std::to_string(width) + "x"
                  + std::to_string(height));
    }

    UINT index = 0;
    if (state_->swap_chain3 != nullptr) {
        index = state_->swap_chain3->GetCurrentBackBufferIndex();
    }
    ID3D12Resource* back = nullptr;
    HRESULT status = state_->swap_chain->GetBuffer(index, IID_PPV_ARGS(&back));
    if (FAILED(status) || back == nullptr) {
        log::warn("gpu: swap chain back buffer " + std::to_string(index)
                  + " unavailable, hr " + hr_text(status));
        return FAILED(status) ? status : E_FAIL;
    }

    status = state_->allocator->Reset();
    if (SUCCEEDED(status)) {
        status = state_->list->Reset(state_->allocator, nullptr);
    }
    if (FAILED(status)) {
        back->Release();
        log::warn("gpu: present command list reset failed, hr " + hr_text(status));
        return status;
    }

    D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON;
    bool fixed = false;
    if (!tracked_resource_state(texture, before, fixed)) {
        const auto known = state_->states.find(texture);
        if (known != state_->states.end()) {
            before = known->second;
        }
    }
    {
        const D3D12_RESOURCE_DESC source = texture->GetDesc();
        const D3D12_RESOURCE_DESC target = back->GetDesc();
        if (source.Width != target.Width || source.Height != target.Height
            || source.Format != target.Format || source.MipLevels != target.MipLevels
            || source.DepthOrArraySize != target.DepthOrArraySize) {
            static std::atomic<std::uint32_t> mismatches{0};
            if (mismatches.fetch_add(1, std::memory_order_relaxed) < 4) {
                log::warn("gpu: present texture " + std::to_string(source.Width) + "x"
                          + std::to_string(source.Height) + " format "
                          + std::to_string(source.Format) + " mips "
                          + std::to_string(source.MipLevels) + " differs from back buffer "
                          + std::to_string(target.Width) + "x" + std::to_string(target.Height)
                          + " format " + std::to_string(target.Format));
            }
        }
    }

    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT barrier_count = 0;
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[barrier_count] =
            transition(texture, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ++barrier_count;
    }
    barriers[barrier_count] =
        transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    ++barrier_count;
    state_->list->ResourceBarrier(barrier_count, barriers);
    state_->list->CopyResource(back, texture);
    barrier_count = 0;
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barriers[barrier_count] =
            transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, before);
        ++barrier_count;
    }
    barriers[barrier_count] =
        transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ++barrier_count;
    state_->list->ResourceBarrier(barrier_count, barriers);
    status = state_->list->Close();
    if (FAILED(status)) {
        back->Release();
        log::warn("gpu: present command list close failed, hr " + hr_text(status));
        return status;
    }

    if (queue != state_->queue) {
        const std::uint64_t handoff = ++state_->fence_value;
        if (SUCCEEDED(queue->Signal(state_->fence, handoff))) {
            state_->queue->Wait(state_->fence, handoff);
        }
    }
    ID3D12CommandList* lists[] = {state_->list};
    state_->queue->ExecuteCommandLists(1, lists);
    const std::uint64_t value = ++state_->fence_value;
    status = state_->queue->Signal(state_->fence, value);
    if (FAILED(status)) {
        back->Release();
        log::warn("gpu: present fence signal failed, hr " + hr_text(status));
        return status;
    }
    if (state_->fence->GetCompletedValue() < value) {
        status = state_->fence->SetEventOnCompletion(value, state_->fence_event);
        if (FAILED(status)) {
            back->Release();
            log::warn("gpu: present fence wait setup failed, hr " + hr_text(status));
            return status;
        }
        const DWORD waited = WaitForSingleObject(state_->fence_event,
                                                 k_fence_timeout_milliseconds);
        if (waited != WAIT_OBJECT_0) {
            back->Release();
            log::warn("gpu: present fence wait failed, state "
                      + std::to_string(static_cast<unsigned>(waited)));
            return E_FAIL;
        }
    }

    status = state_->swap_chain->Present(0, 0);
    back->Release();
    state_->states[texture] = before;
    ++state_->frame_index;

    if (FAILED(status)) {
        log::warn("gpu: present frame " + std::to_string(state_->frame_index)
                  + " failed, hr " + hr_text(status));
        return status;
    }
    if (state_->frame_index == 1 || state_->frame_index % k_log_interval == 0) {
        log::info("gpu: presented frame " + std::to_string(state_->frame_index)
                  + " token " + std::to_string(plane.token) + " hr " + hr_text(status));
    } else {
        log::debug("gpu: presented frame " + std::to_string(state_->frame_index)
                   + " token " + std::to_string(plane.token) + " hr " + hr_text(status));
    }
    return S_OK;
}

HRESULT Presenter::present_resource(void* queue,
                                    void* resource,
                                    std::uint64_t token) noexcept
{
    if (resource == nullptr) {
        log::warn("gpu: present_resource without a resource");
        return E_INVALIDARG;
    }
    const Plane plane{token, 1, &resource};
    return present(queue, 1, &plane, nullptr);
}

void Presenter::resize(std::uint32_t width, std::uint32_t height) noexcept
{
    if (state_ == nullptr) {
        return;
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->swap_chain == nullptr) {
        log::debug("gpu: resize before a swap chain exists");
        return;
    }
    if (width == 0 || height == 0) {
        log::warn("gpu: swap chain resize to an empty size");
        return;
    }
    const HRESULT status = state_->swap_chain->ResizeBuffers(k_buffer_count,
                                                             width,
                                                             height,
                                                             state_->format,
                                                             0);
    if (FAILED(status)) {
        log::warn("gpu: swap chain resize to " + std::to_string(width) + "x"
                  + std::to_string(height) + " failed, hr " + hr_text(status));
        return;
    }
    state_->width = width;
    state_->height = height;
    log::info("gpu: swap chain resized to " + std::to_string(width) + "x"
              + std::to_string(height));
}

HRESULT present_plane(void* queue,
                      std::uint32_t planes,
                      const Plane* plane_parameters,
                      const void* parameters) noexcept
{
    return Presenter::instance().present(queue, planes, plane_parameters, parameters);
}

HRESULT present_resource(void* queue, void* resource, std::uint64_t token) noexcept
{
    return Presenter::instance().present_resource(queue, resource, token);
}

}
