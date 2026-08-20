include ../toolchain.mk
include ../nolibc.mk

CFLAGS += -I../include -I../smoldrm -I../smolinput -O2

ARCHIVES = libsmol2ddrm.a

NOLIBC_ARCHIVE = libsmol2ddrm_nolibc.a

ifdef NOLIBCDIR
ARCHIVES += $(NOLIBC_ARCHIVE)
endif

all: $(ARCHIVES)

libsmol2ddrm.a: src/smol2ddrm.o
	$(AR) rcs $@ $^

src/smol2ddrm.o: src/smol2ddrm.c ../smoldrm/smoldrm.h ../smolinput/smolinput.h

$(NOLIBC_ARCHIVE): src/smol2ddrm_nolibc.o
	$(AR) rcs $@ $^

src/smol2ddrm_nolibc.o: src/smol2ddrm.c ../smoldrm/smoldrm.h ../smolinput/smolinput.h
	$(CC) $(NOLIBC_CFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f libsmol2ddrm.a $(NOLIBC_ARCHIVE) src/smol2ddrm.o src/smol2ddrm_nolibc.o
