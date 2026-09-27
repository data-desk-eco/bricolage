.load ./ext/bric

-- two jobs in a chain over a leak on an aleph server: one lists the oil
-- and gas cargoes a collection documents, the next fills in each cargo.
-- rows cite an aleph entity, read with the aleph skill, never a web page.
-- the skill reads ALEPH_URL and ALEPH_API_KEY from the environment:
-- `insert into leak values ('ALET/АЛЕТ Leak 2024')` runs the pipeline.

create table if not exists leak (key text primary key);

create table if not exists leak_cargo (
  key        text not null,
  cargo      text not null,
  source     text not null,
  quote      text not null,
  primary key (key, cargo),
  constraint "a cargo is its vessel and loading date, as `VESSEL YYYY-MM-DD`"
    check (cargo glob '* [12][0-9][0-9][0-9]-[01][0-9]-[0-3][0-9]'
      and cargo = upper(cargo)),
  constraint "source is an aleph entity url"
    check (source like 'http%/entities/%')
);

drop trigger if exists leak_cargo_cite;
create trigger leak_cargo_cite
before insert on leak_cargo
begin
  select cites(new.source, new.quote);
end;

create table if not exists cargo (key text primary key);

create trigger if not exists leak_cargo_seed
after insert on leak_cargo
begin
  insert or ignore into cargo (key) values (new.cargo);
end;

create table if not exists cargo_fact (
  key        text not null,
  field      text not null check (
    field in ('commodity', 'quantity', 'seller', 'buyer', 'shipper',
              'consignee', 'notify party', 'charterer', 'shipowner',
              'load port', 'discharge port', 'load date', 'discharge date',
              'price', 'contract', 'bank', 'inspector', 'agent')
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
  'You list the cargoes of crude oil, oil products, LNG or LPG that one leaked
collection documents. The key is the collection''s name on the aleph server;
find its id with `aleph /collections q=...`. Read `aleph` before your first
search, and search only within that collection.

A cargo is one shipment on one vessel: a bill of lading, a cargo manifest,
a certificate of quantity, a nomination or a sale contract naming the
vessel. Search for those documents in the collection''s languages, read
each, and insert one row a cargo: `cargo` is the vessel''s name and the
date loading finished (the bill of lading date), upper case, as in
`CONTI BENGUELA 2018-03-12`. Market reports and price assessments are not
cargoes. Aim for twenty cargoes; the same cargo in several documents is one
row.

`source` is the url of the aleph entity you read it in and `quote` is a
phrase from it naming the vessel, exactly as printed.',
  './skills/aleph',
  '{"max_tokens": 16384}'
);

insert or replace into bric_job (source, target, brief, skills, params) values (
  'cargo',
  'cargo_fact',
  'You fill in one oil or gas cargo from a leaked collection on an aleph
server. The key is the vessel and loading date, as in `CONTI BENGUELA
2018-03-12`; `db "select * from leak_cargo where cargo = ''<key>''"` gives
the collection and the document it was found in. Read `aleph` first, then
that document, its folder (`parent`) and the documents that name the same
vessel near that date: the contract, invoice, letter of credit, nomination,
certificate of quantity and quality, and the emails around them.

Insert one row a fact: the commodity and quantity with its unit, the
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
