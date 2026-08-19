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
	bool canflip;
	uint32_t palette[256];
	struct drm_tex backbuffer;
	struct smol2d_rect clip;
	bool hasclip;
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

int smol2d_tex_clear(void *backend_cntx, struct smol2d_tex *tex, const struct smol2d_colour *colour)
{
	struct drm_backend *be = backend_cntx;
	struct drm_tex *drmtex = (struct drm_tex *)tex;

	if (!be || !drmtex || !colour)
		return -1;

	if (!be->hasclip) {
		memset(drmtex->pixels, colour->indexed.index,
		       (size_t)drmtex->tex.w * drmtex->tex.h);
		return 0;
	}

	smol2d_c8_fill(drmtex->pixels, drmtex->tex.w, drmtex->tex.h,
		       be->clip.x, be->clip.y, be->clip.w, be->clip.h,
		       colour->indexed.index);

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

int smol2d_present(void *backend_cntx)
{
	struct drm_backend *be = backend_cntx;
	struct smoldrm_dumbbuffer *buffer;

	if (!be)
		return -1;

	buffer = &be->buffers[be->back];

	smol2d_c8_copy(buffer->mapped, SMOLDRM_DUMBBUFFER_PITCH(buffer),
		       be->backbuffer.pixels, be->backbuffer.tex.w, be->backbuffer.tex.h);

	smoldrm_waitforvblank(be->card);

	if (smoldrm_pageflip(be->card, be->crtc_id, buffer->fbid)) {
		if (smoldrm_attachdumbbuffertocrtc(buffer, be->conn_id, be->crtc_id, &be->mode))
			return -1;
	}

	be->back = (be->back + 1) % NBUFFERS;

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
