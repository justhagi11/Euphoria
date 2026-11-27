#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

struct ID3D12Resource;

namespace euphoria::gpu {

enum class Placement {
    DeviceLocal,
    Unified,
    Upload,
    Readback,
};

struct MemoryStats {
    std::uint64_t device_bytes{};
    std::uint64_t unified_bytes{};
    std::uint64_t upload_bytes{};
    std::uint64_t readback_bytes{};
    std::uint64_t staging_peak{};
    std::uint64_t staging_live{};
    std::uint64_t uploads{};
};

class MemoryManager {
public:
    MemoryManager();
    ~MemoryManager();
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    bool initialize();
    ID3D12Resource* create(std::uint64_t size, Placement placement);
    ID3D12Resource* upload(std::span<const std::uint8_t> data, Placement placement);
    void release(ID3D12Resource* resource);

    const MemoryStats& stats() const noexcept;
    std::string description() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
