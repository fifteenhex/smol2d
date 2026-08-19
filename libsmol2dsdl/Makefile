include ../toolchain.mk

CFLAGS += -I../include $(shell $(PKG_CONFIG) --cflags sdl3) -Wall -Wextra -O2

libsmol2dsdl.a: src/smol2dsdl.o
	$(AR) rcs $@ $^

clean:
	rm -f libsmol2dsdl.a src/smol2dsdl.o
