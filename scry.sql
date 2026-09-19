.load ./ext/bric

create table if not exists site (key text primary key);

create table if not exists site_record (
  key      text primary key,
  operator text,
  pages    integer not null check (pages >= 0),
  relation text not null,
  url      text,
  quote    text,
  source   integer,
  summary  text not null,
  constraint "a quote has a url and a receipt, and no quote has neither"
    check ((quote is null) = (url is null) and (quote is null) = (source is null)),
  constraint "summary is at least fifteen words"
    check (length(trim(summary)) - length(replace(trim(summary), ' ', '')) >= 14)
);

create trigger if not exists site_record_cite before insert on site_record
  when new.quote is not null
begin
  select raise(abort, 'quote not found in source: copy it from one scry receipt, within one line of the snippet')
  where not exists (
    select 1 from bric_page('"' || replace(new.quote, '"', '""') || '"') where rowid = new.source
  );
  select raise(abort, 'url is not in the source receipt')
  where (select instr(text, new.url) from bric_receipt where seq = new.source) = 0;
end;

insert or replace into bric_job (source, target, brief, skills) values (
  'site',
  'site_record',
  'You find what the archived public web says about methane, flaring, venting
or leaks at one oil, gas, coal or waste site. The key is the site''s name,
e.g. `Korpeje gas field`. Read `scry` first and work in it alone: no web
search. Try the name''s spellings and scripts, and more than one relation.

- `pages` counts the distinct urls in `relation` that name the site beside
  an emissions word, from a `count(distinct url) as n` you ran.
- `relation` is the scry relation that served you best.
- `operator` is who runs the site, if a row says so.
- `quote` is the most specific sentence on the site''s emissions, verbatim
  from a scry receipt; `source` is that receipt''s number and `url` the
  row''s url. All three are null if nothing says anything.
- `summary` says what is there, what is missing, and how far to trust it.',
  './skills/scry'
);
