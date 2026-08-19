#include <SDL3/SDL.h>

#include <smol2d.h>

#include "show.h"

#define DEFAULT_WIDTH	640
#define DEFAULT_HEIGHT	480

struct sdl_tex {
	struct smol2d_tex tex;
	SDL_Surface *surface;
	bool builtin;
};

struct sdl_backend {
	SDL_Window *window;
	struct sdl_tex backbuffer;
	SDL_Palette *palette;
	struct smol2d_rect clip;
	bool hasclip;
	const struct smol2d_mask *mask;
	Uint64 start;
	Uint64 period;
	Uint64 nextframe;
	bool closing;
	struct show *show;
};

static bool showpump(void *cntx);

static void getsize(int *width, int *height)
{
	const char *size = SDL_getenv("SMOL2D_SDL_SIZE");
	int w, h;

	if (size && SDL_sscanf(size, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
		*width = w;
		*height = h;
		return;
	}

	*width = DEFAULT_WIDTH;
	*height = DEFAULT_HEIGHT;
}

static int fail(void)
{
	SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "smol2d: %s", SDL_GetError());
	return -1;
}

/* Textures share the backbuffer's palette, so blits stay index to index */
static int textureinit(struct sdl_tex *sdltex, SDL_Palette *palette,
		       unsigned int width, unsigned int height)
{
	sdltex->surface = SDL_CreateSurface((int)width, (int)height, SDL_PIXELFORMAT_INDEX8);
	if (!sdltex->surface)
		return fail();

	if (palette && !SDL_SetSurfacePalette(sdltex->surface, palette)) {
		SDL_DestroySurface(sdltex->surface);
		sdltex->surface = NULL;
		return fail();
	}

	sdltex->tex.w = width;
	sdltex->tex.h = height;
	return 0;
}

int smol2d_init(void **backend_cntx, enum smol2d_colourspace cs)
{
	struct sdl_backend *be;
	int width, height;

	if (!backend_cntx)
		return -1;

	if (cs != SMOL2D_CS_C8) {
		SDL_SetError("only SMOL2D_CS_C8 is implemented");
		return fail();
	}

	if (!SDL_Init(SDL_INIT_VIDEO))
		return fail();

	be = SDL_calloc(1, sizeof(*be));
	if (!be)
		goto err_quit;

	getsize(&width, &height);

	be->start = SDL_GetTicksNS();

	be->window = SDL_CreateWindow("smol2d", width, height, 0);
	if (!be->window)
		goto err_free;

	if (textureinit(&be->backbuffer, NULL, (unsigned int)width, (unsigned int)height))
		goto err_window;
	be->backbuffer.builtin = true;

	be->palette = SDL_CreateSurfacePalette(be->backbuffer.surface);
	if (!be->palette)
		goto err_surface;

	/* nothing if SMOL2D_SHOW is unset, and it is never fatal */
	be->show = show_open(be->backbuffer.surface, showpump, be);

	*backend_cntx = be;
	return 0;

err_surface:
	SDL_DestroySurface(be->backbuffer.surface);
err_window:
	SDL_DestroyWindow(be->window);
err_free:
	SDL_free(be);
err_quit:
	SDL_Quit();
	return fail();
}

int smol2d_setpalette(void *backend_cntx, const struct smol2d_palette *palette)
{
	struct sdl_backend *be = backend_cntx;
	SDL_Color colours[256];
	int i;

	if (!be || !palette)
		return -1;

	for (i = 0; i < 256; i++) {
		colours[i].r = palette->colours[i].r;
		colours[i].g = palette->colours[i].g;
		colours[i].b = palette->colours[i].b;
		colours[i].a = SDL_ALPHA_OPAQUE;
	}

	if (!SDL_SetPaletteColors(be->palette, colours, 0, 256))
		return fail();

	return 0;
}

struct smol2d_tex *smol2d_getbackbuffer(void *backend_cntx)
{
	struct sdl_backend *be = backend_cntx;

	if (!be)
		return NULL;

	return &be->backbuffer.tex;
}

