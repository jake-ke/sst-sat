# SAT Solver SST Element

An asynchronous SAT solver implementation for the Structural Simulation Toolkit (SST).

## Quick Start

### Prerequisites
- SST Framework installed and configured
- Boost libraries (context, coroutine)
- C++ compiler with C++11 support

### Building

```bash
# Build the library
make

# Register with SST
make install

# Run tests
make test

# Clean build artifacts
make clean
```

## Project Structure

```
sst-sat/
├── src/           # Source code and headers
├── tests/         # Test files and scripts
├── tools/         # Analysis and utility scripts
├── examples/      # Example CNF files and data
└── build/         # Build artifacts (generated)
```

## Usage

Run the SAT solver with a CNF file:

```bash
cd tests
python test_basic.py --cnf test.cnf --verbose 1
```

## Build Targets

- `make` or `make build` - Build the shared library
- `make install` - Build and register with SST
- `make clean` - Remove build artifacts
- `make test` - Run basic tests
