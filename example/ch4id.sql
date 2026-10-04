.load ./ext/bric

-- the to-do table: a plume's key and the seed's record of it, which the
-- engine hands the agent as its first message
create table if not exists plume (key text primary key, data text);

create table if not exists plume_source (
  key            text primary key,
  source_label   text not null,
  source_kind    text not null check (
    source_kind in ('well', 'facility', 'pipeline', 'mine', 'landfill', 'other', 'none')
  ),
  source_name    text,
  operator       text,
  attributed_ids text,
  lat            real,
  lon            real,
  confidence     text not null check (
    confidence in ('high', 'medium', 'low')
  ),
  paragraph      text not null,
  evidence       text,
  run_at         text default (datetime('now')),
  constraint "source_label is one to eight words"
    check (length(trim(source_label)) > 0
       and length(source_label) - length(replace(source_label, ' ', '')) < 8),
  constraint "a source has a position and none has none"
    check ((source_kind = 'none') = (lat is null and lon is null)
       and (lat is null) = (lon is null)),
  constraint "lat and lon are degrees"
    check (lat between -90 and 90 and lon between -180 and 180),
  constraint "attributed_ids is a json array of archive ids, or null"
    check (attributed_ids is null
        or (json_valid(attributed_ids) and json_type(attributed_ids) = 'array')),
  constraint "paragraph is 60 to 100 words"
    check (length(trim(paragraph)) - length(replace(trim(paragraph), ' ', ''))
           between 59 and 99),
  constraint "paragraph names no function: write for a reader, not the tools"
    check (paragraph not like '%()%'),
  constraint "evidence is a json array of urls, or null"
    check (evidence is null
        or (json_valid(evidence) and json_type(evidence) = 'array'))
);

create trigger if not exists plume_source_ids before insert on plume_source
  when new.attributed_ids is not null
   and exists (select 1
                 from json_each(new.attributed_ids)
                where substr(value, 1, instr(value, ':') - 1)
                      not in ('OGIM', 'OSM', 'GEM', 'MPS', 'GOGI', 'OVT', 'DD'))
begin
  select raise(abort,
    'every attributed id is an archive feature id, prefixed OGIM:, OSM:, GEM:, MPS:, GOGI:, OVT: or DD:');
end;

-- the archive is where a lead is found, not what it rests on: its tables
-- hold other publishers' records, and the evidence is those publishers' pages
create trigger if not exists plume_source_archive before insert on plume_source
  when exists (select 1 from json_each(new.evidence)
                where value like '%cloudferro.com/%')
begin
  select raise(abort, 'evidence is never an archive url: cite the page '
    || 'or record the archive row came from');
end;

create trigger if not exists plume_source_confidence before insert on plume_source
  when new.confidence in ('high', 'medium')
   and (new.evidence is null or json_array_length(new.evidence) = 0)
begin
  select raise(abort,
    'high or medium confidence needs at least one evidence url, or drop to low');
end;

-- the sweep: a claim that names an operator is resolved to the register
-- entity behind it, so the published row carries an id and not only a string
create table if not exists claim (
  key text primary key, operator text, source_name text, attributed_ids text
);

create trigger if not exists plume_source_claim after insert on plume_source
  when new.operator is not null
begin
  insert or ignore into claim
  values (new.key, new.operator, new.source_name, new.attributed_ids);
end;

create table if not exists claim_operator (
  key           text primary key,
  operator_id   text,
  operator_name text,
  confidence    text not null check (confidence in ('high', 'medium', 'low')),
  paragraph     text not null check (paragraph not like '%()%'
                                    and length(paragraph) < 400),
  constraint "operator_id is lei:<20 chars>, gem:E<digits> or permid:<digits>"
    check (operator_id is null
        or operator_id glob 'lei:*' and length(operator_id) = 24
           and substr(operator_id, 5) not glob '*[^0-9A-Z]*'
        or operator_id glob 'gem:E[0-9]*'
           and substr(operator_id, 6) not glob '*[^0-9]*'
        or operator_id glob 'permid:[0-9]*'
           and substr(operator_id, 8) not glob '*[^0-9]*'),
  constraint "an id has its register name and no id has none"
    check ((operator_id is null) = (operator_name is null))
);

