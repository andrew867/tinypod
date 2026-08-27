# N31 actual library shape (probed sample)

Probed from a real nano7 `iPod_Control` tree (path supplied via `TINYPOD_MOUNT` at probe time — not compiled into TinyPod).

## Top level

```text
iPod_Control/
  Artwork/
  Device/
  iTunes/
  Music/
  Speakable/
```

## iTunes

```text
iTunes/
  iTunes Library.itlp/
    Library.itdb      # SQLite3, user_version 25
    Locations.itdb
    Dynamic.itdb
    Extras.itdb
    Genius.itdb
    Locations.itdb.cbk
  iTunesCDB           # mhbd magic (classic-family)
  iTunesControl
  iTunesPrefs*
  Play Counts
  …
```

## Library.itdb tables (sample)

`album`, `artist`, `avformat_info`, `category_map`, `composer`, `container`,
`container_seed`, `db_info`, `genre_map`, `item`, `item_to_container`,
`location_kind_map`, `podcast_info`, `store_info`, `store_link`, `track_artist`,
`track_size_calc`, `version_info`, `video_characteristics`, `video_info`

- `item` count: **496** (all `is_song=1`)
- playlists via `container` / `item_to_container`

## Locations.itdb

- `base_location`: id=1, path=`iPod_Control/Music`
- `location.location`: e.g. `F13/QRLF.mp3` (extension included)
- `location.extension`: FourCC int (e.g. `0x4d503320` = `MP3 `)

## Music

- Folders `F00`…`F49` (50 folders)
- ~496 audio files (`.mp3` / `.m4a`)
- TinyPod libcheck: **496 playable, 0 missing** against this sample
