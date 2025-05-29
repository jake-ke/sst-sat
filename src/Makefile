CXX=$(shell sst-config --CXX)
CXXFLAGS=$(shell sst-config --ELEMENT_CXXFLAGS) -DBOOST_COROUTINES_NO_DEPRECATION_WARNING
LDFLAGS=$(shell sst-config --ELEMENT_LDFLAGS)

all: libsatsolver.so install

libsatsolver.so: async_heap.cc satsolver.cc 
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ -lboost_context -lboost_coroutine

install:
	sst-register satsolver satsolver_LIBDIR=$(CURDIR)
	sst-register SST_ELEMENT_SOURCE satsolver=$(CURDIR)
	sst-register SST_ELEMENT_TESTS satsolver=$(CURDIR)/../tests

clean:
	rm -f *.o libsatsolver.so
