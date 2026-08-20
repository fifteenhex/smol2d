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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

#include <smol2d.h>

#define MINMS		300	/* how long to keep at each one */
#define MAXBATCH	(1u << 22)

#define SPRITEW		32
#define SPRITEH		32
#define NSPRITES	64

#define COLUMNS		256	/* one pixel wide fills, the shape of the wave */
#define BAND		48	/* how tall a piece of it moves in a frame */

/*
 * move16 moves a cache line at a time and does not read the line it is about
 * to overwrite, which is the thing a fill otherwise pays for. It is a 68040
 * and 68060 instruction, so it is asked for by name with .chip and only run
 * after the machine says it has one -- a 68030 would take an exception.
 *
 * The shapes are the kernel's, out of arch/m68k: copying is both ends
 * stepping, and filling is the same instruction with the source pinned to the
 * first line, which has been written by hand, and rewound every time.
 */
#ifdef __m68k__
#define MOVE16	1

static int has_move16(void)
{
	char buf[1024];
	int fd, got, i;

	fd = open("/proc/cpuinfo", 0 /* O_RDONLY */);
	if (fd < 0)
		return 0;

	got = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);

	if (got <= 0)
		return 0;

	buf[got] = 0;

	for (i = 0; i + 5 < got; i++)
		if (!memcmp(buf + i, "68040", 5) || !memcmp(buf + i, "68060", 5))
			return 1;

	return 0;
}

static void move16_copy(void *to, void *from, unsigned long lines)
{
	__asm__ __volatile__("1:\t"
			     ".chip 68040\n\t"
			     "move16 %1@+,%0@+\n\t"
			     ".chip 68k\n\t"
			     "subql #1,%2\n\t"
			     "bnes 1b"
			     : "=a" (to), "=a" (from), "=d" (lines)
			     : "0" (to), "1" (from), "2" (lines)
			     : "memory");
}

/* the first line has to hold the pattern before this is called */
static void move16_fill(void *at, unsigned long lines)
{
	void *src = at;
	void *dst = (uint8_t *)at + 16;

	__asm__ __volatile__("1:\t"
			     ".chip 68040\n\t"
			     "move16 %2@+,%0@+\n\t"
			     ".chip 68k\n\t"
			     "subqw #8,%2\n\t"
			     "subqw #8,%2\n\t"
			     "subql #1,%1\n\t"
			     "bnes 1b"
			     : "=a" (dst), "=d" (lines), "=a" (src)
			     : "0" (dst), "1" (lines), "2" (src)
			     : "memory");
}
#endif

static void *cntx;
static struct smol2d_tex *backbuffer, *sprite;
static struct smol2d_mask *mask;
static struct smol2d_sprite sprites[NSPRITES];
static struct smol2d_sprite *spritelist[NSPRITES];
static struct smol2d_drawlist drawlist;
static uint8_t *from, *to;
static uint8_t *lined_from, *lined_to;	/* the same, on a 16 byte line */
static unsigned long lines;
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

/* what the libc will do for a screen's worth, and what we do instead */
static void b_memset(void)
{
	memset(to, 3, screen);
}

static void b_memcpy(void)
{
	memcpy(to, from, screen);
}

static void b_widefill(void)
{
	smol2d_c8_set(to, 3, screen);
}

static void b_widecopy(void)
{
	smol2d_c8_copyrun(to, from, screen);
}

#ifdef MOVE16
static void b_move16fill(void)
{
	/* the pattern line is also the source, and a copy run may have eaten it */
	((uint32_t *)lined_to)[0] = 0x03030303;
	((uint32_t *)lined_to)[1] = 0x03030303;
	((uint32_t *)lined_to)[2] = 0x03030303;
	((uint32_t *)lined_to)[3] = 0x03030303;

	move16_fill(lined_to, lines - 1);
}

static void b_move16copy(void)
{
	move16_copy(lined_to, lined_from, lines);
}
#endif

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

	from = malloc(screen + 32);
	to = malloc(screen + 32);
	pixels = malloc((size_t)SPRITEW * SPRITEH);
	if (!from || !to || !pixels)
		return -1;

	memset(from, 7, screen);
	memset(to, 0, screen);

	/* move16 works on whole cache lines, from a line boundary */
	lined_from = (uint8_t *)(((uintptr_t)from + 15) & ~(uintptr_t)15);
	lined_to = (uint8_t *)(((uintptr_t)to + 15) & ~(uintptr_t)15);
	lines = screen / 16;

	for (i = 0; i < 16; i++)
		lined_to[i] = 3;

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
	const struct result *copy = find("copy a screen, 32 bits at a time");
	const struct result *libcset = find("memset a screen (the libc)");
	const struct result *wide = find("fill a screen, 32 bits at a time");

	if (!idle || !dirty || !clear || !copy || !libcset || !wide)
		return;

	printf("\n");

	/* whether the libc is the thing in the way */
	if (wide->ns && libcset->ns > wide->ns * 3 / 2)
		printf("  filling a screen a long at a time is %llu times quicker than the\n"
		       "  libc's memset, so the libc is what a frame was waiting for.\n\n",
		       libcset->ns / wide->ns);
	else if (libcset->ns * 3 / 2 < wide->ns)
		printf("  the libc's memset is quicker than filling a long at a time, so\n"
		       "  leave it to the libc on this machine.\n\n");

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
		printf("  against %llu us to copy the same pixels in plain memory",
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

	timeit("memset a screen (the libc)", screen, b_memset);
	timeit("memcpy a screen (the libc)", screen, b_memcpy);
	timeit("fill a screen, 32 bits at a time", screen, b_widefill);
	timeit("copy a screen, 32 bits at a time", screen, b_widecopy);

#ifdef MOVE16
	if (has_move16()) {
		timeit("fill a screen with move16", lines * 16, b_move16fill);
		timeit("copy a screen with move16", lines * 16, b_move16copy);
	} else {
		printf("  %-34s %8s %12s %10s\n", "move16", "-", "-", "no 68040");
	}
#else
	printf("  %-34s %8s %12s %10s\n", "move16", "-", "-", "not m68k");
#endif
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
