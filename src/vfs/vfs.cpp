#include <euphoria/vfs/vfs.hpp>

#include <euphoria/log/log.hpp>

#include <fstream>
#include <system_error>

namespace euphoria::vfs {

bool FileSystem::mount(const std::filesystem::path& container)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(container, ec)) {
        log::warn("vfs: container mounting is not implemented: " + container.string());
        return false;
    }

    root_ = container;
    log::info("vfs: mounted directory " + root_.string());
    return true;
}

std::optional<std::vector<std::byte>> FileSystem::read(std::string_view path) const
{
    if (root_.empty()) {
        log::warn("vfs: read before mount");
        return std::nullopt;
    }

    const std::filesystem::path full = root_ / std::filesystem::path{path};
    std::ifstream input(full, std::ios::binary | std::ios::ate);
    if (!input) {
        log::warn("vfs: cannot open " + full.string());
        return std::nullopt;
    }

    const std::streamoff size = input.tellg();
    if (size < 0) {
        return std::nullopt;
    }

    std::vector<std::byte> data(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(data.data()), size);
    if (!input) {
        log::warn("vfs: short read " + full.string());
        return std::nullopt;
    }
    return data;
}

std::vector<std::string> FileSystem::list(std::string_view path) const
{
    std::vector<std::string> entries;
    if (root_.empty()) {
        log::warn("vfs: list before mount");
        return entries;
    }

    std::error_code ec;
    const std::filesystem::path full = root_ / std::filesystem::path{path};
    for (const auto& entry : std::filesystem::directory_iterator(full, ec)) {
        entries.push_back(entry.path().filename().string());
    }
    return entries;
}

const std::filesystem::path& FileSystem::root() const noexcept
{
    return root_;
}

}
