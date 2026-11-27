#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <d3d12.h>

namespace euphoria::gpu {

inline constexpr std::uint64_t title_page_size = 64ull * 1024ull;

enum class TitlePageFlags : std::uint32_t {
    None = 0,
    Committed = 1u << 0,
    Graphics = 1u << 1,
    Written = 1u << 2,
};

constexpr TitlePageFlags operator|(TitlePageFlags left, TitlePageFlags right) noexcept
{
    return static_cast<TitlePageFlags>(static_cast<std::uint32_t>(left)
                                       | static_cast<std::uint32_t>(right));
}

constexpr TitlePageFlags operator&(TitlePageFlags left, TitlePageFlags right) noexcept
{
    return static_cast<TitlePageFlags>(static_cast<std::uint32_t>(left)
                                       & static_cast<std::uint32_t>(right));
}

constexpr TitlePageFlags operator~(TitlePageFlags value) noexcept
{
    return static_cast<TitlePageFlags>(~static_cast<std::uint32_t>(value));
}

constexpr bool has_flag(TitlePageFlags value, TitlePageFlags flag) noexcept
{
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

struct TitleRange {
    std::uint64_t address{};
    std::size_t size{};
};

struct TitleShadowBuffer {
    ID3D12Resource* host{};
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address{};
    std::uint64_t address{};
    std::size_t size{};
};

struct TitlePageTableEntry {
    std::uint32_t address_low{};
    std::uint32_t address_high{};
    std::uint32_t gpu_low{};
    std::uint32_t gpu_high{};
    std::uint32_t size{};
    std::uint32_t flags{};
    std::uint32_t reserved[2]{};
};

struct TitlePageTable {
    ID3D12Resource* host{};
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address{};
    std::size_t entries{};
};

struct TitleSubmitStats {
    std::size_t pages{};
    std::size_t refreshed{};
    std::size_t dirty{};
    bool dirty_all{};
};

struct TitleTextureDesc {
    std::uint32_t width{};
    std::uint32_t height{1};
    std::uint32_t depth{1};
    std::uint32_t mips{1};
    std::uint32_t array_size{1};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    D3D12_RESOURCE_DIMENSION dimension{D3D12_RESOURCE_DIMENSION_TEXTURE2D};
    bool tiled{};
};

class TitleMemory {
public:
    TitleMemory();
    ~TitleMemory();

    TitleMemory(const TitleMemory&) = delete;
    TitleMemory& operator=(const TitleMemory&) = delete;

    bool register_page(std::uint64_t address, std::size_t size);
    bool register_page(std::uint64_t address, std::size_t size, TitlePageFlags flags);
    void unregister_pages(std::uint64_t address, std::size_t size);
    std::uint64_t page_of(std::uint64_t address) const noexcept;

    TitleSubmitStats prepare_for_submit(std::span<const TitleRange> ranges);
    TitleSubmitStats prepare_for_submit(ID3D12Device* device,
                                        std::span<const TitleRange> ranges);

    TitleShadowBuffer create_shadow_buffer(ID3D12Device* device, std::uint64_t address,
                                           std::size_t size);
    ID3D12Resource* create_shadow_buffer(ID3D12Device* device, std::uint64_t address,
                                         std::size_t size,
                                         D3D12_GPU_VIRTUAL_ADDRESS* gpu_address);
    bool refresh_shadow(std::uint64_t address, std::size_t size);

    TitlePageTable bind_page_table(ID3D12Device* device);

    void mark_written(std::uint64_t address, std::size_t size);
    void consume_dirty(std::vector<TitleRange>& ranges);
    std::vector<TitleRange> consume_dirty();

    ID3D12Resource* create_title_texture(ID3D12Device* device, const void* console_desc,
                                         std::uint64_t address);
    ID3D12Resource* create_title_texture(ID3D12Device* device,
                                         const TitleTextureDesc& console_desc,
                                         std::uint64_t address);
    bool copy_title_texture(std::uint64_t address, std::size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

TitleMemory& title_memory();

bool register_page(std::uint64_t address, std::size_t size);
bool register_page(std::uint64_t address, std::size_t size, TitlePageFlags flags);
void unregister_pages(std::uint64_t address, std::size_t size);
std::uint64_t page_of(std::uint64_t address) noexcept;

TitleSubmitStats prepare_for_submit(std::span<const TitleRange> ranges);
TitleSubmitStats prepare_for_submit(ID3D12Device* device,
                                    std::span<const TitleRange> ranges);

TitleShadowBuffer create_shadow_buffer(ID3D12Device* device, std::uint64_t address,
                                       std::size_t size);
ID3D12Resource* create_shadow_buffer(ID3D12Device* device, std::uint64_t address,
                                     std::size_t size,
                                     D3D12_GPU_VIRTUAL_ADDRESS* gpu_address);
bool refresh_shadow(std::uint64_t address, std::size_t size);

TitlePageTable bind_page_table(ID3D12Device* device);

void mark_written(std::uint64_t address, std::size_t size);
void consume_dirty(std::vector<TitleRange>& ranges);
std::vector<TitleRange> consume_dirty();

ID3D12Resource* create_title_texture(ID3D12Device* device, const void* console_desc,
                                     std::uint64_t address);
ID3D12Resource* create_title_texture(ID3D12Device* device,
                                     const TitleTextureDesc& console_desc,
                                     std::uint64_t address);
bool copy_title_texture(std::uint64_t address, std::size_t size);

}
