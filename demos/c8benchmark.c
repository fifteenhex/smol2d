/*
 * Where a frame goes.
 *
 * Each thing a frame is made of, timed on its own, against two baselines the
 * machine cannot beat: a memset and a memcpy of a screen's worth of ordinary
 * memory. Drawing that is much slower than the memset is slow because of us;
 * drawing that is near it is going as fast as the machine goes.
 *
 * The last few are about getting the frame onto the screen rather than
 * drawing it, and they are what differs between backends. On drm a present
 * moves the changed parts into the scanout buffer, waits for the vblank and
 * flips, and only the first of those is work: the difference between
 * presenting after nothing and presenting after a clear is that move, which
 * is printed at the end next to the memcpy it should be compared against.
 */

#ifndef NOLIBC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#include <smol2d.h>

#define MINMS		300	/* how long to keep at each one */
#define MAXBATCH	(1u << 22)

#define SPRITEW		32
#define SPRITEH		32
#define NSPRITES	64

#define COLUMNS		256	/* one pixel wide fills, the shape of the wave */
#define BAND		48	/* how tall a piece of it moves in a frame */

static void *cntx;
static struct smol2d_tex *backbuffer, *sprite;
static struct smol2d_mask *mask;
static struct smol2d_sprite sprites[NSPRITES];
static struct smol2d_sprite *spritelist[NSPRITES];
static struct smol2d_drawlist drawlist;
static uint8_t *from, *to;
static unsigned int width, height;
static unsigned long screen;

static struct smol2d_pipeline *fillscreen, *fillcolumns, *fillbands, *tinyfills;

struct result {
	const char *what;
	unsigned long long ns;		/* per go */
	unsigned long long kpixels;	/* per second, 0 when it is not about pixels */
	unsigned long batch;
};

static struct result results[24];
static unsigned int nresults;

/*
 * Time one thing. The batch doubles until it takes long enough to measure,
 * because the clock only has milliseconds in it and asking costs more than
 * some of these do -- so it is asked twice for a run rather than twice a go.
 */
static void timeit(const char *what, unsigned long pixels, void (*go)(void))
{
	unsigned long long elapsed = 0;
	unsigned long batch, n;
	uint64_t start;

	go();	/* warm up, so anything that only happens once happens here */

	for (batch = 1; batch < MAXBATCH; batch *= 2) {
		start = smol2d_getticks(cntx);

		for (n = 0; n < batch; n++)
			go();

		elapsed = smol2d_getticks(cntx) - start;
		if (elapsed >= MINMS)
			break;
	}

	if (!elapsed)
		elapsed = 1;

	results[nresults].what = what;
	results[nresults].batch = batch;
	results[nresults].ns = (elapsed * 1000000ull) / batch;
	results[nresults].kpixels = pixels ?
				   ((unsigned long long)pixels * batch) / elapsed : 0;

	/* microseconds, with enough of a fraction for the quick ones */
	printf("  %-34s %8lu %8llu.%03llu %10llu\n", what, batch,
	       results[nresults].ns / 1000, results[nresults].ns % 1000,
	       results[nresults].kpixels);

	nresults++;
}

static const struct result *find(const char *what)
{
	unsigned int i;

	for (i = 0; i < nresults; i++)
		if (!strcmp(results[i].what, what))
			return &results[i];

	return NULL;
}

/* the two the machine cannot beat */
static void b_memset(void)
{
	memset(to, 3, screen);
}

static void b_memcpy(void)
{
	memcpy(to, from, screen);
}

/* what it costs to ask the time, which pacing does every frame */
static void b_getticks(void)
{
	volatile uint64_t now = smol2d_getticks(cntx);

	(void)now;
}

/* drawing */
static void b_clear(void)
{
	const struct smol2d_colour colour = { .indexed = { .index = 1 } };

	smol2d_tex_clear(cntx, backbuffer, &colour);
}

static void b_clearclipped(void)
{
	const struct smol2d_colour colour = { .indexed = { .index = 2 } };
	struct smol2d_rect clip = { 0, 0, width, height };

	smol2d_setclip(cntx, &clip);
	smol2d_tex_clear(cntx, backbuffer, &colour);
	smol2d_setclip(cntx, NULL);
}

