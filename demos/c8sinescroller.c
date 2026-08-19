#ifndef NOLIBC
#include <stdio.h>
#include <string.h>
#endif

#include <smol2d.h>

#define BLUE	1

static void makepalette(struct smol2d_palette *palette)
{
	memset(palette, 0, sizeof(*palette));

	/* light electric blue */
	palette->colours[BLUE].r = 0x7d;
	palette->colours[BLUE].g = 0xf9;
	palette->colours[BLUE].b = 0xff;
}

int main(void)
{
	const struct smol2d_colour blue = { .indexed = { .index = BLUE } };
	struct smol2d_palette palette;
	struct smol2d_tex *backbuffer;
	void *backend_cntx;

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

	while (!smol2d_waitkey(backend_cntx, 0)) {
		if (smol2d_tex_clear(backend_cntx, backbuffer, &blue)) {
			smol2d_close(backend_cntx);
			return 1;
		}

		if (smol2d_present(backend_cntx))
			break;
	}

	smol2d_close(backend_cntx);
	return 0;
}
