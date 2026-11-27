#include <euphoria/host/host.hpp>
#include <euphoria/log/log.hpp>

#include <exception>
#include <string>

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try {
        return euphoria::host::run();
    } catch (const std::exception& error) {
        euphoria::log::error(std::string("host: fatal exception: ") + error.what());
        return 1;
    } catch (...) {
        euphoria::log::error("host: fatal unknown exception");
        return 1;
    }
}
