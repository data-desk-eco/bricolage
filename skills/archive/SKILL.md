---
name: archive
description: The Data Desk methane archive. Query plumes, mapped infrastructure, attributions and company registers with `q "select ..."`. Read this first.
---
# The archive

`q "select ..."` runs one DuckDB query against the archive and prints CSV.
These tables are available:

| Table | Contents | Columns |
|---|---|---|
| `plumes()` | About 75,000 methane plumes | `id` (CM:, IMEO:, SRON: or DD: prefix), `src` (provider), `lat`, `lon`, `dt` (date), `rate` and `unc` (kg/h), `sat` (sensor), `link`, `overlay`, `bounds` |
| `features()` | 17 million mapped oil, gas, coal and waste features from OGIM, OpenStreetMap, GEM, MapStand and NETL's 2018 global database | `id` (OGIM:, OSM:, GEM:, MPS: or GOGI: prefix), `dataset`, `kind`, `name`, `operator`, `status`, `fuel`, `lat`, `lon`, `geometry` |
| `near(lat, lon, km)` | The features within `km` of a point, with distance as `km` | As `features()`, plus `km` |
| `attributions()` | Published attributions by earlier models | `id`, `source_label`, `source_kind`, `source_name`, `operator`, `operator_id`, `operator_name`, `attributed_ids`, `lat`, `lon`, `confidence`, `paragraph`, `evidence`, `verified` |
| `entities()` | GEM's company register | `entity_id`, `name`, `full_name`, `name_local`, `name_other`, `lei`, `permid`, `gem_parents_ids`, `hq_country` |
| `owners()` | GEM's ownership records, for assets and companies | `subject_kind` (asset or entity), `subject_id`, `owner_id`, `owner_name`, `share_pct` |
| `gleif()` | Every name GLEIF holds for each LEI | `id` (GLEIF: and the LEI), `name`, `kind` (legal, trading, translit, alternative, previous) |
| `detections(tile)` | Sentinel-2 detections for one MGRS tile, such as '30UVE' | |

A GEM feature `GEM:L100…` is the asset `L100…` in `owners()`.

Examples:

    q "select * from plumes() where id = 'CM:tan20250101t113529c00s4001-A'"
    q "select id, kind, name, operator, round(km, 2) as km
       from near(38.49, 54.21, 5) order by km limit 20"

Rules:

- Search around a point with `near()`. A query on `features()` without a
  position filter takes minutes.
- Filter `gleif()` by name. It holds four million names.
- Ask for the rows you need. If a query returns more than a few hundred
  rows, narrow it. Do not download whole files.