int smol2d_tex_create(void *backend_cntx, struct smol2d_tex **tex,
		      unsigned int width, unsigned int height)
{
	struct sdl_backend *be = backend_cntx;
	struct sdl_tex *sdltex;

	if (!be || !tex || !width || !height)
		return -1;

	sdltex = SDL_calloc(1, sizeof(*sdltex));
	if (!sdltex)
		return fail();

	if (textureinit(sdltex, be->palette, width, height)) {
		SDL_free(sdltex);
		return -1;
	}

	*tex = &sdltex->tex;
	return 0;
}

void smol2d_tex_destroy(void *backend_cntx, struct smol2d_tex *tex)
{
	struct sdl_tex *sdltex = (struct sdl_tex *)tex;

	(void)backend_cntx;

	if (!sdltex || sdltex->builtin)
		return;

	SDL_DestroySurface(sdltex->surface);
	SDL_free(sdltex);
}

int smol2d_tex_load(void *backend_cntx, struct smol2d_tex *tex, const uint8_t *pixels)
{
	struct sdl_backend *be = backend_cntx;
	struct sdl_tex *sdltex = (struct sdl_tex *)tex;
	unsigned int y;

	if (!sdltex || !pixels)
		return -1;

	for (y = 0; y < sdltex->tex.h; y++)
		SDL_memcpy((uint8_t *)sdltex->surface->pixels + y * sdltex->surface->pitch,
			   pixels + (size_t)y * sdltex->tex.w, sdltex->tex.w);

	if (be)
		show_call(be->show, sdltex->surface, SHOW_LOAD, 0, 0,
			  sdltex->tex.w, sdltex->tex.h);

	return 0;
}

int smol2d_setclip(void *backend_cntx, const struct smol2d_rect *clip)
{
	struct sdl_backend *be = backend_cntx;

	if (!be)
		return -1;

	be->hasclip = clip != NULL;
	if (clip)
		be->clip = *clip;

	return 0;
}


int smol2d_mask_create(void *backend_cntx, struct smol2d_mask **mask,
		       unsigned int width, unsigned int height)
{
	struct smol2d_mask *m;
	unsigned int stride;

	if (!backend_cntx || !mask || !width || !height)
		return -1;

	m = SDL_calloc(1, sizeof(*m));
	if (!m)
		return -1;

	stride = (width + 7) / 8;

	m->bits = SDL_calloc(stride, height);
	if (!m->bits) {
		SDL_free(m);
		return -1;
	}

	m->w = width;
	m->h = height;
	m->stride = stride;

	*mask = m;
	return 0;
}

void smol2d_mask_destroy(void *backend_cntx, struct smol2d_mask *mask)
{
	(void)backend_cntx;

	if (!mask)
		return;

	SDL_free(mask->bits);
	SDL_free(mask);
}

int smol2d_setmask(void *backend_cntx, const struct smol2d_mask *mask)
{
	struct sdl_backend *be = backend_cntx;

	if (!be)
		return -1;

	be->mask = mask;

	return 0;
}

int smol2d_tex_clear(void *backend_cntx, struct smol2d_tex *tex, const struct smol2d_colour *colour)
{
	struct sdl_backend *be = backend_cntx;
	struct sdl_tex *sdltex = (struct sdl_tex *)tex;
	SDL_Rect rect;

	if (!be || !sdltex || !colour)
		return -1;

	if (be->hasclip) {
		rect.x = be->clip.x;
		rect.y = be->clip.y;
		rect.w = (int)be->clip.w;
		rect.h = (int)be->clip.h;
	} else {
		rect.x = 0;
		rect.y = 0;
		rect.w = sdltex->surface->w;
		rect.h = sdltex->surface->h;
	}

	if (be->mask) {
		if (!SDL_LockSurface(sdltex->surface))
			return fail();

		smol2d_c8_fill_masked(sdltex->surface->pixels,
				      (unsigned int)sdltex->surface->w,
				      (unsigned int)sdltex->surface->h,
				      (unsigned int)sdltex->surface->pitch,
				      rect.x, rect.y,
				      (unsigned int)rect.w, (unsigned int)rect.h,
				      colour->indexed.index, be->mask);

		SDL_UnlockSurface(sdltex->surface);
		show_call(be->show, sdltex->surface, SHOW_MASK, rect.x, rect.y,
			  (unsigned int)rect.w, (unsigned int)rect.h);
		return 0;
	}

	if (!SDL_FillSurfaceRect(sdltex->surface, be->hasclip ? &rect : NULL,
				 colour->indexed.index))
		return fail();

	show_call(be->show, sdltex->surface, SHOW_CLEAR, rect.x, rect.y,
		  (unsigned int)rect.w, (unsigned int)rect.h);
	return 0;
}

