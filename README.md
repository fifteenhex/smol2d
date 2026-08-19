# smol2d

## The plan

- Small framework for doing 2D GFX on Linux using DRM/KMS or FBDEV (Amiga etc), i.e. no wayland shenanigans
- SDL backend for easier development/testing.
- Pull in smolinput and smolalsa to do input and sound too.

## Watching a frame get built

The SDL backend can open a second window showing how a frame is drawn: the
screen as it stands on the left, and on the right what the frame changed, in
the colours it put down, marked up with what it cost.

    SMOL2D_SHOW=1           keep up with the app, a look at each finished frame
    SMOL2D_SHOW=step        stop after every drawing call
    SMOL2D_SHOW=step:16     stop every 16 drawing calls
    SMOL2D_SHOW_DELAY=<ms>  how long to hold each stop, 60 by default

    yellow  what each op went over
    cyan    the same, for an op that went through a mask
    red     the damage, as the drm backend would work it out and report it

The three are rarely the same size, which is the point of showing them: a fill
through a mask goes over the whole clip, changes a handful of pixels, and gets
reported as damage covering everything.

Space pauses, the right arrow takes one step, d and o turn the damage and op
boxes off and on. None of those reach the app.

Stepping through a frame takes far longer than the frame is meant to last, so
while show is on the app is given one frame period per frame it presents rather
than the time of day, and draws what it would have drawn running at speed.
