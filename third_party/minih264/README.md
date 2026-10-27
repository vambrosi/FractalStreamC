# minih264

The H.264 encoder used to write videos (`src/video.c`), from https://github.com/lieff/minih264
(commit b0baea7a80ef9d12da97301dd1099b8791b5ba43, 2020-12-10), public domain (CC0, see LICENSE).

Changes (search for "FractalStreamC" in `minih264e.h`):

- The sequence parameter set says how the colors are coded (VUI with BT.709 colour primaries,
  transfer and matrix, video range), so that players don't guess.
- On arm64 the zigzag scan looks up its table with `vqtbl2_u8`, whatever the compiler. It called
  `vtbl2q_u8` on Apple's clang, an intrinsic that doesn't exist. The videos are byte for byte the
  same as the SSE2 code's (checked on Linux arm64, GCC and clang).
- The alignment asserts of two NEON functions cast pointers to `uintptr_t`, not `unsigned`
  (warnings on 64-bit ARM).
- On macOS the byte order comes from the compiler (`__BYTE_ORDER__`). It came from `BYTE_ORDER`
  and `BIG_ENDIAN`, which nothing defined there on Apple silicon, so the encoder took the Mac for
  a big-endian machine and wrote frames that the MP4 writer rejected.
