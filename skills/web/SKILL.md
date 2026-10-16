---
name: web
description: Search the web and read pages and PDFs through Exa. `search
  QUERY` lists results; `page URL` returns a page as text.
---
# Web pages

Find pages with the web search tool or with `search`, searching in the local
language as well as English. `search` prints each result's title, url, date
and the passages that match:

    search 'zaap c platform pemex'
    search 'zaap c pemex' 20     # 20 results, not 10

Read a page or a PDF with `page`. Exa returns its own copy, or reads the page
live when it has none:

    page https://example.org/permit/1234          # the text
    page https://example.org/permit/1234 links    # its links
    page https://example.org/report.pdf

A page that fails will do so again: try another copy, such as the operator's
release or an archive.org snapshot. A MediaWiki site that blocks Exa often
answers through its API:

    page 'https://site/w/api.php?action=parse&page=Title&format=json&prop=wikitext'

Stop after three searches that find nothing useful.
