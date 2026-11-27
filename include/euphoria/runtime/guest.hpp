#pragma once

#include <euphoria/loader/pe.hpp>
#include <euphoria/relinker/resolver.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace euphoria::runtime {

struct GuestConfig {
    std::filesystem::path title;
    std::filesystem::path cache_directory;
};

class Guest {
public:
    bool initialize(const GuestConfig& config);
    int run();
    void shutdown() noexcept;

    const loader::Image& image() const noexcept { return image_; }
    const relinker::Resolver& resolver() const noexcept { return resolver_; }

private:
    bool load_title_dlls();
    void attach_title_dlls();

    GuestConfig config_{};
    loader::Image image_;
    std::vector<std::unique_ptr<loader::Image>> title_dlls_;
    std::vector<std::string> title_dll_names_;
    relinker::Resolver resolver_;
    bool initialized_{};
};

}
