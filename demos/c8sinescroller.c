#ifndef NOLIBC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#include <smol2d.h>

#define BLUE	1
#define PINK	2

#define SINESTEPS	64
#define SINESHIFT	2
#define SINEMAX		48

#define ENVSHIFT	1
#define AMPMIN		12
#define AMPMAX		48

#define AMPBITS		8	/* amptable is amplitude over SINEMAX, 8 bits of it */

#define WAVESPEED	128
#define FRAMERATE	60

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

/*
 * How tall the wave stands at each step of the envelope, kept as amplitude
 * over SINEMAX in 8 bits of fraction. Working it out per column per frame
 * costs two divides, and a 68040 is slower over a divide than over most of
 * what a column otherwise needs, so it is worked out once at startup instead.
 */
static uint16_t amptable[SINESTEPS];

static void maketables(void)
{
	unsigned int e;

	for (e = 0; e < SINESTEPS; e++) {
		int amp = (AMPMAX + AMPMIN) / 2 +
			  sinetable[e] * ((AMPMAX - AMPMIN) / 2) / SINEMAX;

		amptable[e] = (uint16_t)((amp << AMPBITS) / SINEMAX);
	}
}

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

/* Where the wave stands in each column: two lookups, a multiply and a shift */
static void wavetops(uint16_t *top, unsigned int width, unsigned int height,
		     unsigned int phase)
{
	unsigned int x;

	for (x = 0; x < width; x++) {
		unsigned int step = (x + phase) >> SINESHIFT;
		unsigned int envstep = (x - phase / 2) >> (SINESHIFT + ENVSHIFT);
		int y = (int)(height / 2) +
			((sinetable[step & (SINESTEPS - 1)] *
			  amptable[envstep & (SINESTEPS - 1)]) >> AMPBITS);

		if (y < 0)
			y = 0;
		if (y > (int)height)
			y = (int)height;

		top[x] = (uint16_t)y;
	}
}

/*
 * The screen already holds last frame's wave, so a column only needs the strip
 * between where its surface was and where it is now: sky repainted as wave
 * where it has risen, wave repainted as sky where it has fallen, and nothing
 * at all for a column that did not move. One fill op per column, and which
 * column it is, how wide, and what it draws into were all settled when the
 * pipeline was made ready.
 */
static void wavebands(struct smol2d_pipeline *pipeline, const uint16_t *top,
		      uint16_t *was, unsigned int width,
		      const struct smol2d_colour *blue,
		      const struct smol2d_colour *pink)
{
	unsigned int x;

	for (x = 0; x < width; x++) {
		struct smol2d_op *op = smol2d_pipeline_params(pipeline, x);

		if (top[x] == was[x]) {
			op->fill.rect.h = 0;
			continue;
		}

		if (top[x] < was[x]) {
			op->fill.rect.y = top[x];
			op->fill.rect.h = was[x] - top[x];
			op->fill.colour = *pink;
		} else {
			op->fill.rect.y = was[x];
			op->fill.rect.h = top[x] - was[x];
			op->fill.colour = *blue;
		}

		was[x] = top[x];
	}
}

int main(void)
{
	const struct smol2d_colour blue = { .indexed = { .index = BLUE } };
	const struct smol2d_colour pink = { .indexed = { .index = PINK } };
	struct smol2d_palette palette;
	struct smol2d_pipeline *pipeline;
	struct smol2d_tex *backbuffer;
	struct smol2d_op *ops;
	uint16_t *top, *was;
	void *backend_cntx;
	unsigned int width, height, x;

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
	width = backbuffer->w;
	height = backbuffer->h;
	printf("%ux%u\n", width, height);

	maketables();
	makepalette(&palette);
	if (smol2d_setpalette(backend_cntx, &palette)) {
		fprintf(stderr, "no palette\n");
		smol2d_close(backend_cntx);
		return 1;
	}

	ops = calloc(width, sizeof(*ops));
	top = calloc(width, sizeof(*top));
	was = calloc(width, sizeof(*was));
	if (!ops || !top || !was) {
		fprintf(stderr, "out of memory\n");
		smol2d_close(backend_cntx);
		return 1;
	}

	for (x = 0; x < width; x++) {
		ops[x].type = SMOL2D_OP_FILL;
		ops[x].dst = backbuffer;
		ops[x].fill.rect.x = (int)x;
		ops[x].fill.rect.w = 1;

		/* the sky reaches all the way down until the first frame draws */
		was[x] = (uint16_t)height;
	}

	if (smol2d_pipeline_create(backend_cntx, ops, width, &pipeline)) {
		fprintf(stderr, "no pipeline\n");
		smol2d_close(backend_cntx);
		return 1;
	}
	free(ops);

	/* the only time the whole screen is painted */
	if (smol2d_tex_clear(backend_cntx, backbuffer, &blue)) {
		fprintf(stderr, "could not clear\n");
		smol2d_pipeline_destroy(backend_cntx, pipeline);
		smol2d_close(backend_cntx);
		return 1;
	}

	smol2d_setframerate(backend_cntx, FRAMERATE);

	while (!smol2d_waitkey(backend_cntx, 0)) {
		unsigned int phase = (unsigned int)(smol2d_getticks(backend_cntx) *
						    WAVESPEED / 1000);

		wavetops(top, width, height, phase);
		wavebands(pipeline, top, was, width, &blue, &pink);

		if (smol2d_pipeline_run(backend_cntx, pipeline))
			break;

		if (smol2d_present(backend_cntx))
			break;
	}

	smol2d_pipeline_destroy(backend_cntx, pipeline);
	free(was);
	free(top);
	smol2d_close(backend_cntx);
	return 0;
}
