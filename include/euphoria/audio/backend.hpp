#pragma once

namespace euphoria::audio {

class Backend {
public:
    bool initialize();
    void shutdown() noexcept;

private:
    bool initialized_{};
};

}
