# smol2d

## The plan

- Small framework for doing 2D GFX on Linux using DRM/KMS or FBDEV (Amiga etc), i.e. no wayland shenanigans
- SDL backend for easier development/testing.
- Pull in smolinput and smolalsa to do input and sound too.

## Watching a frame get built

The SDL backend can open a second window showing how a frame is drawn: the
screen as it stands on the left, everything the frame has touched on the right,
in the colours it put down, with the counters underneath.

    SMOL2D_SHOW=1           keep up with the app, a look at each finished frame
    SMOL2D_SHOW=step        stop after every drawing call
    SMOL2D_SHOW=step:16     stop every 16 drawing calls
    SMOL2D_SHOW_DELAY=<ms>  how long to hold each stop, 60 by default

Space pauses and the right arrow takes one step; neither reaches the app.

Stepping through a frame takes far longer than the frame is meant to last, so
while show is on the app is given one frame period per frame it presents rather
than the time of day, and draws what it would have drawn running at speed.
