#ifndef MEMORY_ALLOCATOR_H
#define MEMORY_ALLOCATOR_H

#include <sst/core/output.h>
#include "async_base.h"
#include <vector>
#include <cstring>

// Block header structure with allocated flag and block size
struct BlockHeader {
    uint32_t allocated : 1;
    uint32_t block_size : 31; // Size in bytes
};

typedef BlockHeader BlockFooter;

// Constants for memory allocation
static const uint32_t TAG_SIZE = sizeof(BlockHeader);
static const uint32_t MIN_BLOCK_SIZE = 2 * TAG_SIZE + 2 * sizeof(Cref);  // 2 literals

// Number of size classes for segregated free lists
static const int NUM_SIZE_CLASSES = 8;

// Size class thresholds (in bytes)
static const uint32_t SIZE_CLASSES[NUM_SIZE_CLASSES] = {
    MIN_BLOCK_SIZE,                     // Class 0: [2 literals, class 1)
    MIN_BLOCK_SIZE + sizeof(Lit),       // Class 1: [3 literals, class 2)
    MIN_BLOCK_SIZE + 2 * sizeof(Lit),   // Class 2: [4 literals, class 3)
    MIN_BLOCK_SIZE + 6 * sizeof(Lit),   // Class 3: [8 literals, class 4)
    MIN_BLOCK_SIZE + 10 * sizeof(Lit),  // Class 4: [12 literals, class 5)
    MIN_BLOCK_SIZE + 18 * sizeof(Lit),  // Class 5: [20 literals, class 6)
    MIN_BLOCK_SIZE + 30 * sizeof(Lit),  // Class 6: [32 literals, class 7)
    MIN_BLOCK_SIZE + 62 * sizeof(Lit),  // Class 7: [64 literals, class 8)
};

class MemoryAllocator {
public:
    MemoryAllocator(int verbose, uint64_t mem_base_addr, uint64_t total_size);
    
    // Core allocation functions
    Cref allocateBlock(uint32_t size);
    void freeBlock(Cref addr, size_t req_size);
    
    // Initialization
    void setReorderBuffer(ReorderBuffer* rb) { reorder_buffer = rb; }
    void initialize(AsyncBase* async_base, Cref reserved_size = 0);
    uint64_t getMemoryEnd() const { return mem_base_addr + heap_size; }
    
    // Fragmentation tracking
    double fragRatio() const { return frag_ratio; }
    double peakFragRatio() const { return peak_frag_ratio; }
    void printFragStats() const;
    
private:
    SST::Output output;
    AsyncBase* async_base;  // Reference to AsyncBase for memory operations
    ReorderBuffer* reorder_buffer; // Direct reference to reorder buffer
    uint64_t mem_base_addr;
    uint64_t heap_size;
    Cref reserved_size;
    
    // Segregated free lists
    Cref free_lists[NUM_SIZE_CLASSES];
    
    // Fragmentation tracking
    uint64_t req_mem;        // Total requested memory by client
    uint64_t alloc_mem;      // Total allocated memory (including overhead)
    double frag_ratio;       // Current internal fragmentation ratio
    double peak_frag_ratio;  // Peak internal fragmentation ratio
    void updateFragStats();
    
    // Memory allocator helpers
    int getSizeClass(uint32_t size) const;
    
    // Block manipulation helpers
    BlockHeader readBlockTag(Cref addr, int worker_id = 0);
    void setTags(Cref addr, uint32_t size, bool allocated);
    Cref getNextFreeBlock(Cref addr, int worker_id = 0);
    Cref getPrevFreeBlock(Cref addr, int worker_id = 0);
    void setNextFreeBlock(Cref addr, Cref next);
    void setPrevFreeBlock(Cref addr, Cref prev);
    void insertFreeBlock(Cref addr, uint32_t size);
    void removeFreeBlock(Cref addr, uint32_t block_size);
};

#endif // MEMORY_ALLOCATOR_H
