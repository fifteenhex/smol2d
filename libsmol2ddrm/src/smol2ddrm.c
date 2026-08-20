#ifndef NOLIBC
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

#include <smoldrm.h>
#include <smolinput.h>

#include <smol2d.h>

#define NBUFFERS	2

/*
 * What has changed since a buffer was last put on screen. Each buffer keeps
 * its own list: the one about to be shown was last seen NBUFFERS frames ago,
 * so it needs everything drawn since then, not just this frame's worth.
 *
 * The list is capped at what the kernel will take as damage in one go. When it
 * fills up neighbouring rectangles are merged rather than the whole thing
 * being given up on, so it stays bounded without falling back to the screen.
 */
#define MAXDAMAGE	DRM_MODE_FB_DIRTY_MAX_CLIPS

struct damage {
	struct smol2d_rect rects[MAXDAMAGE];
	unsigned int n;
	unsigned long area;
	bool all;
};

struct drm_tex {
	struct smol2d_tex tex;
	uint8_t *pixels;
	int key;
	bool builtin;
};

struct drm_backend {
	int card;
	uint32_t conn_id;
	uint32_t crtc_id;
	struct drm_mode_modeinfo mode;
	struct smoldrm_dumbbuffer buffers[NBUFFERS];
	struct damage damage[NBUFFERS];
	unsigned int back;
	bool canflip;
	bool nodirty;
	uint32_t palette[256];
	struct drm_tex backbuffer;
	struct smol2d_rect clip;
	bool hasclip;
	const struct smol2d_mask *mask;
	uint64_t start;
	uint64_t period;
	uint64_t nextframe;

	struct smolinput_keyboard keyboard;
	bool haskeyboard;
	bool grabbed;
};

static int findoutput(int card, struct drm_mode_card_res *res,
		      uint32_t *conn_id, uint32_t *crtc_id,
		      struct drm_mode_modeinfo *mode)
{
	uint32_t id, encoder_id;
	unsigned int i, j;
	int ret;

	smoldrm_foreach_res_conn(i, res, id) {
		struct drm_mode_get_connector __smoldrm_cleanup_connector conn = { 0 };

		if (smoldrm_getconnector(card, id, &conn))
			continue;

		if (!smoldrm_connectorisusable(&conn))
			continue;

		/* First mode is the preferred one */
		memcpy(mode, SMOLDRM_CAST_FROM_DRM_PTR(conn.modes_ptr), sizeof(*mode));

		smoldrm_foreach_conn_enc(j, &conn, encoder_id) {
			ret = smoldrm_getcrtcforencoder(card, res, encoder_id, crtc_id);
			if (ret > 0) {
				*conn_id = id;
				return 0;
			}
		}
	}

	return -ENODEV;
}

#define NSPERSEC	1000000000ull
#define NSPERMS		1000000ull

static uint64_t now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;

	return (uint64_t)ts.tv_sec * NSPERSEC + (uint64_t)ts.tv_nsec;
}

static void merge(struct smol2d_rect *into, const struct smol2d_rect *with)
{
	int x = into->x < with->x ? into->x : with->x;
	int y = into->y < with->y ? into->y : with->y;
	int right = into->x + (int)into->w;
	int bottom = into->y + (int)into->h;

	if (with->x + (int)with->w > right)
		right = with->x + (int)with->w;
	if (with->y + (int)with->h > bottom)
		bottom = with->y + (int)with->h;

	into->x = x;
	into->y = y;
	into->w = (unsigned int)(right - x);
	into->h = (unsigned int)(bottom - y);
}

/*
 * Fold the list down to at most want entries by merging runs of neighbours.
 * Drawing tends to arrive in some sort of order, so neighbours in the list are
 * usually neighbours on screen and the boxes stay tight. When it does not, the
 * boxes swell to cover the gaps, which is what the running total is for.
 */
