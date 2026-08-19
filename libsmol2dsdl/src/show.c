#include <SDL3/SDL.h>

#include "show.h"

#define SEP		4	/* between the two panes */
#define STATUS		68	/* under them, for the counters */
#define TEXTW		8	/* the debug font, before scaling */
#define TEXTLINE	18	/* between the lines of it */

#define MARKER		3	/* how far the step marker sits outside the paint */

/*
 * The damage boxes are worked out the way the drm backend works them out, so
 * what they show is what the hardware would be told to move: at most this many
 * rectangles, neighbours merged once there are more than that, and the whole
 * screen once the pieces add up to more than the screen is worth.
 */
#define MAXDAMAGE	256

#define DIM		56	/* how much of the frame shows through the trace */

#define DEFAULT_DELAY	60

struct show {
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *frametex;
	SDL_Texture *tracetex;

	SDL_Surface *target;		/* what the app draws into */
	SDL_Surface *frame;		/* the same thing in colour, as it stands */
	SDL_Surface *trace;		/* what this frame has touched, over a dimmed copy */
	unsigned int w, h;

	unsigned int step;		/* calls between stops, 0 to only stop at present */
	unsigned int delay;		/* ms to hold a stop */
	float textscale;		/* the counters, as large as they will fit */

	bool paused;
	bool onestep;
	bool gone;			/* the window was closed, carry on without it */

	bool (*pump)(void *cntx);
	void *cntx;

	/* this frame */
	unsigned int calls;
	unsigned int offscreen;
	unsigned long covered;
	SDL_Rect damage[MAXDAMAGE];
	unsigned int ndamage;
	unsigned long damaged;
	bool damagedall;
	SDL_Rect last;
	bool haslast;
	SDL_Rect dirty;
	bool hasdirty;

	/* since the first frame */
	unsigned long frames;
	unsigned long worst;
};

static void dirtied(struct show *s, const SDL_Rect *r)
{
	if (!s->hasdirty) {
		s->dirty = *r;
		s->hasdirty = true;
		return;
	}

	SDL_GetRectUnion(&s->dirty, r, &s->dirty);
}

static void wholeframe(struct show *s)
{
	const SDL_Rect all = { 0, 0, (int)s->w, (int)s->h };

	dirtied(s, &all);
}

/*
 * The trace pane starts each frame as a dark copy of the screen, so that what
 * gets drawn over it stands out and you can still see where you are.
 */
static void dimframe(struct show *s)
{
	SDL_FillSurfaceRect(s->trace, NULL, SDL_MapSurfaceRGB(s->trace, 0, 0, 0));
	SDL_BlitSurface(s->frame, NULL, s->trace, NULL);
	wholeframe(s);
}

static bool parsemode(struct show *s, const char *want)
{
	if (!SDL_strncmp(want, "step", 4)) {
		int n = want[4] == ':' ? SDL_atoi(want + 5) : 1;

		s->step = n > 0 ? (unsigned int)n : 1;
		return true;
	}

	s->step = 0;
	return true;
}

struct show *show_open(SDL_Surface *target, bool (*pump)(void *cntx), void *cntx)
{
	const char *want = SDL_getenv("SMOL2D_SHOW");
	const char *delay = SDL_getenv("SMOL2D_SHOW_DELAY");
	struct show *s;
	int width, height;

	if (!target || !want || !SDL_strcmp(want, "0") || !*want)
		return NULL;

	s = SDL_calloc(1, sizeof(*s));
	if (!s)
		return NULL;

	s->target = target;
	s->w = (unsigned int)target->w;
	s->h = (unsigned int)target->h;
	s->pump = pump;
	s->cntx = cntx;
	s->delay = delay ? (unsigned int)SDL_atoi(delay) : DEFAULT_DELAY;

	parsemode(s, want);

	width = (int)s->w * 2 + SEP;
	height = (int)s->h + STATUS;

	/* the counters run to about 70 characters, so fit them to the window */
	s->textscale = (float)(width / (70 * TEXTW));
	if (s->textscale < 1)
		s->textscale = 1;
	if (s->textscale > 3)
		s->textscale = 3;

	s->window = SDL_CreateWindow("smol2d: how the frame gets built",
				     width, height, SDL_WINDOW_RESIZABLE);
	if (!s->window)
		goto err;

	s->renderer = SDL_CreateRenderer(s->window, NULL);
	if (!s->renderer)
		goto err;

	SDL_SetRenderLogicalPresentation(s->renderer, width, height,
					 SDL_LOGICAL_PRESENTATION_LETTERBOX);

