---
name: scry
description: sql over scry.io's archived public web (a 540 million page crawl, SEC filings, LinkedIn companies, OpenAlex, Reddit, Twitter, forums); `scry "select ..."` finds pages a search engine will not rank, by the words in them
---
# scry

`scry "select ..."` runs one read-only ClickHouse-flavoured statement against
[scry.io](https://scry.io) and prints csv. Where web search ranks pages for
a question, this matches every archived page holding the tokens you name: a
field, a plant, a well pad, a village, in whatever language. Use it when a
name is rare and search returns roundups.

    scry "select url, observed_on, substring(text, greatest(1, positionCaseInsensitive(text, 'korpeje') - 300), 700) as snip
          from crawl.pages where extraction = 'ok' and hasAllTokens(lower(text), ['korpeje', 'compressor'])
          order by observed_on desc limit 1 by url limit 10"

- Every statement needs a `limit`, start at 10; alias every aggregate
  (`count() as n`); a page has a row per version, so `limit 1 by url`.
- Match with `hasToken` / `hasAllTokens` / `hasAnyTokens` over the indexed
  expression, lowercase tokens, one word each: `lower(text)` on
  `crawl.pages`, `search_text_lc` on most others. A wrong column or
  expression is refused with the right one in the error: read it and retry.
- Never select a whole `text`: cut a snippet around the name as above.
- `partial:` as the last line means the scan was cut: a rarer token, a
  `host = '...'` filter or a smaller limit.

Relations worth knowing, each `relation(text expression)`:
`crawl.pages(lower(text))` the web, keyed by `host`;
`commoncrawl.distillate` cleaned Common Crawl articles;
`sec.edgar_markdown(markdown)` filings, with `sec.filers` for names;
`linkedin.companies`, `linkedin.posts`; `openalex.works` papers;
`reddit.posts`, `reddit.comments_popular`, `twitter.tweets`,
`mastodon.posts`, `youtube.transcripts`, `mailing_lists.messages`.
The catalogue is itself a relation:

    scry "select relation, purpose from scry.relations where relation like 'sec.%' limit 20"
    scry "select name, type, indexed from scry.columns where relation = 'linkedin.companies' limit 60"

A scry row is a lead with a url. To cite the page, read it with `page URL`;
if the live page is gone, the archived text in your receipt is the evidence
and its url still names it.
