# Release gates

| Gate | Criteria |
|------|----------|
| 0 Build | Host + N31 musl static build; no warnings in project code |
| 1 Fixtures | Unit + fixture tests pass; malformed inputs do not crash |
| 2 Real volume | Detects mount + DB; lists tracks; resolves paths; no writes |
| 3 Playback | At least one track plays; stop/next work; unsupported reported cleanly |
| 4 UX | Clean startup; human errors; fast enough list/search |
| 5 Appliance | Boot-to-player with hardware buttons (v1.0) |

Versions: v0.1 CLI, v0.2 ALSA, v0.3 FB UI, v1.0 appliance.