static void b_clearmasked(void)
{
	const struct smol2d_colour colour = { .indexed = { .index = 3 } };

	smol2d_setmask(cntx, mask);
	smol2d_tex_clear(cntx, backbuffer, &colour);
	smol2d_setmask(cntx, NULL);
}

static void b_fillscreen(void)
{
	smol2d_pipeline_run(cntx, fillscreen);
}

static void b_fillcolumns(void)
{
	smol2d_pipeline_run(cntx, fillcolumns);
}

static void b_fillbands(void)
{
	smol2d_pipeline_run(cntx, fillbands);
}

static void b_tinyfills(void)
{
	smol2d_pipeline_run(cntx, tinyfills);
}

static void b_sprites(void)
{
	smol2d_tex_renderto(cntx, backbuffer, &drawlist);
}

/* and getting it on screen */
static void b_present(void)
{
	smol2d_present(cntx);
}

static void b_clearpresent(void)
{
	b_clear();
	smol2d_present(cntx);
}

static int makepipelines(void)
{
	struct smol2d_op *ops;
	unsigned int i;

	ops = calloc(COLUMNS, sizeof(*ops));
	if (!ops)
		return -1;

	/* one fill over everything */
	ops[0].type = SMOL2D_OP_FILL;
	ops[0].dst = backbuffer;
	ops[0].fill.rect = (struct smol2d_rect){ 0, 0, width, height };
	ops[0].fill.colour.indexed.index = 4;
	if (smol2d_pipeline_create(cntx, ops, 1, &fillscreen))
		goto err;

	/* a fill per column, all the way down, which is the worst the wave does */
	for (i = 0; i < COLUMNS; i++) {
		ops[i].type = SMOL2D_OP_FILL;
		ops[i].dst = backbuffer;
		ops[i].fill.rect = (struct smol2d_rect){ (int)i, 0, 1, height };
		ops[i].fill.colour.indexed.index = 5;
	}
	if (smol2d_pipeline_create(cntx, ops, COLUMNS, &fillcolumns))
		goto err;

	/* and what it really does: a short piece of each column */
	for (i = 0; i < COLUMNS; i++)
		ops[i].fill.rect = (struct smol2d_rect){ (int)i, (int)(height / 2), 1, BAND };
	if (smol2d_pipeline_create(cntx, ops, COLUMNS, &fillbands))
		goto err;

	/* the same number of ops doing one pixel each, to price the op itself */
	for (i = 0; i < COLUMNS; i++)
		ops[i].fill.rect = (struct smol2d_rect){ (int)i, 0, 1, 1 };
	if (smol2d_pipeline_create(cntx, ops, COLUMNS, &tinyfills))
		goto err;

	free(ops);
	return 0;

err:
	free(ops);
	return -1;
}

static int makethings(void)
{
	struct smol2d_palette palette;
	uint8_t *pixels;
	unsigned int i;

	memset(&palette, 0, sizeof(palette));
	for (i = 0; i < 256; i++) {
		palette.colours[i].r = (uint8_t)i;
		palette.colours[i].g = (uint8_t)(i * 2);
		palette.colours[i].b = (uint8_t)(255 - i);
	}
	if (smol2d_setpalette(cntx, &palette))
		return -1;

	from = malloc(screen);
	to = malloc(screen);
	pixels = malloc((size_t)SPRITEW * SPRITEH);
	if (!from || !to || !pixels)
		return -1;

	memset(from, 7, screen);
	memset(to, 0, screen);

	/* a quarter of it transparent, so a colour key has something to skip */
	for (i = 0; i < SPRITEW * SPRITEH; i++)
		pixels[i] = (uint8_t)((i % SPRITEW) < SPRITEW / 2 ||
				      (i / SPRITEW) < SPRITEH / 2 ? 1 + (i % 200) : 0);

	if (smol2d_tex_create(cntx, &sprite, SPRITEW, SPRITEH) ||
	    smol2d_tex_load(cntx, sprite, pixels) ||
	    smol2d_tex_setkey(cntx, sprite, 0))
		return -1;

	free(pixels);

	for (i = 0; i < NSPRITES; i++) {
		sprites[i].tex = sprite;
		sprites[i].x = (i * 37) % (width - SPRITEW);
		sprites[i].y = (i * 53) % (height - SPRITEH);
		spritelist[i] = &sprites[i];
	}

	drawlist.sprites = spritelist;
	drawlist.nsprites = NSPRITES;

	/* half the bits, so the masked runs are as awkward as they get */
	if (smol2d_mask_create(cntx, &mask, width, height))
		return -1;

	for (i = 0; i < mask->stride * mask->h; i++)
		mask->bits[i] = (uint8_t)(i % 2 ? 0xaa : 0x55);

	return 0;
}

