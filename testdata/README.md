# Test data (local only)

TinyPod **never** hardcodes developer paths. Supply a mount at runtime:

```sh
export TINYPOD_MOUNT=/path/to/volume_root   # contains iPod_Control/
# or:
export TINYPOD_MOUNT=/path/to/iPod_Control  # the control folder itself
```

Optional untracked file `testdata/local.env`:

```sh
TINYPOD_MOUNT=/mnt/c/src
```

Then:

```sh
make check-real
```

Do **not** commit real music libraries. Carve small fixtures into `tests/fixtures/` for CI.
