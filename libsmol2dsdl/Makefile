CFLAGS += -I../include $(shell pkg-config --cflags sdl3) -Wall -Wextra -O2

libsmol2dsdl.a: src/smol2dsdl.o
	$(AR) rcs $@ $^

clean:
	rm -f libsmol2dsdl.a src/smol2dsdl.o
