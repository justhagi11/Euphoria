#include <euphoria/audio/backend.hpp>

#include <euphoria/log/log.hpp>

namespace euphoria::audio {

bool Backend::initialize()
{
    log::warn("audio: host backend is not implemented");
    initialized_ = false;
    return false;
}

void Backend::shutdown() noexcept
{
    initialized_ = false;
}

}
