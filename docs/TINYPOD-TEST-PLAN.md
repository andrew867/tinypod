# Test plan

## Unit (`make test`)

Volume detect, path colon/traversal, utf8, json escape, sort, codec probe.

## Integration

```sh
./out/host/tinypod --mount tests/fixtures/sqlite-itdb-small libcheck
./out/host/tinypod --mount tests/fixtures/raw-files-small list
```

## Real sample (optional)

```sh
export TINYPOD_MOUNT=...   # see testdata/README.md
make check-real
./out/host/tinypod --mount "$TINYPOD_MOUNT" --backend null play <id>
```

## N31

```sh
tinypod --mount /mnt/disk libcheck
tinypod --mount /mnt/disk list | head
tinypod --backend alsa play <id>
```
