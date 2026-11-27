#include <euphoria/graphics/context.hpp>

#include <euphoria/log/log.hpp>

namespace euphoria::graphics {

bool Context::initialize(void* native_window)
{
    (void)native_window;
    log::warn("graphics: host device is not implemented");
    initialized_ = false;
    return false;
}

void Context::present()
{
    if (!initialized_) {
        return;
    }
}

void Context::shutdown() noexcept
{
    initialized_ = false;
}

}
