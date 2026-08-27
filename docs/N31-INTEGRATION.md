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
make TARGET=n31   # requires arm-linux-musleabi-gcc (musl static)
# install out/n31/tinypod into rootfs overlay /usr/bin/tinypod
```

Same toolchain pattern as `tools/linux-n31/build-n31-sine.sh` in the parent ipod tree.

## Safety

- Open DB and audio with read-only flags
- Never write under `iPod_Control`
- Config/cache: `~/.config/tinypod`, `/var/lib/tinypod`, `/tmp/tinypod`

## Audio

ALSA backend expects `/dev/snd` (CS42 / nano7-audio). If missing, TinyPod prints a clear error and does not crash.
