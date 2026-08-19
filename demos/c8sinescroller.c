#ifndef NOLIBC
#include <stdio.h>
#include <string.h>
#endif

#include <smol2d.h>

#define BLUE	1
#define PINK	2

#define SINESTEPS	64
#define SINESHIFT	2

static const int8_t sinetable[SINESTEPS] = {
	   0,    5,    9,   14,   18,   23,   27,   30,
	  34,   37,   40,   42,   44,   46,   47,   48,
	  48,   48,   47,   46,   44,   42,   40,   37,
	  34,   30,   27,   23,   18,   14,    9,    5,
	   0,   -5,   -9,  -14,  -18,  -23,  -27,  -30,
	 -34,  -37,  -40,  -42,  -44,  -46,  -47,  -48,
	 -48,  -48,  -47,  -46,  -44,  -42,  -40,  -37,
	 -34,  -30,  -27,  -23,  -18,  -14,   -9,   -5,
};

static void makepalette(struct smol2d_palette *palette)
{
	memset(palette, 0, sizeof(*palette));

	/* light electric blue */
	palette->colours[BLUE].r = 0x7d;
	palette->colours[BLUE].g = 0xf9;
	palette->colours[BLUE].b = 0xff;

	/* electric pink */
	palette->colours[PINK].r = 0xe8;
	palette->colours[PINK].g = 0x2a;
	palette->colours[PINK].b = 0xff;
}

static void makewave(struct smol2d_mask *mask, unsigned int phase)
{
	unsigned int x, y;

	memset(mask->bits, 0, (size_t)mask->stride * mask->h);

	for (x = 0; x < mask->w; x++) {
		int top = (int)(mask->h / 2) +
			  sinetable[((x >> SINESHIFT) + phase) & (SINESTEPS - 1)];

		if (top < 0)
			top = 0;

		for (y = (unsigned int)top; y < mask->h; y++)
			mask->bits[(size_t)y * mask->stride + x / 8] |= 0x80u >> (x % 8);
	}
}

int main(void)
{
	const struct smol2d_colour blue = { .indexed = { .index = BLUE } };
	const struct smol2d_colour pink = { .indexed = { .index = PINK } };
	struct smol2d_palette palette;
	struct smol2d_tex *backbuffer;
	struct smol2d_mask *wave;
	void *backend_cntx;
	unsigned int phase = 0;

	if (smol2d_init(&backend_cntx, SMOL2D_CS_C8)) {
		fprintf(stderr, "could not open a display\n");
		return 1;
	}

	backbuffer = smol2d_getbackbuffer(backend_cntx);
	if (!backbuffer) {
		fprintf(stderr, "no backbuffer\n");
		smol2d_close(backend_cntx);
		return 1;
	}
	printf("%ux%u\n", backbuffer->w, backbuffer->h);

	makepalette(&palette);
	if (smol2d_setpalette(backend_cntx, &palette)) {
		fprintf(stderr, "no palette\n");
		smol2d_close(backend_cntx);
		return 1;
	}

	if (smol2d_mask_create(backend_cntx, &wave, backbuffer->w, backbuffer->h)) {
		fprintf(stderr, "no mask\n");
		smol2d_close(backend_cntx);
		return 1;
	}

	while (!smol2d_waitkey(backend_cntx, 0)) {
		makewave(wave, phase++);

		smol2d_setmask(backend_cntx, NULL);
		if (smol2d_tex_clear(backend_cntx, backbuffer, &blue))
			break;

		smol2d_setmask(backend_cntx, wave);
		if (smol2d_tex_clear(backend_cntx, backbuffer, &pink))
			break;

		if (smol2d_present(backend_cntx))
			break;
	}

	smol2d_setmask(backend_cntx, NULL);
	smol2d_mask_destroy(backend_cntx, wave);
	smol2d_close(backend_cntx);
	return 0;
}
