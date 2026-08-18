#include <stdio.h>

#include <smol2d.h>

static void makepalette(struct smol2d_palette *palette)
{
	int i;

	for (i = 0; i < 256; i++) {
		palette->colours[i].r = i;
		palette->colours[i].g = i;
		palette->colours[i].b = i;
	}
}

int main(void)
{
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

	smol2d_close(backend_cntx);
	return 0;
}
