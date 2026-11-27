#include "openscarlett/core/status.h"

namespace openscarlett {

const char* StatusCodeName(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok: return "Ok";
        case StatusCode::InvalidArgument: return "InvalidArgument";
        case StatusCode::NotFound: return "NotFound";
        case StatusCode::ParseError: return "ParseError";
        case StatusCode::UnsupportedFormat: return "UnsupportedFormat";
        case StatusCode::OutOfMemory: return "OutOfMemory";
        case StatusCode::DeviceError: return "DeviceError";
        case StatusCode::ShaderError: return "ShaderError";
        case StatusCode::RaytracingError: return "RaytracingError";
        case StatusCode::IoError: return "IoError";
        case StatusCode::NotImplemented: return "NotImplemented";
    }
    return "Unknown";
}

}  // namespace openscarlett
