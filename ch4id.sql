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
