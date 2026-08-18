SDL_BACKEND = ../libsmol2dsdl/libsmol2dsdl.a
SDL_LIBS = $(shell pkg-config --libs sdl3)

DEMOS = $(patsubst %.c,%_sdl,$(wildcard *.c))

all: $(DEMOS)

%_sdl: %.c $(SDL_BACKEND)
	$(CC) -I ../include -o $@ $^ $(SDL_LIBS) -lm

clean:
	rm -f $(DEMOS)
