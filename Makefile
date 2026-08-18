.PHONY: backends demos clean

demos: backends
	$(MAKE) -C demos

backends:
	$(MAKE) -C libsmol2dsdl

clean:
	$(MAKE) -C libsmol2dsdl clean
	$(MAKE) -C demos clean
