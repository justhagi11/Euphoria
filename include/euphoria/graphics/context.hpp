#pragma once

namespace euphoria::graphics {

class Context {
public:
    bool initialize(void* native_window);
    void present();
    void shutdown() noexcept;

private:
    bool initialized_{};
};

}
