.load ./ext/bric

-- two jobs in a chain over a leak on an aleph server: one lists the oil
-- and gas cargoes a collection documents, the next fills in each cargo.
-- rows cite an aleph entity, read with the aleph skill, never a web page.
-- the skill reads ALEPH_URL and ALEPH_API_KEY from the environment:
-- `insert into leak values ('ALET/АЛЕТ Leak 2024')` runs the pipeline.

create table if not exists leak (key text primary key);

create table if not exists leak_cargo (
  key        text not null,
  lead       text not null,
  source     text not null,
  quote      text not null,
  primary key (key, lead),
  constraint "a lead is a sentence, not a paragraph"
    check (length(lead) <= 200),
  constraint "source is an aleph entity url"
    check (source like 'http%/entities/%')
);

drop trigger if exists leak_cargo_cite;
create trigger leak_cargo_cite
before insert on leak_cargo
begin
  select cites(new.source, new.quote);
end;

-- a cargo's key is only a task id: what the cargo is, its vessel and
-- loading date, is for the second job to find and cite.
create table if not exists cargo (
  key    integer primary key,
  leak   text not null,
  lead   text not null,
  source text not null,
  quote  text not null,
  unique (leak, lead)
);

create trigger if not exists leak_cargo_seed
after insert on leak_cargo
begin
  insert or ignore into cargo (leak, lead, source, quote)
  values (new.key, new.lead, new.source, new.quote);
end;

create table if not exists cargo_fact (
  key        integer not null,
  field      text not null check (
    field in ('vessel', 'commodity', 'quantity', 'seller', 'buyer',
              'shipper', 'consignee', 'notify party', 'charterer',
              'shipowner', 'load port', 'discharge port', 'load date',
              'discharge date', 'price', 'contract', 'bank', 'inspector', 'agent')
  ),
  value      text not null,
  source     text not null,
  quote      text not null,
  run_at     text default (datetime('now')),
  primary key (key, field, value),
  constraint "one value per row" check (value not like '%;%'),
  constraint "a date is YYYY-MM-DD"
    check (field not like '% date' or value glob
      '[12][0-9][0-9][0-9]-[01][0-9]-[0-3][0-9]'),
  constraint "source is an aleph entity url"
    check (source like 'http%/entities/%')
);

drop trigger if exists cargo_fact_cite;
create trigger cargo_fact_cite
before insert on cargo_fact
begin
  select cites(new.source, new.quote);
end;

insert or replace into bric_job (source, target, brief, skills, params) values (
  'leak',
  'leak_cargo',
  'You find the cargoes of crude oil, oil products, LNG or LPG that one
leaked collection documents or mentions. The key is the collection''s name
on the aleph server; find its id with `aleph /collections q=...`. Read
`aleph` before your first search, and search only within that collection.

A cargo is one shipment on one vessel. Search for the documents that
record them, in the collection''s languages: bills of lading, manifests,
certificates of quantity, nominations, sale contracts, invoices, and the
emails and reports that mention a shipment. Insert one row a cargo you find
evidence of, however little you know of it: `lead` says what you know in a
sentence, as in `fuel oil on the CONTI BENGUELA, St Petersburg to
Rotterdam, March 2018` or `a naphtha cargo NewCoal sold to Regalway in
November 2018, vessel unnamed`. Another agent will work each lead out, so
list cargoes rather than researching them, and aim for twenty. Market
reports and price assessments are not cargoes, and one cargo in several
documents is one row.

`source` is the url of the aleph entity you read it in and `quote` is a
phrase from it about that cargo, exactly as printed.',
  './skills/aleph',
  '{"max_tokens": 16384}'
);

insert or replace into bric_job (source, target, brief, skills, params) values (
  'cargo',
  'cargo_fact',
  'You work out one oil or gas cargo from a leaked collection on an aleph
server. You are given a lead: the collection, what another agent found and
the document it found it in, quoted. Read `aleph` first, then that
document, its folder (`parent`) and the documents that name the same
vessel, parties or dates: the contract, invoice, letter of credit,
nomination, certificate of quantity and quality, and the emails around
them.

Insert one row a fact: the vessel, the commodity and quantity with its unit, the
seller, buyer, shipper, consignee, notify party, charterer and shipowner,
the load and discharge ports, the load and discharge dates, the price
or its formula, the contract''s number and date, the banks and the
inspector. Anything else is left out. `value` is one company, place,
date or figure as the document prints it, dates as `YYYY-MM-DD`. A field
the documents do not give is left out; conflicting documents give one row
each.

`source` is the url of the aleph entity you read it in and `quote` is a
phrase from it giving the value, exactly as printed. The web is only for
telling you what a name is, never a source.',
  './skills/aleph',
  '{"max_tokens": 16384}'
);

-- leads that turn out to be the same cargo share a vessel and load date
create view if not exists cargo_voyage as
select v.value as vessel, d.value as load_date,
  group_concat(distinct v.key) as cargoes
from cargo_fact v join cargo_fact d on d.key = v.key
where v.field = 'vessel' and d.field = 'load date'
