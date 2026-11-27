#include <euphoria/input/backend.hpp>

#include <euphoria/log/log.hpp>

namespace euphoria::input {

bool Backend::initialize()
{
    log::warn("input: host backend is not implemented");
    initialized_ = false;
    return false;
}

void Backend::shutdown() noexcept
{
    initialized_ = false;
}

}
