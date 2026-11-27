#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace euphoria::vfs {

class FileSystem {
public:
    bool mount(const std::filesystem::path& container);
    std::optional<std::vector<std::byte>> read(std::string_view path) const;
    std::vector<std::string> list(std::string_view path) const;
    const std::filesystem::path& root() const noexcept;

private:
    std::filesystem::path root_;
};

}
