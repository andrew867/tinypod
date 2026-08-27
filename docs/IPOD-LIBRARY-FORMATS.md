# iPod library formats (N31 / Nano)

## Preferred: SQLite `.itlp`

```text
iPod_Control/iTunes/iTunes Library.itlp/
  Library.itdb      # item, container, artist, album, …
  Locations.itdb    # location, base_location
  Dynamic.itdb
  Extras.itdb
  Genius.itdb
```

Join (from RetailOS SQL + real N7G sample):

```sql
-- Library.itdb
SELECT pid, title, artist, album, album_artist, composer,
       total_time_ms, track_number, disc_number, year
FROM item WHERE is_song = 1;

-- Locations.itdb
SELECT base_location_id, location, file_size FROM location WHERE item_pid = ?;
SELECT id, path FROM base_location;  -- e.g. path = 'iPod_Control/Music'
```

**Path assembly:** `mount_root / base_location.path / location`

Note: `location` already includes the filename extension (e.g. `F13/QRLF.mp3`).
The `extension` column is a FourCC integer (e.g. `MP3 `), not a string suffix.

## Classic-family: iTunesCDB / iTunesDB

Binary `mhbd` database. On nano7 the file is often named `iTunesCDB`.

## Raw fallback

Scan `iPod_Control/Music/F00`…`F99`, read ID3v2 / MP4 tags when present.
