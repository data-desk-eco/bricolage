create table if not exists bric_log (
  seq     integer primary key,
  ts      text not null default (datetime('now')),
  job     text not null,
  key     text,
  attempt integer,
  turn    integer,
  kind    text not null,
  tool    text,
  detail  text,
  text    text,
  usage   text
) strict;

create index if not exists bric_job on bric_log (job, key);

create unique index if not exists bric_claim on bric_log (job, key, attempt, kind)
where kind in ('open', 'close', 'error');

create view if not exists bric_attempt as
select l.job, l.key, l.attempt, l.turn, l.ts, l.kind, l.tool, l.detail, u.input, u.output, u.cache_read
from bric_log as l
join (
  select job, key, max(seq) as seq,
    sum(usage ->> 'input') as input,
    sum(usage ->> 'output') as output,
    sum(usage ->> 'cache_read') as cache_read
  from bric_log
  group by job, key
) as u using (seq);
