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
- Playback backends: `null`, `external` (mpv/ffplay/mpg123), `alsa` (N31)

## Build (WSL / Linux)

```sh
cd tinypod
make                # host ELF → out/host/tinypod
make test           # unit selftest
make TARGET=n31     # musl-static ARMv7 (needs arm-linux-musleabi-gcc)
```

## Run

```sh
# On N31 after FTL mount:
tinypod libcheck
tinypod list
tinypod play <track-id>

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
