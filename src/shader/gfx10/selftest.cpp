#include <euphoria/shader/gfx10/selftest.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>

namespace euphoria::shader::gfx10::selftest {
namespace {

struct Entry {
    std::string module;
    std::string name;
    Case test;
};

std::vector<Entry>& registry()
{
    static std::vector<Entry> entries;
    return entries;
}

std::mutex& registry_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::string where(const std::source_location& location)
{
    return std::format("{}:{}", std::filesystem::path{location.file_name()}.filename().string(), location.line());
}

}

bool Context::expect(bool condition, std::string_view what, std::source_location location)
{
    if (!condition) {
        ++failures_;
        log_ += std::format("    {}: {}\n", where(location), what);
    }
    return condition;
}

bool Context::expect_eq(std::string_view actual, std::string_view expected, std::source_location location)
{
    if (actual != expected) {
        ++failures_;
        log_ += std::format("    {}: got \"{}\", expected \"{}\"\n", where(location), actual, expected);
        return false;
    }
    return true;
}

void register_case(std::string_view module, std::string_view name, Case test)
{
    std::scoped_lock lock(registry_mutex());
    auto& entries = registry();
    const bool known = std::ranges::any_of(entries, [&](const Entry& entry) {
        return entry.module == module && entry.name == name;
    });
    if (!known) {
        entries.push_back({std::string{module}, std::string{name}, test});
    }
}

Summary run(std::string_view module, std::string& report)
{
    std::vector<Entry> entries;
    {
        std::scoped_lock lock(registry_mutex());
        entries = registry();
    }
    Summary summary;
    for (const Entry& entry : entries) {
        if (!module.empty() && entry.module != module) {
            continue;
        }
        Context context;
        try {
            entry.test(context);
        } catch (const std::exception& error) {
            context.expect(false, std::format("exception: {}", error.what()));
        } catch (...) {
            context.expect(false, "unknown exception");
        }
        const bool ok = context.failures() == 0;
        report += std::format("{} {}::{}\n", ok ? "pass" : "FAIL", entry.module, entry.name);
        report += context.log();
        if (ok) {
            ++summary.passed;
        } else {
            ++summary.failed;
        }
    }
    return summary;
}

std::vector<std::string> modules()
{
    std::scoped_lock lock(registry_mutex());
    std::vector<std::string> names;
    for (const Entry& entry : registry()) {
        if (std::ranges::find(names, entry.module) == names.end()) {
            names.push_back(entry.module);
        }
    }
    return names;
}

std::vector<std::uint8_t> bytes(std::span<const std::uint32_t> words)
{
    std::vector<std::uint8_t> out;
    out.reserve(words.size() * 4);
    for (const std::uint32_t word : words) {
        out.push_back(static_cast<std::uint8_t>(word));
        out.push_back(static_cast<std::uint8_t>(word >> 8));
        out.push_back(static_cast<std::uint8_t>(word >> 16));
        out.push_back(static_cast<std::uint8_t>(word >> 24));
    }
    return out;
}

}
