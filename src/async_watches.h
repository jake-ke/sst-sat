#ifndef ASYNC_WATCHES_H
#define ASYNC_WATCHES_H

#include <sst/core/sst_config.h> // This include is REQUIRED for all implementation files
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <cstring>
#include <vector>
#include <queue>
#include "structs.h"
#include "async_base.h"

// Node in the linked list of watchers
struct WatcherNode {
    int clause_idx;     // Index of the clause
    Lit blocker;        // Blocker literal
    uint64_t next;      // Address of next node (0 = nullptr)
    
    WatcherNode() : clause_idx(ClauseRef_Undef), blocker(lit_Undef), next(0) {}
    WatcherNode(int ci, Lit b, uint64_t n = 0) : clause_idx(ci), blocker(b), next(n) {}
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
          next_free_node(nodes_base_addr) {
        output.verbose(CALL_INFO, 1, 0, 
            "base addresses: watchlist=0x%lx, nodes=0x%lx\n", 
            watches_base_addr, nodes_base_addr);
    }

    WatchListProxy operator[](int idx) { return WatchListProxy(this, idx); }
    void freeNode(uint64_t addr) { free_nodes.push(addr); }
    uint64_t getLastHeadPointer() const { return last_head_ptr; }
    WatcherNode getLastReadNode() const { return last_node; }
        
    // Memory address calculations
    uint64_t watchesAddr(int idx) const { return watches_base_addr + idx * sizeof(uint64_t); }
    uint64_t allocateNode();
    
    void readHeadPointer(int lit_idx);
    void writeHeadPointer(int start_idx, const uint64_t headptr);
    void readNode(uint64_t addr);
    void writeNode(uint64_t addr, const WatcherNode& node);

    void initWatches(size_t watch_count, std::vector<Clause>& clauses);
    void insertWatcher(int lit_idx, int clause_idx, Lit blocker);
    void removeWatcher(int lit_idx, int clause_idx);
    void handleMem(SST::Interfaces::StandardMem::Request* req) override;

private:
    uint64_t watches_base_addr;    // Base address of the watches array (head pointers)
    uint64_t nodes_base_addr;      // Base address for watcher nodes
    uint64_t next_free_node;       // Next free address for node allocation
    
    // Free list for recycling nodes
    std::queue<uint64_t> free_nodes;
    
    // Results of memory operations
    uint64_t last_head_ptr;        // Last read head pointer
    WatcherNode last_node;         // Last read watcher node
};

#endif // ASYNC_WATCHES_H
