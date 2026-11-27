#include <euphoria/shader/cache.hpp>

#include <euphoria/shader/codec.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <windows.h>

namespace euphoria::shader {
namespace {

std::string file_name(std::uint64_t key)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "%016llx.esc", static_cast<unsigned long long>(key));
    return text;
}

}

Cache::Cache(std::filesystem::path directory)
    : directory_(std::move(directory))
{
}

std::optional<std::vector<std::uint8_t>> Cache::load(std::uint64_t key) const
{
    std::ifstream input(directory_ / file_name(key), std::ios::binary | std::ios::ate);
    if (!input) {
        return std::nullopt;
    }

    const std::streamoff size = input.tellg();
    if (size <= 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> stored(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(stored.data()), size);
    if (!input) {
        return std::nullopt;
    }

    return decode(stored);
}

bool Cache::store(std::uint64_t key, std::span<const std::uint8_t> payload) const
{
    std::error_code error;
    std::filesystem::create_directories(directory_, error);

    const std::vector<std::uint8_t> stored = encode(payload);
    const std::filesystem::path final_path = directory_ / file_name(key);
    const std::filesystem::path temp_path = directory_ / (file_name(key) + ".tmp");

    {
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output.write(reinterpret_cast<const char*>(stored.data()),
                     static_cast<std::streamsize>(stored.size()));
        output.close();
        if (!output) {
            return false;
        }
    }

    return MoveFileExW(temp_path.wstring().c_str(), final_path.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
        != 0;
}

std::uint64_t Cache::key(std::span<const std::uint8_t> source) noexcept
{
    return hash64(source);
}

}
