CXX=$(shell sst-config --CXX)
CXXFLAGS=$(shell sst-config --ELEMENT_CXXFLAGS) -DBOOST_COROUTINES_NO_DEPRECATION_WARNING
LDFLAGS=$(shell sst-config --ELEMENT_LDFLAGS)

SOURCES = async_base.cc async_heap.cc async_watches.cc async_clauses.cc async_activity.cc async_var_activity.cc satsolver.cc
OBJECTS = $(SOURCES:.cc=.o)

all: libsatsolver.so install

libsatsolver.so: $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ -lboost_context -lboost_coroutine

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

install:
	sst-register satsolver satsolver_LIBDIR=$(CURDIR)
	sst-register SST_ELEMENT_SOURCE satsolver=$(CURDIR)
	sst-register SST_ELEMENT_TESTS satsolver=$(CURDIR)/../tests

clean:
	rm -f *.o libsatsolver.so
