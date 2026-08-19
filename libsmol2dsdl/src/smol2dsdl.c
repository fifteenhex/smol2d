#include <SDL3/SDL.h>

#include <smol2d.h>

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
	bool closing;
};

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

	be->window = SDL_CreateWindow("smol2d", width, height, 0);
	if (!be->window)
		goto err_free;

	if (textureinit(&be->backbuffer, NULL, (unsigned int)width, (unsigned int)height))
		goto err_window;
	be->backbuffer.builtin = true;

	be->palette = SDL_CreateSurfacePalette(be->backbuffer.surface);
	if (!be->palette)
		goto err_surface;

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
	struct sdl_tex *sdltex = (struct sdl_tex *)tex;
	unsigned int y;

	(void)backend_cntx;

	if (!sdltex || !pixels)
		return -1;

	for (y = 0; y < sdltex->tex.h; y++)
		SDL_memcpy((uint8_t *)sdltex->surface->pixels + y * sdltex->surface->pitch,
			   pixels + (size_t)y * sdltex->tex.w, sdltex->tex.w);

	return 0;
}

int smol2d_tex_clear(void *backend_cntx, struct smol2d_tex *tex, const struct smol2d_colour *colour)
{
	struct sdl_backend *be = backend_cntx;
	struct sdl_tex *sdltex = (struct sdl_tex *)tex;

	if (!be || !sdltex || !colour)
		return -1;

	if (!SDL_FillSurfaceRect(sdltex->surface, NULL, colour->indexed.index))
		return fail();

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
	struct sdl_tex *sdltex = (struct sdl_tex *)tex;
	unsigned int i;

	if (!backend_cntx || !sdltex || !drawlist)
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
	}

	return 0;
}

static bool takeevent(struct sdl_backend *be, const SDL_Event *event)
{
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

static void pumpevents(struct sdl_backend *be)
{
	drainevents(be);
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

	pumpevents(be);

	return be->closing ? 1 : 0;
}

void smol2d_close(void *backend_cntx)
{
	struct sdl_backend *be = backend_cntx;

	if (!be)
		return;

	SDL_DestroySurface(be->backbuffer.surface);
	SDL_DestroyWindow(be->window);
	SDL_free(be);
	SDL_Quit();
}