int smol2d_tex_setkey(void *backend_cntx, struct smol2d_tex *tex, int key)
{
	struct sdl_tex *sdltex = (struct sdl_tex *)tex;

	(void)backend_cntx;

	if (!sdltex || key > 255)
		return -1;

	if (!SDL_SetSurfaceColorKey(sdltex->surface, key >= 0, key < 0 ? 0 : (Uint32)key))
		return fail();

	return 0;
}

int smol2d_tex_renderto(void *backend_cntx, struct smol2d_tex *tex, struct smol2d_drawlist *drawlist)
{
	struct sdl_backend *be = backend_cntx;
	struct sdl_tex *sdltex = (struct sdl_tex *)tex;
	unsigned int i;

	if (!be || !sdltex || !drawlist)
		return -1;

	for (i = 0; i < drawlist->nsprites; i++) {
		const struct smol2d_sprite *sprite = drawlist->sprites[i];
		struct sdl_tex *from;
		SDL_Rect dstrect;

		if (!sprite || !sprite->tex)
			continue;

		from = (struct sdl_tex *)sprite->tex;
		dstrect.x = (int)sprite->x;
		dstrect.y = (int)sprite->y;
		dstrect.w = from->surface->w;
		dstrect.h = from->surface->h;

		if (!SDL_BlitSurface(from->surface, NULL, sdltex->surface, &dstrect))
			return fail();

		show_call(be->show, sdltex->surface, SHOW_BLIT, dstrect.x, dstrect.y,
			  (unsigned int)dstrect.w, (unsigned int)dstrect.h);
	}

	return 0;
}

