---
name: carbon-mapper
description: Carbon Mapper's own data for a CM plume: the plume mask, the scene and the wind. `cm ID` lists it.
---
# Carbon Mapper plumes

For a plume with a CM: ID, Carbon Mapper publishes the plume mask over the
scene, the scene alone and its wind estimate:

    cm CM:tan20250101t113529c00s4001-A         # links, plume bounds, wind
    cm CM:tan20250101t113529c00s4001-A plume   # the plume mask, as a picture
    cm CM:tan20250101t113529c00s4001-A rgb     # the scene, as a picture

The plume mask shows where the methane is. Not every plume has this data.
The links expire within an hour, so request them when you need them.
