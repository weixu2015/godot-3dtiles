# Vendored Basis Universal transcoder + Zstandard

**Do not edit anything under `transcoder/` or `zstd/`.** It is upstream source.

## What is here

| Path | Origin |
|---|---|
| `transcoder/*` | `basis_universal/transcoder`, tag `v1_60` (the version vcpkg's `basisu` port 2.1.0 pins) |
| `zstd/zstd.c`, `zstd/zstd.h`, `zstd/zstd_errors.h` | Zstandard 1.5.7, official single-file amalgamation |
| `LICENSE` | basisu's Apache-2.0 license (plus its bundled third-party notices) |
| `zstd/LICENSE` | zstd's BSD-3-Clause |

`transcoder/` was copied from the vcpkg build tree at
`D:\vcpkg\buildtrees\basisu\src\v1_60-*\transcoder`. That tree already carries vcpkg's
`devendor-zstd.diff`, which is why `basisu_transcoder.cpp` includes `"zstd.h"` (angles)
rather than `"../zstd/zstd.h"`.

`zstd/` was generated with zstd's own amalgamation tool so the whole decoder is two
files and needs no C toolchain gymnastics:

```sh
cd <zstd-1.5.7>/build/single_file_libs
python combine.py -r ../../lib -x legacy/zstd_legacy.h zstd-in.c -o zstd.c
# then copy zstd.h and zstd_errors.h next to it (zstd.h includes zstd_errors.h)
```

## Why not just fetch it

`extern/CMakeLists.txt` deliberately keeps every third-party dependency buildable
offline: the machine this project is built on may have no network. Draco is vendored
for the same reason.

## Why only the transcoder

The encoder half (`basisu_encoder`, `encoder/`) is not needed to *read* tiles and pulls
in tinyexr, lodepng and optionally OpenCL. Only `transcoder/` is vendored.

## Wiring

`extern/third_party/CMakeLists.txt` builds the `tiles3d_basisu` static target:

- `zstd.c` is compiled as C++ (`LANGUAGE CXX`). The project never calls
  `enable_language(C)`, so adding a `.c` source without it fails at generate time with
  `Error required internal CMake variable not set: CMAKE_C_COMPILE_OBJECT`.
- `/bigobj` is required: the 24k-line transcoder exceeds the COFF section limit (C1128).
- A handful of warning codes are suppressed for this target only. The vendored tree is
  not ours to fix, and it is not built with `/WX`.

## Runtime contract

`base::ensureTranscoderInitialized()` in `src/core/content/Ktx2Decoder.cpp` calls
`basist::basisu_transcoder_init()` exactly once per process through `std::call_once`.
Skipping it aborts on `assert(g_transcoder_initialized)` inside the transcoder; calling
it twice logs an error. Both matter because the call site is a worker-thread decode path.

## Updating

Re-copy `transcoder/` from a newer vcpkg build tree, regenerate the zstd amalgamation,
and re-run `tests/test_ktx2_decoder.cpp`. The real-file case in that test reads an
actual 1.1 Photogrammetry tile and asserts non-uniform output, so a decoder that
silently produces a flat buffer fails the suite.
