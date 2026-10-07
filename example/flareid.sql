.load ./ext/bric

-- flareid: names the site each VIIRS Nightfire flare stands at, what the site
-- does, who runs it and the group behind it, then resolves each operator to a
-- register entity.
--
--   sqlite3 flareid.db < example/flareid.sql
--   duckdb -c ".read example/flareid-seed.sql" -c "
--     attach 'flareid.db' as db (type sqlite);
--     insert into db.flare select * from record(['17412'])
--     where key not in (select key from db.flare)"
--   sqlite3 flareid.db '.load ./ext/bric'
--
-- and out, in the shape data/flareid.parquet has always had:
--
--   duckdb -c "attach 'flareid.db' as db (type sqlite, read_only);
--     copy (select s.key as record_id, site_label, site_name, sector,
--       gas_flare::boolean as gas_flare, s.operator, parent,
--       from_json(feature_ids, '[\"varchar\"]') as feature_ids, lat, lon,
--       confidence, paragraph, from_json(evidence, '[\"varchar\"]') as evidence,
--       operator_id, operator_name, parent_id, parent_name
--     from db.flare_site s left join db.claim_operator o on o.key = s.operator)
--     to 'flareid.parquet'"

-- the to-do table: a flare's id and the seed's record of it, which the engine
-- hands the agent as its first message
create table if not exists flare (key text primary key, data text);

create table if not exists flare_site (
  key         text primary key,
  site_label  text not null,
  site_name   text,
  sector      text not null check (sector in (
    'upstream_oil', 'upstream_gas', 'gas_processing', 'gas_transport', 'lng',
    'refinery', 'petrochemical', 'chemical', 'steel', 'coke',
    'metallurgy_other', 'cement', 'landfill', 'biogas', 'power',
    'industrial_other', 'volcano', 'none')),
  gas_flare   integer not null check (gas_flare in (0, 1)),
  operator    text,
  parent      text,
  feature_ids text not null default '[]',
  lat         real,
  lon         real,
  confidence  text not null check (confidence in ('high', 'medium', 'low')),
  paragraph   text not null,
  evidence    text,
  run_at      text default (datetime('now')),
  constraint "site_label is one to eight words"
    check (length(trim(site_label)) > 0
       and length(site_label) - length(replace(site_label, ' ', '')) < 8),
  constraint "sector none has no position and no features, and any other has a position"
    check ((sector = 'none') = (lat is null and lon is null)
       and (lat is null) = (lon is null)
       and (sector <> 'none' or feature_ids = '[]')),
  constraint "lat and lon are degrees"
    check (lat between -90 and 90 and lon between -180 and 180),
  constraint "a volcano is not a gas flare"
    check (sector <> 'volcano' or gas_flare = 0),
  constraint "a parent differs from the operator: leave it empty when the operator is its own parent"
    check (parent is null or parent <> operator),
  constraint "feature_ids is a json array of archive ids"
    check (json_valid(feature_ids) and json_type(feature_ids) = 'array'),
  constraint "paragraph is 60 to 100 words"
    check (length(trim(paragraph)) - length(replace(trim(paragraph), ' ', ''))
           between 59 and 99),
  constraint "paragraph names no function: write for a reader, not the tools"
    check (paragraph not like '%()%'),
  constraint "evidence is a json array of urls, or null"
    check (evidence is null
        or (json_valid(evidence) and json_type(evidence) = 'array'))
);

create trigger if not exists flare_site_ids before insert on flare_site
  when exists (select 1 from json_each(new.feature_ids)
                where substr(value, 1, instr(value, ':') - 1)
                      not in ('OGIM', 'OSM', 'GEM', 'MPS', 'GOGI', 'OVT', 'DD'))
begin
  select raise(abort,
    'every feature id is an archive feature id, prefixed OGIM:, OSM:, GEM:, MPS:, GOGI:, OVT: or DD:');
end;

-- the record lists the licence, field and basin outlines the flare is inside.
-- one of those is where the site is, not the site
create trigger if not exists flare_site_area before insert on flare_site
  when new.feature_ids ->> '$[0]' in (
    select a.value ->> 'id' from flare f, json_each(f.data, '$.inside_areas') a
     where f.key = new.key)
