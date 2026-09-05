#!/usr/bin/env bash
#
# Build a minimal audio-only FFmpeg for TinyPod.
#
# TinyPod decodes AAC and MP3 in-process with the Helix fixed-point decoders,
# which is enough for a library synced by iTunes and not enough for a library
# that is anything else. This builds libavcodec/libavformat/libavutil trimmed
# to audio, statically, so "plays anything" is a link-time decision rather than
# a second application.
#
# The build is deliberately subtractive: everything is disabled, then the audio
# pieces are switched back on. FFmpeg's default configuration brings in every
# video decoder, every protocol, x86 assembly and a filter graph, and on a
# device with 55 MiB of RAM the useful question is not "what can we remove"
# but "what do we actually need".
#
# Hardware floating point IS used. The N31 is a Cortex-A8 with VFPv3-D16, which
# every other app in this project already targets; only TinyPod was building
# without it, so its float code was going through soft-float library calls.
# -mfloat-abi=softfp is the ABI, not the arithmetic: FP instructions are
# generated, arguments are passed in integer registers. That is the right
# choice for an eabi (not eabihf) toolchain.
#
#   ./tools/fetch-ffmpeg.sh                 host build, for testing
#   CROSS=arm-linux-musleabi- ./tools/fetch-ffmpeg.sh    N31 build
#
# Output: third_party/ffmpeg-build/{host,n31}/{include,lib}
set -euo pipefail

VER="${FFMPEG_VERSION:-8.1.2}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TP="$HERE/third_party"
SRC="$TP/ffmpeg-$VER"
CROSS="${CROSS:-}"

# The core, the FPU and the ABI, in one place.
#
# These said cortex-a8 and vfpv3-d16, which the Cortex-A5 correction missed:
# /proc/cpuinfo reports CPU part 0xc05 with vfpv4 and vfpd32, so this was
# tuning for a dual-issue pipeline the part does not have and holding the
# compiler to half the double registers it does have.
#
# FLOAT_ABI is overridable for the hard-float tree. softfp is not soft float -
# the FPU does the arithmetic either way and only argument passing differs -
# but hard float needs the whole parallel toolchain, so it cannot simply be
# switched on here.
FLOAT_ABI="${FLOAT_ABI:-softfp}"
ARCH_CFLAGS="-mcpu=cortex-a5 -mfpu=vfpv4 -mfloat-abi=$FLOAT_ABI"

# The tag names the ABI as well as the target, because a hard-float build is a
# different library and not a newer one. Overridable so both can exist:
#
#   CROSS=<hf prefix> TAG=n31hf FLOAT_ABI=hard ./tools/fetch-<x>.sh
#
# Without that they share a directory and the second build silently becomes
# the one everything links against.
if [ -n "$CROSS" ]; then
	TAG="${TAG:-n31}"
else
	TAG="${TAG:-host}"
fi
OUT="$TP/ffmpeg-build/$TAG"

mkdir -p "$TP"

# ---- source ---------------------------------------------------------------

if [ ! -d "$SRC" ]; then
	TAR="$TP/ffmpeg-$VER.tar.xz"
	if [ ! -f "$TAR" ]; then
		echo "fetching ffmpeg $VER"
		curl -fL --retry 3 -o "$TAR.part" \
			"https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
		mv "$TAR.part" "$TAR"
	fi
	echo "unpacking"
	tar -C "$TP" -xf "$TAR"
fi