static void coalesce(struct damage *d, unsigned int want)
{
	unsigned int run, at, i;

	if (d->n <= want)
		return;

	run = (d->n + want - 1) / want;

	for (at = 0, i = 0; i < d->n; at++) {
		unsigned int end = i + run < d->n ? i + run : d->n;

		d->rects[at] = d->rects[i];
		for (i++; i < end; i++)
			merge(&d->rects[at], &d->rects[i]);
	}

	d->n = at;
	d->area = 0;
	for (i = 0; i < d->n; i++)
		d->area += (unsigned long)d->rects[i].w * d->rects[i].h;
}

/* Every buffer has to be told: they all have to catch up eventually */
static void damaged(struct drm_backend *be, const struct smol2d_tex *tex,
		    int x, int y, unsigned int w, unsigned int h)
{
	struct smol2d_rect rect;
	unsigned int i;

	if (tex != &be->backbuffer.tex)
		return;

	/* the same clipping the fills do, so this is what really got painted */
	if (x < 0) {
		if ((unsigned int)-x >= w)
			return;
		w -= (unsigned int)-x;
		x = 0;
	}

	if (y < 0) {
		if ((unsigned int)-y >= h)
			return;
		h -= (unsigned int)-y;
		y = 0;
	}

	if (!w || !h ||
	    (unsigned int)x >= be->backbuffer.tex.w || (unsigned int)y >= be->backbuffer.tex.h)
		return;

	if (w > be->backbuffer.tex.w - (unsigned int)x)
		w = be->backbuffer.tex.w - (unsigned int)x;
	if (h > be->backbuffer.tex.h - (unsigned int)y)
		h = be->backbuffer.tex.h - (unsigned int)y;

	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;

	for (i = 0; i < NBUFFERS; i++) {
		struct damage *d = &be->damage[i];

		if (d->all)
			continue;

		if (d->n == MAXDAMAGE)
			coalesce(d, MAXDAMAGE / 2);

		d->rects[d->n++] = rect;
		d->area += (unsigned long)w * h;

		/*
		 * Once the pieces add up to the screen there is nothing left to
		 * save, and scattered drawing merges into boxes that cover more
		 * than they hold, so stop counting and take the whole thing.
		 */
		if (d->area >= (unsigned long)be->backbuffer.tex.w * be->backbuffer.tex.h) {
			d->all = true;
			d->n = 0;
			d->area = 0;
		}
	}
}

int smol2d_init(void **backend_cntx, enum smol2d_colourspace cs)
{
	struct drm_mode_card_res __smoldrm_cleanup_resources res = { 0 };
	struct drm_backend *be;
	unsigned int i;
	int card;

	if (!backend_cntx)
		return -1;

	if (cs != SMOL2D_CS_C8)
		return -1;

	card = smoldrm_open(getenv("SMOL2D_DRM_CARD"));
	if (card < 0)
		return -1;

	be = calloc(1, sizeof(*be));
	if (!be)
		goto err_close;

	be->card = card;
	be->start = now_ns();

	if (smoldrm_getresources(card, &res))
		goto err_free;

	if (findoutput(card, &res, &be->conn_id, &be->crtc_id, &be->mode))
		goto err_free;

	for (i = 0; i < NBUFFERS; i++) {
		if (smoldrm_dumbbuffer_c8(card, &be->mode, &be->buffers[i]))
			goto err_buffers;
	}

	if (smoldrm_attachdumbbuffertocrtc(&be->buffers[0], be->conn_id, be->crtc_id, &be->mode))
		goto err_buffers;

	be->back = 1;

	be->backbuffer.tex.w = be->mode.hdisplay;
	be->backbuffer.tex.h = be->mode.vdisplay;
	be->backbuffer.key = -1;
	be->backbuffer.builtin = true;
	be->backbuffer.pixels = calloc((size_t)be->backbuffer.tex.w, be->backbuffer.tex.h);
	if (!be->backbuffer.pixels)
		goto err_buffers;

	/* none of the buffers has anything of ours in it yet */
	damaged(be, &be->backbuffer.tex, 0, 0, be->backbuffer.tex.w, be->backbuffer.tex.h);

	*backend_cntx = be;
	return 0;

err_buffers:
	while (i--)
		smoldrm_cleanupdumbbuffer(&be->buffers[i]);
err_free:
	free(be);
err_close:
	smoldrm_close(card);
	return -1;
}