static bool takeevent(struct sdl_backend *be, const SDL_Event *event)
{
	/* show's own keys, and its window closing, are not the app's business */
	if (show_key(be->show, event))
		return false;

	if (event->type == SDL_EVENT_QUIT ||
	    event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
		be->closing = true;

	if (event->type != SDL_EVENT_KEY_DOWN)
		return false;

	if (event->key.key == SDLK_ESCAPE)
		be->closing = true;

	return true;
}

static bool drainevents(struct sdl_backend *be)
{
	SDL_Event event;
	bool key = false;

	SDL_PumpEvents();

	while (SDL_PeepEvents(&event, 1, SDL_GETEVENT,
			      SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0)
		key |= takeevent(be, &event);

	return key;
}

/*
 * A source rectangle of nothing means the whole texture. Whatever it says, it
 * has to be inside the texture: a pipeline that would read past the end of one
 * is refused when it is made ready rather than when it runs.
 */
static int resolvefrom(struct smol2d_rect *to, const struct smol2d_rect *from,
		       unsigned int w, unsigned int h)
{
	if (!from->w || !from->h) {
		to->x = 0;
		to->y = 0;
		to->w = w;
		to->h = h;
		return 0;
	}

	if (from->x < 0 || from->y < 0 ||
	    (unsigned int)from->x + from->w > w || (unsigned int)from->y + from->h > h)
		return -1;

	*to = *from;
	return 0;
}

/* Read back rather than remembered, so setting a key later still takes */
static int texkey(const struct sdl_tex *tex)
{
	Uint32 key;

	if (!SDL_SurfaceHasColorKey(tex->surface) ||
	    !SDL_GetSurfaceColorKey(tex->surface, &key))
		return -1;

	return (int)key;
}

/*
 * A made ready pipeline. The op list is copied in and what the backend can
 * settle in advance is settled here: where each target's pixels are, how big
 * it is and how long a row is. Running it does none of that again, so a frame
 * costs the dynamic fields the caller wrote and the paint itself.
 *
 * Holding on to the pixel pointer means the target has to keep it, which is
 * true of the plain surfaces this backend makes but not of ones SDL wants
 * locked, so those are turned away when the pipeline is made ready.
 */
struct pipeline_op {
	uint8_t *pixels;
	unsigned int w, h;
	unsigned int stride;
	const SDL_Surface *surface;	/* only so show can tell targets apart */

	/* blits: where the source is and which part of it is wanted */
	const struct sdl_tex *srctex;
	const uint8_t *src;
	unsigned int srcstride;
	struct smol2d_rect from;
};

struct smol2d_pipeline {
	struct smol2d_op *ops;
	struct pipeline_op *ready;
	unsigned int nops;
};

int smol2d_pipeline_create(void *backend_cntx, const struct smol2d_op *ops,
			   unsigned int nops, struct smol2d_pipeline **pipeline)
{
	struct smol2d_pipeline *p;
	unsigned int i;

	if (!backend_cntx || !pipeline || (!ops && nops))
		return -1;

	p = SDL_calloc(1, sizeof(*p));
	if (!p)
		return -1;

	p->ops = SDL_calloc(nops ? nops : 1, sizeof(*p->ops));
	p->ready = SDL_calloc(nops ? nops : 1, sizeof(*p->ready));
	if (!p->ops || !p->ready) {
		smol2d_pipeline_destroy(backend_cntx, p);
		return -1;
	}

	if (nops)
		SDL_memcpy(p->ops, ops, nops * sizeof(*ops));
	p->nops = nops;

	for (i = 0; i < nops; i++) {
		const struct sdl_tex *dst = (const struct sdl_tex *)ops[i].dst;

		if (!dst || SDL_MUSTLOCK(dst->surface))
			goto err;

		p->ready[i].pixels = dst->surface->pixels;
		p->ready[i].w = (unsigned int)dst->surface->w;
		p->ready[i].h = (unsigned int)dst->surface->h;
		p->ready[i].stride = (unsigned int)dst->surface->pitch;
		p->ready[i].surface = dst->surface;

		switch (ops[i].type) {
		case SMOL2D_OP_FILL:
			break;
		case SMOL2D_OP_BLIT: {
			const struct sdl_tex *src = (const struct sdl_tex *)ops[i].blit.src;

			if (!src || SDL_MUSTLOCK(src->surface) ||
			    resolvefrom(&p->ready[i].from, &ops[i].blit.from,
					(unsigned int)src->surface->w,
					(unsigned int)src->surface->h))
				goto err;

			p->ready[i].srctex = src;
			p->ready[i].src = src->surface->pixels;
			p->ready[i].srcstride = (unsigned int)src->surface->pitch;
			break;
		}
		default:
			goto err;
		}
	}

	*pipeline = p;
	return 0;

err:
	smol2d_pipeline_destroy(backend_cntx, p);
	return -1;
}

struct smol2d_op *smol2d_pipeline_params(struct smol2d_pipeline *pipeline, unsigned int op)
{
	if (!pipeline || op >= pipeline->nops)
		return NULL;

	return &pipeline->ops[op];
}

int smol2d_pipeline_run(void *backend_cntx, struct smol2d_pipeline *pipeline)
{
	struct sdl_backend *be = backend_cntx;
	unsigned int i;

	if (!be || !pipeline)
		return -1;

	for (i = 0; i < pipeline->nops; i++) {
		const struct smol2d_op *op = &pipeline->ops[i];
		const struct pipeline_op *ready = &pipeline->ready[i];

		switch (op->type) {
		case SMOL2D_OP_FILL:
			if (!op->fill.rect.w || !op->fill.rect.h)
				continue;

			smol2d_c8_fill_masked(ready->pixels, ready->w, ready->h, ready->stride,
					      op->fill.rect.x, op->fill.rect.y,
					      op->fill.rect.w, op->fill.rect.h,
					      op->fill.colour.indexed.index, op->mask);

			show_call(be->show, ready->surface,
				  op->mask ? SHOW_MASK : SHOW_FILL,
				  op->fill.rect.x, op->fill.rect.y,
				  op->fill.rect.w, op->fill.rect.h);
			break;

		case SMOL2D_OP_BLIT:
			smol2d_c8_blit_masked(ready->pixels, ready->w, ready->h, ready->stride,
					      ready->src, ready->srcstride,
					      (unsigned int)ready->from.x,
					      (unsigned int)ready->from.y,
					      ready->from.w, ready->from.h,
					      op->blit.x, op->blit.y, texkey(ready->srctex),
					      op->blit.flip, op->rop, op->mask);

			show_call(be->show, ready->surface,
				  op->mask ? SHOW_MASK : SHOW_BLIT,
				  op->blit.x, op->blit.y, ready->from.w, ready->from.h);
			break;
		}
	}

	return 0;
}

void smol2d_pipeline_destroy(void *backend_cntx, struct smol2d_pipeline *pipeline)
{
	(void)backend_cntx;

	if (!pipeline)
		return;

	SDL_free(pipeline->ready);
	SDL_free(pipeline->ops);
	SDL_free(pipeline);
}

static void pumpevents(struct sdl_backend *be)
{
	drainevents(be);
}

/* What show calls to stay alive while it is holding a frame still */
static bool showpump(void *cntx)
{
	struct sdl_backend *be = cntx;

	drainevents(be);

	return be->closing;
}

int smol2d_waitkey(void *backend_cntx, unsigned int timeout)
{
	struct sdl_backend *be = backend_cntx;
	SDL_Event event;
	bool key;

	if (!be)
		return -1;

	key = drainevents(be);

	if (key || !timeout)
		return key;

	while (SDL_WaitEventTimeout(&event, (Sint32)timeout)) {
		if (takeevent(be, &event))
			return 1;

		if (be->closing)
			break;
	}

	return 0;
}

uint64_t smol2d_getticks(void *backend_cntx)
{
	struct sdl_backend *be = backend_cntx;

	if (!be)
		return 0;

	/*
	 * Watching a frame get built takes far longer than the frame is meant
	 * to last, so while show is stepping the clock counts frames instead of
	 * time and the app draws what it would have drawn at speed.
	 */
	if (be->show && be->period)
		return show_ticks(be->show, be->period);

	return (SDL_GetTicksNS() - be->start) / SDL_NS_PER_MS;
}

int smol2d_setframerate(void *backend_cntx, unsigned int fps)
{
	struct sdl_backend *be = backend_cntx;

	if (!be)
		return -1;

	be->period = fps ? SDL_NS_PER_SECOND / fps : 0;
	be->nextframe = SDL_GetTicksNS() + be->period;

	return 0;
}

static void pace(struct sdl_backend *be)
{
	Uint64 now;

	if (!be->period)
		return;

	now = SDL_GetTicksNS();
	if (now < be->nextframe)
		SDL_DelayNS(be->nextframe - now);

	be->nextframe += be->period;

	now = SDL_GetTicksNS();
	if (now > be->nextframe)
		be->nextframe = now + be->period;
}

int smol2d_present(void *backend_cntx)
{
	struct sdl_backend *be = backend_cntx;
	SDL_Surface *backbuffer, *window_fb;
	bool blitted;

	if (!be)
		return -1;

	backbuffer = be->backbuffer.surface;

	window_fb = SDL_GetWindowSurface(be->window);
	if (!window_fb)
		return fail();

	if (window_fb->w == backbuffer->w && window_fb->h == backbuffer->h)
		blitted = SDL_BlitSurface(backbuffer, NULL, window_fb, NULL);
	else
		blitted = SDL_BlitSurfaceScaled(backbuffer, NULL, window_fb, NULL, SDL_SCALEMODE_NEAREST);

	if (!blitted)
		return fail();

	if (!SDL_UpdateWindowSurface(be->window))
		return fail();

	show_present(be->show);

	pumpevents(be);
	pace(be);

	return be->closing ? 1 : 0;
}

void smol2d_close(void *backend_cntx)
{
	struct sdl_backend *be = backend_cntx;

	if (!be)
		return;

	show_close(be->show);
	SDL_DestroySurface(be->backbuffer.surface);
	SDL_DestroyWindow(be->window);
	SDL_free(be);
	SDL_Quit();
}
