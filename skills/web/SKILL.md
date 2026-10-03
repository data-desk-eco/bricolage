---
name: web
description: Read web pages and PDFs. `page URL` returns a page as text.
---
# Web pages

Find pages with the web search tool, searching in the local language as
well as English. Read a page with `page`:

    page https://example.org/permit/1234          # text with links
    page https://example.org/permit/1234 text     # text only
    page https://example.org/permit/1234 links    # links only
    page https://example.org/permit/1234 shot     # a screenshot
    page https://example.org/report.pdf           # the text of a PDF

A page that fails, or shows a cookie wall or login, will do so again: try
another copy, such as the operator's release or an archive.org snapshot. A MediaWiki site that blocks the browser often
answers through its API:

    page 'https://site/w/api.php?action=parse&page=Title&format=json&prop=wikitext'

Stop after three searches that find nothing useful.