int smol2d_setpalette(void *backend_cntx, const struct smol2d_palette *palette)
{
	struct drm_backend *be = backend_cntx;
	struct smoldrm_clut clut;
	unsigned int i;

	if (!be || !palette)
		return -1;

	for (i = 0; i < SMOLDRM_CLUT_SIZE; i++) {
		const struct smol2d_colour_chunky *c = &palette->colours[i];

		smoldrm_clut_setentry(&clut, (uint8_t)i, c->r, c->g, c->b);
	}

	return smoldrm_setclut(be->card, be->crtc_id, &clut) ? -1 : 0;
}

struct smol2d_tex *smol2d_getbackbuffer(void *backend_cntx)
{
	struct drm_backend *be = backend_cntx;

	if (!be)
		return NULL;

	return &be->backbuffer.tex;
}

int smol2d_tex_create(void *backend_cntx, struct smol2d_tex **tex,
		      unsigned int width, unsigned int height)
{
	struct drm_tex *drmtex;

	if (!backend_cntx || !tex || !width || !height)
		return -1;

	drmtex = calloc(1, sizeof(*drmtex));
	if (!drmtex)
		return -1;

	drmtex->pixels = calloc((size_t)width, height);
	if (!drmtex->pixels) {
		free(drmtex);
		return -1;
	}

	drmtex->tex.w = width;
	drmtex->tex.h = height;
	drmtex->key = -1;

	*tex = &drmtex->tex;
	return 0;
}

void smol2d_tex_destroy(void *backend_cntx, struct smol2d_tex *tex)
{
	struct drm_tex *drmtex = (struct drm_tex *)tex;

	(void)backend_cntx;

	if (!drmtex || drmtex->builtin)
		return;

	free(drmtex->pixels);
	free(drmtex);
}

int smol2d_tex_load(void *backend_cntx, struct smol2d_tex *tex, const uint8_t *pixels)
{
	struct drm_tex *drmtex = (struct drm_tex *)tex;

	(void)backend_cntx;

	if (!drmtex || !pixels)
		return -1;

	memcpy(drmtex->pixels, pixels, (size_t)drmtex->tex.w * drmtex->tex.h);

	return 0;
}

