---
name: carbon-mapper
description: a Carbon Mapper record's own retrieval, the plume mask over the scene and the scene alone; `cm ID` lists, `cm ID plume|rgb` shows
---
# carbon mapper retrievals

Carbon Mapper publishes its own retrieval for a record: the plume drawn over
the scene, and the scene alone. The plume mask is the evidence; the scene is
what it fell on.

    cm CM:tan20250101t113529c00s4001-A         # the urls and the plume bounds
    cm CM:tan20250101t113529c00s4001-A plume   # the mask, as a png on stdout
    cm CM:tan20250101t113529c00s4001-A rgb     # the scene

The signed urls expire within the hour, so ask when you need them rather
than storing them. Not every record has a retrieval; ask and see. The
sector and wind in the listing are the provider's own.
