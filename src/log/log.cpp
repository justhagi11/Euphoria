#include <euphoria/log/log.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <windows.h>

namespace euphoria::log {
namespace {

std::recursive_mutex g_mutex;
std::ofstream g_file;
bool g_file_ready = false;
bool g_verbose = false;
int g_write_depth = 0;
HANDLE g_console = INVALID_HANDLE_VALUE;
bool g_console_ready = false;

struct WritingGuard {
    WritingGuard() { ++g_write_depth; }
    ~WritingGuard() { --g_write_depth; }
};

std::string_view label(Level level) noexcept
{
    switch (level) {
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warn:
        return "warn";
    case Level::Error:
        return "error";
    }
    return "info";
}

WORD colour(Level level) noexcept
{
    switch (level) {
    case Level::Debug:
        return FOREGROUND_INTENSITY;
    case Level::Info:
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    case Level::Warn:
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case Level::Error:
        return FOREGROUND_RED | FOREGROUND_INTENSITY;
    }
    return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
}

void ensure_file()
{
    if (g_file_ready) {
        return;
    }
    g_file_ready = true;

    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        return;
    }
    std::filesystem::path path{self};
    path.replace_filename(L"euphoria.log");
    g_file.open(path, std::ios::out | std::ios::trunc);
}

void ensure_console()
{
    if (g_console_ready) {
        return;
    }
    g_console_ready = true;
    if (GetConsoleWindow() == nullptr) {
        return;
    }
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_console == INVALID_HANDLE_VALUE || g_console == nullptr) {
        g_console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    }
}

void timestamp(std::string& out)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%02u:%02u:%02u.%03u ", time.wHour, time.wMinute, time.wSecond,
                  time.wMilliseconds);
    out += text;
}

}

void set_verbose(bool enabled) noexcept
{
    std::scoped_lock lock(g_mutex);
    g_verbose = enabled;
    g_file_ready = false;
}

bool verbose() noexcept
{
    std::scoped_lock lock(g_mutex);
    return g_verbose;
}

bool in_write() noexcept
{
    return g_write_depth > 0;
}

void write(Level level, std::string_view message)
{
    std::scoped_lock lock(g_mutex);
    if (g_write_depth > 0) {
        return;
    }
    if (level == Level::Debug && !g_verbose) {
        return;
    }
    WritingGuard guard;

    ensure_file();
    ensure_console();

    std::string line;
    line.reserve(message.size() + 40);
    timestamp(line);
    line += label(level);
    line += ": ";
    line += message;
    line += '\n';

    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }

    if (g_console != INVALID_HANDLE_VALUE && g_console != nullptr) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        const bool has_info = GetConsoleScreenBufferInfo(g_console, &info) != 0;
        const WORD previous = has_info ? info.wAttributes : 0;
        SetConsoleTextAttribute(g_console, colour(level));
        DWORD written = 0;
        WriteConsoleA(g_console, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        if (has_info) {
            SetConsoleTextAttribute(g_console, previous);
        }
    }

    OutputDebugStringA(line.c_str());
}

}
