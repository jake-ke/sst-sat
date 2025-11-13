# Directories
BUILD_DIR = ../build

# Compiler configuration
CXX = $(shell sst-config --CXX)
CXXFLAGS = $(shell sst-config --ELEMENT_CXXFLAGS) -DBOOST_COROUTINES_NO_DEPRECATION_WARNING

# Heap implementation selection
# Default (no flag): pipelined heap
# Use: make HEAP_IMPL=classic   to use classic external heap
HEAP_IMPL ?= pipelined
ifeq ($(HEAP_IMPL),classic)
  CXXFLAGS += -DUSE_CLASSIC_HEAP=1
  $(info Building with classic external Heap implementation)
else
  $(info Building with PipelinedHeap implementation)
endif
LDFLAGS = $(shell sst-config --ELEMENT_LDFLAGS)

# Add SST Elements include path
SST_ELEMENTS_INCLUDE = -I$(SST_ELEMENTS_HOME)/include
CXXFLAGS += $(SST_ELEMENTS_INCLUDE)

# Add Boost include and library paths if BOOST_ROOT is defined by the module
ifdef BOOST_ROOT
	BOOST_INCLUDE := -I$(BOOST_ROOT)/include
	BOOST_LIBDIR := $(BOOST_ROOT)/lib
	CXXFLAGS += $(BOOST_INCLUDE)
	LDFLAGS += -L$(BOOST_LIBDIR) -Wl,-rpath,$(BOOST_LIBDIR)
endif

# Source files
SOURCES = memory_allocator.cc cache_profiler.cc async_base.cc async_heap.cc async_watches.cc async_clauses.cc async_activity.cc async_var_activity.cc satsolver.cc directedprefetch.cc pipelined_heap.cc pipelined_heap_test.cc
OBJECTS = $(SOURCES:%.cc=$(BUILD_DIR)/%.o)
TARGET = $(BUILD_DIR)/libsatsolver-confl4.so

# Build library
all: $(TARGET)

$(BUILD_DIR):
	@mkdir -p $@

$(TARGET): $(OBJECTS) | $(BUILD_DIR)
	$(CXX) $(LDFLAGS) -shared -o $@ $^ -lboost_context -lboost_coroutine

$(BUILD_DIR)/%.o: %.cc | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -fPIC -c -o $@ $<

# Install
install: $(TARGET)
	sst-register satsolver-confl4 satsolver-confl4_LIBDIR=$(abspath $(BUILD_DIR))
	sst-register SST_ELEMENT_SOURCE satsolver-confl4=$(CURDIR)
	sst-register SST_ELEMENT_TESTS satsolver-confl4=$(abspath ../tests)

# Clean
clean:
	rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.so $(BUILD_DIR)/*.a

distclean:
	rm -rf $(BUILD_DIR)

.PHONY: all install clean distclean
