#include <euphoria/shader/dxc.hpp>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <system_error>

#include <windows.h>

namespace euphoria::shader {
namespace {

std::atomic<unsigned> g_counter{0};

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::nullopt;
    }
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

std::string read_text(const std::filesystem::path& path)
{
    const auto data = read_file(path);
    return data ? std::string(data->begin(), data->end()) : std::string{};
}

}

std::filesystem::path find_dxc()
{
    if (const DWORD length = GetEnvironmentVariableW(L"EUPHORIA_DXC", nullptr, 0); length > 0) {
        std::wstring value(length, L'\0');
        GetEnvironmentVariableW(L"EUPHORIA_DXC", value.data(), length);
        if (!value.empty() && value.back() == L'\0') {
            value.pop_back();
        }
        if (std::filesystem::is_regular_file(value)) {
            return value;
        }
    }

    std::error_code error;
    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) != 0) {
        const std::filesystem::path local =
            std::filesystem::path{self}.parent_path() / L"dxc" / L"dxc.exe";
        if (std::filesystem::is_regular_file(local, error)) {
            return local;
        }
    }

    wchar_t local_app_data[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH) > 0) {
        const std::filesystem::path installed =
            std::filesystem::path{local_app_data} / L"Euphoria" / L"dxc" / L"bin" / L"x64" / L"dxc.exe";
        if (std::filesystem::is_regular_file(installed, error)) {
            return installed;
        }
    }

    std::vector<std::filesystem::path> roots{
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin",
        L"C:\\Program Files\\Windows Kits\\10\\bin",
    };
    for (const auto& root : roots) {
        if (!std::filesystem::is_directory(root, error)) {
            continue;
        }
        std::filesystem::path best;
        for (const auto& version : std::filesystem::directory_iterator(root, error)) {
            const std::filesystem::path candidate = version.path() / L"x64" / L"dxc.exe";
            if (std::filesystem::is_regular_file(candidate, error) && candidate > best) {
                best = candidate;
            }
        }
        if (!best.empty()) {
            return best;
        }
    }

    const std::filesystem::path vulkan = L"C:\\VulkanSDK\\1.4.363.0\\Bin\\dxc.exe";
    if (std::filesystem::is_regular_file(vulkan, error)) {
        return vulkan;
    }
    return {};
}

DxcResult compile_hlsl(const std::string& hlsl, std::uint8_t stage,
                       const std::filesystem::path& work_directory)
{
    DxcResult result;
    const std::filesystem::path dxc = find_dxc();
    if (dxc.empty()) {
        result.diagnostics = "dxc.exe not found";
        return result;
    }

    std::error_code error;
    std::filesystem::create_directories(work_directory, error);
    const unsigned id = g_counter.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path input = work_directory / ("lift" + std::to_string(id) + ".hlsl");
    const std::filesystem::path output = work_directory / ("lift" + std::to_string(id) + ".dxil");
    const std::filesystem::path log = work_directory / ("lift" + std::to_string(id) + ".log");

    {
        std::ofstream file(input, std::ios::binary | std::ios::trunc);
        if (!file) {
            result.diagnostics = "cannot write HLSL";
            return result;
        }
        file.write(hlsl.data(), static_cast<std::streamsize>(hlsl.size()));
    }

    const wchar_t* profile = stage == 4 ? L"vs_6_6"
                          : stage == 5 ? L"ps_6_6"
                          : stage == 6 ? L"ms_6_6"
                                       : L"cs_6_6";
    std::wstring command = L"\"" + dxc.wstring() + L"\" -T " + profile + L" -E main -Fo \""
                         + output.wstring() + L"\" \"" + input.wstring() + L"\"";

    BOOL started = 0;
    DWORD exit_code = 1;
    for (int attempt = 0; attempt < 4; ++attempt) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE log_handle = INVALID_HANDLE_VALUE;
        for (int open_attempt = 0; open_attempt < 8 && log_handle == INVALID_HANDLE_VALUE;
             ++open_attempt) {
            log_handle = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log_handle == INVALID_HANDLE_VALUE) {
                Sleep(20);
            }
        }
        if (log_handle == INVALID_HANDLE_VALUE) {
            STARTUPINFOW fallback_startup{};
            fallback_startup.cb = sizeof(fallback_startup);
            PROCESS_INFORMATION fallback_process{};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, work_directory.wstring().c_str(), &fallback_startup,
                               &fallback_process)
                == 0) {
                result.diagnostics = "cannot start dxc";
                return result;
            }
            WaitForSingleObject(fallback_process.hProcess, INFINITE);
            DWORD fallback_code = 1;
            GetExitCodeProcess(fallback_process.hProcess, &fallback_code);
            CloseHandle(fallback_process.hThread);
            CloseHandle(fallback_process.hProcess);
            if (fallback_code != 0) {
                result.diagnostics = "dxc failed (no diagnostics)";
                return result;
            }
            const auto dxil = read_file(output);
            if (!dxil) {
                result.diagnostics = "dxc produced no output";
                return result;
            }
            result.ok = true;
            result.dxil = *dxil;
            return result;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = log_handle;
        startup.hStdError = log_handle;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};

        started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                 nullptr, work_directory.wstring().c_str(), &startup, &process);
        if (started == 0) {
            CloseHandle(log_handle);
            result.diagnostics = "cannot start dxc";
            return result;
        }

        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(log_handle);

        if (exit_code == 0) {
            break;
        }
        const std::string diagnostics = read_text(log);
        if (diagnostics.find("being used by another process") == std::string::npos) {
            result.diagnostics = diagnostics;
            return result;
        }
        Sleep(40);
    }

    if (exit_code != 0) {
        result.diagnostics = read_text(log);
        return result;
    }

    const auto dxil = read_file(output);
    if (!dxil) {
        result.diagnostics = "dxc produced no output";
        return result;
    }
    result.ok = true;
    result.dxil = *dxil;
    return result;
}

}
