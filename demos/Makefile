SDL_BACKEND = ../libsmol2dsdl/libsmol2dsdl.a
SDL_LIBS = $(shell pkg-config --libs sdl3)
DRM_BACKEND = ../libsmol2ddrm/libsmol2ddrm.a

SOURCES = $(wildcard *.c)
DEMOS = $(patsubst %.c,%_sdl,$(SOURCES)) $(patsubst %.c,%_drm,$(SOURCES))

ifdef NOLIBCDIR
DEMOS += $(patsubst %.c,%_drm_nolibc,$(SOURCES))
endif

all: $(DEMOS)

%_sdl: %.c $(SDL_BACKEND)
	$(CC) -I ../include -o $@ $^ $(SDL_LIBS) -lm

%_drm: %.c $(DRM_BACKEND)
	$(CC) -I ../include -o $@ $^ -lm

ifdef NOLIBCDIR
%_drm_nolibc: %.c $(DRM_BACKEND)
	$(CC) -nostdlib -include $(NOLIBCDIR)/nolibc.h -I ../include -o $@ $^
endif

clean:
	rm -f $(DEMOS)