begin
  select raise(abort,
    'the first feature id is the facility that burns, never a licence, field or basin outline');
end;

-- one place, one name: the sessions share this table, so a site another flare
-- already answered keeps that answer's name and operator
create trigger if not exists flare_site_same before insert on flare_site
  when exists (select 1 from flare_site s
    where s.feature_ids ->> '$[0]' = new.feature_ids ->> '$[0]'
      and (s.site_name is not new.site_name or s.operator is not new.operator))
begin
  select raise(abort,
    'another flare names this first feature with a different site_name or operator: copy them from flare_site, or put first the feature of the site this flare stands at');
end;

create trigger if not exists flare_site_spelling before insert on flare_site
  when exists (select 1 from flare_site s, (select new.operator as v union all
                                            select new.parent) n
    where n.v is not null
      and lower(replace(replace(replace(replace(n.v, ' ', ''), '.', ''), ',', ''), '-', ''))
        in (lower(replace(replace(replace(replace(s.operator, ' ', ''), '.', ''), ',', ''), '-', '')),
            lower(replace(replace(replace(replace(s.parent, ' ', ''), '.', ''), ',', ''), '-', '')))
      and n.v not in (s.operator, coalesce(s.parent, '')))
begin
  select raise(abort,
    'this company is already in flare_site under another spelling: use that spelling exactly');
end;

-- the archive is where a lead is found, not what it rests on: its tables
-- hold other publishers' records, and the evidence is those publishers' pages
create trigger if not exists flare_site_archive before insert on flare_site
  when exists (select 1 from json_each(new.evidence)
                where value like '%cloudferro.com/%')
begin
  select raise(abort,
    'evidence is never an archive url: cite the source behind the row');
end;

create trigger if not exists flare_site_confidence before insert on flare_site
  when new.confidence in ('high', 'medium')
   and (new.evidence is null or json_array_length(new.evidence) = 0)
begin
  select raise(abort,
    'high or medium confidence needs at least one evidence url, or drop to low');
end;

-- a url is evidence only if this flare's session met it: in a tool's output,
-- a command other than the insert, or a search result. one written from
-- memory is refused, and so is an imagery request, which is a picture and
-- not a source
create trigger if not exists flare_site_evidence before insert on flare_site
  when exists (select 1 from json_each(new.evidence) e
    where e.value like '%/MapServer/export?%'
       or not exists (select 1 from bric_log l where l.key = new.key and (
      l.kind = 'receipt' and instr(l.text, e.value)
      or l.kind = 'call' and l.detail not like '{"command":"db %'
        and instr(l.detail, e.value)
      or l.kind = 'reply' and exists (select 1 from json_each(l.detail) b
        where b.value ->> 'type' = 'web_search_tool_result'
          and instr(b.value, e.value)))))
begin
  select raise(abort,
    'evidence holds only urls this session met, and never imagery');
end;

-- the sweep: each operator, once, resolved to the register entity behind it
-- and to its group, so the published row carries ids and not only strings
create table if not exists claim (key text primary key, data text);

create trigger if not exists flare_site_claim after insert on flare_site
  when new.operator is not null
begin
  insert or ignore into claim values (new.operator, json_object(
    'operator', new.operator, 'parent', new.parent,
    'site_name', new.site_name, 'country', (select data ->> '$.country' from flare where key = new.key),
    'feature_ids', json(new.feature_ids)));
end;

create table if not exists claim_operator (
  key           text primary key,
  operator_id   text,
  operator_name text,
  parent_id     text,
  parent_name   text,
  confidence    text not null check (confidence in ('high', 'medium', 'low')),
  paragraph     text not null check (paragraph not like '%()%'
                                    and length(paragraph) < 500),
  constraint "an id is lei:<20 chars>, gem:E<digits> or permid:<digits>"
    check ((operator_id is null
         or operator_id glob 'lei:*' and length(operator_id) = 24
            and substr(operator_id, 5) not glob '*[^0-9A-Z]*'
         or operator_id glob 'gem:E[0-9]*'
            and substr(operator_id, 6) not glob '*[^0-9]*'
         or operator_id glob 'permid:[0-9]*'
            and substr(operator_id, 8) not glob '*[^0-9]*')
       and (parent_id is null
         or parent_id glob 'lei:*' and length(parent_id) = 24
            and substr(parent_id, 5) not glob '*[^0-9A-Z]*'
         or parent_id glob 'gem:E[0-9]*'
            and substr(parent_id, 6) not glob '*[^0-9]*'
         or parent_id glob 'permid:[0-9]*'
            and substr(parent_id, 8) not glob '*[^0-9]*')),
  constraint "an id has its register name and no id has none"
    check ((operator_id is null) = (operator_name is null)
       and (parent_id is null) = (parent_name is null)),
  constraint "a parent is another entity: leave it empty when the operator is its own parent"
    check (parent_id is null or parent_id is not operator_id)
);