/*
 * What a present costs on top of the drawing, worked out rather than measured:
 * presenting after nothing was drawn is the flip and the wait for the vblank,
 * and presenting after a clear is that plus the clear plus moving the screen
 * over. The difference is the moving.
 */
static void report(void)
{
	const struct result *idle = find("present, nothing drawn");
	const struct result *dirty = find("present, whole screen drawn");
	const struct result *clear = find("clear the screen");
	const struct result *copy = find("memcpy a screen (plain memory)");

	if (!idle || !dirty || !clear || !copy)
		return;

	printf("\n");

	/* what those two would come to if that was the whole frame */
	if (idle->ns)
		printf("  presenting alone would be %llu frames a second, and clearing\n"
		       "  and presenting %llu.\n\n",
		       1000000000ull / idle->ns,
		       dirty->ns ? 1000000000ull / dirty->ns : 0);

	if (dirty->ns > idle->ns + clear->ns) {
		unsigned long long moving = (dirty->ns - idle->ns - clear->ns) / 1000;

		printf("  moving the screen into the scanout buffer: about %llu us,\n",
		       moving);
		printf("  against %llu us to memcpy the same pixels in plain memory",
		       copy->ns / 1000);

		if (copy->ns && moving * 1000 > copy->ns * 2)
			printf(",\n  so writing to the scanout buffer is around %llu times slower\n"
			       "  than writing to memory, which is where a frame is going.\n",
			       (moving * 1000) / copy->ns);
		else
			printf(".\n");
	} else {
		printf("  presenting after a clear costs no more than presenting after\n");
		printf("  nothing, so either little is being copied or both are waiting\n");
		printf("  on the vblank, which sets the rate whatever the drawing costs.\n");
	}

	printf("\n  a frame at 60fps has 16666 us in it, at 25fps 40000 us.\n");
}

int main(void)
{
	if (smol2d_init(&cntx, SMOL2D_CS_C8)) {
		fprintf(stderr, "could not open a display\n");
		return 1;
	}

	backbuffer = smol2d_getbackbuffer(cntx);
	if (!backbuffer) {
		smol2d_close(cntx);
		return 1;
	}

	width = backbuffer->w;
	height = backbuffer->h;
	screen = (unsigned long)width * height;

	if (makethings() || makepipelines()) {
		fprintf(stderr, "could not set up\n");
		smol2d_close(cntx);
		return 1;
	}

	printf("%ux%u, %lu pixels a screen\n\n", width, height, screen);
	printf("  %-34s %8s %12s %10s\n", "what", "times", "us each", "kpixel/s");

	timeit("memset a screen (plain memory)", screen, b_memset);
	timeit("memcpy a screen (plain memory)", screen, b_memcpy);
	timeit("ask the time", 0, b_getticks);

	timeit("clear the screen", screen, b_clear);
	timeit("clear it with a clip set", screen, b_clearclipped);
	timeit("clear it through a mask", screen, b_clearmasked);

	timeit("fill the screen, one op", screen, b_fillscreen);
	timeit("fill 256 whole columns", (unsigned long)COLUMNS * height, b_fillcolumns);
	timeit("fill 256 bands of 48", (unsigned long)COLUMNS * BAND, b_fillbands);
	timeit("256 fills of one pixel", COLUMNS, b_tinyfills);

	timeit("64 sprites of 32x32, keyed", (unsigned long)NSPRITES * SPRITEW * SPRITEH,
	       b_sprites);

	timeit("present, nothing drawn", 0, b_present);
	timeit("present, whole screen drawn", screen, b_clearpresent);

	report();

	smol2d_close(cntx);

	return 0;
}
