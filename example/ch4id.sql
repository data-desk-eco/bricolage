.load ./ext/bric

create table if not exists plume (key text primary key);

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
                      not in ('OGIM', 'OSM', 'GEM', 'MPS', 'OVT', 'DD'))
begin
  select raise(abort,
    'every attributed id is an archive feature id, prefixed OGIM:, OSM:, GEM:, MPS:, OVT: or DD:');
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
  'You attribute one methane plume to the most likely source: what you see in
the archive and on the ground, what you read, and how the pieces agree or
fail to.

The key is the `id` of one row of `plumes()`, and its prefix says who
detected it: `CM:` Carbon Mapper, `IMEO:`, `SRON:` or `DD:` Data Desk. Read
`archive` first, then your row; `imagery` before your first picture;
`carbon-mapper` when the key starts `CM:`; `web` before your first search.

Name the facility and its operator from the ground, the maps and what you
read. Earlier attributions near the plume are other models'' claims, not
evidence: use one only to find something to check, never as the answer.

- `source_label` names the source in one to eight words.
- `source_kind` is what the methane comes out of, not where you read about it.
- `attributed_ids` holds every archive feature id for the site, the site''s
  own first, spelt as `features()` spells them.
- `lat` and `lon` are where the source stands: the plume''s own coordinate
  only when the source is there, which a coarse sensor rarely shows.
- `confidence` is how sure you are that the methane comes from the site you
  name, and nothing else: not the operator, not the facility''s exact name.
  high: the sensor places the plume on that one site, closer than its own
  error, ground or imagery shows equipment there that vents or leaks
  methane, and no other candidate stands within the error.
  medium: that site is the likeliest of a few, or the source is clear only
  as a kind of place (a field''s pads, a pipeline corridor).
  low: a coarse position over several candidates, or no equipment seen.
  high or medium needs a url in `evidence`; a low answer that says what it
  does not know beats no answer at all.
- `paragraph` is for a reader who will never see your tools: 80 to 100
  words on the source, the evidence that places the methane there and what
  leaves doubt. Name no function, table, file, id or step of your own work,
  and do not narrate your search.
- `evidence` lists the urls you actually fetched.',
  './skills/{archive,imagery,carbon-mapper,web}'
);

insert or replace into bric_job (source, target, brief, skills) values (
  'claim',
  'claim_operator',
  'You resolve the operator one methane attribution names to the entity a
register knows it as. The key is the plume; the row gives the operator as the
attribution wrote it, the source name and the archive feature ids. Read
`archive` first.

Work in this order and stop at the first that answers:
1. the features: a `GEM:` feature id leads through `owners()` to the GEM
   entities that own it, and `entities()` gives their `lei` and `permid`.
2. the names: `entities()` and `gleif()` by name, trading and local names
   included.
Another attribution''s `operator_id` is a claim, not a register: never copy
one.
- `operator_id` is the operating entity, not its ultimate parent: `lei:`
  where one exists, else `gem:`, else `permid:`. Copy it from an
  `entities()` or `gleif()` row you queried: an id found anywhere else, a
  GEM wiki page included, is not an answer.
- `operator_name` is that row''s name, verbatim.
- both are null when no register holds the entity; say so in `paragraph`.
- `confidence` is how sure you are that the entity operates the source.
  high: an ownership record for the site''s own feature names it. medium: the
  names match and the place fits, with no ownership record. low: a name
  match only, or several entities fit.
- `paragraph` says in one or two sentences why the register entity is the
  operator named, in plain words: no function, table or step of your own.',
  './skills/archive'
);
