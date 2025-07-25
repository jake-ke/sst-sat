# Directories
BUILD_DIR = ../build

# Compiler configuration
CXX = $(shell sst-config --CXX)
CXXFLAGS = $(shell sst-config --ELEMENT_CXXFLAGS) -DBOOST_COROUTINES_NO_DEPRECATION_WARNING
LDFLAGS = $(shell sst-config --ELEMENT_LDFLAGS)

# Add SST Elements include path
SST_ELEMENTS_INCLUDE = -I$(SST_ELEMENTS_HOME)/include
CXXFLAGS += $(SST_ELEMENTS_INCLUDE)

# Source files
SOURCES = memory_allocator.cc cache_profiler.cc async_base.cc async_heap.cc async_watches.cc async_clauses.cc async_activity.cc async_var_activity.cc satsolver.cc
OBJECTS = $(SOURCES:%.cc=$(BUILD_DIR)/%.o)
TARGET = $(BUILD_DIR)/libsatsolver.so

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
	sst-register satsolver satsolver_LIBDIR=$(abspath $(BUILD_DIR))
	sst-register SST_ELEMENT_SOURCE satsolver=$(CURDIR)
	sst-register SST_ELEMENT_TESTS satsolver=$(abspath ../tests)

# Clean
clean:
	rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.so $(BUILD_DIR)/*.a

distclean:
	rm -rf $(BUILD_DIR)

.PHONY: all install clean distclean
