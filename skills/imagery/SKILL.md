---
name: imagery
description: Satellite imagery of a location, to check what is on the ground. `frame LAT LON KM` returns a picture.
---
# Satellite imagery

`frame LAT LON KM [PX]` returns a north-up satellite picture KM kilometres
wide, 1,200 pixels by default:

    frame 53.805 39.36 6
    frame 53.805 39.36 1.5 1500

Run `frame` on its own, so that the picture is the only output. Keep the
size at 1,500 pixels or less.

Two pictures are usually enough: a wide view of the search area and a
close view, 1 to 2 km wide, of the main candidate.
