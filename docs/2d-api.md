# A 2D API for smol2d

A plan for the drawing API, aimed at old skool 2D: tile layers, sprites,
masking, blitter tricks, scrolling. Written so that the work can be handed to
hardware or to generated code later without the API changing shape.

Parts of this are in the tree now; the rest is still a plan. See "Where this
stands" below for which is which. The op model was prototyped and run in
software first to check it can express a real scene; see "Prototype" at the
end.

## Where this stands

In the tree:

- Pipelines: `pipeline_create/params/run/destroy`, the description consumed at
  create time, the structural and dynamic split, per-op targets resolved once.
- Ops: `FILL`, `BLIT`, `SPRITES`, `SCROLL`, `TILES`, each with an optional mask
  and a raster op, and colour keys on blit sources.
- Damage: the drm backend keeps the changed rectangles per buffer, copies only
  those into the scanout buffer, and passes the same list to the kernel as
  `DIRTYFB` for drivers that keep their own copy of the screen.
- One colourspace, `C8`, with the pixel loops as static inlines in the header.
- A visualiser in the SDL backend, `SMOL2D_SHOW`, that draws what each op went
  over, what actually changed, and what the damage would be.

Still a plan: the rest of the colourspaces, `CLIP` and `PALETTE` as ops,
`LINE`, `getcaps()`, `flush()` and asynchronous execution, `tex_map()`, and
palette banks for the narrow indexed modes. Clipping is backend state today
(`smol2d_setclip`) rather than an op.

## Goals

- Old skool primitives as first class citizens: tiles, sprites, masks, raster
  ops, scrolls. Not a general purpose canvas API with tiles bolted on.
- Everything the app asks for is **data, not calls**. A frame is a list of ops
  that a backend can validate once and then execute many times.
- Offloadable. Each op maps to something a blitter, a 2D engine, a DRM plane or
  a generated inner loop can do, and the API never forces a round trip through
  the CPU to decide what happens next.
- No colourspace conversion inside the API. The app picks a colourspace and
  everything it hands over is already in it.

## Non-goals

- Rotation, arbitrary scaling, alpha compositing pipelines. Old hardware could
  not do them and the software fallbacks are where 2D APIs go to die. Simple
  integer scaling can be added per op later if it is wanted.
- A scene graph, or anything that owns the app's game state.
- Threading. Ops may be executed asynchronously by a backend, but the API does
  not hand out threads.

## The model

Five kinds of thing:

| Thing | What it is |
| --- | --- |
| context | the backend, the display, and the backbuffer |
| texture | a rectangle of pixels in the context's colourspace |
| resource | palette, mask, tileset, tilemap, sprite array |
| op | one drawing operation, plain data, targeting a texture |
| pipeline | a validated, possibly compiled, list of ops |

A frame is: update the dynamic fields of your ops, run the pipeline, present.

```c
smol2d_pipeline_run(cntx, frame);
smol2d_present(cntx);
```

The pipeline is built once at startup. Per frame the app writes new sprite
positions and scroll offsets into the parameter block the pipeline handed back,
and the backend reads them when the pipeline runs. Nothing is re-validated,
nothing is re-compiled, and there is no per-op call overhead to pay on a slow
CPU.

## Colourspaces

```c
enum smol2d_colourspace {
	SMOL2D_CS_C1,		/* indexed, 1bpp */
	SMOL2D_CS_C2,
	SMOL2D_CS_C4,
	SMOL2D_CS_C8,
	SMOL2D_CS_RGB332,	/* chunky */
	SMOL2D_CS_RGB565,
	SMOL2D_CS_RGB888,
	SMOL2D_CS_XRGB8888,
	SMOL2D_CS_ARGB8888,
};
```

The enum in the header only lists what the backends can actually do, and grows
as they learn more: today that is `C8`, `RGB565` and `XRGB8888`, with
`smol2d_cs_bpp()` and `smol2d_cs_isindexed()` alongside it so an app can size
its own buffers.

