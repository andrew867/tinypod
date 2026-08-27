# Appliance mode (v1.0)

After FTL mounts the user FAT read-only at `/mnt/disk`:

```sh
# from init or a oneshot service
tinypod --backend alsa ui
# or
TINYPOD_BACKEND=alsa /usr/bin/tinypod-boot.sh
```

Resume: last track id / shuffle stored under `~/.config/tinypod/config.json` or `/tmp/tinypod/` — never on the iPod volume.

Hardware buttons: map Vol+/Vol− / Center / Menu into `tp_ui_keys` (terminal fallback uses j/k/Enter/q).