insert or replace into bric_job (source, target, brief, skills) values (
  'flare',
  'flare_site',
  'You are an expert on industrial gas flaring. Identify the site one
satellite-detected flare stands at, what the site does, who operates it and
the group that owns the operator. Your findings will be published and
measured against the EU Methane Regulation, which binds oil and gas sites
and no others, so the sector matters as much as the name. Be careful,
specific and honest about uncertainty.

The message is the flare''s record from VIIRS Nightfire, which detects hot
spots at night from about 750 m pixels. It gives the position, EOG''s class,
first and last detection, each year''s lit nights, mean flame temperature and
a gas volume that holds only if gas is burning, the mapped flare stacks
within 250 m, the mapped sites within 1,500 m collapsed by holder with the
distance to each outline (0 is inside it), the licence, field and basin
outlines the flare lies in, and the other catalogued flares within 3 km.
archive_found means the flare is missing from EOG''s frozen index and was
found in the nightly record, so it is probably recent.

Method

1. Read the record. The position is a centroid over years of detections,
   good to about 200 m. A mapped stack within 250 m confirms the position
   but not whose it is.
2. Check flare_site for the flares within 3 km:
   db "select * from flare_site where key in (''17383'', ''17416'')"
   Several flares often stand at one works. If one is already answered and
   this flare is at the same site, reuse its site_name, operator, parent
   and first feature ID exactly. Before you write any company name, check
   how flare_site already spells it:
   db "select distinct operator, parent from flare_site where operator like ''%MOL%''"
3. List what is mapped around the position. The record has the nearest
   sites; query the archive with near() for more, or wider if nothing
   within 1,500 m explains the flame.
4. Look at the ground. Take a wide picture, 3 to 5 km, to read the plant
   as a whole, then a close one, about 1 km, to find the stack. Cracking
   towers and tank farms are a refinery; blast furnaces, coke ovens and
   slag yards are a steel works; a long kiln beside a quarry is cement;
   terraced mounds with gas wells are a landfill; a fenced pad with
   separators and tanks among wells is an oil or gas site. What is
   standing at the position beats a mapped outline kilometres away.
5. Decide whether gas burns. A hydrocarbon flare burns near 1,800 K.
   Molten slag, a torpedo car, a kiln mouth and lava are hot surfaces
   near 1,000 to 1,400 K, catalogued alongside real flares. Use the
   temperature, what is at the position and what you can read.
6. Identify the operator and its group from permits, regulator and
   mining registers, operator websites, environmental impact documents
   and dated reports. Search in the country''s own language as well as
   English: a Hungarian permit, a Romanian ANRM licence or an Italian
   AIA decree names the plant and its operator where English sources do
   not. Read a page before you rely on it.

Names, sectors and grain

- Name the facility that burns, at the grain of the thing itself: the
  gathering station, gas plant, well site, compressor station, refinery
  or steel works. A concession, licence, field or basin is where a site
  is, not the site. If six wells share one field, the flare is at one of
  them or at the field''s station, and the name says which.
- Prefer the specific to the generic. A refinery inside an industrial
  estate is the answer; the estate is not. A steel works and the power
  station burning its blast furnace gas are one site: name the works.
- Beyond about 1,000 m a candidate needs a reason, such as a large site
  whose stack is at its edge.
- EOG''s class is a prior, not evidence. It has no term for a gas plant or
  compressor station, and files LNG terminals and oil centres as
  refineries often enough to check every time.
- Provider strings are not clean. GEM appends a working interest, so
  "Repsol SA [100%]" is Repsol SA. MapStand drops accents. Give the
  company''s own name in its own spelling, with its legal form.

Earlier attributions in the archive are other models'' unreviewed claims.
Use them only as leads to check.

Output

- site_label: what burns, in one to eight plain words, such as "Pettend
  oil field gathering station flare".
- site_name: the facility''s own name, as its operator or permit gives it.
- sector: what the site does, not what it burns.
  - upstream_oil, upstream_gas: production, from the well to the field''s
    treatment and gathering stations, by the main product.
  - gas_processing: gas plants that treat or strip gas.
  - gas_transport: transmission compressor stations, underground storage
    and pipeline stations.
  - lng: liquefaction, regasification and LNG terminals.
  - refinery, petrochemical, chemical, steel, coke, metallurgy_other,
    cement, landfill, biogas, power, industrial_other, volcano.
  - none: no site explains the flame. Say what you looked for.
- gas_flare: 1 when gas or vapour burns, 0 for a hot surface, kiln,
  furnace mouth or volcano.
- operator: the company that runs the site.
- parent: the group that controls the operator; empty when the operator is
  its own parent. For a joint venture with no controlling owner, name
  each owner with its share, such as "MVM CEEnergy Zrt. 50% / Horizon
  General Kft. 50%".
- feature_ids: the archive IDs of the mapped features of this site, the
  facility that burns first, then its other features. The first ID is what
  groups flares into one facility, so choose it for the site. Never put
  a licence, field or basin outline first. Empty for none.
- lat, lon: the position of the site, from its outline or the stack you
  saw, not a copy of the flare position.
- confidence: certainty that the flare is at this site, ignoring the
  operator.
  - high: the flare is on the site, the plant is seen, and no other
    candidate is that close.
  - medium: the most likely of a few candidates, or certain only of the
    field or estate.
  - low: no plant seen, or several fit.
  High or medium needs a URL in evidence.
- paragraph: 60 to 100 words for a reader who has not seen your work:
  the site and what it does, the evidence that places the flare there,
  what the imagery shows, and what remains uncertain. Plain English,
  short sentences. No tools, tables, IDs or account of your steps.
- evidence: the URLs you read that support the row. An archive file is
  not evidence: cite the provider''s page or the record the row came from.

An honest low-confidence answer is better than a confident guess.',
  './skills/{archive,imagery,web}'
);