The context is created in one colourspace and every texture in it has that
colourspace. Ops never convert: a blit is a copy of bits, a fill writes a
pixel value, a raster op combines source and destination bits. This is what
makes the ops mappable onto a blitter, and it is what was asked for -- the
program knows what it wants and requests it.

The one place conversion happens is scanout, and it is not part of the op
model: a C8 backbuffer on a DRM device whose dumb buffers are XRGB8888 gets
expanded through the palette by `smol2d_present()`. An app that asks for
XRGB8888 on that device gets no conversion at all, and an app that asks for C8
on hardware with a real CLUT gets none either. `smol2d_getcaps()` says which
you are getting so an app that cares can choose.

### Very bad hardware

A 1bpp panel is the case that proves the rule. fbdevgl fakes greyscale on one
by turning each logical pixel into a 2x2 cell of bits taken from a pattern
table, which is a colourspace conversion by any other name -- and it belongs in
exactly the same place as the C8 to XRGB8888 expansion the DRM backend does: at
scanout, once a frame, outside the op model. Ops keep working in whatever the
app asked for and never learn that the panel is 1bpp.

For an indexed context the conversion is a lookup like any other. At
`setpalette()` time work out the luminance of each of the 256 entries and store
the pattern it maps to; `present()` then walks the backbuffer through that
table. Same shape as the DRM backend's index to pixel table, different
contents, so the two backends differ in what one array holds rather than in
how they work.

Two things such a panel wants that the op model can give it cheaply:

- **Damage.** Slow panels are pushed over SPI or I2C and want only the changed
  region. Tracking that per pixel costs a comparison per pixel drawn. A
  pipeline knows the destination rectangle of every op before it runs, so the
  damaged region is the union of those rectangles: a few compares per frame
  instead of per pixel. This is in the drm backend now, and it is the
  difference between moving 300k pixels a frame and moving a few thousand.
  A masked op is the case it cannot help with: the mask decides which pixels
  move, so the op reports its whole rectangle and the saving goes away.
- **A real 1bpp colourspace.** Dithering is for when the app wants more greys
  than the panel has. An app that only ever draws two colours should be able to
  ask for `C1` and have its pixels land in the panel's bits untouched, which
  needs sub-byte addressing in the pixel layer.

Colours are given as a union, tagged by the context's colourspace rather than
by a field, because the app already knows which arm is live:

```c
struct smol2d_colour {
	union {
		struct smol2d_colour_chunky chunky;	/* r, g, b, a */
		struct smol2d_colour_indexed indexed;	/* index */
	};
};
```

Palettes are only meaningful for indexed colourspaces; `smol2d_setpalette()`
fails in a chunky one. Indexed colourspaces narrower than 8bpp use palette
**banks**: a C4 texture's 4 bit pixels select within a 16 entry bank, and the
bank comes from the op (or from the tilemap entry, so tiles can each use a
different 16 colours, which is how most tile hardware worked).

## Resources

**Texture.** Width, height, colourspace, pixels. Also the app's drawing
surface: the backbuffer is a texture, offscreen composition targets are
textures, sprite images are textures. `smol2d_tex_map()` hands out a pointer
and a stride for direct poking, which is how an app draws things the op set
does not cover; `smol2d_tex_load()` stays for the "here is a blob of pixels"
case.

A texture carries an optional **colour key**: the pixel value that is skipped
when it is a blit source. Per texture rather than per op, because that is how
sprites actually work and it lets the compiler specialise the blit once.

**Mask.** A 1bpp bitmap, always the size of the texture it is used with. Where
the bit is 0 the destination is untouched. This is the Amiga cookie cut, and it
is the thing colour keying cannot do: a mask lets a sprite use every value in
the colourspace, including the one that would otherwise be transparent, and it
lets the same image be drawn with different cutouts.

**Tileset.** A texture plus tile width, height and the number of tiles across
it. Tile *n* is a rectangle in that texture; nothing is copied.

