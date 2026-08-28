# N31 integration

## Mount

After FTL recover on-device:

```sh
mount -t vfat -o ro /dev/s5l8740-ipod /mnt/disk
tinypod libcheck
tinypod --backend alsa play <id>
```

TinyPod auto-probes `/mnt/disk`, `/mnt/ipod`, `/media/ipod`, `/run/media/ipod`, then `/proc/mounts`.

## Packaging

```sh
tools/linux-n31/build-n31-tinypod.sh    # in the parent ipod tree
```

That fetches the Helix decoders, builds against the same musl toolchain and
tinyalsa tree `build-n31-sine.sh` uses, and stages
`artifacts/linux-n31/tinypod`. `mk-initramfs.sh` then installs it at
`/bin/tinypod` with `/bin/tinypod-boot`; `install-n31os-disk.ps1` also copies it
to `n31os/apps/tinypod/` on the FAT volume.

By hand:

```sh
make TARGET=n31 CROSS=arm-linux-musleabi- TINYALSA_DIR=/path/to/tinyalsa-2.0.0
```

`-march=armv7-a` is required and set by the Makefile: Helix AAC's fast path uses
`ssat` (ARMv6+), and `-DARM` selects Helix MP3's `smull` assembly.

## Decoding

In-process, integer only:

| Format | Decoder |
| --- | --- |
| AAC-LC and HE-AAC in `.m4a`/`.m4b`/`.mp4` | Helix AAC + our MP4 demuxer |
| AAC in ADTS (`.aac`) | Helix AAC |
| MP3 | Helix MP3 |
| WAV (16-bit PCM) | direct |
| ALAC, FairPlay `.m4p` | refused with a clear message |

iTunes `.m4a` files hold raw AAC frames with no ADTS headers, so the decoder is
configured from the AudioSpecificConfig in `stsd/mp4a/esds` and frames are
located through `stsz`/`stsc`/`stco`. Only the sizes and chunk offsets are held
in memory, so an hour-long file costs a few hundred KB rather than tens of MB.

Verified against a real iTunes library: 28 of 28 `.m4a` files decoded to within
4 ms of their container duration, and the ARM build's PCM output is byte-identical
to the host build's.

## Output

Two paths, tried in order, because the N31 kernel can present either:

1. **ALSA** via tinyalsa - card 0 device 0, S16_LE, 10 ms periods x 4, the shape
   `n31-sine` proved on the glass. `TINYPOD_ALSA_CARD` / `TINYPOD_ALSA_DEVICE`.
2. **OSS** on `/dev/dsp` if ALSA will not open. `TINYPOD_OSS_DEVICE`.

`TINYPOD_AUDIO=alsa` or `=oss` pins one instead of falling back, and the failure
message reports what both said rather than only the last one.

The OSS path sets format, channels and rate explicitly and refuses the device if
it answers with anything else. An unconfigured `/dev/dsp` defaults to 8 kHz 8-bit
mono and will happily accept a 44.1 kHz stereo stream, draining it about sixty
times too slowly - which is indistinguishable from a broken driver until you
measure it.

Playback runs on its own thread, so the UI stays responsive and pause/resume/stop
and end-of-track auto-advance all work while audio is flowing.

## Safety

- Open DB and audio with read-only flags
- Never write under `iPod_Control`
- Config/cache: `~/.config/tinypod`, `/var/lib/tinypod`, `/tmp/tinypod`

## Audio

The ALSA backend needs `/dev/snd` (CS42 / nano7-audio, via `load-mods periph`).
If it is missing TinyPod says so and does not crash.

No sound? Separate the two halves:

```sh
tinypod decode <file> /tmp/t.wav          # decoder only, no device touched
TINYPOD_ALSA_WAV=/tmp/cap.wav tinypod play <id>   # full path, captured to file
```

If both produce audio but the speaker does not, the fault is below TinyPod -
check `n31-sine` and the CS42 rails.
