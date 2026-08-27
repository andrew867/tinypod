# UX standard

- No mystery errors — always say what failed and the paths involved
- No debug spew by default (`--debug` / `--verbose` for guts)
- No mandatory flags on-device (auto-detect `/mnt/disk`)
- No database rebuild prompt
- No writing to the iPod volume
- One bad track must not abort the library

Startup should feel like:

```text
TinyPod found your iPod library.
1234 songs ready.
```