int smol2d_setclip(void *backend_cntx, const struct smol2d_rect *clip)
{
	struct drm_backend *be = backend_cntx;

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

	m = calloc(1, sizeof(*m));
	if (!m)
		return -1;

	stride = (width + 7) / 8;

	m->bits = calloc(stride, height);
	if (!m->bits) {
		free(m);
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

	free(mask->bits);
	free(mask);
}

int smol2d_setmask(void *backend_cntx, const struct smol2d_mask *mask)
{
	struct drm_backend *be = backend_cntx;

	if (!be)
		return -1;

	be->mask = mask;

	return 0;
}

int smol2d_tex_clear(void *backend_cntx, struct smol2d_tex *tex, const struct smol2d_colour *colour)
{
	struct drm_backend *be = backend_cntx;
	struct drm_tex *drmtex = (struct drm_tex *)tex;

	if (!be || !drmtex || !colour)
		return -1;

	if (!be->hasclip && !be->mask) {
		memset(drmtex->pixels, colour->indexed.index,
		       (size_t)drmtex->tex.w * drmtex->tex.h);
		damaged(be, tex, 0, 0, drmtex->tex.w, drmtex->tex.h);
		return 0;
	}

	if (!be->hasclip) {
		smol2d_c8_fill_masked(drmtex->pixels, drmtex->tex.w, drmtex->tex.h,
				      drmtex->tex.w, 0, 0, drmtex->tex.w, drmtex->tex.h,
				      colour->indexed.index, be->mask);
		damaged(be, tex, 0, 0, drmtex->tex.w, drmtex->tex.h);
		return 0;
	}

	smol2d_c8_fill_masked(drmtex->pixels, drmtex->tex.w, drmtex->tex.h,
			      drmtex->tex.w, be->clip.x, be->clip.y,
			      be->clip.w, be->clip.h,
			      colour->indexed.index, be->mask);
	damaged(be, tex, be->clip.x, be->clip.y, be->clip.w, be->clip.h);

	return 0;
}

int smol2d_tex_setkey(void *backend_cntx, struct smol2d_tex *tex, int key)
{
	struct drm_tex *drmtex = (struct drm_tex *)tex;

	(void)backend_cntx;

	if (!drmtex || key > 255)
		return -1;

	drmtex->key = key < 0 ? -1 : key;

	return 0;
}

int smol2d_tex_renderto(void *backend_cntx, struct smol2d_tex *tex, struct smol2d_drawlist *drawlist)
{
	struct drm_backend *be = backend_cntx;
	struct drm_tex *drmtex = (struct drm_tex *)tex;
	unsigned int i;

	if (!be || !drmtex || !drawlist)
		return -1;

	for (i = 0; i < drawlist->nsprites; i++) {
		const struct smol2d_sprite *sprite = drawlist->sprites[i];
		const struct drm_tex *from;

		if (!sprite || !sprite->tex)
			continue;

		from = (const struct drm_tex *)sprite->tex;
		smol2d_c8_blit(drmtex->pixels, drmtex->tex.w, drmtex->tex.h,
			       from->pixels, from->tex.w, from->tex.h,
			       sprite->x, sprite->y, from->key);
		damaged(be, tex, sprite->x, sprite->y, from->tex.w, from->tex.h);
	}

	return 0;
}

/* the few a game names, out of the hundreds an input device can send */
static enum smol2d_key whichkey(uint16_t code)
{
	switch (code) {
	case KEY_SPACE:
		return SMOL2D_KEY_SPACE;
	case KEY_UP:
		return SMOL2D_KEY_UP;
	case KEY_DOWN:
		return SMOL2D_KEY_DOWN;
	case KEY_LEFT:
		return SMOL2D_KEY_LEFT;
	case KEY_RIGHT:
		return SMOL2D_KEY_RIGHT;
	case KEY_ENTER:
	case KEY_KPENTER:
		return SMOL2D_KEY_ENTER;
	case KEY_ESC:
		return SMOL2D_KEY_ESC;
	default:
		return SMOL2D_KEY_OTHER;
	}
}

int smol2d_getkey(void *backend_cntx, enum smol2d_key *key, int *down)
{
	struct drm_backend *be = backend_cntx;
	struct smolinput_key got;
	int ret;

	if (!be || !key || !down)
		return -1;

	if (!be->haskeyboard)
		return 0;

	ret = smolinput_readkey(&be->keyboard, &got);
	if (ret <= 0)
		return ret;

	/* a key that repeats has not been let go, so it is still down */
	*key = whichkey(got.code);
	*down = got.state != SMOLINPUT_RELEASED;

	return 1;
}

int smol2d_waitkey(void *backend_cntx, unsigned int timeout)
{
	struct drm_backend *be = backend_cntx;
	struct smolinput_key got;
	int ret;

	if (!be)
		return -1;

	if (!be->haskeyboard)
		return 0;

	ret = smolinput_waitkey(&be->keyboard, &got, timeout ? (int)timeout : 0);
	if (ret <= 0)
		return ret;

	/* only a press counts, or letting go of the key that got here counts */
	return got.state == SMOLINPUT_PRESSED;
}

uint64_t smol2d_getticks(void *backend_cntx)
{
	struct drm_backend *be = backend_cntx;

	if (!be)
		return 0;

	return (now_ns() - be->start) / NSPERMS;
}

int smol2d_setframerate(void *backend_cntx, unsigned int fps)
{
	struct drm_backend *be = backend_cntx;

	if (!be)
		return -1;

	be->period = fps ? NSPERSEC / fps : 0;
	be->nextframe = now_ns() + be->period;

	return 0;
}

static void pace(struct drm_backend *be)
{
	uint64_t now = now_ns();

	if (!be->period)
		return;

	if (now < be->nextframe) {
		struct timespec left = {
			.tv_sec = (time_t)((be->nextframe - now) / NSPERSEC),
			.tv_nsec = (long)((be->nextframe - now) % NSPERSEC),
		};

		nanosleep(&left, NULL);
	}

	be->nextframe += be->period;

	now = now_ns();
	if (now > be->nextframe)
		be->nextframe = now + be->period;
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

/*
 * A made ready pipeline. The op list is copied in and what the backend can
 * settle in advance is settled here: where each target's pixels are and how
 * big it is. Running it does none of that again, so a frame costs the dynamic
 * fields the caller wrote and the paint itself.
 */
struct pipeline_op {
	uint8_t *pixels;
	unsigned int w, h;

	/* blits: where the source is and which part of it is wanted */
	const struct drm_tex *srctex;
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

	p = calloc(1, sizeof(*p));
	if (!p)
		return -1;

	p->ops = calloc(nops ? nops : 1, sizeof(*p->ops));
	p->ready = calloc(nops ? nops : 1, sizeof(*p->ready));
	if (!p->ops || !p->ready) {
		smol2d_pipeline_destroy(backend_cntx, p);
		return -1;
	}

	if (nops)
		memcpy(p->ops, ops, nops * sizeof(*ops));
	p->nops = nops;

	for (i = 0; i < nops; i++) {
		const struct drm_tex *dst = (const struct drm_tex *)ops[i].dst;

		if (!dst)
			goto err;

		p->ready[i].pixels = dst->pixels;
		p->ready[i].w = dst->tex.w;
		p->ready[i].h = dst->tex.h;

		switch (ops[i].type) {
		case SMOL2D_OP_FILL:
			break;
		case SMOL2D_OP_BLIT: {
			const struct drm_tex *src = (const struct drm_tex *)ops[i].blit.src;

			if (!src || resolvefrom(&p->ready[i].from, &ops[i].blit.from,
						src->tex.w, src->tex.h))
				goto err;

			p->ready[i].srctex = src;
			p->ready[i].src = src->pixels;
			p->ready[i].srcstride = src->tex.w;
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
	struct drm_backend *be = backend_cntx;
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

			smol2d_c8_fill_masked(ready->pixels, ready->w, ready->h, ready->w,
					      op->fill.rect.x, op->fill.rect.y,
					      op->fill.rect.w, op->fill.rect.h,
					      op->fill.colour.indexed.index, op->mask);
			damaged(be, op->dst, op->fill.rect.x, op->fill.rect.y,
				op->fill.rect.w, op->fill.rect.h);
			break;

		case SMOL2D_OP_BLIT:
			smol2d_c8_blit_masked(ready->pixels, ready->w, ready->h, ready->w,
					      ready->src, ready->srcstride,
					      (unsigned int)ready->from.x,
					      (unsigned int)ready->from.y,
					      ready->from.w, ready->from.h,
					      op->blit.x, op->blit.y, ready->srctex->key,
					      op->blit.flip, op->rop, op->mask);
			damaged(be, op->dst, op->blit.x, op->blit.y,
				ready->from.w, ready->from.h);
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

	free(pipeline->ready);
	free(pipeline->ops);
	free(pipeline);
}

/* Only what changed goes over, which on a slow bus is most of the frame time */
static void flush(struct drm_backend *be, struct smoldrm_dumbbuffer *buffer,
		  const struct damage *d)
{
	unsigned int pitch = SMOLDRM_DUMBBUFFER_PITCH(buffer);
	unsigned int stride = be->backbuffer.tex.w;
	unsigned int i, row;

	if (d->all) {
		smol2d_c8_copy(buffer->mapped, pitch, be->backbuffer.pixels,
			       be->backbuffer.tex.w, be->backbuffer.tex.h);
		return;
	}

	for (i = 0; i < d->n; i++) {
		const struct smol2d_rect *r = &d->rects[i];
		uint8_t *to = (uint8_t *)buffer->mapped +
			      (size_t)r->y * pitch + (unsigned int)r->x;
		const uint8_t *from = be->backbuffer.pixels +
				      (size_t)r->y * stride + (unsigned int)r->x;

		if (r->w == 1) {
			for (row = 0; row < r->h; row++, to += pitch, from += stride)
				*to = *from;
			continue;
		}

		for (row = 0; row < r->h; row++, to += pitch, from += stride)
			memcpy(to, from, r->w);
	}
}

/*
 * Hand the kernel the same rectangles. Drivers that keep the screen in their
 * own memory copy only these instead of the lot; the ones that scan out of our
 * buffer directly have no dirty handler at all and the ioctl comes back as
 * ENOSYS, so ask once and take the answer.
 */
static void hint(struct drm_backend *be, uint32_t fbid, const struct damage *d)
{
	struct drm_clip_rect clips[MAXDAMAGE];
	struct drm_mode_fb_dirty_cmd dirty = { 0 };
	unsigned int i;

	if (be->nodirty || (!d->n && !d->all))
		return;

	if (d->all) {
		clips[0].x1 = 0;
		clips[0].y1 = 0;
		clips[0].x2 = (unsigned short)be->backbuffer.tex.w;
		clips[0].y2 = (unsigned short)be->backbuffer.tex.h;
	}

	for (i = 0; !d->all && i < d->n; i++) {
		const struct smol2d_rect *r = &d->rects[i];

		clips[i].x1 = (unsigned short)r->x;
		clips[i].y1 = (unsigned short)r->y;
		clips[i].x2 = (unsigned short)(r->x + (int)r->w);
		clips[i].y2 = (unsigned short)(r->y + (int)r->h);
	}

	dirty.fb_id = fbid;
	dirty.num_clips = d->all ? 1 : d->n;
	dirty.clips_ptr = SMOLDRM_CAST_TO_DRM_PTR(clips);

	if (ioctl(be->card, DRM_IOCTL_MODE_DIRTYFB, &dirty))
		be->nodirty = true;
}

int smol2d_present(void *backend_cntx)
{
	struct drm_backend *be = backend_cntx;
	struct smoldrm_dumbbuffer *buffer;
	struct damage *d;

	if (!be)
		return -1;

	buffer = &be->buffers[be->back];
	d = &be->damage[be->back];

	flush(be, buffer, d);
	hint(be, buffer->fbid, d);
	d->n = 0;
	d->area = 0;
	d->all = false;

	smoldrm_waitforvblank(be->card);

	if (smoldrm_pageflip(be->card, be->crtc_id, buffer->fbid)) {
		if (smoldrm_attachdumbbuffertocrtc(buffer, be->conn_id, be->crtc_id, &be->mode))
			return -1;
	}

	be->back = (be->back + 1) % NBUFFERS;
	pace(be);

	return 0;
}

void smol2d_close(void *backend_cntx)
{
	struct drm_backend *be = backend_cntx;
	unsigned int i;

	if (!be)
		return;

	if (be->haskeyboard) {
		if (be->grabbed)
			smolinput_grab(&be->keyboard, 0);

		smolinput_close(&be->keyboard);
	}

	for (i = 0; i < NBUFFERS; i++)
		smoldrm_cleanupdumbbuffer(&be->buffers[i]);

	free(be->backbuffer.pixels);
	smoldrm_close(be->card);
	free(be);
}
