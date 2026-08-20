#ifndef __SMOL2D_H
#define __SMOL2D_H

#ifndef NOLIBC
#include <stdint.h>
#include <string.h>
#endif

struct smol2d_tex {
	unsigned int w, h;
};

struct smol2d_rect {
	int x, y;
	unsigned int w, h;
};

/*
 * Positions are signed: sprites come in from the left and the top, and a
 * source rectangle picks a frame out of a sheet rather than needing a texture
 * per frame. A zero sized source rectangle means the whole texture.
 */
struct smol2d_sprite {
	struct smol2d_tex *tex;
	struct smol2d_rect from;
	int x, y;
	unsigned int flip;
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

struct smol2d_mask {
	unsigned int w, h;
	unsigned int stride;
	uint8_t *bits;
};

struct smol2d_drawlist {
	struct smol2d_sprite **sprites;
	unsigned int nsprites;
};

/*
 * How a source pixel is combined with the one already there. Only the useful
 * few rather than all the minterms an Amiga blitter can do; XOR is worth
 * having on its own, since it is how a cursor or a rubber band is drawn and
 * undrawn without keeping a copy of the background.
 */
enum smol2d_rop {
	SMOL2D_ROP_COPY,
	SMOL2D_ROP_AND,
	SMOL2D_ROP_OR,
	SMOL2D_ROP_XOR,
};

#define SMOL2D_FLIP_X	(1u << 0)
#define SMOL2D_FLIP_Y	(1u << 1)

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
	SMOL2D_OP_BLIT,
};

struct smol2d_op {
	enum smol2d_optype type;		/* structural */
	struct smol2d_tex *dst;			/* structural */
	const struct smol2d_mask *mask;		/* structural, none if null */
	enum smol2d_rop rop;			/* structural */

	union {
		struct {
			struct smol2d_rect rect;	/* dynamic */
			struct smol2d_colour colour;	/* dynamic */
		} fill;

		struct {
			struct smol2d_tex *src;		/* structural */
			struct smol2d_rect from;	/* structural */
			unsigned int flip;		/* structural */
			int x, y;			/* dynamic */
		} blit;
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

/*
 * Keys, as far as a game is concerned. The backends know them by different
 * numbers -- one reads a linux input device, the other asks SDL -- so this is
 * the small set that both can name and that a demo actually uses.
 */
enum smol2d_key {
	SMOL2D_KEY_OTHER = 0,
	SMOL2D_KEY_SPACE,
	SMOL2D_KEY_UP,
	SMOL2D_KEY_DOWN,
	SMOL2D_KEY_LEFT,
	SMOL2D_KEY_RIGHT,
	SMOL2D_KEY_ENTER,
	SMOL2D_KEY_ESC,
};

/*
 * The next key that was pressed or let go: 1 if there was one, 0 if not.
 * Keys the list above has no name for come back as SMOL2D_KEY_OTHER rather
 * than being dropped, so waiting for "any key" still works.
 */
int smol2d_getkey(void *backend_cntx, enum smol2d_key *key, int *down);

/* 0 == non-blocking  */
int smol2d_waitkey(void *backend_cntx, unsigned int timeout);

/* Pack up and go home */
void smol2d_close(void *backend_cntx);

/* Helpers */

static inline uint8_t smol2d_c8_combine(uint8_t was, uint8_t with, enum smol2d_rop rop)
{
	switch (rop) {
	case SMOL2D_ROP_AND:
		return was & with;
	case SMOL2D_ROP_OR:
		return was | with;
	case SMOL2D_ROP_XOR:
		return was ^ with;
	case SMOL2D_ROP_COPY:
	default:
		return with;
	}
}

static inline int smol2d_mask_bit(const struct smol2d_mask *mask,
				  unsigned int x, unsigned int y)
{
	if (x >= mask->w || y >= mask->h)
		return 0;

	return mask->bits[(size_t)y * mask->stride + x / 8] & (0x80u >> (x % 8));
}

/*
 * The general blit: a rectangle of a source texture onto a destination, with
 * an optional colour key, an optional mask in destination coordinates, either
 * flip, and a raster op. Everything else is this with the arms it does not
 * need turned off.
 *
 * Flipping mirrors within the source rectangle, so the clipping has to be
 * counted in source columns and rows rather than folded into a pointer: a
 * sprite half off the left edge with FLIP_X showing has to drop the pixels
 * from the far end of its image, not the near one.
 */
static inline void smol2d_c8_blit_masked(uint8_t *dst, unsigned int dstw, unsigned int dsth,
					 unsigned int dststride,
					 const uint8_t *src, unsigned int srcstride,
					 unsigned int sx, unsigned int sy,
					 unsigned int sw, unsigned int sh,
					 int x, int y, int key, unsigned int flip,
					 enum smol2d_rop rop, const struct smol2d_mask *mask)
{
	unsigned int skipx = 0, skipy = 0, w = sw, h = sh, row, col;

	if (!sw || !sh)
		return;

	if (x < 0) {
		if ((unsigned int)-x >= w)
			return;
		skipx = (unsigned int)-x;
		w -= skipx;
		x = 0;
	}

	if (y < 0) {
		if ((unsigned int)-y >= h)
			return;
		skipy = (unsigned int)-y;
		h -= skipy;
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
		unsigned int srow = skipy + row;
		const uint8_t *from;
		uint8_t *to = dst + (size_t)dy * dststride + (unsigned int)x;

		if (flip & SMOL2D_FLIP_Y)
			srow = sh - 1 - srow;

		from = src + (size_t)(sy + srow) * srcstride + sx;

		for (col = 0; col < w; col++) {
			unsigned int scol = skipx + col;
			uint8_t pixel;

			if (flip & SMOL2D_FLIP_X)
				scol = sw - 1 - scol;

			pixel = from[scol];

			if (key >= 0 && pixel == (uint8_t)key)
				continue;

			if (mask && !smol2d_mask_bit(mask, (unsigned int)x + col, dy))
				continue;

			to[col] = smol2d_c8_combine(to[col], pixel, rop);
		}
	}
}

/* The plain one: whole texture, no mask, no flip, straight copy */
static inline void smol2d_c8_blit(uint8_t *dst, unsigned int dstw, unsigned int dsth,
				  const uint8_t *src, unsigned int srcw, unsigned int srch,
				  unsigned int x, unsigned int y, int key)
{
	smol2d_c8_blit_masked(dst, dstw, dsth, dstw, src, srcw, 0, 0, srcw, srch,
			      (int)x, (int)y, key, 0, SMOL2D_ROP_COPY, NULL);
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

	if (!mask && w == 1) {
		uint8_t *to = dst + (size_t)((unsigned int)y) * dststride + (unsigned int)x;

		for (row = 0; row < h; row++, to += dststride)
			*to = index;

		return;
	}

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
