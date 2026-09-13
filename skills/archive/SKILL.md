---
name: archive
description: the data desk methane archive, queried with duckdb straight off public object storage; run `q "select ..."` and read this first
---
# the archive

`q "select ..."` runs one query against the archive and prints csv.
The first call builds `archive.db` in the working directory with httpfs and
spatial loaded and these table macros defined, so every later call is cheap:

- `plumes()`: every plume the archive holds, about 70,000 rows: `id`, `src`
  (cm, imeo, sron, dd), `lat`, `lon`, `dt`, `rate` in kg/hr, `unc`, `sat`,
  `sec`, `link`, `overlay`, `bounds`.
- `features()`: 15 million mapped features: `id`, `dataset` (ogim, osm, gem,
  mapstand), `kind` (snake case, 228 kinds), `name`, `operator`, `status`,
  `fuel`, `lat`, `lon`, `geometry`.
- `attributions()`: about 2,000 published attributions in the shape you answer
  in: `id`, `source_label`, `source_kind`, `source_name`, `operator`,
  `attributed_ids`, `lat`, `lon`, `confidence`, `paragraph`, `evidence`,
  `model`, `run_at`, `verified`. Read this before you answer: a record near
  yours in place, date or operator is the best single piece of evidence you
  will get.
- `detections(tile)`: Sentinel-2 detections for one MGRS tile, as in
  `detections('30UVE')`.
- `near(lat, lon, km)`: features within a box of that half-width, with `km`
  as distance. This is how to search around a plume; it filters by bounding
  box before it measures, which is the only way `features()` answers in
  seconds rather than minutes.
- `bucket(path)`: the https url of any path in the archive.

Examples:

    q "select * from plumes() where id = 'tan20250101t113529c00s4001-A'"
    q "select id, kind, name, operator, round(km, 2) as km from near(38.49, 54.21, 5) order by km limit 20"
    q "select id, source_label, operator, confidence from attributions() where lat between 38.4 and 38.6 and lon between 54.1 and 54.3"

## what a plume's coordinate means

It depends on the sensor, and `sat` says which.

- TROPOMI (`imeo` and `sron`, most rows) is coarse: a pixel is about 5.5 by
  7 km and the source is commonly 2 to 10 km from the coordinate, usually
  upwind. The coordinate is a search area, not a place.
- Carbon Mapper is precise. Aircraft (Global Airborne Observatory, AVIRIS-NG,
  AVIRIS-3) is good to tens of metres; satellite (Tanager, ISS) to a few
  hundred. `imeo` high-resolution records are good to about a kilometre.
- Sentinel-2 (`S2`, `S2A`, `S2B`, `S2C`) is precise to a few hundred metres.

Do not move a precise position upwind: wind explains the plume's shape, not
a different origin. For a precise sensor, unmapped equipment standing at the
coordinate beats a named facility kilometres away; for a coarse one, search
the whole plausible radius and weight the upwind side. `sec` is the sector
the provider assigned, and a `waste` plume is very rarely a gas well. A
cluster of detections at one position over months is strong evidence of a
persistent source, and one no single page will state.

The per-provider records under `carbon-mapper/plumes`, `imeo/plumes`,
`sron/plumes` and `data-desk/plumes` are the raw feeds, one file a year as
`bucket('imeo/plumes/year=2025/data.parquet')`. Name the year: the bucket
does not answer a `year=*` glob. Use them only when you want a provider's
own fields rather than the archive's flattened view.

Ask for rows and columns, never files. A query that returns more than a few
hundred rows is asking too broad a question. Never download a parquet file
or an archive to look at it: one attempt pulled a 1.3 GB tile and lost every
file it had written.
