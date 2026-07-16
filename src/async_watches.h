#ifndef ASYNC_WATCHES_H
#define ASYNC_WATCHES_H

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <cstring>
#include <vector>
#include <queue>
#include <unordered_set>
#include "structs.h"
#include "async_base.h"

// Node in the linked list of watchers
struct WatcherNode {
    // Use bit fields: 1 bit for valid flag
    uint32_t valid : 1;
    // Cref address of the watched clause (only meaningful when valid=1)
    uint32_t addr31 : 31;  // Cref >> 1

    union {
        // When used as a watcher
        Lit blocker;        // Blocker literal
        // When used in free list (valid=0): singly linked, next node only
        uint32_t next_free; // Next free node pointer (block_addr | node_idx)
    };

    WatcherNode() : valid(0), addr31(0), blocker(lit_Undef) {}
    // Constructor for watcher nodes
    WatcherNode(uint32_t ca, Lit b) : valid(1), addr31(ca >> 1), blocker(b) {
        assert((ca & 1) == 0 && "Clause address LSB must be 0 (bit fields)");
    }
    // Constructor for free list nodes (singly linked: only next_free is stored)
    explicit WatcherNode(uint32_t n) : valid(0), addr31(0), next_free(n) {}

    // left-shift to restore the original address
    Cref getClauseAddr() const { return addr31 << 1; }
};

// Block of watchers - dynamically sized based on PROPAGATORS
struct WatcherBlock {
    WatcherNode nodes[PROPAGATORS]; // Array of nodes sized according to PROPAGATORS
    uint32_t next_block : 29;       // Pointer to next block (0 = nullptr)
    uint32_t free_index : 3;        // Index of the node used in free list (0-7)
    uint32_t padding : 32;          // Padding for alignment

    WatcherBlock() : next_block(0), free_index(PROPAGATORS), padding(0) {}

    uint32_t countValidNodes() const {
        uint32_t count = 0;
        for (int i = 0; i < PROPAGATORS; i++) {
            count += nodes[i].valid;
        }
        return count;
    }
    
    // Check if this block is in the free list
    bool isInFreeList() const {
        return free_index < PROPAGATORS;
    }

    // find the first free (invalid) slot, ignoring free-list bookkeeping.
    // Used by the propagate-time free-list rebuild, which is authoritative
    // over free_index and cannot trust any prior value.
    int firstFreeSlot() const {
        for (int i = 0; i < PROPAGATORS; i++) {
            if (!nodes[i].valid) return i;
        }
        return -1;
    }

    // find next free slot, prioritizing slots not used in free list
    int findNextFreeNode() const {
        if (free_index == PROPAGATORS) return -1;
        for (int i = 0; i < PROPAGATORS; i++) {
            if (!nodes[i].valid && i != free_index) return i;
        }
        return free_index;
    }
    
    // Get the actual next block address (without free_index bits)
    uint32_t getNextBlock() const {
        return next_block << 3;
    }
    
    // Set the next block address while preserving the free_index
    void setNextBlock(uint32_t addr) {
        next_block = addr >> 3;
    }
};

// Metadata for each watch list
struct WatchMetaData {
    uint32_t head_ptr;      // Head pointer to the first block
    uint32_t free_head;     // Head ptr of free list (block_addr | node_idx)
    WatcherNode pre_watchers[PRE_WATCHERS];  // Pre-watchers stored directly in metadata

    WatchMetaData() : head_ptr(0), free_head(0) {}
};

class Watches : public AsyncBase {
public:
    Watches(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr,
            uint64_t watches_base_addr = 0, uint64_t nodes_base_addr = 0,
            coro_t::push_type** yield_ptr = nullptr,
            uint64_t nodes_region_end = 0)
        : AsyncBase("WATCH-> ", verbose, mem, yield_ptr),
          watches_base_addr(watches_base_addr),
          nodes_base_addr(nodes_base_addr),
          nodes_region_end(nodes_region_end),
          next_free_block(nodes_base_addr),
          block_size(sizeof(WatcherBlock)) {
        // head_ptr/free_head/next_free pack absolute node addresses into
        // uint32 (and next_block stores addr>>3 in 29 bits), so the node
        // region must end at or below 4GiB. output.fatal, not assert: this
        // must survive NDEBUG builds like every other region guard.
        if (nodes_region_end > (1ULL << 32)) {
            output.fatal(CALL_INFO, -1,
                "watch node region end 0x%lx exceeds 4GiB: node pointers are "
                "packed into uint32 fields\n", nodes_region_end);
        }
        output.verbose(CALL_INFO, 1, 0,
            "base addresses: watchlist=0x%lx, nodes=0x%lx-0x%lx\n",
            watches_base_addr, nodes_base_addr, nodes_region_end);
    }

    // Memory address calculations
    uint64_t watchesAddr(int idx) const { return watches_base_addr + idx * sizeof(WatchMetaData); }
    // only support one worker at a time for now
    bool isBusy(int lit_idx) const { return busy.find(lit_idx) != busy.end(); }
    
    // helper functions
    void freeBlock(uint32_t addr) { free_blocks.push(addr); }
    uint32_t allocateBlock();
    WatchMetaData readMetaData(int lit_idx, int worker_id = 0);
    void writeMetaData(int lit_idx, const WatchMetaData& metadata);
    void writeHeadPointer(int lit_idx, const uint32_t headptr);
    void writeFreeHead(int lit_idx, const uint32_t freehead);
    void writeSize(int lit_idx, const uint32_t size);
    void writePreWatcher(int lit_idx, const WatcherNode node, const int index);
    void writePreWatchers(int lit_idx, const WatcherNode pre_watchers[PRE_WATCHERS]);

    WatcherBlock readBlock(uint32_t addr, int worker_id = 0);
    void writeBlock(uint32_t addr, const WatcherBlock& block);
    void writeNextFree(uint32_t node_addr, const uint32_t next_free);

    // Free list management functions
    int addToFreeList(int lit_idx, WatchMetaData& metadata, WatcherBlock& block,
        uint32_t block_addr, int node_idx);
    int removeFromFreeList(int lit_idx, WatchMetaData& metadata, WatcherBlock& block);

    void initWatches(size_t watch_count, std::vector<Clause>& clauses);
    void updateBlock(int lit_idx, uint32_t prev_addr, uint32_t curr_addr, 
                     WatcherBlock& prev_block, WatcherBlock& curr_block, WatchMetaData& metadata);
    int insertWatcher(int lit_idx, Cref clause_addr, Lit blocker, int worker_id = 0);
    void removeWatcher(int lit_idx, Cref clause_addr, int worker_id = 0);

    
private:
    uint64_t watches_base_addr;    // Base address of the watches array (head pointers)
    uint64_t nodes_base_addr;      // Base address for watcher nodes
    uint64_t nodes_region_end;     // First address past the node region (0 = unchecked)
    uint64_t next_free_block;      // Next free address for block allocation (uint64 so the
                                   //  exhaustion check below cannot be defeated by uint32 wrap)
    size_t block_size;             // Size of a watcher block in bytes
    
    // Free list for recycling blocks
    std::queue<uint32_t> free_blocks;
    
    // literal indices that are currently busy
    std::unordered_set<int> busy;
};

#endif // ASYNC_WATCHES_H
