# TinyPod

A read-only, iPod-native music player for N31 Linux (iPod nano 7th generation).

TinyPod reads the library the iPod already has. It does not sync, does not
rebuild a database, and never writes anything under `iPod_Control`.

## Features

- Reads Nano SQLite `.itlp` libraries (`Library.itdb` + `Locations.itdb`)
- Falls back to `iTunesCDB` / classic `iTunesDB`, then to a raw scan of
  `Music/Fxx` with tag parsing
- In-process decoding: Helix fixed-point AAC (including HE-AAC/SBR), MP3, and
  WAV. No external player process is required, and the N31 initramfs has none
- Its own MP4/M4A demuxer, so iTunes-synced `.m4a` files play unmodified
- Three interfaces: a command line, a terminal UI, and a graphical UI on the
  framebuffer
- Playback backends: `alsa` (N31; ALSA via tinyalsa, with an OSS `/dev/dsp`
  fallback), `external` (a development host's mpv/ffplay/mplayer/cvlc/mpg123),
  and `null` (resolve without playing)

## Browsing

Songs, Artists, Albums and Playlists each open into their contents. An artist
opens into that artist's albums, and an album into its tracks; an artist with
a single album skips the intermediate list.

Lists of thirty entries or more carry a **Jump to...** row at the top, which
opens the initials present in that list and moves the selection to the first
entry under the one chosen. The device has three usable buttons, so this is a
row rather than a gesture.

Artists and albums are sorted by name. Tracks are sorted by artist and then
album, so the Songs list is in artist order and its jump indexes by artist.

## Build

Requires a Linux or WSL host.

```sh
cd tinypod
./tools/fetch-decoders.sh   # third_party/helix-aac, helix-mp3 (also run by make)
make                        # host binary -> out/host/tinypod
make test                   # unit tests
make TARGET=n31 TINYALSA_DIR=/path/to/tinyalsa-2.0.0   # musl-static ARMv7
```

Add `UI_LVGL=1 LVGL=/path/to/lvgl` for the graphical UI. Graphical and plain
builds use separate object directories, because their compile flags differ and
the makefile does not track flags.

The Helix decoders are RealNetworks code under the RPSL/RCSL, so they are
fetched at build time rather than vendored. See
[tools/fetch-decoders.sh](tools/fetch-decoders.sh) to point the build at a
different source. The decoders must be fixed-point: the N31 is ARMv7 with
soft-float ABI.

In the ipod tree, `tools/linux-n31/build-n31-tinypod.sh` performs the above and
stages the binary for the initramfs and for the disk.

### Build identification

Every binary reports when it was built and from which commit:

```sh
tinypod --help          # first two lines
```

The same string appears on the About screen of both UIs. An old copy on the
device is otherwise indistinguishable from a current one.

## Run

```sh
tinypod libcheck             # library health report
tinypod list
tinypod search <query>
tinypod play <track-id>      # blocks until the track ends; --no-wait returns
tinypod ui                   # terminal UI
tinypod gui                  # framebuffer UI
```

Both UIs start even when no volume is mounted, and say so on screen. The
command-line tools treat a missing volume as an error.

### Diagnosing playback

```sh
# Decode without an audio device, to separate a decode fault from a
# sound-card fault:
tinypod decode song.m4a /tmp/out.wav

# Capture what playback would have sent to the card:
TINYPOD_ALSA_WAV=/tmp/cap.wav tinypod play <track-id>
```

The playback position is bounded by elapsed time. Audio cannot leave a device
faster than real time, so a position that would run ahead of the clock
indicates the device is accepting samples without playing them; TinyPod
reports that state rather than showing a position that races.

### Development against a sample volume

```sh
export TINYPOD_MOUNT=/path/to/volume   # parent of iPod_Control, or iPod_Control
./out/host/tinypod --mount "$TINYPOD_MOUNT" --backend null libcheck
./out/host/tinypod --mount "$TINYPOD_MOUNT" --backend external play <id>
```

See [testdata/README.md](testdata/README.md) for local sample setup.

### Screenshots

```sh
tinypod --mount /path/to/volume gui-shots <dir>
```

Renders the graphical UI to BMPs without a framebuffer, driving the same draw
and key-handling code the device runs. Pointed at a real library, this
exercises the view logic rather than the widgets alone.

## Documentation

- [TINYPOD-SPEC.md](docs/TINYPOD-SPEC.md)
- [IPOD-LIBRARY-FORMATS.md](docs/IPOD-LIBRARY-FORMATS.md)
- [N31-INTEGRATION.md](docs/N31-INTEGRATION.md)
- [N31-ACTUAL-LIBRARY-SHAPE.md](docs/N31-ACTUAL-LIBRARY-SHAPE.md)
- [UX-STANDARD.md](docs/UX-STANDARD.md)
- [TINYPOD-RELEASE-GATES.md](docs/TINYPOD-RELEASE-GATES.md)

## Licence

MIT.
