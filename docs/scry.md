# scry.io as a research source, 2026-09-19

[Scry](https://scry.io) is read-only ClickHouse-flavoured sql over archived
public corpora: a 540 million page crawl, Common Crawl, SEC, LinkedIn,
OpenAlex, Reddit, Twitter. `skills/scry` wraps it as `scry "select ..."`;
the key is `SCRY_API_KEY`, which reaches the sandbox as any non-`BRIC_`
variable does. Queries were free (`billing_mode: free_slack`), token
matches over the crawl answer in under a second, and a refused statement
names the right column, so `deepseek-flash` corrected itself: 20 errors in
302 receipts, no attempt lost.

**ch4id, six unattributed plumes, with and without the skill.** No
difference that matters: the same site and operator on all six, 207 turns
against 232, and not one evidence url came from a scry receipt. A plume is
found by position, imagery and the archive; by the time the agent has a
name, web search already ranks the operator's page. Left out of `ch4id.sql`.

**`scry.sql`, six site names, scry alone and no web search.** All six
closed, 18 to 38 turns. Four got a quote that the trigger found verbatim
in a receipt (Ghazipur's 2022 event, Appin and Tahmoor's 24%, Korpeje's
2019 leaks, Kayrros's 83 events at Hassi Messaoud); Kandym and Lenghu got
an honest nothing, after Russian and Chinese spellings, with the
co-occurrence count called an upper bound. Operators were right on all six.

So: it is a good source when the key is a name and the question is what
the web says, how often and since when, since it counts and dates, which
search cannot, and every row is a receipt a constraint can check. It is
thin on Chinese and regulator pages, a page has a row per version (`limit
1 by url`), and personal keys are licensed for non-commercial research:
data desk use needs a commercial engagement, and published work credits
scry with a link.
