.load ./ext/bric

create table if not exists company_parent (
  key        text primary key,
  parent     text not null,
  confidence text not null check (
    confidence in ('high', 'medium', 'low')
  ),
  source     integer not null,
  quote      text not null
);

create trigger if not exists company_parent_cite
before insert on company_parent
begin
  select raise(abort, 'quote not found in source ' || new.source)
  where not exists (
    select 1
    from bric_log
    where
      seq = new.source and
      instr(text, squeeze(new.quote))
  );
end;

select run(
  'company_parent',
  'Resolve each operator to its registered parent company. Cite the page you
   read it on and quote it exactly.',
  key
)
from company
where key not in (
  select key from company_parent
);
