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
    // Use bit fields: 1 bit for valid flag, 31 bits for clause address
    uint32_t valid : 1;
    uint32_t clause_addr : 31;
    Lit blocker;        // Blocker literal

    WatcherNode() : valid(0), clause_addr(ClauseRef_Undef), blocker(lit_Undef) {}
    WatcherNode(Cref ca, Lit b) : valid(1), clause_addr(ca >> 1), blocker(b) {
        assert((ca & 1) == 0 && "Clause address LSB must be 0 (aligned)");
    }
    
    Cref getClauseAddr() const { 
        // Left-shift to restore the original address (add back the 0 bit)
        return clause_addr << 1; 
    }
};

// Block of watchers - dynamically sized based on PROPAGATORS
struct WatcherBlock {
    WatcherNode nodes[PROPAGATORS]; // Array of nodes sized according to PROPAGATORS
    uint32_t next_block;           // Pointer to next block (0 = nullptr)
    
    WatcherBlock() : next_block(0) {}

    uint32_t countValidNodes() const {
        uint32_t count = 0;
        for (int i = 0; i < PROPAGATORS; i++) {
            count += nodes[i].valid;
        }
        return count;
    }
};

// Metadata for each watch list
struct WatchMetaData {
    uint32_t head_ptr;  // Head pointer to the first block
    uint32_t size;      // Number of watchers in the list
    WatcherNode pre_watchers[PRE_WATCHERS];  // Pre-watchers stored directly in metadata
    
    WatchMetaData() : head_ptr(0), size(0) {}
};

class Watches : public AsyncBase {
public:
    // Proxy class for watch list access using []
    class WatchListProxy {
    private:
        Watches* parent;
        int lit_idx;

    public:
        WatchListProxy(Watches* p, int idx) : parent(p), lit_idx(idx) {}
        void remove(Cref clause_addr) { parent->removeWatcher(lit_idx, clause_addr); }
        void insert(Cref clause_addr, Lit blocker) { parent->insertWatcher(lit_idx, clause_addr, blocker); }
    };

    Watches(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr,
            uint64_t watches_base_addr = 0, uint64_t nodes_base_addr = 0,
            coro_t::push_type** yield_ptr = nullptr)
        : AsyncBase("WATCH-> ", verbose, mem, yield_ptr), 
          watches_base_addr(watches_base_addr), 
          nodes_base_addr(nodes_base_addr), 
          next_free_block(nodes_base_addr),
          block_size(sizeof(WatcherBlock)) {
        output.verbose(CALL_INFO, 1, 0, 
            "base addresses: watchlist=0x%lx, nodes=0x%lx\n", 
            watches_base_addr, nodes_base_addr);
    }

    WatchListProxy operator[](int idx) { return WatchListProxy(this, idx); }
    
    // Memory address calculations
    uint64_t watchesAddr(int idx) const { return watches_base_addr + idx * sizeof(WatchMetaData); }
    // only support one worker at a time for now
    bool isBusy(int lit_idx) const { return busy.find(lit_idx) != busy.end(); }
    
    WatchMetaData readMetaData(int lit_idx, int worker_id = 0);
    void writeMetaData(int start_idx, const WatchMetaData& metadata);
    void writePreWatchers(int start_idx, const WatcherNode* pre_watchers);
    WatcherBlock readBlock(uint32_t addr, int worker_id = 0);
    void writeBlock(uint32_t addr, const WatcherBlock& block);
    void updateBlock(int lit_idx, uint32_t prev_addr, uint32_t curr_addr, 
                     WatcherBlock& prev_block, WatcherBlock& curr_block);
    void initWatches(size_t watch_count, std::vector<Clause>& clauses);
    void insertWatcher(int lit_idx, Cref clause_addr, Lit blocker, int worker_id = 0);
    void removeWatcher(int lit_idx, Cref clause_addr, int worker_id = 0);

    // helper functions
    void writeHeadPointer(int start_idx, const uint32_t headptr);
    void writeSize(int start_idx, const uint32_t size);
    uint32_t allocateBlock();
    void freeBlock(uint32_t addr) { free_blocks.push(addr); }

private:
    uint64_t watches_base_addr;    // Base address of the watches array (head pointers)
    uint64_t nodes_base_addr;      // Base address for watcher nodes
    uint32_t next_free_block;      // Next free address for block allocation
    size_t block_size;             // Size of a watcher block in bytes
    
    // Free list for recycling blocks
    std::queue<uint32_t> free_blocks;
    
    // literal indices that are currently busy
    std::unordered_set<int> busy;
};

#endif // ASYNC_WATCHES_H