**Tilemap.** A grid of entries, each a tile number, flip flags and a palette
bank. Entries are 4 bytes, and like every other resource the array is allocated
by the backend and handed back, so a game writes new tiles straight into it as
the map scrolls in and the backend can keep it somewhere its hardware can
reach.

**Sprite array.** An array of `{ texture, source rect, x, y, flags }`. The
`SPRITES` op declares the most it will ever draw, `pipeline_create()` allocates
that many, and each frame the app fills in the first *n* through the parameter
block. This is the batching unit: one op, many sprites, clipped by the backend,
and the obvious thing to hand to hardware that has a sprite list of its own.

Positions are **signed**. Sprites come in from the left and the top, and the
current `unsigned int x, y` cannot say that.

## Operations

| Op | Does |
| --- | --- |
| `CLIP` | set the clip rectangle for the ops that follow |
| `FILL` | fill a rectangle with a colour |
| `BLIT` | copy a texture, with key, mask, flip and raster op |
| `SPRITES` | a batch of blits from a sprite array |
| `TILES` | draw a tilemap through a tileset, with scroll and wrap |
| `SCROLL` | move a rectangle within a texture, overlap safe |
| `LINE` | a line, with a raster op and a pattern |
| `PALETTE` | load palette entries, optionally at a scanline |

Raster ops are the small useful set rather than all 256 Amiga minterms:

```c
enum smol2d_rop { SMOL2D_ROP_COPY, SMOL2D_ROP_AND, SMOL2D_ROP_OR,
		  SMOL2D_ROP_XOR, SMOL2D_ROP_NOT };
```

`XOR` is worth having on its own: it is how you draw and undraw a cursor or a
rubber band without saving the background.

`TILES` deserves its scroll offsets rather than being expressed as a clipped
blit per tile, because that is the form hardware wants: a backend with a tile
engine programs it directly, a backend with a blitter emits one blit per
visible tile, and a software backend runs one specialised loop over the
destination. The op says *what*, not *how*.

`SCROLL` is a copy within one texture. It exists because the old trick for a
scrolling playfield is to move what you already drew and only draw the newly
exposed strip, and because a hardware blitter does it in one go with the
correct overlap direction.

`PALETTE` with a scanline is the copper-ish case: split the screen and change
colours partway down. A backend that cannot do it fails at compile time and
says so through `getcaps()`, rather than silently producing something else.

## Pipelines

```c
int smol2d_pipeline_create(void *backend_cntx, const struct smol2d_op *ops,
			   unsigned int nops, struct smol2d_pipeline **pipeline);
struct smol2d_op *smol2d_pipeline_params(struct smol2d_pipeline *pipeline,
					 unsigned int op);
int smol2d_pipeline_run(void *backend_cntx, struct smol2d_pipeline *pipeline);
void smol2d_pipeline_destroy(void *backend_cntx, struct smol2d_pipeline *pipeline);
```

Two stages, and the description is **consumed** by the first one. `create()`
copies everything it needs -- the ops, and storage for `maxsprites` sprites per
sprite op -- into memory the backend owns, and after it returns the app's array
can be freed or reused. From then on the only thing passed around is the
compiled pipeline pointer.

Per frame updates go through `smol2d_pipeline_params()`, which hands back the
backend's own copy of an op. The app writes the dynamic fields of that copy:

```c
struct smol2d_op *layer = smol2d_pipeline_params(pipeline, 0);
struct smol2d_op *baddies = smol2d_pipeline_params(pipeline, 2);

layer->tiles.scrollx = camera_x;
baddies->sprites.nsprites = nalive;
baddies->sprites.sprites[0].x = 20;

smol2d_pipeline_run(cntx, pipeline);
```

Still plain stores, no call per change, but into memory the backend chose. That
is the point: a blitter wants its descriptors in uncached or DMA-able memory, a
JIT wants to bake the address of the parameter block into the code it
generates, and neither can do that with an array the app allocated on its own
stack. It also means a compiled pipeline is self contained, so it can be cached,
or built once and handed to something else, without the description hanging
around.