insert or replace into bric_job (source, target, brief, skills) values (
  'claim',
  'claim_operator',
  'You are an expert on corporate ownership in the oil, gas and heavy
industry sectors. Colleagues have named the operator of one or more flaring
sites, and its parent group. Match the operator, and its parent, to the
legal entities a company register holds for them. The message gives the
operator, the parent, one site, its country and its feature IDs. The key is
the operator''s name. Read the archive skill first.

Method

1. If the features include a GEM asset, find its owners in GEM''s
   ownership records. GEM''s company register gives each owner''s LEI and
   PermID, and its parents.
2. Otherwise, search GEM''s company register and GLEIF by name. Try the
   name in its original language and script as well as in English, and
   trading and former names. Use the country to choose between namesakes.
3. The operator is the entity that runs the site, not its group. If no
   register holds the operator itself, leave its ID empty. Do not
   substitute the parent.
4. The parent is the entity that ultimately controls the operator. Follow
   GEM''s parent links or the operator''s own annual report or website. If
   the operator is its own parent, or control is split with no
   controlling owner, leave the parent empty and say so.

Take every ID from a register record you have read, never from an earlier
attribution.

Output

- operator_id: the operator''s LEI as lei:<LEI> if it has one, otherwise
  its GEM ID as gem:<ID>, otherwise its PermID as permid:<ID>. Leave it
  empty if no register holds the entity.
- operator_name: the entity''s name exactly as the register spells it.
  Leave it empty when operator_id is empty.
- parent_id, parent_name: the same, for the controlling parent.
- confidence: how certain you are of the operator''s entity.
  - high: an ownership record for the site''s own feature names it.
  - medium: the names match and the country fits, without an ownership
    record.
  - low: only the name matches, or several entities fit.
- paragraph: one to three plain sentences on why these are the entities,
  or why no register holds them. Do not mention tools or tables.',
  './skills/{archive,web}'
);
