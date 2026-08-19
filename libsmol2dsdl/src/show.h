#ifndef SMOL2DSDL_SHOW_H
#define SMOL2DSDL_SHOW_H

#include <SDL3/SDL.h>

/*
 * A second window that shows how a frame gets built: the target as it stands
 * on the left, and everything the frame has touched so far on the right. Off
 * unless SMOL2D_SHOW is set, and every entry point does nothing without it, so
 * a backend can call these unconditionally.
 *
 * SMOL2D_SHOW=1		keep up with the app, a look at each finished frame
 * SMOL2D_SHOW=step		stop after every drawing call
 * SMOL2D_SHOW=step:16		stop every 16 drawing calls
 * SMOL2D_SHOW_DELAY=<ms>	how long to hold each stop, 60 by default
 *
 * Space pauses and the right arrow takes one step; d and o turn the damage and
 * op boxes off and on. None of those four reach the app.
 */

struct show;

enum show_kind {
	SHOW_CLEAR,
	SHOW_FILL,
	SHOW_BLIT,
	SHOW_LOAD,
	SHOW_MASK,	/* went through a mask, so it covers far more than it moves */
};

/*
 * pump() is the backend's event handling: show needs it to stay responsive
 * while it is holding a frame, and takes its returning true to mean the app is
 * on its way out.
 */
struct show *show_open(SDL_Surface *target, bool (*pump)(void *cntx), void *cntx);
void show_close(struct show *show);

void show_call(struct show *show, const SDL_Surface *dst, enum show_kind kind,
	       int x, int y, unsigned int w, unsigned int h);
void show_present(struct show *show);

/* True if the event was for show and the app should not hear about it */
bool show_key(struct show *show, const SDL_Event *event);

/*
 * Stepping through a frame takes far longer than a frame, so a clock off the
 * wall would make the app skip whole seconds between the frames being watched
 * and every one of them would redraw half the screen. This hands out one frame
 * period per frame presented instead, which is what the app would have seen
 * running at speed.
 */
uint64_t show_ticks(const struct show *show, uint64_t period);

#endif
