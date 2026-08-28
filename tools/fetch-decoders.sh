#!/usr/bin/env bash
#
# Fetch the fixed-point audio decoders TinyPod links against.
#
#   third_party/helix-aac   Helix AAC  (AAC-LC + HE-AAC/SBR), integer only
#   third_party/helix-mp3   Helix MP3, integer only
#
# Both are RealNetworks Helix code under RPSL/RCSL, not MIT like TinyPod, which
# is why they are fetched rather than vendored into this repository: whoever
# runs the build decides. Override the sources to taste:
#
#   AAC_URL=... AAC_STRIP=... ./tools/fetch-decoders.sh
#
# Fixed point matters here. The N31 is armv7 soft-float, so a floating-point
# decoder (faad2, stock) burns the CPU budget in software float emulation.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
TP="$HERE/third_party"
WORK="${WORK:-${TMPDIR:-/tmp}/tinypod-decoders}"

# Helix AAC has no upstream standalone repo. ESP8266Audio carries the most
# widely used clean subtree of it (with SBR), which is what we take.
AAC_URL="${AAC_URL:-https://codeload.github.com/earlephilhower/ESP8266Audio/tar.gz/refs/heads/master}"
AAC_SUBDIR="${AAC_SUBDIR:-ESP8266Audio-master/src/libhelix-aac}"
MP3_URL="${MP3_URL:-https://codeload.github.com/ultraembedded/libhelix-mp3/tar.gz/refs/heads/master}"

mkdir -p "$WORK" "$TP"

fetch() {
	url="$1"
	out="$2"
	if [[ -s "$out" ]]; then
		echo "have $(basename "$out")"
		return 0
	fi
	echo "fetch $url"
	curl -fsSL --max-time 300 -o "$out.part" "$url"
	mv -f "$out.part" "$out"
}

# ---------------------------------------------------------------- Helix AAC
if [[ -f "$TP/helix-aac/aacdec.c" ]]; then
	echo "have third_party/helix-aac"
else
	fetch "$AAC_URL" "$WORK/aac-src.tar.gz"
	rm -rf "$TP/helix-aac"
	mkdir -p "$TP/helix-aac"
	tar xzf "$WORK/aac-src.tar.gz" -C "$TP/helix-aac" \
		--strip-components="$(awk -F/ '{print NF}' <<<"$AAC_SUBDIR")" \
		"$AAC_SUBDIR"
	echo "unpacked third_party/helix-aac ($(ls "$TP/helix-aac"/*.c | wc -l) sources)"
fi

# The subtree targets Arduino: <Arduino.h>/<pgmspace.h> for PROGMEM, which on a
# hosted target is nothing at all. Everything else in it is portable C.
cat > "$TP/helix-aac/arduino_compat.h" <<'SHIM'
/*
 * Stand-in for the Arduino headers the Helix AAC subtree expects.
 * PROGMEM is an AVR/ESP flash-placement attribute: on Linux, const data is
 * already directly addressable, so it degrades to nothing and every
 * pgm_read_* becomes a plain load.
 */
#ifndef TINYPOD_ARDUINO_COMPAT_H
#define TINYPOD_ARDUINO_COMPAT_H

#include <stdint.h>
#include <stdio.h>   /* sbr.c prints its own OOM message and expected Arduino.h */
#include <stdlib.h>
#include <string.h>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PGM_P
#define PGM_P const char *
#endif

#define pgm_read_byte(a)        (*(const unsigned char *)(a))
#define pgm_read_word(a)        (*(const unsigned short *)(a))
#define pgm_read_dword(a)       (*(const unsigned int *)(a))
#define memcpy_P                memcpy
#define strcpy_P                strcpy

#endif /* TINYPOD_ARDUINO_COMPAT_H */
SHIM

# Point the sources at the shim instead of Arduino.
if grep -q 'Arduino\.h' "$TP/helix-aac/aaccommon.h"; then
	sed -i \
		-e 's|#include <Arduino\.h>|#include "arduino_compat.h"|' \
		-e '/#include <pgmspace\.h>/d' \
		"$TP/helix-aac/aaccommon.h"
	echo "patched aaccommon.h -> arduino_compat.h"
fi

# Two sources reach for Helix's own libc wrapper, which the subtree does not
# carry. On a hosted target it is the system header.
mkdir -p "$TP/helix-aac/hlxclib"
cat > "$TP/helix-aac/hlxclib/stdlib.h" <<'HLX'
/* Helix's libc wrapper; on a hosted target it is just the system header. */
#ifndef TINYPOD_HLXCLIB_STDLIB_H
#define TINYPOD_HLXCLIB_STDLIB_H
#include <stdlib.h>
#endif
HLX
cat > "$TP/helix-aac/hlxclib/string.h" <<'HLX'
#ifndef TINYPOD_HLXCLIB_STRING_H
#define TINYPOD_HLXCLIB_STRING_H
#include <string.h>
#endif
HLX

