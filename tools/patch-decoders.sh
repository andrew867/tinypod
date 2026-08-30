#!/bin/sh
# Make the Helix decoders build under mingw.
#
# third_party/ is fetched rather than vendored, so a change made there is lost
# on the next fetch. This re-applies it, and is idempotent - run it as often as
# you like.
#
# What it fixes: Helix's assembly.h picks an MSVC code path that uses __asm { }
# blocks and #pragma warning. Its own comment says "toolchain: MSFT Visual C++",
# but the condition only tests _WIN32 - which mingw also defines. So a mingw
# build lands on inline assembly gcc cannot parse. Requiring _MSC_VER as well
# selects the portable GNU x86 path instead.
#
# Escaping it by defining _WIN32_WCE does not work: that branch never typedefs
# Word64, and the MP3 polyphase code needs it.
#
# No effect on the device build, which takes the __arm__ branch either way.
set -e
cd "$(dirname "$0")/.."

patched=0

fix() {
    f=$1
    old=$2
    new=$3

    [ -f "$f" ] || return 0
    if grep -q '_MSC_VER) && defined (_WIN32\|_MSC_VER && defined _WIN32' "$f"; then
        echo "  already patched: $f"
        return 0
    fi
    if ! grep -qF "$old" "$f"; then
        echo "  WARNING: pattern not found in $f - upstream may have changed"
        return 0
    fi

    tmp=$(mktemp)
    # A plain textual swap of the one condition. sed would need the whole line
    # escaped; this replaces the fragment and leaves the rest alone.
    awk -v old="$old" -v new="$new" '
        index($0, old) { sub(old, new, $0) }
        { print }
    ' "$f" > "$tmp"
    mv "$tmp" "$f"
    echo "  patched: $f"
    patched=$((patched + 1))
}

echo "patching Helix for mingw:"

fix third_party/helix-aac/assembly.h \
    "#if (defined (_WIN32) && !defined (_WIN32_WCE))" \
    "#if (defined (_MSC_VER) && defined (_WIN32) && !defined (_WIN32_WCE))"

fix third_party/helix-mp3/real/assembly.h \
    "#if (defined _WIN32 && !defined _WIN32_WCE)" \
    "#if (defined _MSC_VER && defined _WIN32 && !defined _WIN32_WCE)"

echo "done ($patched changed)"