	s->frame = SDL_CreateSurface((int)s->w, (int)s->h, SDL_PIXELFORMAT_XRGB8888);
	s->trace = SDL_CreateSurface((int)s->w, (int)s->h, SDL_PIXELFORMAT_XRGB8888);
	if (!s->frame || !s->trace)
		goto err;

	/* the trace pane wants the frame under it faint, not solid */
	SDL_SetSurfaceBlendMode(s->frame, SDL_BLENDMODE_BLEND);
	SDL_SetSurfaceAlphaMod(s->frame, DIM);

	s->frametex = SDL_CreateTexture(s->renderer, SDL_PIXELFORMAT_XRGB8888,
					SDL_TEXTUREACCESS_STREAMING, (int)s->w, (int)s->h);
	s->tracetex = SDL_CreateTexture(s->renderer, SDL_PIXELFORMAT_XRGB8888,
					SDL_TEXTUREACCESS_STREAMING, (int)s->w, (int)s->h);
	if (!s->frametex || !s->tracetex)
		goto err;

	SDL_SetTextureScaleMode(s->frametex, SDL_SCALEMODE_NEAREST);
	SDL_SetTextureScaleMode(s->tracetex, SDL_SCALEMODE_NEAREST);

	SDL_BlitSurface(s->target, NULL, s->frame, NULL);
	dimframe(s);

	return s;

err:
	SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "smol2d: no show window: %s", SDL_GetError());
	show_close(s);
	return NULL;
}

void show_close(struct show *s)
{
	if (!s)
		return;

	if (s->frametex)
		SDL_DestroyTexture(s->frametex);
	if (s->tracetex)
		SDL_DestroyTexture(s->tracetex);
	if (s->frame)
		SDL_DestroySurface(s->frame);
	if (s->trace)
		SDL_DestroySurface(s->trace);
	if (s->renderer)
		SDL_DestroyRenderer(s->renderer);
	if (s->window)
		SDL_DestroyWindow(s->window);

	SDL_free(s);
}

/* The same clipping the fill helpers do, so the counts are the real ones */
static bool cliprect(const struct show *s, SDL_Rect *r)
{
	if (r->x < 0) {
		r->w += r->x;
		r->x = 0;
	}

	if (r->y < 0) {
		r->h += r->y;
		r->y = 0;
	}

	if (r->w <= 0 || r->h <= 0 || r->x >= (int)s->w || r->y >= (int)s->h)
		return false;

	if (r->x + r->w > (int)s->w)
		r->w = (int)s->w - r->x;
	if (r->y + r->h > (int)s->h)
		r->h = (int)s->h - r->y;

	return true;
}

static void merge(SDL_Rect *into, const SDL_Rect *with)
{
	int right = into->x + into->w > with->x + with->w ?
		    into->x + into->w : with->x + with->w;
	int bottom = into->y + into->h > with->y + with->h ?
		     into->y + into->h : with->y + with->h;

	into->x = into->x < with->x ? into->x : with->x;
	into->y = into->y < with->y ? into->y : with->y;
	into->w = right - into->x;
	into->h = bottom - into->y;
}

static void coalesce(struct show *s, unsigned int want)
{
	unsigned int run, at, i;

	if (s->ndamage <= want)
		return;

	run = (s->ndamage + want - 1) / want;

	for (at = 0, i = 0; i < s->ndamage; at++) {
		unsigned int end = i + run < s->ndamage ? i + run : s->ndamage;

		s->damage[at] = s->damage[i];
		for (i++; i < end; i++)
			merge(&s->damage[at], &s->damage[i]);
	}

	s->ndamage = at;
	s->damaged = 0;
	for (i = 0; i < s->ndamage; i++)
		s->damaged += (unsigned long)s->damage[i].w * s->damage[i].h;
}

static void damaged(struct show *s, const SDL_Rect *r)
{
	if (s->damagedall)
		return;

	if (s->ndamage == MAXDAMAGE)
		coalesce(s, MAXDAMAGE / 2);

	s->damage[s->ndamage++] = *r;
	s->damaged += (unsigned long)r->w * r->h;

	if (s->damaged >= (unsigned long)s->w * s->h) {
		s->damagedall = true;
		s->ndamage = 0;
		s->damaged = (unsigned long)s->w * s->h;
	}
}

