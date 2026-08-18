#ifndef __SMOL2D_H
#define __SMOL2D_H

#include <stdint.h>

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

/* Pack up and go home */
void smol2d_close(void *backend_cntx);

#endif /* __SMOL2D_H */