# ---- what we keep ---------------------------------------------------------
#
# Decoders: everything a music library realistically contains. Listed rather
# than taken wholesale, because --enable-decoder=* would bring in the video
# decoders and their tables, which is most of the size.
#
# Names are as ./configure --list-decoders reports them for this version,
# not as they read in prose: Musepack is mpc7/mpc8. A name FFmpeg does not
# know fails configure, which is the behaviour worth having - the
# alternative is a format quietly missing from the build.
DECODERS="aac,aac_latm,aac_fixed,mp3,mp3float,mp2,mp1,alac,flac,vorbis,opus"
DECODERS="$DECODERS,wmav1,wmav2,wmalossless,wmapro,ape,tta,wavpack,mpc7"
DECODERS="$DECODERS,mpc8,pcm_s16le,pcm_s16be,pcm_s24le,pcm_u8,pcm_f32le"
DECODERS="$DECODERS,adpcm_ima_qt,adpcm_ms,ac3,eac3,dca,als,atrac3,atrac3p"
DECODERS="$DECODERS,cook,gsm,amrnb,amrwb,speex,sipr,qdm2,truehd,mlp,shorten"

# Containers. The demuxer list is what actually determines whether a file
# opens, and it is cheap - a demuxer is parsing, not tables.
DEMUXERS="mov,mp3,flac,ogg,matroska,wav,aiff,ape,tta,wv,mpc,mpc8,asf,ac3"
DEMUXERS="$DEMUXERS,eac3,dts,aac,amr,caf,au,w64,xwma,dsf,iff,voc,rm"

PARSERS="aac,aac_latm,mpegaudio,flac,vorbis,opus,ac3,dca,mlp,cook"

COMMON=(
	--disable-everything
	--disable-programs --disable-doc --disable-avdevice --disable-swscale
	--disable-avfilter --disable-network
	--disable-autodetect --disable-iconv --disable-xlib --disable-sdl2
	--disable-zlib --disable-lzma --disable-bzlib --disable-schannel
	--disable-securetransport --disable-vaapi --disable-vdpau
	--disable-videotoolbox --disable-audiotoolbox --disable-encoders
	--disable-muxers --disable-bsfs --disable-devices --disable-filters
	--disable-protocols --enable-protocol=file
	--enable-static --disable-shared --enable-small --disable-debug
	# No hand-written x86 assembly. It needs nasm, it is irrelevant to the
	# device, and the host build exists only to test the integration.
	--disable-x86asm
	--enable-swresample
	--enable-decoder="$DECODERS"
	--enable-demuxer="$DEMUXERS"
	--enable-parser="$PARSERS"
)

# ---- configure and build --------------------------------------------------

BUILD="$SRC/build-$TAG"
mkdir -p "$BUILD"
cd "$BUILD"

if [ ! -f config.h ]; then
	if [ -n "$CROSS" ]; then
		# Cortex-A8 with VFPv3-D16, matching every other app on this device.
		# NEON is deliberately NOT enabled: the A8's NEON unit does not
		# handle denormals and FFmpeg's float paths can produce them, and a
		# wrong sample is worse than a slow one.
		"$SRC/configure" "${COMMON[@]}" \
			--enable-cross-compile \
			--cross-prefix="$CROSS" \
			--arch=arm --cpu=cortex-a5 --target-os=linux \
			--extra-cflags="$ARCH_CFLAGS -Os -fPIC" \
			--extra-ldflags="-static" \
			--disable-neon \
			--prefix="$OUT"
	else
		# The architecture comes from the compiler, not from uname.
		#
		# At least one WSL image reports "armv7l" from uname -m while
		# running an x86_64 kernel with an x86_64 toolchain. FFmpeg trusts
		# uname, so it configured for ARM and fed ARM inline assembly to
		# an x86 assembler. The compiler triple is the authority on what
		# the compiler will actually produce, which is the thing the build
		# has to agree with.
		HOST_ARCH="$("${CC:-gcc}" -dumpmachine | cut -d- -f1)"
		"$SRC/configure" "${COMMON[@]}" \
			--arch="$HOST_ARCH" \
			--prefix="$OUT"
	fi
fi

make -j"$(nproc)"
make install

echo
echo "installed to $OUT"
du -sh "$OUT/lib" 2>/dev/null || true
ls -la "$OUT/lib"/*.a 2>/dev/null | awk '{printf "  %9s  %s\n", $5, $9}'
