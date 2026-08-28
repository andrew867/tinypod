# TinyPod

A tiny, fast, **read-only**, iPod-native music player for **N31 Linux** (iPod nano 7th generation).

```text
Boot N31 Linux.
Mount iPod volume read-only.
TinyPod opens instantly.
Music library appears with metadata.
Play / shuffle / search works.
No sync required.
No database rebuild required.
```

## Features

- Reads Nano SQLite `.itlp` libraries (`Library.itdb` + `Locations.itdb`)
- Falls back to `iTunesCDB` / classic `iTunesDB`, then raw `Music/Fxx` + tags
- Never writes under `iPod_Control`
- CLI + optional terminal/framebuffer UI
- **Decodes in-process**: Helix fixed-point AAC (incl. HE-AAC/SBR) and MP3, plus
  WAV. No mpv, no ffmpeg, no helper process - the N31 initramfs has none.
- Its own MP4/M4A demuxer, so iTunes-synced `.m4a` files play as they are
- Playback backends: `alsa` (N31, decodes in-process, ALSA with an OSS
  `/dev/dsp` fallback), `external` (a dev host's mpv/ffplay/mplayer/cvlc/
  mpg123), `null` (resolve only)

## Build (WSL / Linux)

```sh
cd tinypod
./tools/fetch-decoders.sh   # third_party/helix-aac, helix-mp3 (also run by make)
make                        # host ELF -> out/host/tinypod
make test                   # unit selftest
make TARGET=n31 TINYALSA_DIR=/path/to/tinyalsa-2.0.0   # musl-static ARMv7
```

The decoders are RealNetworks Helix (RPSL/RCSL), so they are fetched at build
time rather than vendored: see [tools/fetch-decoders.sh](tools/fetch-decoders.sh)
to point the build at different sources. Fixed point is the requirement - the
N31 is armv7 soft-float, where a floating-point decoder spends its time
emulating floats.

In the ipod tree, `tools/linux-n31/build-n31-tinypod.sh` does all of the above
and stages the binary for the initramfs and the disk.

## Run

```sh
# On N31 after FTL mount:
tinypod libcheck
tinypod list
tinypod play <track-id>      # decodes and plays; blocks until the track ends
tinypod ui                   # p pause, n next, b prev, x stop, q back

# Decode without an audio device - the way to tell a decode fault from a
# sound-card fault:
tinypod decode song.m4a /tmp/out.wav
# Or capture what playback would have sent to the card:
TINYPOD_ALSA_WAV=/tmp/cap.wav tinypod play <track-id>

# Dev / WSL against a sample volume (path is yours — never hardcoded):
export TINYPOD_MOUNT=/path/to/volume   # parent of iPod_Control, or iPod_Control itself
./out/host/tinypod --mount "$TINYPOD_MOUNT" --backend null libcheck
./out/host/tinypod --mount "$TINYPOD_MOUNT" --backend external play <id>
```

See [testdata/README.md](testdata/README.md) for local sample setup.

## Docs

- [TINYPOD-SPEC.md](docs/TINYPOD-SPEC.md)
- [IPOD-LIBRARY-FORMATS.md](docs/IPOD-LIBRARY-FORMATS.md)
- [N31-INTEGRATION.md](docs/N31-INTEGRATION.md)
- [N31-ACTUAL-LIBRARY-SHAPE.md](docs/N31-ACTUAL-LIBRARY-SHAPE.md)
- [UX-STANDARD.md](docs/UX-STANDARD.md)
- [TINYPOD-RELEASE-GATES.md](docs/TINYPOD-RELEASE-GATES.md)

## License

MIT — free for everyone.
