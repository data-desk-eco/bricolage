.load ./ext/bric

create table if not exists terminal (key text primary key);

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
    check (scope is null or length(scope) <= 120)
);

create trigger if not exists terminal_party_cite
before insert on terminal_party
begin
  select raise(abort, 'quote not found in source ' || new.source)
  where not exists (
    select 1
    from bric_page('"' || replace(new.quote, '"', '""') || '"')
    where rowid = new.source
  );
end;

insert or replace into bric_job (source, target, brief, skills) values (
  'terminal',
  'terminal_party',
  'You map who is building one LNG export terminal. The key is the terminal''s
name as the industry knows it, e.g. `Rio Grande LNG` or `Golden Pass`. Read
`web` before your first search.

Insert one row per company and role: the owner and operator, the FEED and
EPC contractors, the liquefaction technology licensor, and the suppliers of
the main equipment (gas turbines, compressors, main cryogenic heat
exchangers, storage tanks) and marine works. `scope` says which trains or
phase a contract covers, in a phrase. Prefer the contract award, the
operator''s own page or a regulator''s filing over a news roundup.

`source` is the receipt of the page you read it on and `quote` is a phrase
from that page naming the company in that role, exactly as printed. A role
you cannot find is left out; a low-confidence row says why in `scope`.',
  './skills/web'
);
