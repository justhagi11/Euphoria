#pragma once

#include <cstdint>
#include <string>

namespace euphoria::shim::dstorage {

std::int32_t get_factory(const void* iid, void** factory);

int self_test(std::string& report);

}
