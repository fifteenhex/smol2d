#ifndef __SMOL2D_H
#define __SMOL2D_H

#ifndef NOLIBC
#include <stdint.h>
#include <string.h>
#endif

struct smol2d_tex {
	unsigned int w, h;
};

struct smol2d_sprite {
	struct smol2d_tex *tex;
	unsigned int x, y;
};

struct smol2d_colour_chunky {
	uint8_t r, g, b;
};

struct smol2d_colour_indexed {
	uint8_t index;
};

struct smol2d_colour {
	union {
		struct smol2d_colour_chunky chunky;
		struct smol2d_colour_indexed indexed;
	};
};

struct smol2d_palette {
	struct smol2d_colour_chunky colours[256];
};

struct smol2d_rect {
	int x, y;
	unsigned int w, h;
};

struct smol2d_mask {
	unsigned int w, h;
	unsigned int stride;
	uint8_t *bits;
};

struct smol2d_drawlist {
	struct smol2d_sprite **sprites;
	unsigned int nsprites;
};

enum smol2d_colourspace {
	/* 8bit indexed colour */
	SMOL2D_CS_C8
};

/* Bring up the backend */
int smol2d_init(void **backend_cntx, enum smol2d_colourspace cs);

/* For indexed colour modes set the palette */
int smol2d_setpalette(void *backend_cntx, const struct smol2d_palette *palette);

int smol2d_setclip(void *backend_cntx, const struct smol2d_rect *clip);

int smol2d_setmask(void *backend_cntx, const struct smol2d_mask *mask);

int smol2d_mask_create(void *backend_cntx, struct smol2d_mask **mask,
		       unsigned int width, unsigned int height);
void smol2d_mask_destroy(void *backend_cntx, struct smol2d_mask *mask);

/* Get a texture for the screen you should be building */
struct smol2d_tex *smol2d_getbackbuffer(void *backend_cntx);


int smol2d_tex_create(void *backend_cntx,
		      struct smol2d_tex **tex,
		      unsigned int width, unsigned int height);

void smol2d_tex_destroy(void *backend_cntx, struct smol2d_tex *tex);

int smol2d_tex_load(void *backend_cntx, struct smol2d_tex *tex, const uint8_t *pixels);

int smol2d_tex_clear(void *backend_cntx, struct smol2d_tex *tex, const struct smol2d_colour *colour);

int smol2d_tex_setkey(void *backend_cntx, struct smol2d_tex *tex, int key);

int smol2d_tex_renderto(void *backend_cntx, struct smol2d_tex *tex, struct smol2d_drawlist *drawlist);

/* Put the backbuffer onto the screen */

int smol2d_present(void *backend_cntx);

/*
 * Drawing described as data. The list is handed over once and made ready then;
 * per frame only the fields marked dynamic are written, through
 * smol2d_pipeline_params(), which hands back the pipeline's own copy of an op.
 * Whatever the backend can work out in advance it works out once.
 */
enum smol2d_optype {
	SMOL2D_OP_FILL,
};

struct smol2d_op {
	enum smol2d_optype type;		/* structural */
	struct smol2d_tex *dst;			/* structural */

	union {
		struct {
			struct smol2d_rect rect;	/* dynamic */
			struct smol2d_colour colour;	/* dynamic */
		} fill;
	};
};

struct smol2d_pipeline;

int smol2d_pipeline_create(void *backend_cntx, const struct smol2d_op *ops,
			   unsigned int nops, struct smol2d_pipeline **pipeline);
struct smol2d_op *smol2d_pipeline_params(struct smol2d_pipeline *pipeline, unsigned int op);
int smol2d_pipeline_run(void *backend_cntx, struct smol2d_pipeline *pipeline);
void smol2d_pipeline_destroy(void *backend_cntx, struct smol2d_pipeline *pipeline);

uint64_t smol2d_getticks(void *backend_cntx);

int smol2d_setframerate(void *backend_cntx, unsigned int fps);

/* 0 == non-blocking  */
int smol2d_waitkey(void *backend_cntx, unsigned int timeout);

/* Pack up and go home */
void smol2d_close(void *backend_cntx);

/* Helpers */

static inline void smol2d_c8_blit(uint8_t *dst, unsigned int dstw, unsigned int dsth,
				  const uint8_t *src, unsigned int srcw, unsigned int srch,
				  unsigned int x, unsigned int y, int key)
{
	unsigned int w, h, row, col;

	if (x >= dstw || y >= dsth)
		return;

	w = srcw < dstw - x ? srcw : dstw - x;
	h = srch < dsth - y ? srch : dsth - y;

	for (row = 0; row < h; row++) {
		const uint8_t *from = src + (size_t)row * srcw;
		uint8_t *to = dst + (size_t)(y + row) * dstw + x;

		if (key < 0) {
			memcpy(to, from, w);
			continue;
		}

		for (col = 0; col < w; col++) {
			if (from[col] != (uint8_t)key)
				to[col] = from[col];
		}
	}
}

static inline int smol2d_mask_bit(const struct smol2d_mask *mask,
				  unsigned int x, unsigned int y)
{
	if (x >= mask->w || y >= mask->h)
		return 0;

	return mask->bits[(size_t)y * mask->stride + x / 8] & (0x80u >> (x % 8));
}

static inline void smol2d_c8_fill_masked(uint8_t *dst, unsigned int dstw, unsigned int dsth,
					 unsigned int dststride,
					 int x, int y, unsigned int w, unsigned int h,
					 uint8_t index, const struct smol2d_mask *mask)
{
	unsigned int row, col;

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

	if ((unsigned int)x >= dstw || (unsigned int)y >= dsth)
		return;

	if (w > dstw - (unsigned int)x)
		w = dstw - (unsigned int)x;
	if (h > dsth - (unsigned int)y)
		h = dsth - (unsigned int)y;

	for (row = 0; row < h; row++) {
		unsigned int dy = (unsigned int)y + row;
		uint8_t *to = dst + (size_t)dy * dststride + (unsigned int)x;

		if (!mask) {
			memset(to, index, w);
			continue;
		}

		for (col = 0; col < w; col++) {
			if (smol2d_mask_bit(mask, (unsigned int)x + col, dy))
				to[col] = index;
		}
	}
}

static inline void smol2d_c8_fill(uint8_t *dst, unsigned int dstw, unsigned int dsth,
				  int x, int y, unsigned int w, unsigned int h,
				  uint8_t index)
{
	smol2d_c8_fill_masked(dst, dstw, dsth, dstw, x, y, w, h, index, NULL);
}

static inline void smol2d_c8_copy(void *dst, unsigned int dstpitch,
				  const uint8_t *src, unsigned int w, unsigned int h)
{
	unsigned int row;

	for (row = 0; row < h; row++)
		memcpy((uint8_t *)dst + (size_t)row * dstpitch,
		       src + (size_t)row * w, w);
}

#endif /* __SMOL2D_H */
