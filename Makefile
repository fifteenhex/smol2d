include toolchain.mk

.PHONY: backends demos clean

demos: backends
	$(MAKE) -C demos

backends:
ifdef HAVE_SDL
	$(MAKE) -C libsmol2dsdl
endif
	$(MAKE) -C libsmol2ddrm

clean:
	$(MAKE) -C libsmol2dsdl clean
	$(MAKE) -C libsmol2ddrm clean
	$(MAKE) -C demos clean
