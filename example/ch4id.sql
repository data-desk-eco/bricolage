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
  constraint "paragraph is at least fifteen words"
    check (length(trim(paragraph)) - length(replace(trim(paragraph), ' ', '')) >= 14),
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
  paragraph     text not null,
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

The key is the `id` of one row of `plumes()`, verbatim, and `src` there says
who detected it: a Carbon Mapper id looks like `tan20250101t113529c00s4001-A`,
an IMEO id is a bare uuid, an SRON id is `sron_20230304_32.20N_93.35W`, a
Data Desk id starts `DD:`. Read `archive` first, then your row, then
`attributions()` near it; `imagery` before your first picture; `carbon-mapper`
when the key is a Carbon Mapper record; `web` before your first search.

- `source_label` names the source in one to eight words.
- `source_kind` is what the methane comes out of, not where you read about it.
- `attributed_ids` holds every archive feature id for the site, the site''s
  own first, spelt as `features()` spells them.
- `lat` and `lon` are the assessed source position.
- high or medium `confidence` needs a url in `evidence`; a low answer that
  says what it does not know beats no answer at all.
- `paragraph` says what you saw, what you read, and how they agree.
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
1. precedent: `attributions()` rows near the plume or naming the same
   operator that already carry an `operator_id`; confirm it still applies.
2. the features: a `GEM:` feature id leads through `owners()` to the GEM
   entities that own it, and `entities()` gives their `lei` and `permid`.
3. the names: `entities()` and `gleif()` by name, trading and local names
   included.
- `operator_id` is the operating entity, not its ultimate parent: `lei:`
  where one exists, else `gem:`, else `permid:`. Copy it from an
  `entities()` or `gleif()` row you queried: an id found anywhere else, a
  GEM wiki page included, is not an answer.
- `operator_name` is that row''s name, verbatim.
- both are null when no register holds the entity; say so in `paragraph`.
- `paragraph` names the rows you matched and why they are the same entity.',
  './skills/archive'
);