Writing a structural field of the returned op does nothing useful; a debug
build should catch it. Changing a structural field means a new pipeline.

### Structural and dynamic fields

Every field of an op is one or the other, and the split is the whole reason
this design can be compiled or handed to hardware:

- **Structural**: op type, target texture, source textures, mask, tileset,
  tilemap, sprite array and its maximum length, raster op, flip flags, wrap.
  Read at `pipeline_create()`. Changing one means creating a new pipeline.
- **Dynamic**: rectangles, positions, scroll offsets, colours, sprite array
  contents, the number of sprites, palette contents. Read every run, from the
  backend's copy, which the app reaches through `smol2d_pipeline_params()`.

So a backend can, at create time, check formats agree, work out which ops touch
which textures, pick or generate a specialised routine per op (the format, the
raster op, whether there is a key, whether there is a mask, and whether the
blit is aligned are all known), and reduce each op to a descriptor. At run time
it reloads only the numbers that move.

A JIT gets exactly what it needs: the inner loop of `BLIT` with `ROP_COPY`, a
colour key, no mask and C8 pixels is a different function from the same op with
`ROP_XOR` and a mask, and both are known before the first frame. Because the
parameter block is allocated by `create()`, its address is known then too, so
generated code can load the dynamic fields from a fixed address rather than
chasing a pointer the app owns.

### Execution

Ops execute in list order. A backend may reorder or run in parallel any two ops
whose targets differ and where neither reads the other's target, which is
decidable at create time from the structural fields alone.

`CLIP` looks stateful but is not, at runtime: the compiler folds each `CLIP`
into the clip rectangle of the ops that follow it, so every op ends up
self contained. The state is a convenience for the app, not a serialisation
point for the backend.

`pipeline_run()` may return before the drawing has finished. `smol2d_present()`
implies completion of everything targeting the backbuffer; `smol2d_flush()`
waits for everything else, and is what you call before reading pixels back with
`tex_map()`. This is how a blitter behaves and pretending otherwise now would
mean an API break later.

### Immediate mode

```c
int smol2d_run(void *backend_cntx, const struct smol2d_op *ops, unsigned int nops);
```

Compile, run, discard. For setup code, tools, and getting something on screen
without ceremony. Same code path, so there is one implementation to get right.

## Rendering into a texture

Every op names its own `dst`, and the backbuffer is just a texture, so drawing
into an offscreen texture is the same code path as drawing on the screen. There
is no separate render-to-texture mode to switch in and out of. Three ways to
use it, depending on how often the texture is rebuilt:

**Built once.** A sprite that is assembled at runtime -- a ship with its
weapons bolted on, a font strip, a banner -- is a one shot `smol2d_run()` with
`dst` set to the new texture. Compile, run, discard the pipeline, keep the
texture. Give it a colour key afterwards and it is a sprite source like any
other.

```c
struct smol2d_op build[] = {
	{ .type = SMOL2D_OP_FILL,    .dst = ship, .fill = { ..., transparent } },
	{ .type = SMOL2D_OP_BLIT,    .dst = ship, .blit = { hull, ... } },
	{ .type = SMOL2D_OP_SPRITES, .dst = ship, .sprites = { guns, 4, ROP_COPY, 4 } },
};

smol2d_run(cntx, build, 3);
smol2d_tex_setkey(cntx, ship, &transparent);
```

**Rebuilt every frame.** Make the pipeline once at startup with `dst` pointing
at the texture, and run it each frame before the one that draws the screen. The
contents change through the dynamic fields, so an animated banner or a damage
overlay costs no recompilation.

**Both in one list.** A pipeline can target several textures: early ops build a
scratch texture, later ops use it as a source when drawing the backbuffer. That
is a read-after-write dependency, and it is visible at `pipeline_create()` from
the structural fields alone, so a backend that reorders knows it must not
reorder across it. One list, one run, correct order.

