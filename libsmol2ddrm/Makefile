CFLAGS += -I../include -I../smoldrm -O2

libsmol2ddrm.a: src/smol2ddrm.o
	$(AR) rcs $@ $^

src/smol2ddrm.o: src/smol2ddrm.c ../smoldrm/smoldrm.h

clean:
	rm -f libsmol2ddrm.a src/smol2ddrm.o
