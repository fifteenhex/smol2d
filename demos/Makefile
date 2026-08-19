SDL_BACKEND = ../libsmol2dsdl/libsmol2dsdl.a
SDL_LIBS = $(shell pkg-config --libs sdl3)
DRM_BACKEND = ../libsmol2ddrm/libsmol2ddrm.a

SOURCES = $(wildcard *.c)
DEMOS = $(patsubst %.c,%_sdl,$(SOURCES)) $(patsubst %.c,%_drm,$(SOURCES))

all: $(DEMOS)

%_sdl: %.c $(SDL_BACKEND)
	$(CC) -I ../include -o $@ $^ $(SDL_LIBS) -lm

%_drm: %.c $(DRM_BACKEND)
	$(CC) -I ../include -o $@ $^ -lm

clean:
	rm -f $(DEMOS)