The one restriction is that `dst` is structural. A pipeline is bound to the
textures it was created against, so "build into whichever texture is free this
frame" means either one pipeline per target or a one shot `smol2d_run()`. That
is deliberate: knowing the target's size and colourspace up front is half of
what makes an op specialisable, and rotating targets is rare enough not to
deserve an indirection layer in the fast path. If it turns out to matter, the
fix is a slot table bound at run time rather than pointers in the ops, and it
can be added without disturbing anything else.

Two things to know:

- The texture is in the context's colourspace like everything else, so no
  conversion happens and none is available. Build C8 sprites in a C8 context.
- If you build a texture and then read it with `tex_map()` rather than using it
  as a source, call `smol2d_flush()` first: drawing may still be in flight.
  Using it as a source in a later op needs no flush, the dependency handles it.

Hardware that can only draw into the scanout buffer falls back to software for
this, and `getcaps()` says so, so an app that builds a lot of textures at
runtime can find out whether it is getting help.

## Capabilities

```c
struct smol2d_caps {
	uint32_t ops;		/* bit per op the backend accelerates */
	uint32_t rops;		/* bit per raster op */
	uint32_t features;	/* masks, scanline palette, tile engine, ... */
	int converts_on_scanout;
};
```

Every op works on every backend: if the hardware cannot do it, the software
fallback does. Caps say what is *fast*, so an app can pick a strategy, and
`pipeline_create()` is where an app finds out that something it asked for
cannot be done at all.

## Mapping to backends

**Software (the DRM backend today).** Each op is a specialised loop over the
destination. `TILES` is the interesting one: iterate destination pixels, index
into the map, index into the sheet -- the prototype does exactly this and it is
one pass with no per tile call overhead.

**SDL.** `BLIT` and `SPRITES` could be `SDL_BlitSurface` with a colour key and
`FILL` could be `SDL_FillSurfaceRect`, but the backend runs the same header
loops the drm backend does, so that what is being watched in the visualiser is
what the slow machine will run. The visualiser is the reason this backend
earns its keep: `SMOL2D_SHOW=step` walks a frame op by op, and the boxes show
what each op went over against what it changed and what the hardware would be
told to move.

**A blitter (Amiga, or an SoC 2D engine).** `BLIT` with a mask and a raster op
is literally what the Amiga blitter's A/B/C/D channels and minterms do. On
something like i.MX PXP or STM32 DMA2D, `FILL`, `BLIT` and `SCROLL` are single
descriptors. The op list becomes a descriptor chain and the CPU only writes the
dynamic numbers.

**DRM planes.** A `TILES` layer that is the whole screen, or a large `BLIT`
that never changes, can become a plane the display engine composites for free,
with scroll offsets turning into plane coordinates.

**JIT.** Generate one function per op at `pipeline_create()`, then one call per
op per frame. The structural fields are the specialisation key, which also
makes a code cache trivial: same key, same code.

## Where this leaves today's API

`smol2d_tex_renderto(cntx, tex, drawlist)` is `SPRITES` with an explicit target,
so it survives as the convenience form. The backbuffer stays a texture.
`tex_create`, `tex_load`, `tex_clear`, `tex_setkey`, `setpalette`, `present`
and `close` all keep their shape. What is new is masks, tilesets, tilemaps, the
op list, pipelines, `tex_map()`, caps, and signed sprite positions.

Suggested order, each step useful on its own:

Suggested order, each step useful on its own. The first four are done, in a
different order than planned: pipelines came before immediate mode because a
slow target wants the compile step more than it wants the convenience, and
colourspaces were parked because C8 is what the hardware in front of us has.

1. `struct smol2d_op` and `pipeline_create/run/destroy` with the structural and
   dynamic split. **Done.**
2. `FILL`, then `BLIT` with key, mask, flip and raster op. **Done.**
3. `SPRITES`, with the array allocated by the pipeline. **Done.**
4. `SCROLL`, then `TILES` with a tileset and tilemap. **Done.**
5. Colourspaces and chunky support: the enum, `tex_setkey()` taking a
   `struct smol2d_colour`, backends honouring the requested colourspace.
   Written and parked on the `intern` branch.
