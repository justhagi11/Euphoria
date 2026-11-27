#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <d3d12.h>
#include <dxgi1_3.h>

namespace euphoria::gpu {

inline constexpr std::size_t command_queue_slot_count = 36;
inline constexpr std::size_t command_list_slot_count = 160;

enum class CommandLayout {
    Legacy,
    Newer,
};

inline constexpr CommandLayout default_command_layout = CommandLayout::Newer;

struct CommandQueueProxy {
    void** vtable;
    ID3D12CommandQueue* host;
    ID3D12Device* device;
    void* owner_device;
    std::atomic<std::uint32_t> references{1};
    std::array<std::atomic<std::uint32_t>, command_queue_slot_count> slot_calls{};
    std::mutex completion_mutex;
    ID3D12Fence* completion = nullptr;
    std::uint64_t completion_value = 0;
    HANDLE completion_event = nullptr;
};

struct PendingWrite {
    std::uint64_t address = 0;
    std::uint64_t value = 0;
    std::uint32_t bytes = 0;
    std::uint32_t kind = 0;
};

struct CommandListProxy {
    void** vtable;
    ID3D12GraphicsCommandList* host;
    void* allocator;
    void* device;
    std::atomic<std::uint32_t> references{1};
    std::array<std::atomic<std::uint32_t>, command_list_slot_count> slot_calls{};
    void* draw_state = nullptr;
    std::atomic<bool> closed{false};
    void* graphics_root = nullptr;
    void* compute_root = nullptr;
    std::mutex pending_mutex;
    std::vector<PendingWrite> pending_writes;
    std::vector<std::unique_ptr<std::uint8_t[]>> uploads;
};

CommandQueueProxy* create_command_queue(ID3D12CommandQueue* host,
                                        ID3D12Device* device,
                                        void* owner_device,
                                        CommandLayout layout = default_command_layout);

CommandListProxy* create_command_list(ID3D12GraphicsCommandList* host,
                                      void* allocator,
                                      void* device,
                                      CommandLayout layout = default_command_layout);

ID3D12CommandQueue* unwrap_command_queue(void* object) noexcept;
ID3D12GraphicsCommandList* unwrap_command_list(void* object) noexcept;

bool is_command_queue_proxy(void* object) noexcept;
bool is_command_list_proxy(void* object) noexcept;

}
