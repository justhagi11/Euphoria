#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace euphoria::loader {

struct Section {
    std::string name;
    std::uint32_t virtual_address{};
    std::uint32_t virtual_size{};
    std::uint32_t raw_offset{};
    std::uint32_t raw_size{};
    std::uint32_t characteristics{};
};

struct Import {
    std::string module;
    std::string name;
    std::uint32_t ordinal{};
    std::uint32_t iat_rva{};
    bool by_ordinal{};
};

struct Export {
    std::string name;
    std::uint32_t ordinal{};
    std::uint32_t rva{};
};

struct Relocation {
    std::uint32_t rva{};
    std::uint32_t file_offset{};
    std::uint16_t type{};
};

struct TlsDirectory {
    std::uint64_t start_va{};
    std::uint64_t end_va{};
    std::uint64_t index_va{};
    std::uint64_t callbacks_va{};
};

class Image {
public:
    Image() = default;
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();

    static std::optional<Image> load_file(const std::filesystem::path& path);
    static bool is_pe64(std::span<const std::uint8_t> data) noexcept;

    bool load_as_module(const std::filesystem::path& cache_directory);
    bool redirect_tls();
    void unload_module() noexcept;
    void abandon() noexcept { base_ = nullptr; }

    std::uint8_t* base() const noexcept { return base_; }
    std::uint8_t* at(std::uint32_t rva) const noexcept;
    std::uintptr_t entry_point() const noexcept;
    std::uintptr_t tls_callbacks() const noexcept;
    std::uintptr_t tls_callbacks_original() const noexcept;
    std::uint32_t tls_index() const noexcept;
    std::uintptr_t find_export(std::string_view name) const noexcept;
    std::uintptr_t preferred_base() const noexcept { return preferred_base_; }
    std::uint32_t entry_rva() const noexcept { return entry_rva_; }
    std::uint32_t size_of_image() const noexcept { return size_of_image_; }
    std::uint32_t headers_size() const noexcept { return headers_size_; }
    std::uint32_t stack_reserve() const noexcept { return stack_reserve_; }
    bool is_loaded() const noexcept { return base_ != nullptr; }

    const std::vector<Section>& sections() const noexcept { return sections_; }
    const std::vector<Import>& imports() const noexcept { return imports_; }
    const std::vector<Export>& exports() const noexcept { return exports_; }
    const std::vector<Relocation>& relocations() const noexcept { return relocations_; }
    const TlsDirectory& tls() const noexcept { return tls_; }
    const std::filesystem::path& module_path() const noexcept { return module_path_; }
    bool has_tls() const noexcept { return tls_directory_rva_ != 0; }

private:
    std::vector<std::uint8_t> bytes_;
    std::vector<Section> sections_;
    std::vector<Import> imports_;
    std::vector<Export> exports_;
    std::vector<Relocation> relocations_;
    TlsDirectory tls_{};
    std::uint8_t* base_{};
    std::uintptr_t preferred_base_{};
    std::uint32_t entry_rva_{};
    std::uint32_t size_of_image_{};
    std::uint32_t headers_size_{};
    std::uint32_t stack_reserve_{};
    std::uint32_t import_terminator_rva_{};
    std::uint32_t tls_directory_rva_{};
    std::uint32_t tls_index_rva_{};
    std::filesystem::path module_path_;
};

}
