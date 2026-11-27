#pragma once

#include <string_view>

namespace euphoria::log {

enum class Level {
    Debug,
    Info,
    Warn,
    Error,
};

void set_verbose(bool enabled) noexcept;
bool verbose() noexcept;
bool in_write() noexcept;
void write(Level level, std::string_view message);

inline void debug(std::string_view message) { write(Level::Debug, message); }
inline void info(std::string_view message) { write(Level::Info, message); }
inline void warn(std::string_view message) { write(Level::Warn, message); }
inline void error(std::string_view message) { write(Level::Error, message); }

}
