# SAT Solver SST Element
.PHONY: all install clean distclean test

all:
	@$(MAKE) -C src

install:
	@$(MAKE) -C src install

clean:
	@$(MAKE) -C src clean

distclean:
	@$(MAKE) -C src distclean

test: install
	@./tools/runall.sh -j 32