static void outline(SDL_Surface *into, const SDL_Rect *r, Uint32 colour)
{
	const SDL_Rect edges[] = {
		{ r->x, r->y, r->w, 1 },
		{ r->x, r->y + r->h - 1, r->w, 1 },
		{ r->x, r->y, 1, r->h },
		{ r->x + r->w - 1, r->y, 1, r->h },
	};

	SDL_FillSurfaceRects(into, edges, SDL_arraysize(edges), colour);
}

/*
 * Paint goes into the trace in the colour it actually put down, so a frame's
 * worth of drawing reads as itself. Blits are outlined instead of filled,
 * since what matters there is where the thing landed, not what it looks like.
 */
static void traced(struct show *s, const SDL_Rect *r, enum show_kind kind, uint8_t index)
{
	const SDL_Palette *palette;

	if (kind == SHOW_BLIT || kind == SHOW_LOAD) {
		outline(s->trace, r, kind == SHOW_BLIT ?
			SDL_MapSurfaceRGB(s->trace, 0x40, 0xff, 0x60) :
			SDL_MapSurfaceRGB(s->trace, 0x40, 0xe0, 0xff));
		return;
	}

	palette = SDL_GetSurfacePalette(s->target);
	if (palette && index < palette->ncolors) {
		const SDL_Color *c = &palette->colors[index];

		SDL_FillSurfaceRect(s->trace, r, SDL_MapSurfaceRGB(s->trace, c->r, c->g, c->b));
		return;
	}

	SDL_FillSurfaceRect(s->trace, r, SDL_MapSurfaceRGB(s->trace, 0xff, 0xff, 0xff));
}

static void upload(struct show *s)
{
	const uint8_t *at;

	if (!s->hasdirty)
		return;

	at = (const uint8_t *)s->frame->pixels + (size_t)s->dirty.y * s->frame->pitch +
	     (size_t)s->dirty.x * 4;
	SDL_UpdateTexture(s->frametex, &s->dirty, at, s->frame->pitch);

	at = (const uint8_t *)s->trace->pixels + (size_t)s->dirty.y * s->trace->pitch +
	     (size_t)s->dirty.x * 4;
	SDL_UpdateTexture(s->tracetex, &s->dirty, at, s->trace->pitch);

	s->hasdirty = false;
}

static void line(struct show *s, unsigned int n, const char *text)
{
	SDL_RenderDebugText(s->renderer, 4 / s->textscale,
			    (float)(s->h + 8 + n * TEXTLINE) / s->textscale, text);
}

static void status(struct show *s)
{
	const float full = (float)(s->w * s->h);
	char damage[48];
	char text[160];

	SDL_SetRenderDrawColor(s->renderer, 0xff, 0xff, 0xff, 0xff);
	SDL_SetRenderScale(s->renderer, s->textscale, s->textscale);

	SDL_snprintf(text, sizeof(text), "frame %lu  calls %u  covered %lu px (%.2f%% of %ux%u)",
		     s->frames, s->calls, s->covered,
		     100.0f * (float)s->covered / full, s->w, s->h);
	line(s, 0, text);

	if (s->damagedall)
		SDL_snprintf(damage, sizeof(damage), "damage the lot");
	else
		SDL_snprintf(damage, sizeof(damage), "damage %u boxes, %lu px",
			     s->ndamage, s->damaged);

	if (s->step)
		SDL_snprintf(text, sizeof(text), "%s  worst %lu  every %u call%s, %u ms%s%s",
			     damage, s->worst, s->step, s->step == 1 ? "" : "s", s->delay,
			     s->offscreen ? "  +offscreen" : "", s->paused ? "  PAUSED" : "");
	else
		SDL_snprintf(text, sizeof(text), "%s  worst %lu  keeping up%s%s",
			     damage, s->worst,
			     s->offscreen ? "  +offscreen" : "", s->paused ? "  PAUSED" : "");
	line(s, 1, text);

	line(s, 2, "left: the screen  right: touched, red = damage  [space] [right]");

	SDL_SetRenderScale(s->renderer, 1, 1);
}

static void drawdamage(struct show *s)
{
	const float over = (float)(s->w + SEP);
	unsigned int i;

	SDL_SetRenderDrawColor(s->renderer, 0xff, 0x20, 0x20, 0xff);

	if (s->damagedall) {
		const SDL_FRect all = { over, 0, (float)s->w, (float)s->h };

		SDL_RenderRect(s->renderer, &all);
		return;
	}

	/* drawn around the region rather than over it, so the paint still shows */
	for (i = 0; i < s->ndamage; i++) {
		const SDL_FRect box = { over + (float)s->damage[i].x - 1,
					(float)s->damage[i].y - 1,
					(float)s->damage[i].w + 2,
					(float)s->damage[i].h + 2 };

		SDL_RenderRect(s->renderer, &box);
	}
}

