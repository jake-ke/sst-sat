CXX=$(shell sst-config --CXX)
CXXFLAGS=$(shell sst-config --ELEMENT_CXXFLAGS)
LDFLAGS=$(shell sst-config --ELEMENT_LDFLAGS)

all: libsatsolver.so install

libsatsolver.so: satsolver.cc
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

install:
	sst-register satsolver satsolver_LIBDIR=$(CURDIR)
	sst-register SST_ELEMENT_SOURCE satsolver=$(CURDIR)
	sst-register SST_ELEMENT_TESTS satsolver=$(CURDIR)/../tests

clean:
	rm -f *.o libsatsolver.so
