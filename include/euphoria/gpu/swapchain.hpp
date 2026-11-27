#pragma once

#include <cstdint>

struct IDXGISwapChain;
struct IDXGISwapChain1;
struct IDXGISwapChain2;
struct IDXGISwapChain3;
struct IDXGISwapChain4;
struct IUnknown;

namespace euphoria::gpu {

inline constexpr std::uint32_t console_swap_chain_first_slot = 41;

struct SwapChainProxy {
    void** vtable;
    IDXGISwapChain* host;
    IUnknown* device;
    IDXGISwapChain1* host1;
    IDXGISwapChain2* host2;
    IDXGISwapChain3* host3;
    IDXGISwapChain4* host4;
    std::uint64_t frame;
    std::uint32_t max_slot;
};

void** swap_chain_vtable() noexcept;
bool is_swap_chain_proxy(const void* object) noexcept;
SwapChainProxy* swap_chain_proxy(void* object) noexcept;
const SwapChainProxy* swap_chain_proxy(const void* object) noexcept;
IDXGISwapChain* swap_chain_host(void* object) noexcept;
const IDXGISwapChain* swap_chain_host(const void* object) noexcept;
SwapChainProxy* create_swap_chain_proxy(IDXGISwapChain* host, IUnknown* device) noexcept;
SwapChainProxy* wrap_swap_chain(IDXGISwapChain* host, IUnknown* device) noexcept;

}
