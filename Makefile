.PHONY: backends demos clean

demos: backends
	$(MAKE) -C demos

backends:
	$(MAKE) -C libsmol2dsdl
	$(MAKE) -C libsmol2ddrm

clean:
	$(MAKE) -C libsmol2dsdl clean
	$(MAKE) -C libsmol2ddrm clean
	$(MAKE) -C demos clean