6. `smol2d_run()` immediate mode, `CLIP` as an op rather than backend state.
7. `getcaps()`, `flush()`, async execution.
8. `LINE`, scanline `PALETTE`.
9. Hardware and JIT backends, which by then need no API changes.

## Open questions

- **Bank selection for C4.** Per op, per tilemap entry, or both? Per entry is
  what tile hardware did; per op is simpler for sprites.
- **Multiple palettes.** One CLUT per context, or a palette object that ops
  reference? Scanline splits push towards objects.
- **Sprite sorting.** Does `SPRITES` guarantee array order, or may a backend
  sort for batching? Array order is easier to reason about; sorting is what
  hardware would want. I would guarantee order and add a flag to relax it.
- **Scaling.** Integer 2x/3x for a chunky mode on a big panel is tempting and
  cheap. Worth an op flag rather than a new op.
- **Planar formats.** Real Amiga bitplanes are a colourspace family of their
  own and would touch every loop. Worth deciding early whether they are in
  scope, because retrofitting them is much worse than allowing for them now.

## Prototype

`prototype/pipeline.c` (outside the repo, in my home directory) implements the
op model in software for C8: ops, a compile step that copies the description
into its own memory and binds a specialised routine per op, and `CLIP`, `FILL`,
`BLIT` with key/mask/flip/rop, `SPRITES`, `TILES` with scroll and wrap, and
`SCROLL`. It renders a scene and dumps it as characters, twice, from one
compiled pipeline, writing the second frame's dynamic fields through
`params()`. Between the two frames it scribbles over the description array to
prove nothing reads it after `compile()`.

```
frame 0:
++++++++++++++++++++++++++++++++++++++++++++++++
++++++++++++++++++++++++++++++++++++++++++++++++
....-:-:====........-:-:====........-:-:====....
....:-:-====........:-:-====........:-:-====....
-:-:===@@@..-:-:-:-:====....-:-:-:-:====....-:-:
:-:-==@@@@@.:-:-:-:-====....:-:-:-:-====....:-:-
-:-:===@@@..-:-:-:-:====....-:-:-:-:====....-:-:
:-:-====....:-:-:-:-====....:-:-:-:-====....:-:-
====....-:-:========....-:-:========....-@@@====
====....:-:-========....:-:-========....@@:@@===
====....-:-:========....-:-:========....-@@@====
====....:-:-========....:-:-========....:-:-====
....-:-:====........-:-:====........-:-:====....
....:-:-====........:-:-====........:-:-====....
....-:-:====........-:-:====........-:-:====....
@@..:-:-====........:-:-====........:-:-====....
@@@:====....-:-:-:-:====....-:-:-:-:====....-:-:
@@:-@@:-====....:-:-:-:-====....:-:-:-:-====....
-:-:-:-:====....-:-:-:-:====....-:-:-:-:====....
:-:-:-:-====....:-:-:-:-====....:-:-:-:-====....
```

Reading it: the `+` band is `FILL`, everything under it is `TILES` wrapping a
4x3 map of three tiles, `@` are sprites, the one at the left edge is at x = -2
and horizontally flipped, the one at the right is the masked `BLIT` -- the `:`
in the middle of it is background showing through a hole in the mask -- and the
bottom three rows have been moved right by `SCROLL`.

What the prototype changed my mind about:

- Positions have to be signed, immediately, or half the sprite cases cannot be
  written down.
- Sprites want a source rectangle. One texture per sprite image works but a
  sheet plus a rect is what you actually want for animation frames, and it
  batches far better.
- `TILES` as a destination side loop is simpler *and* faster than emitting per
  tile blits, which is a good sign for the op being at the right level.
- Colour key belongs on the texture, mask belongs on the op. A texture's
  transparent value is a property of the image; which cutout you draw it with
  is a property of the drawing.
- Handing the sprite array to `compile()` and getting backend memory back is
  no harder to use than owning the array, and it is what lets the sprites live
  wherever the hardware needs them.

