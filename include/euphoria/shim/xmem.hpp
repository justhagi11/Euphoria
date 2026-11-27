#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace euphoria::shim::xmem {

bool guest_virtual_alloc(void* address, std::size_t size, std::uint32_t type, std::uint32_t protect,
                         void*& result);
bool guest_virtual_free(void* address, std::size_t size, std::uint32_t type, int& result);

int self_test(std::string& report);

}
