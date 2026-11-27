#pragma once

#include <cstdint>

#include <windows.h>

namespace euphoria::gpu {

struct Plane {
    std::uint64_t token;
    std::uint32_t count;
    void* const* resources;
};

class FramePacer {
public:
    static FramePacer& instance() noexcept;

    HRESULT SetFrameIntervalX(std::uint32_t length_microseconds, std::uint32_t period,
                              std::uint32_t flags) noexcept;
    HRESULT ScheduleFrameEventX(std::uint32_t kind, std::uint32_t offset, const void* list,
                                std::uint32_t flags) noexcept;
    HRESULT WaitForFrameEventX(std::uint32_t kind, std::uint32_t timeout_milliseconds,
                               const void* list, std::uint32_t flags,
                               std::uint64_t* token) noexcept;
    HRESULT GetFrameStatisticsX(std::uint64_t token, std::uint32_t kinds,
                                std::uint32_t* count, void* out) noexcept;

    std::uint64_t current_frame() const noexcept;
    std::uint64_t interval_microseconds() const noexcept;

private:
    FramePacer();
    ~FramePacer();
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    struct State;
    State* state_;
};

class Presenter {
public:
    static Presenter& instance() noexcept;

    void set_window(HWND window) noexcept;
    HWND window() const noexcept;

    HRESULT present(void* queue, std::uint32_t planes, const Plane* plane_parameters,
                    const void* parameters) noexcept;
    HRESULT present_resource(void* queue, void* resource, std::uint64_t token) noexcept;
    void resize(std::uint32_t width, std::uint32_t height) noexcept;
    void shutdown() noexcept;

private:
    Presenter();
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    HRESULT create_swap_chain(void* queue, std::uint32_t width, std::uint32_t height,
                              std::uint32_t format) noexcept;
    void release() noexcept;

    struct State;
    State* state_;
};

HWND title_window() noexcept;
FramePacer& frame_pacer() noexcept;
HRESULT present_plane(void* queue, std::uint32_t planes, const Plane* plane_parameters,
                      const void* parameters) noexcept;
HRESULT present_resource(void* queue, void* resource, std::uint64_t token) noexcept;

}
