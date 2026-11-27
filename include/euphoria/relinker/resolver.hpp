#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace euphoria::relinker {

struct ResolutionStats {
    std::size_t host{};
    std::size_t shim{};
    std::size_t stub{};
    std::size_t failed{};
};

class Resolver {
public:
    Resolver();
    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;
    ~Resolver();

    void* resolve(std::string_view module, std::string_view name, std::uint32_t ordinal, bool by_ordinal);
    void add_title_module(std::string_view module, void* handle);
    const ResolutionStats& stats() const noexcept { return stats_; }
    std::size_t host_module_count() const noexcept { return modules_.size(); }

private:
    void* resolve_host(std::string_view module, const std::string& name, std::uint32_t ordinal, bool by_ordinal);
    void* resolve_shim(std::string_view module, std::string_view name, std::uint32_t ordinal);
    void* make_stub(std::string_view module, std::string_view name);
    static bool is_guest_module(std::string_view module);
    static std::uintptr_t stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

    std::unordered_map<std::string, void*> modules_;
    std::unordered_map<std::string, void*> title_modules_;
    ResolutionStats stats_{};
};

}