insert or replace into bric_job (source, target, brief, skills) values (
  'plume',
  'plume_source',
  'You are an expert methane emissions analyst. Identify the most likely
source of one methane plume. Your findings will be published and may be
used to hold the operator to account, so be careful, specific and honest
about uncertainty.

The message is the plume''s record. Its ID has a prefix that names the
provider: CM (Carbon Mapper), IMEO, SRON or DD (Data Desk). The record
gives the sensor, date, position, emission rate, a search radius for the
sensor, the day''s wind, the nearest mapped features with distance and
bearing, earlier detections nearby, and links to satellite pictures.
Start from the record. Use the archive skill to look further.

Method

1. Set the search area from the sensor''s accuracy:
   - TROPOMI (most IMEO and SRON plumes): the source is usually 2 to
     10 km from the position, often upwind.
   - GOES: several kilometres. VIIRS and Sentinel-3: about a kilometre.
   - Carbon Mapper aircraft: tens of metres. Tanager, EMIT, Sentinel-2,
     Landsat, EnMAP and PRISMA: up to a few hundred metres.
   The record''s search radius follows these figures. Do not move a
   precise position upwind: the wind explains the plume''s shape, not a
   different origin.
2. List the mapped infrastructure in that area. The record has the
   nearest features; query the archive if the area needs more.
3. Check the strongest candidates in satellite imagery. Unmapped
   equipment at a precise position beats a mapped facility kilometres
   away.
4. Check the plume''s history: repeated detections at one position mean a
   persistent source.
5. Identify the operator from regulator records, permits, operator
   websites, dated reports and anything else useful you find online.
   Search in the country''s own languages as well as English: most records
   for Russia, Central Asia, the Middle East, North Africa, Latin America
   and China are published only in Russian, Arabic, Spanish, Portuguese
   or Chinese. Read a page before you rely on it.

Earlier attributions in the archive are other models'' unreviewed
claims. Use them only as leads to check.

Output

- source_label: the source, in one to eight words.
- source_kind: the type of equipment that emits the methane.
- source_name, operator: the facility and its operator, as sources name
  them.
- attributed_ids: the archive IDs of the mapped features at the site,
  the facility or equipment that emits first. Search the archive near
  the source for its ID. A licence area, field or basin outline is not
  a source: leave it out.
- lat, lon: the position of the source. Use the plume''s position only if
  the source is there.
- confidence: certainty that the methane comes from this site, ignoring
  the operator and the facility''s name.
  - high: the plume is on the site within the sensor''s accuracy, the
    equipment is seen, and no other candidate is that close.
  - medium: the most likely of a few candidates, or clear only as an
    area such as a field''s pads or a pipeline corridor.
  - low: a coarse position over several candidates, or no equipment
    seen.
  High or medium needs a URL in evidence.
- paragraph: 80 to 100 words for a reader who has not seen your work:
  the source, the evidence that places the emission there (including
  what the imagery shows) and what remains uncertain. Plain English,
  short sentences. No tools, tables, IDs or account of your steps.
- evidence: the URLs you read. An archive file is not evidence: cite
  the provider''s page or the record the row came from.

An honest low-confidence answer is better than a confident guess.',
  './skills/{archive,imagery,carbon-mapper,web}'
);

insert or replace into bric_job (source, target, brief, skills) values (
  'claim',
  'claim_operator',
  'You are an expert methane emissions analyst. A colleague has attributed
a plume to a source and named its operator. Match that operator to the
legal entity a company register holds for it. The message gives the
operator, the source name and the site''s feature IDs. Read the archive
skill first.

Method

1. If the features include a GEM asset, find its owners in GEM''s
   ownership records. GEM''s company register gives each owner''s LEI and
   PermID.
2. Otherwise, search GEM''s company register and GLEIF by name. Try the
   name in its original language and script as well as in English, and
   trading and former names.
3. Choose the entity that operates the site, not its ultimate parent.
   If the operator is a subsidiary that no register holds, leave the ID
   empty. Do not substitute the parent.

Take every ID from a register record you have read, never from an
earlier attribution.

Output

- operator_id: the entity''s LEI as lei:<LEI> if it has one, otherwise
  its GEM ID as gem:<ID>, otherwise its PermID as permid:<ID>. Leave it
  empty if no register holds the entity.
- operator_name: the entity''s name exactly as the register spells it.
  Leave it empty when operator_id is empty.
- confidence: how certain you are that this entity operates the source.
  - high: an ownership record for the site''s own feature names it.
  - medium: the names match and the location fits, without an
    ownership record.
  - low: only the name matches, or several entities fit.
- paragraph: one or two plain sentences on why this entity is the
  operator, or why no register holds it. Do not mention tools or tables.',
  './skills/archive'
);
