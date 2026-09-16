.load ./ext/bric

-- two jobs in a chain: a coast's terminals, then each terminal's companies.
-- the first job's result table feeds the second's to-do table by trigger,
-- so `insert into coast values ('Mozambique')` runs the whole pipeline.

create table if not exists coast (key text primary key);

create table if not exists coast_terminal (
  key        text not null,
  terminal   text not null,
  status     text not null check (
    status in ('operating', 'under construction', 'fid', 'pre-fid')
  ),
  source     integer not null,
  quote      text not null,
  primary key (key, terminal)
);

create trigger if not exists coast_terminal_cite
before insert on coast_terminal
begin
  select raise(abort, 'quote not found in source')
  where not exists (
    select 1
    from bric_page('"' || replace(new.quote, '"', '""') || '"')
    where rowid = new.source
  );
end;

create table if not exists terminal (key text primary key);

create trigger if not exists coast_terminal_seed
after insert on coast_terminal
begin
  insert or ignore into terminal (key) values (new.terminal);
end;

create table if not exists terminal_party (
  key        text not null,
  role       text not null check (
    role in ('owner', 'operator', 'offtaker', 'feed', 'epc', 'liquefaction technology',
             'gas turbines', 'compressors', 'heat exchangers', 'storage tanks',
             'marine works', 'financing', 'other')
  ),
  company    text not null,
  scope      text,
  confidence text not null check (
    confidence in ('high', 'medium', 'low')
  ),
  source     integer not null,
  quote      text not null,
  run_at     text default (datetime('now')),
  primary key (key, role, company),
  constraint "scope is a phrase, not a paragraph"
    check (scope is null or length(scope) <= 120),
  constraint "one company per row"
    check (company not like '%,%' and company not like '% and %')
);

create trigger if not exists terminal_party_cite
before insert on terminal_party
begin
  select raise(abort, 'quote not found in source')
  where not exists (
    select 1
    from bric_page('"' || replace(new.quote, '"', '""') || '"')
    where rowid = new.source
  );
end;

insert or replace into bric_job (source, target, brief, skills, params) values (
  'coast',
  'coast_terminal',
  'You list the LNG export terminals of one country or coast. The key is the
place, e.g. `Mozambique` or `US Gulf Coast`. Read `web` before your first
search.

Insert one row per liquefaction project that is operating, under
construction, past final investment decision or proposed with a named
developer, floating ones included; import terminals are not wanted.
`terminal` is the project''s name as the industry knows it, e.g. `Rio Grande
LNG` or `Coral South FLNG`. Start from a tracker that lists them all (Global
Energy Monitor''s LNG terminal tracker, GIIGNL''s annual report, the IGU
world LNG report, a regulator''s project list) and confirm each on its
own page. A coast with many projects is still one list: insert every row in
one statement, in one turn, and expect twenty rows for a large exporter.

`source` is the receipt of the page you read it on and `quote` is a phrase
from that page naming the terminal, exactly as printed.',
  './skills/web',
  '{"max_tokens": 16384}'
);

insert or replace into bric_job (source, target, brief, skills, params) values (
  'terminal',
  'terminal_party',
  'You map who is building one LNG export terminal. The key is the terminal''s
name as the industry knows it, e.g. `Rio Grande LNG` or `Golden Pass`. Read
`web` before your first search.

Insert one row per company and role, a syndicate as one row a bank: the owner
and operator, the FEED and EPC contractors, the liquefaction technology
licensor, and the suppliers of the main equipment (gas turbines, compressors,
main cryogenic heat exchangers, storage tanks) and marine works. `scope` says
which trains or phase a contract covers, in a phrase. Prefer the contract award,
the operator''s own page or a regulator''s filing over a news roundup.

`source` is the receipt of the page you read it on and `quote` is a phrase
from that page naming the company in that role, exactly as printed. A role
you cannot find is left out; a low-confidence row says why in `scope`.
The first turn that ends with a row for your key closes the attempt, so
gather every role first, check each quote against `bric_page`, then insert
all your rows in one statement.',
  './skills/web',
  '{"max_tokens": 16384}'
);
