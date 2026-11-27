# DirectXShaderCompiler

Bundled release: **v1.9.2609** (`dxc_2026_09_29`), x64 binaries.

Source: https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609

Contents: `dxc.exe`, `dxcompiler.dll`, `dxil.dll`, `dxv.exe`, headers and x64
import libraries.

Licences: `LICENCE-MIT.txt`, `LICENSE-LLVM.txt`, `LICENSE-MS.txt`.

Euphoria copies `bin/x64` next to its binaries at build time and prefers that
copy at runtime. Set `EUPHORIA_DXC` to override the compiler path.
