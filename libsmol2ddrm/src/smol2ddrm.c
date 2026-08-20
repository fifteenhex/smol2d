#ifndef NOLIBC
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
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

#include <smol2d.h>

#define NBUFFERS	2

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
	unsigned int back;
	unsigned int front;	/* the one being scanned out */
	bool canflip;
	bool novblank;
	bool verbose;
	uint32_t palette[256];
	struct drm_tex backbuffer;
	struct smol2d_rect clip;
	bool hasclip;
	const struct smol2d_mask *mask;
	uint64_t start;
	uint64_t period;
	uint64_t nextframe;
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
	be->front = 0;

	/*
	 * Knobs for working out where a present goes on hardware that cannot be
	 * watched any other way. Waiting for the vblank and flipping are the
	 * two things it does besides the copy, and each can be turned off on
	 * its own, so the difference says which one costs.
	 */
	be->canflip = !getenv("SMOL2D_DRM_NOFLIP");
	be->novblank = getenv("SMOL2D_DRM_NOVBLANK") != NULL;
	be->verbose = getenv("SMOL2D_DRM_VERBOSE") != NULL;

	if (!be->canflip)
		be->back = 0;

	be->backbuffer.tex.w = be->mode.hdisplay;
	be->backbuffer.tex.h = be->mode.vdisplay;
	be->backbuffer.key = -1;
	be->backbuffer.builtin = true;
	be->backbuffer.pixels = calloc((size_t)be->backbuffer.tex.w, be->backbuffer.tex.h);
	if (!be->backbuffer.pixels)
		goto err_buffers;

	if (be->verbose)
		printf("smol2d: %ux%u at %uHz, %u buffers, flipping %s, vblank wait %s\n",
		       be->mode.hdisplay, be->mode.vdisplay, be->mode.vrefresh,
		       NBUFFERS, be->canflip ? "on" : "off",
		       be->novblank ? "off" : "on");

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

	smol2d_c8_copyrun(drmtex->pixels, pixels, drmtex->tex.w * drmtex->tex.h);

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
		smol2d_c8_set(drmtex->pixels, colour->indexed.index,
			      drmtex->tex.w * drmtex->tex.h);
		return 0;
	}

	if (!be->hasclip) {
		smol2d_c8_fill_masked(drmtex->pixels, drmtex->tex.w, drmtex->tex.h,
				      drmtex->tex.w, 0, 0, drmtex->tex.w, drmtex->tex.h,
				      colour->indexed.index, be->mask);
		return 0;
	}

	smol2d_c8_fill_masked(drmtex->pixels, drmtex->tex.w, drmtex->tex.h,
			      drmtex->tex.w, be->clip.x, be->clip.y,
			      be->clip.w, be->clip.h,
			      colour->indexed.index, be->mask);

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
	struct drm_tex *drmtex = (struct drm_tex *)tex;
	unsigned int i;

	if (!backend_cntx || !drmtex || !drawlist)
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
	}

	return 0;
}

/* stub */
int smol2d_waitkey(void *backend_cntx, unsigned int timeout)
{
	(void)timeout;

	return backend_cntx ? 0 : -1;
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
 * A made ready pipeline. The op list is copied in and what the backend can
 * settle in advance is settled here: where each target's pixels are and how
 * big it is. Running it does none of that again, so a frame costs the dynamic
 * fields the caller wrote and the paint itself.
 */
struct pipeline_op {
	uint8_t *pixels;
	unsigned int w, h;
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

		if (ops[i].type != SMOL2D_OP_FILL || !dst) {
			smol2d_pipeline_destroy(backend_cntx, p);
			return -1;
		}

		p->ready[i].pixels = dst->pixels;
		p->ready[i].w = dst->tex.w;
		p->ready[i].h = dst->tex.h;
	}

	*pipeline = p;
	return 0;
}

struct smol2d_op *smol2d_pipeline_params(struct smol2d_pipeline *pipeline, unsigned int op)
{
	if (!pipeline || op >= pipeline->nops)
		return NULL;

	return &pipeline->ops[op];
}

int smol2d_pipeline_run(void *backend_cntx, struct smol2d_pipeline *pipeline)
{
	unsigned int i;

	if (!backend_cntx || !pipeline)
		return -1;

	for (i = 0; i < pipeline->nops; i++) {
		const struct smol2d_op *op = &pipeline->ops[i];
		const struct pipeline_op *ready = &pipeline->ready[i];

		if (!op->fill.rect.w || !op->fill.rect.h)
			continue;

		smol2d_c8_fill(ready->pixels, ready->w, ready->h,
			       op->fill.rect.x, op->fill.rect.y,
			       op->fill.rect.w, op->fill.rect.h,
			       op->fill.colour.indexed.index);
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

/*
 * A driver that will not flip used to get a full modeset instead, once a
 * frame, which on e17 is a tenth of a second whether or not anything was
 * drawn. Nothing about that is better than not flipping at all: the buffer on
 * screen can be drawn into directly, which is what every machine did before
 * page flipping existed. So the first refusal is the last one asked for, and
 * from then on there is one buffer and it is the one being scanned out.
 */
static void nomoreflipping(struct drm_backend *be)
{
	be->canflip = false;
	be->back = be->front;

	if (be->verbose)
		printf("smol2d: the page flip was refused, so every frame would have\n"
		       "smol2d: been a modeset. Drawing into the buffer on screen instead.\n");

	fprintf(stderr,
		"smol2d: this driver will not page flip, so drawing straight into\n"
		"smol2d: the buffer on screen from now on. Expect tearing, not a\n"
		"smol2d: modeset a frame.\n");
}

int smol2d_present(void *backend_cntx)
{
	struct drm_backend *be = backend_cntx;
	struct smoldrm_dumbbuffer *buffer;

	if (!be)
		return -1;

	buffer = &be->buffers[be->back];

	smol2d_c8_copy(buffer->mapped, SMOLDRM_DUMBBUFFER_PITCH(buffer),
		       be->backbuffer.pixels, be->backbuffer.tex.w, be->backbuffer.tex.h);

	if (be->canflip) {
		if (!be->novblank)
			smoldrm_waitforvblank(be->card);

		if (smoldrm_pageflip(be->card, be->crtc_id, buffer->fbid)) {
			nomoreflipping(be);
		} else {
			be->front = be->back;
			be->back = (be->back + 1) % NBUFFERS;
		}
	}

	pace(be);

	return 0;
}

void smol2d_close(void *backend_cntx)
{
	struct drm_backend *be = backend_cntx;
	unsigned int i;

	if (!be)
		return;

	for (i = 0; i < NBUFFERS; i++)
		smoldrm_cleanupdumbbuffer(&be->buffers[i]);

	free(be->backbuffer.pixels);
	smoldrm_close(be->card);
	free(be);
}
