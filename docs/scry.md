# Scry research test

Tested on 19 September 2026. The experimental skill and job are in commit
`1af6df0`; neither is included in the current tree.

[Scry](https://scry.io) provides SQL queries over archived web pages and
other public datasets. The test used a shell command, `scry "SQL"`, to post
queries to `/v1/scry/query` and return CSV. It authenticated with
`SCRY_API_KEY`.

## Results

Adding Scry to the methane attribution job made little difference across
six plumes. Both runs identified the same sites and operators: 207 turns
with Scry, 232 without. None of the final evidence URLs came from Scry.
Location, imagery and web search were enough for these cases.

A separate test used Scry alone to research six named sites. All six
attempts completed in 18–38 turns, with correct operators. Four found
quotations that passed the database's citation check: Ghazipur, Appin and
Tahmoor, Korpeje, and Hassi Messaoud. Kandym and Lenghu returned no supporting
quotation, including after searches with Russian and Chinese spellings.

Queries were free during the test (`billing_mode: free_slack`), and token
searches over the crawl took less than a second. The model corrected 20
query errors across 302 logged tool results; no attempt failed because of them.

## Findings

Scry was more useful for researching known names and counting or dating
mentions than for identifying sites from coordinates. Coverage was limited
for Chinese sources and regulator pages. Pages had multiple archived
versions, so queries needed deduplication, such as `limit 1 by url`.

At the time of the test, personal keys were restricted to non-commercial
research, commercial use required a separate agreement, and published work
required attribution with a link to Scry.
