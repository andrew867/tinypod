#!/usr/bin/env bash
#
# Build libsoxr for TinyPod.
#
# Why a resampler is not optional here: the codec's master clock is 12 MHz,
# which divides exactly into the 48 kHz family and never into the 44.1 kHz one
# (12e6 / 44100 = 272.11). So the device runs at 48 kHz and every 44.1 kHz
# track - which is most of an iTunes library - has to be resampled. This is
# the common path, not an edge case, and it is worth doing properly.
#
# Why it has to happen inside the application: alsa-lib's built-in converter
# is linear interpolation, and its good ones (speexrate, samplerate) are
# dlopen()ed plugins that a statically linked musl binary can never load.
# TinyPod talks to tinyalsa anyway, which converts nothing at all.
#
# soxr's VHQ setting is a windowed-sinc polyphase filter: flat to 20 kHz and
# images below -100 dB, against the -25 dB or so that linear interpolation
# manages. It is about 280 KB of ARM static library.
#
#   ./tools/fetch-soxr.sh                              host build, for tests
#   CROSS=arm-linux-musleabi- ./tools/fetch-soxr.sh    N31 build
#
# Output: third_party/soxr-build/{host,n31}/{include,lib}
set -euo pipefail

VER="${SOXR_VERSION:-0.1.3}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TP="$HERE/third_party"
SRC="$TP/soxr-$VER-Source"
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
OUT="$TP/soxr-build/$TAG"

if [ -f "$OUT/lib/libsoxr.a" ] && [ -z "${SOXR_REBUILD:-}" ]; then
	echo "soxr already built: $OUT/lib/libsoxr.a"
	exit 0
fi
rm -rf "$OUT"

command -v cmake >/dev/null || {
	echo "FAIL: soxr needs cmake"
	exit 1
}

mkdir -p "$TP"

# ---- source ---------------------------------------------------------------

if [ ! -d "$SRC" ]; then
	TAR="$TP/soxr-$VER.tar.xz"
	if [ ! -f "$TAR" ]; then
		echo "fetching soxr $VER"
		curl -fL --retry 3 -o "$TAR.part" \
			"https://sourceforge.net/projects/soxr/files/soxr-$VER-Source.tar.xz/download"
		mv "$TAR.part" "$TAR"
	fi
	echo "unpacking"
	tar -C "$TP" -xf "$TAR"
fi

# ---- configure and build --------------------------------------------------
#
# Static only, no examples, no tests, and no OpenMP - the device has one core
# that matters and the library would otherwise want a threading runtime the
# static musl link does not have.

BUILD="$SRC/build-$TAG"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

CMAKE_ARGS=(
	-DCMAKE_INSTALL_PREFIX="$OUT"
	-DCMAKE_BUILD_TYPE=Release
	-DBUILD_SHARED_LIBS=OFF
	-DBUILD_TESTS=OFF
	-DBUILD_EXAMPLES=OFF
	-DWITH_OPENMP=OFF
	-DWITH_LSR_BINDINGS=OFF
	# No SIMD engines.
	#
	# soxr's FindSIMD32 tries a list of flag sets and keeps the first the
	# compiler accepts, and the first is "-mfpu=neon-vfpv4 -mcpu=cortex-a7".
	# This cross-compiler accepts it, so the SIMD engine came out with fused
	# multiply-accumulates in it - VFPv4, which a Cortex-A8 does not have.
	# That is an undefined instruction on the device, not a slow path.
	#
	# They are a speed optimisation and not a quality one, so the scalar
	# engine gives the same output. It also keeps this consistent with the
	# FFmpeg build here, which turns NEON off deliberately: the A8's NEON
	# flushes denormals to zero instead of handling them.
	-DWITH_CR32S=OFF
	-DWITH_CR64S=OFF
	-DSIMD32_C_FLAGS=
	-DSIMD64_C_FLAGS=
)

if [ -n "$CROSS" ]; then
	# Same flags as everything else on this device: Cortex-A8 with VFPv3-D16,
	# softfp ABI. soxr is float-heavy, so this is the one library where the
	# FPU actually earns its place.
	cat > "$BUILD/toolchain.cmake" <<-EOF
	set(CMAKE_SYSTEM_NAME Linux)
	set(CMAKE_SYSTEM_PROCESSOR arm)
	set(CMAKE_C_COMPILER ${CROSS}gcc)
	set(CMAKE_AR ${CROSS}ar)
	set(CMAKE_RANLIB ${CROSS}ranlib)
	set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
	set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
	EOF
	CMAKE_ARGS+=(
		-DCMAKE_TOOLCHAIN_FILE="$BUILD/toolchain.cmake"
		-DCMAKE_C_FLAGS="-O2 $ARCH_CFLAGS"
	)
else
	CMAKE_ARGS+=(-DCMAKE_C_FLAGS="-O2")
fi

cmake .. "${CMAKE_ARGS[@]}" > "$BUILD/cmake.log" 2>&1 || {
	tail -30 "$BUILD/cmake.log"
	exit 1
}
make -j"$(nproc)" > "$BUILD/build.log" 2>&1 || {
	tail -40 "$BUILD/build.log"
	exit 1
}
make install > /dev/null

echo
echo "installed to $OUT"
ls -la "$OUT/lib"/libsoxr.a | awk '{printf "  %9s  %s\n", $5, $9}'