static void render(struct show *s)
{
	const SDL_FRect left = { 0, 0, (float)s->w, (float)s->h };
	const SDL_FRect right = { (float)(s->w + SEP), 0, (float)s->w, (float)s->h };

	SDL_SetRenderDrawColor(s->renderer, 0x10, 0x10, 0x18, 0xff);
	SDL_RenderClear(s->renderer);

	SDL_RenderTexture(s->renderer, s->frametex, NULL, &left);
	SDL_RenderTexture(s->renderer, s->tracetex, NULL, &right);

	/* what the hardware would be told to move, merged as the backend merges it */
	drawdamage(s);

	/*
	 * Where the last call went, so the sweep is visible while stepping. A
	 * column of a couple of pixels is a marker you would never spot, so the
	 * box is drawn around the paint rather than on it.
	 */
	if (s->haslast && s->step) {
		const SDL_FRect at = { (float)s->last.x - MARKER, (float)s->last.y - MARKER,
				       (float)s->last.w + 2 * MARKER,
				       (float)s->last.h + 2 * MARKER };
		const SDL_FRect over = { at.x + (float)(s->w + SEP), at.y, at.w, at.h };

		SDL_SetRenderDrawColor(s->renderer, 0xff, 0xff, 0xff, 0xff);
		SDL_RenderRect(s->renderer, &at);
		SDL_RenderRect(s->renderer, &over);
	}

	status(s);
	SDL_RenderPresent(s->renderer);
}

/*
 * Holding a stop means sitting still without going deaf: the backend's event
 * handling keeps running, so the window stays alive and the keys work.
 */
static void hold(struct show *s, bool stopping)
{
	const Uint64 until = SDL_GetTicks() + (stopping ? s->delay : 0);

	for (;;) {
		if (s->pump && s->pump(s->cntx))
			return;

		if (s->paused) {
			if (s->onestep) {
				s->onestep = false;
				return;
			}
		} else if (SDL_GetTicks() >= until) {
			return;
		}

		SDL_Delay(2);
	}
}

static void stop(struct show *s, bool stopping)
{
	if (s->gone)
		return;

	upload(s);
	render(s);
	hold(s, stopping);
}

void show_call(struct show *s, const SDL_Surface *dst, enum show_kind kind,
	       int x, int y, unsigned int w, unsigned int h, uint8_t index)
{
	SDL_Rect r = { x, y, (int)w, (int)h };

	if (!s)
		return;

	s->calls++;

	if (dst != s->target) {
		s->offscreen++;
	} else if (cliprect(s, &r)) {
		s->covered += (unsigned long)r.w * (unsigned long)r.h;

		SDL_BlitSurface(s->target, &r, s->frame, &r);
		traced(s, &r, kind, index);
		damaged(s, &r);

		s->last = r;
		s->haslast = true;
		dirtied(s, &r);
	}

	if (s->step && s->calls % s->step == 0)
		stop(s, true);
}

void show_present(struct show *s)
{
	if (!s)
		return;

	s->frames++;
	if (s->covered > s->worst)
		s->worst = s->covered;

	if (!s->gone) {
		/* anything drawn by a path show does not see lands here */
		SDL_BlitSurface(s->target, NULL, s->frame, NULL);
		wholeframe(s);

		stop(s, false);

		dimframe(s);
	}

	s->calls = 0;
	s->offscreen = 0;
	s->covered = 0;
	s->ndamage = 0;
	s->damaged = 0;
	s->damagedall = false;
	s->haslast = false;
}

bool show_key(struct show *s, const SDL_Event *event)
{
	if (!s)
		return false;

	if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
	    s->window && event->window.windowID == SDL_GetWindowID(s->window)) {
		s->gone = true;
		s->paused = false;
		return true;
	}

	if (event->type != SDL_EVENT_KEY_DOWN || s->gone)
		return false;

	switch (event->key.key) {
	case SDLK_SPACE:
		s->paused = !s->paused;
		return true;
	case SDLK_RIGHT:
		s->paused = true;
		s->onestep = true;
		return true;
	}

	return false;
}

uint64_t show_ticks(const struct show *s, uint64_t period)
{
	if (!s)
		return 0;

	return s->frames * period / SDL_NS_PER_MS;
}
