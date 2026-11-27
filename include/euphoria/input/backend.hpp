#pragma once

namespace euphoria::input {

class Backend {
public:
    bool initialize();
    void shutdown() noexcept;

private:
    bool initialized_{};
};

}
