# minih264

The H.264 encoder used to write videos (`src/video.c`), from https://github.com/lieff/minih264
(commit b0baea7a80ef9d12da97301dd1099b8791b5ba43, 2020-12-10), public domain (CC0, see LICENSE).

One change: the sequence parameter set says how the colors are coded (VUI with BT.709 colour primaries,
transfer and matrix, video range), so that players don't guess. Search for "FractalStreamC" in `minih264e.h`.
