.load ./ext/bric
.timeout 30000
create temp table results (
  key        text primary key,
  parent     text not null,
  confidence text not null check (confidence in ('high', 'medium', 'low')),
  source     integer not null,
  quote      text not null
);
create trigger results_cite before insert on results
begin
  select raise(abort, 'quote not found in source ' || new.source)
   where not exists (select 1 from bric_log
                      where seq = new.source and instr(text, new.quote));
end;

insert into results
select r.*
  from (select key from company where parent is null) as todo
  join run('Resolve each operator to its registered parent company. Cite the page you read it on and quote it exactly.', todo.key) as r;

update company
   set parent = r.parent, confidence = r.confidence, source = r.source
  from results as r
 where r.key = company.key;
