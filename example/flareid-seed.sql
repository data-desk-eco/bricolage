-- the flareid seed: one record per flare, read from the archive in duckdb and
-- written to the sqlite database's flare table. the record is arithmetic, not
-- a finding: what burned, how hot, and what is mapped nearby.
--
--   duckdb -c ".read example/flareid-seed.sql" -c "
--     attach 'flareid.db' as db (type sqlite);
--     insert into db.flare select * from record(['17412', '1000001'])
--     where key not in (select key from db.flare)"
--
-- record() takes a list of flare ids, so any query that lists them seeds:
-- (select list(flare_id) from 'data.duckdb'.flares), or every flare in a set
-- of countries from flares(). the next `.load ./ext/bric` starts the work.
install httpfs; load httpfs; install spatial; load spatial; install sqlite;
set geometry_always_xy = true;

create or replace secret (type s3, provider config, key_id '', secret '',
  region 'WAW3-2', endpoint 's3.WAW3-2.cloudferro.com', url_style 'path');

create or replace macro bucket(p) as 's3://data-desk-archive/' || p;

create or replace macro m(alat, alon, blat, blon) as
  6371000 * 2 * asin(sqrt(sin(radians(blat - alat) / 2) ^ 2
    + cos(radians(alat)) * cos(radians(blat))
    * sin(radians(blon - alon) / 2) ^ 2));

create or replace macro deg(alat, alon, blat, blon) as
  (degrees(atan2(radians(blon - alon) * cos(radians(alat)),
                 radians(blat - alat))) + 360) % 360;

-- an area is where a site is, never the site: a licence, field or basin
create or replace macro areas() as ['licence', 'licence_area', 'licence_owner',
  'licence_block', 'field', 'offshore_field', 'oil_field', 'oilfield',
  'gas_field', 'basin', 'project', 'petroleum_cluster'];

-- mapped things no flare stands at
create or replace macro noise() as ['farmyard', 'railway', 'dam',
  'water_tower', 'reservoir_covered', 'animal_keeping', 'salt_pond',
  'embankment', 'dyke', 'pier', 'water_works', 'water_well'];

create or replace macro record(ids) as table
with f as (
  select * from read_parquet(bucket('eog/flares/data.parquet'))
  where list_contains(ids, id)
),
-- radiant heat and flame temperature since 2018, the v3.0 era
burn as (
  select site_id as id, year(date) as year, count(*) as lit,
    round(avg(temp_k)) as temp_k, round(sum(rh_mw), 1) as rh_sum
  from read_parquet(bucket('eog/detections/*/data.parquet'),
                    hive_partitioning = true)
  where cell in (select cell from f) and list_contains(ids, site_id)
    and valid and date >= date '2018-01-01'
  group by all
),
-- observed nights reach back only to 2023 in the catalogue's quarters
seen as (
  select id, year(q.quarter) as year, sum(q.observations) as nights
  from (select id, unnest(quarters) as q from f) group by all
),
years as (
  select id, list({year: year, nights: s.nights, lit: coalesce(b.lit, 0),
      temp_k: b.temp_k, mcm_if_gas: round(b.rh_sum * 0.0315, 2)}
      order by year) as years,
    round(sum(b.temp_k * b.lit) / sum(b.lit)) as temp_k
  from seen s full join burn b using (id, year) group by id
),
box as (
  select min(lat) - 0.05 as a, max(lat) + 0.05 as b,
         min(lon) - 0.08 as c, max(lon) + 0.08 as d from f
),
feature as (
  select id, provider, kind, name, operator, lat, lon, geometry
  from read_parquet(list_transform(
      ['ogim', 'osm', 'gem', 'mapstand', 'netl'],
      lambda p: bucket(p || '/infrastructure/data.parquet')),
      union_by_name = true), box
  where lat between a and b and lon between c and d
),
-- metres to the outline, not the centroid: 0 is inside the plant
near as (
  select f.id as flare, g.* exclude (geometry),
    round(st_distance(st_transform(g.geometry, 'OGC:CRS84', 'EPSG:3857'),
      st_transform(st_point(f.lon, f.lat), 'OGC:CRS84', 'EPSG:3857'))
      * cos(radians(f.lat))) as metres,
    deg(f.lat, f.lon, g.lat, g.lon) as bearing
  from f join feature g
    on g.lat between f.lat - 0.05 and f.lat + 0.05
   and g.lon between f.lon - 0.05 / cos(radians(f.lat))
                 and f.lon + 0.05 / cos(radians(f.lat))
  where metres <= 1500
),
-- one holder's many mapped buildings are one candidate, nearest first
site as (
  select flare, coalesce(operator, name) as holder, min(metres) as metres, round(arg_min(bearing, metres)) as bearing,
    arg_min(id, metres) as nearest_id, arg_min(name, metres) as name,
    list(distinct kind order by kind) as kinds,
    list(distinct provider order by provider) as providers,
    count(*) as features
  from near
  where coalesce(operator, name) is not null
    and not list_contains(['flare'] || areas() || noise(), kind)
  group by all
),
sites as (
  select flare as id, list(site order by metres)[:8] as sites,
    count(*) as candidates
  from (select flare, metres, struct_pack(holder, name, metres, bearing, nearest_id,
      kinds, providers, features)::json as site from site) group by flare
),
inside as (
  select flare as id, list(distinct struct_pack(kind, name, operator, id)::json)
    as areas
  from near where metres = 0 and list_contains(areas(), kind) group by flare
),
stacks as (
  select flare as id, count(*) as stacks
  from near where kind = 'flare' and metres <= 250 group by flare
),
-- other catalogued flares close by, which often stand at the same works
neighbours as (
  select f.id, list({id: g.id, metres: round(m(f.lat, f.lon, g.lat, g.lon))}
      order by m(f.lat, f.lon, g.lat, g.lon)) as neighbours
  from f join read_parquet(bucket('eog/flares/data.parquet')) g
    on g.id <> f.id
   and g.lat between f.lat - 0.03 and f.lat + 0.03
   and g.lon between f.lon - 0.045 and f.lon + 0.045
  where m(f.lat, f.lon, g.lat, g.lon) <= 3000
  group by f.id
)
select f.id as key, {
    id: f.id, lat: f.lat, lon: f.lon, cell: f.cell, country: f.country,
    eog_class: f.detail, first_seen: f.first_seen, last_seen: f.last_seen,
    archive_found: coalesce(list_contains(f.flags, 'unprofiled'), false),
    mean_temp_k: y.temp_k, years: y.years,
    mapped_stacks_within_250m: coalesce(k.stacks, 0),
    candidate_sites_within_1500m: coalesce(s.candidates, 0),
    nearest_sites: s.sites, inside_areas: i.areas, flares_within_3km: n.neighbours
  }::json::varchar as data
from f
left join years y using (id)
left join sites s using (id)
left join stacks k using (id)
left join inside i using (id)
left join neighbours n using (id);
