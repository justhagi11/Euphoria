# OpenScarlett

This directory contains a minimal subset of
https://github.com/californiaAlmonds/OpenScarlett used by Euphoria's shader
tooling.

Files included:

- `include/openscarlett/core/status.h`, `src/core/status.cpp`
- `include/openscarlett/gpu/shader_ir.h`
- `include/openscarlett/gpu/rdna2_disasm.h`, `src/gpu/rdna2_disasm.cpp`
- `include/openscarlett/io/zlib_rfc1950.h`
- `include/openscarlett/gpu/rdna2_xbox_container.h` (reduced)

Upstream licence: MIT, copyright (c) 2026 OpenScarlett Contributors. The full
licence text is in `LICENSE`.

Euphoria changes:

- the reduced container header;
- `rdna2_disasm.cpp` is made tolerant: unknown opcodes and encodings produce
  an `unknown` instruction instead of aborting the stream, and SOPK `0x15`
  consumes its 32-bit immediate.

The decoder is wrapped by Euphoria's own container parser, AOT driver, ESC1
cache and HLSL/DXIL lifter.
