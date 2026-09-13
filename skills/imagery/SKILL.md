---
name: imagery
description: a satellite picture of a place, to see whether the mapped thing is really there; `frame LAT LON KM` prints a jpeg
---
# looking at the ground

The archive says where a thing is mapped. It does not say whether the pad was
ever drilled, whether the tanks were built, whether the site was demolished,
or which of two neighbours the plume sits on. A picture does.

    frame 53.805 39.36 6        # 6 km across, north up, 1200 px
    frame 53.805 39.36 1.5 1500 # closer, at 1500 px

The jpeg goes to stdout and is shown to you when it is the whole of the
output, so run `frame` on its own, never after an `ls` or an `echo`. It is a
few hundred kilobytes; keep the pixel size at or under 1500.

Two pictures answer most records: one wide frame at the whole search radius,
so you see what stands around the plume, and one close frame at a kilometre
or two on the candidate. Do not walk a grid of frames, do not re-shoot the
same ground at slightly different sizes, and do not render ground you already
decided was uninformative.

Say what you saw in your paragraph. A picture you did not describe is a
picture you did not read.
