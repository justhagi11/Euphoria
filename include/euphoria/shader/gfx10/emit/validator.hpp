#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace euphoria::shader::gfx10::emit {

class Validator {
public:
    Validator() = default;
    Validator(const Validator&) = delete;
    Validator& operator=(const Validator&) = delete;
    Validator(Validator&& other) noexcept;
    Validator& operator=(Validator&& other) noexcept;
    ~Validator();

    static std::optional<std::filesystem::path> default_path();
    static std::expected<Validator, std::string> load(const std::optional<std::filesystem::path>& path);

    std::expected<void, std::string> validate(std::span<std::uint8_t> container);

private:
    void* object_{nullptr};
};

}
