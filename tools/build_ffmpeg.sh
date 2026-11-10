#!/usr/bin/env bash
# tools/build_ffmpeg.sh - builds the small FFmpeg the package ships: FFmpeg 8.1.1 (LGPL 2.1 or later, no external
# libraries) with only what the port uses, into downloads/ffmpeg-8.1.1-min (bin\: the DLLs and ffmpeg.exe;
# lib\pkgconfig: for building the runtime against it). MSYS2's own FFmpeg pulls in about 130 MB of codec libraries
# (x265, AV1, JPEG XL, SVG ...) that nothing here uses.
#
#   the runtime (gow3_pc)   decodes H.264 and HEVC (the game's video decoder and its movie player, with MP4/MOV,
#                           AAC and MP3 files), MP3 (sound) and PNG; it converts with swscale and swresample
#   the U3 multiplayer      ffmpeg.exe: JPEG -> Lanczos-scaled RGB (EULA pages, make_eula.py) and MP3 -> 16-bit PCM
#   import (u3mp/bin)       (sound banks, convert_banks.py); both pinned to 8.1.1 so their output stays the same
#
# Run from an MSYS2 CLANG64 shell (needs make and nasm); a finished build is kept. Prints the pkg-config folder.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
version=8.1.1
sha256=b6863adde98898f42602017462871b5f6333e65aec803fdd7a6308639c52edf3   # as MSYS2's recipe for 8.1.1
# Windows form (C:/...): the .pc files carry it, and CLANG64's pkg-config and clang are Windows programs
prefix=$(cygpath -m "$PWD/downloads/ffmpeg-$version-min")
if [[ -f $prefix/bin/ffmpeg.exe && -f $prefix/lib/pkgconfig/libavcodec.pc ]] && grep -q "^prefix=$prefix\$" "$prefix/lib/pkgconfig/libavcodec.pc"; then
    echo "$prefix/lib/pkgconfig"
    exit 0
fi
msys=${MSYS2_ROOT:-/c/msys64}
msys=${msys%/}
[[ -d $msys/clang64/bin ]] && export PATH="$msys/clang64/bin:$msys/usr/bin:$PATH"

source=downloads/ffmpeg-$version.tar.xz
if [[ ! -s $source ]]; then
    mkdir -p downloads
    curl -fsSL --retry 3 -o "$source.part" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
    mv -- "$source.part" "$source"
fi
echo "$sha256 *$source" | sha256sum -c --quiet - >&2
work=downloads/ffmpeg-$version-build
rm -rf -- "$work"
mkdir -p "$work"
tar -C "$work" --strip-components=1 -xJf "$source"

cd "$work"
# configure's scratch files, in a POSIX path (a Windows TEMP like C:\Users\... loses its backslashes there)
mkdir -p tmp
export TMPDIR=$PWD/tmp
TMP=$(cygpath -w "$PWD/tmp")   # and windres', in Windows form
export TMP TEMP=$TMP
./configure --prefix="$prefix" --target-os=mingw32 --arch=x86_64 --cc=clang --cxx=clang++ --windres=false \
    --enable-shared --disable-static --disable-debug --disable-doc --disable-autodetect --disable-network \
    --enable-w32threads --enable-zlib --disable-everything --disable-programs --enable-ffmpeg --disable-avdevice \
    --enable-decoder=h264,hevc,aac,aac_latm,mp3,mp3float,png,mjpeg \
    --enable-parser=h264,hevc,aac,aac_latm,mpegaudio,png,mjpeg \
    --enable-demuxer=mov,mp3,aac,h264,hevc,wav,image_jpeg_pipe,image_png_pipe \
    --enable-muxer=rawvideo,pcm_s16le,wav \
    --enable-encoder=rawvideo,pcm_s16le \
    --enable-filter=scale,format,aresample,aformat,null,anull,buffer,buffersink,abuffer,abuffersink \
    --enable-protocol=file,pipe,fd > configure.log 2>&1 || { tail -30 configure.log >&2; exit 1; }
make -j"$(nproc)" > make.log 2>&1 || { tail -40 make.log >&2; exit 1; }
make install > /dev/null
cp COPYING.LGPLv2.1 "$prefix/COPYING.LGPLv2.1"
echo "$prefix/lib/pkgconfig"
