include ../toolchain.mk
include ../nolibc.mk

SDL_BACKEND = ../libsmol2dsdl/libsmol2dsdl.a
SDL_LIBS = $(shell $(PKG_CONFIG) --libs sdl3)
DRM_BACKEND = ../libsmol2ddrm/libsmol2ddrm.a
DRM_BACKEND_NOLIBC = ../libsmol2ddrm/libsmol2ddrm_nolibc.a

SOURCES = $(wildcard *.c)
SDL_DEMOS = $(patsubst %.c,%_sdl,$(SOURCES))
DRM_DEMOS = $(patsubst %.c,%_drm,$(SOURCES))
NOLIBC_DEMOS = $(patsubst %.c,%_drm_nolibc,$(SOURCES))

DEMOS = $(DRM_DEMOS)

ifdef HAVE_SDL
DEMOS += $(SDL_DEMOS)
endif

ifdef NOLIBCDIR
DEMOS += $(NOLIBC_DEMOS)
endif

all: $(DEMOS)

%_sdl: %.c $(SDL_BACKEND)
	$(CC) -I ../include -o $@ $^ $(SDL_LIBS) -lm

%_drm: %.c $(DRM_BACKEND)
	$(CC) -I ../include -o $@ $^ -lm

ifdef NOLIBCDIR
%_drm_nolibc: %.c $(DRM_BACKEND_NOLIBC)
	$(CC) $(NOLIBC_CFLAGS) $(NOLIBC_LDFLAGS) -I ../include -o $@ $^
endif

clean:
	rm -f $(SDL_DEMOS) $(DRM_DEMOS) $(NOLIBC_DEMOS)
