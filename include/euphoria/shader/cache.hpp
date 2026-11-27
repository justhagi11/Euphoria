#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace euphoria::shader {

class Cache {
public:
    explicit Cache(std::filesystem::path directory);

    std::optional<std::vector<std::uint8_t>> load(std::uint64_t key) const;
    bool store(std::uint64_t key, std::span<const std::uint8_t> payload) const;

    const std::filesystem::path& directory() const noexcept { return directory_; }

    static std::uint64_t key(std::span<const std::uint8_t> source) noexcept;

private:
    std::filesystem::path directory_;
};

}
