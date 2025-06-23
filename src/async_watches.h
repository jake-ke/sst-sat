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
    int clause_idx;     // Index of the clause
    Lit blocker;        // Blocker literal
    
    WatcherNode() : clause_idx(ClauseRef_Undef), blocker(lit_Undef) {}
    WatcherNode(int ci, Lit b) : clause_idx(ci), blocker(b) {}
};

// Block of watchers - hardcoded to 64 byte cache line!
struct WatcherBlock {
    WatcherNode nodes[7];     // Array of nodes (size will be adjusted based on line_size)
    uint32_t next_block;      // Pointer to next block (0 = nullptr)
    uint8_t valid_mask;       // Bit mask for valid nodes
    // No need for padding as we'll handle line_size directly
    
    WatcherBlock() : valid_mask(0), next_block(0) {}
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
        void remove(int clause_idx) { parent->removeWatcher(lit_idx, clause_idx); }
        void insert(int clause_idx, Lit blocker) { parent->insertWatcher(lit_idx, clause_idx, blocker); }
    };

    Watches(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr,
            uint64_t watches_base_addr = 0, uint64_t nodes_base_addr = 0,
            coro_t::push_type** yield_ptr = nullptr)
        : AsyncBase("WATCH-> ", verbose, mem, yield_ptr), 
          watches_base_addr(watches_base_addr), 
          nodes_base_addr(nodes_base_addr), 
          next_free_block(nodes_base_addr),
          nodes_per_block(0) {
        output.verbose(CALL_INFO, 1, 0, 
            "base addresses: watchlist=0x%lx, nodes=0x%lx\n", 
            watches_base_addr, nodes_base_addr);
    }

    WatchListProxy operator[](int idx) { return WatchListProxy(this, idx); }
    void freeBlock(uint64_t addr) { free_blocks.push(addr); }
        
    // Memory address calculations
    uint64_t watchesAddr(int idx) const { return watches_base_addr + idx * sizeof(uint64_t); }
    uint64_t allocateBlock();
    
    // Helper method to access nodes_per_block
    size_t getNodesPerBlock() const { return nodes_per_block; }
    
    uint64_t readHeadPointer(int lit_idx, int worker_id = 0);
    void writeHeadPointer(int start_idx, const uint64_t headptr);
    WatcherBlock readBlock(uint64_t addr, int worker_id = 0);
    void writeBlock(uint64_t addr, const WatcherBlock& block);
    void updateBlock(int lit_idx, uint64_t prev_addr, uint64_t curr_addr, 
                     WatcherBlock& prev_block, WatcherBlock& curr_block);

    void initWatches(size_t watch_count, std::vector<Clause>& clauses);
    void insertWatcher(int lit_idx, int clause_idx, Lit blocker, int worker_id = 0);
    void removeWatcher(int lit_idx, int clause_idx, int worker_id = 0);

    // only support one worker at a time for now
    bool isBusy(int lit_idx) const { return busy.find(lit_idx) != busy.end(); }

private:
    uint64_t watches_base_addr;    // Base address of the watches array (head pointers)
    uint64_t nodes_base_addr;      // Base address for watcher nodes
    uint64_t next_free_block;      // Next free address for block allocation
    
    // Free list for recycling blocks
    std::queue<uint64_t> free_blocks;
    
    // Number of nodes that can fit in a cache line
    size_t nodes_per_block;
    
    // literal indices that are currently busy
    std::unordered_set<int> busy;
};

#endif // ASYNC_WATCHES_H
