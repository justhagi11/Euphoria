target_sources(euphoria_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/opcodes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/fields.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/sp3.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/ir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/selftest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/decode_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/opcodes_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/sp3_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/ir_test.cpp
)
