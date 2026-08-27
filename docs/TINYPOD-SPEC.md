# TinyPod Spec (summary)

See the product package in the project plan. Core promise:

1. Find mounted iPod FAT volume
2. Detect library format (sqlite-itdb → itunescdb → itunesdb → raw)
3. Build in-memory track list with metadata
4. Resolve paths under `iPod_Control/Music`
5. Artists / Albums / Songs / Playlists
6. Play tracks (null / external / alsa)
7. Resume via Linux-side config only
8. Never modify iPod music DB by default

Non-goals: sync, delete, write iTunesDB, artwork UI, video, DRM bypass, Bluetooth.
