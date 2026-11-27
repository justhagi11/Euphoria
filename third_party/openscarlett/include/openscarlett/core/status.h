#pragma once

#include <cstdint>
#include <string>

namespace openscarlett {

enum class StatusCode : int32_t {
    Ok = 0,
    InvalidArgument = -1,
    NotFound = -2,
    ParseError = -3,
    UnsupportedFormat = -4,
    OutOfMemory = -5,
    DeviceError = -6,
    ShaderError = -7,
    RaytracingError = -8,
    IoError = -9,
    NotImplemented = -10,
};

struct Status {
    StatusCode code = StatusCode::Ok;
    std::string message;

    bool ok() const noexcept { return code == StatusCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }

    static Status Ok() { return {StatusCode::Ok, {}}; }
    static Status Error(StatusCode c, std::string msg) { return {c, std::move(msg)}; }
};

const char* StatusCodeName(StatusCode code) noexcept;

}  // namespace openscarlett
