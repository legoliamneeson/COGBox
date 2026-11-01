#!/bin/sh
# CogBox: the library music's XMA decoder - a minimal FFmpeg (libavcodec + libavutil only, the xma1 / xma2 decoders
# and nothing else; LGPL 2.1, no GPL parts) cross-built for 64-bit Windows with MinGW. Output: $PREFIX/bin/avcodec-61.dll
# and avutil-59.dll (~1 MB together), $PREFIX/include for build.sh (its default FFMPEG_DIR). CogBox loads the DLLs at run time.
# LGPL: they ship as separate DLLs, unmodified FFmpeg 7.1.1 source (https://github.com/FFmpeg/FFmpeg, tag n7.1.1),
# configured with the line below; licenses/FFmpeg-LGPL-2.1.txt goes in the release with them.
# Usage: tools/build_ffmpeg_xma.sh <ffmpeg source folder> [prefix, default cogbox/third_party/ffmpeg-xma]
set -e
SRC=$(cd "$1" && pwd); PREFIX=${2:-$(cd "$(dirname "$0")/.." && pwd)/third_party/ffmpeg-xma}
B=$(mktemp -d); cd "$B"
"$SRC/configure" --prefix="$PREFIX" --target-os=mingw32 --arch=x86_64 --cross-prefix=x86_64-w64-mingw32- \
  --enable-shared --disable-static --disable-everything --disable-programs --disable-doc --disable-avdevice \
  --disable-avformat --disable-swresample --disable-swscale --disable-postproc --disable-avfilter --disable-network \
  --disable-autodetect --disable-iconv --enable-decoder=xma1,xma2 --disable-debug --enable-small
make -j"$(nproc)" && make install
ls -la "$PREFIX/bin"