# The subtree disables its own ARM path by misspelling the guard, so every
# target lands on the portable C MULSHIFT32/CLIPTOSHORT. We want the real one
# back on ARM: smull and ssat are the decoder's hottest two operations.
# ssat is ARMv6+, hence -march=armv7-a in the n31 build.
if grep -q 'XXXX__arm__' "$TP/helix-aac/assembly.h"; then
	sed -i 's|defined(XXXX__arm__)|defined(__arm__)|' "$TP/helix-aac/assembly.h"
	echo "patched assembly.h: re-enabled the ARM smull/ssat path"
fi

# ---------------------------------------------------------------- Helix MP3
if [[ -f "$TP/helix-mp3/mp3dec.c" ]]; then
	echo "have third_party/helix-mp3"
else
	fetch "$MP3_URL" "$WORK/mp3-src.tar.gz"
	rm -rf "$TP/helix-mp3"
	mkdir -p "$TP/helix-mp3"
	tar xzf "$WORK/mp3-src.tar.gz" -C "$TP/helix-mp3" --strip-components=1
	echo "unpacked third_party/helix-mp3"
fi

# Helix MP3's assembly.h knows ARM (behind -DARM), a few dead embedded targets,
# and MSVC. Anything else hits "#error Unsupported platform", which includes the
# x86 host build we develop and test against. Give that #else a portable C body;
# ARM still matches its own branch earlier in the chain and keeps the asm.
ASM="$TP/helix-mp3/real/assembly.h"
if grep -q '#error Unsupported platform' "$ASM"; then
	cat > "$WORK/generic.h" <<'GEN'
/* TinyPod: portable C for platforms with no hand-written path (e.g. x86 host).
   ARM matches "__GNUC__ && ARM" earlier and still uses its assembly. */

typedef long long Word64;

static __inline int MULSHIFT32(int x, int y)
{
	return (int)(((long long)x * (long long)y) >> 32);
}

static __inline int FASTABS(int x)
{
	int sign = x >> (int)(sizeof(int) * 8 - 1);
	x ^= sign;
	x -= sign;
	return x;
}

static __inline int CLZ(int x)
{
	if (!x)
		return (int)(sizeof(int) * 8);
	return __builtin_clz((unsigned int)x);
}

static __inline Word64 MADD64(Word64 sum, int x, int y)
{
	return sum + (Word64)x * (Word64)y;
}

static __inline Word64 SHL64(Word64 x, int n)
{
	return x << n;
}

static __inline Word64 SAR64(Word64 x, int n)
{
	return x >> n;
}
GEN
	awk -v gen="$WORK/generic.h" '
		/#error Unsupported platform/ {
			while ((getline line < gen) > 0)
				print line
			close(gen)
			next
		}
		{ print }
	' "$ASM" > "$ASM.new"
	mv -f "$ASM.new" "$ASM"
	echo "patched helix-mp3 assembly.h: portable C for non-ARM hosts"
fi

# The ARM branch splits on ARM7DI. That old sub-branch carries the 64-bit
# helpers; the modern smull one defines only MULSHIFT32, so polyphase.c fails
# to build for ARM on a stock checkout. Supply the rest.
if ! grep -q 'TinyPod: 64-bit helpers' "$ASM"; then
	cat > "$WORK/arm64.h" <<'ARM64'

/* TinyPod: 64-bit helpers for the smull ARM path, which omits them (only the
   legacy ARM7DI sub-branch defines them). polyphase.c needs all three; GCC
   compiles MADD64 to smlal. */
#if !defined(ARM7DI)
typedef long long Word64;

static __inline Word64 MADD64(Word64 sum, int x, int y)
{
	return sum + (Word64)x * (Word64)y;
}

static __inline Word64 SHL64(Word64 x, int n)
{
	return x << n;
}

static __inline Word64 SAR64(Word64 x, int n)
{
	return x >> n;
}
#endif
ARM64
	awk -v add="$WORK/arm64.h" '
		/__asm__ volatile \("smull/ { seen = 1 }
		{ print }
		seen && /^#endif/ {
			while ((getline line < add) > 0)
				print line
			close(add)
			seen = 0
		}
	' "$ASM" > "$ASM.new"
	mv -f "$ASM.new" "$ASM"
	echo "patched helix-mp3 assembly.h: 64-bit helpers for the ARM smull path"
fi

echo
echo "AAC $(ls "$TP/helix-aac"/*.c | wc -l) sources, MP3 $(ls "$TP/helix-mp3"/*.c "$TP/helix-mp3"/real/*.c | wc -l) sources"
echo "Licence: RealNetworks RPSL/RCSL - see third_party/helix-*/LICENSE.txt"
echo "DECODERS_OK"
