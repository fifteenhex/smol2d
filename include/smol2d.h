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

/*
 * Filling and copying runs of pixels.
 *
 * nolibc's memset is a byte at a time with an asm barrier in the loop to stop
 * the compiler making anything of it, and its memcpy and memmove are the same.
 * On a 68040 that is the slowest part of a frame by a long way: a screen's
 * worth measures about a megabyte a second, which is a couple of frames a
 * second before anything has been drawn.
 *
 * So under nolibc these go a long at a time, after squaring up the alignment,
 * which is four times fewer bus cycles on a 32 bit machine. With a real libc
 * they are memset and memcpy, which will be better than anything written here.
 */
static inline void smol2d_c8_set(uint8_t *at, uint8_t value, unsigned int n)
{
#ifdef NOLIBC
	uint32_t wide = value;

	wide |= wide << 8;
	wide |= wide << 16;

	while (n && ((uintptr_t)at & 3)) {
		*at++ = value;
		n--;
	}

	while (n >= 16) {
		((uint32_t *)at)[0] = wide;
		((uint32_t *)at)[1] = wide;
		((uint32_t *)at)[2] = wide;
		((uint32_t *)at)[3] = wide;
		at += 16;
		n -= 16;
	}

	while (n >= 4) {
		*(uint32_t *)at = wide;
		at += 4;
		n -= 4;
	}

	while (n--)
		*at++ = value;
#else
	memset(at, value, n);
#endif
}

static inline void smol2d_c8_copyrun(uint8_t *to, const uint8_t *from, unsigned int n)
{
#ifdef NOLIBC
	/*
	 * Only worth widening when both ends can be, which is the usual case:
	 * rows of a texture and rows of a scanout buffer are both aligned, and
	 * their widths and pitches are multiples of four. Anything else goes a
	 * byte at a time rather than reading and writing across the grain.
	 */
	if (((uintptr_t)to ^ (uintptr_t)from) & 3) {
		while (n--)
			*to++ = *from++;

		return;
	}

	while (n && ((uintptr_t)to & 3)) {
		*to++ = *from++;
		n--;
	}

	while (n >= 16) {
		((uint32_t *)to)[0] = ((const uint32_t *)from)[0];
		((uint32_t *)to)[1] = ((const uint32_t *)from)[1];
		((uint32_t *)to)[2] = ((const uint32_t *)from)[2];
		((uint32_t *)to)[3] = ((const uint32_t *)from)[3];
		to += 16;
		from += 16;
		n -= 16;
	}

	while (n >= 4) {
		*(uint32_t *)to = *(const uint32_t *)from;
		to += 4;
		from += 4;
		n -= 4;
	}

	while (n--)
		*to++ = *from++;
#else
	memcpy(to, from, n);
#endif
}

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
			smol2d_c8_copyrun(to, from, w);
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
			smol2d_c8_set(to, index, w);
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
		smol2d_c8_copyrun((uint8_t *)dst + (size_t)row * dstpitch,
				  src + (size_t)row * w, w);
}

#endif /* __SMOL2D_H */
