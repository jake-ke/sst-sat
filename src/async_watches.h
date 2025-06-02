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

// Node in the linked list of watchers
struct WatcherNode {
    int clause_idx;     // Index of the clause
    Lit blocker;        // Blocker literal
    uint64_t next;      // Address of next node (0 = nullptr)
    
    WatcherNode() : clause_idx(ClauseRef_Undef), blocker(lit_Undef), next(0) {}
    WatcherNode(int ci, Lit b, uint64_t n = 0) : clause_idx(ci), blocker(b), next(n) {}
};

class Watches {
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

    Watches(int verbose, SST::Interfaces::StandardMem* mem, uint64_t watches_base_addr, 
            uint64_t nodes_base_addr, coro_t::push_type** yield_ptr = nullptr)
        : memory(mem), watches_base_addr(watches_base_addr), 
          nodes_base_addr(nodes_base_addr), num_watches(0),
          next_free_node(nodes_base_addr), busy(false), yield_ptr(yield_ptr) {
        output.init("WATCH-> ", verbose, 0, SST::Output::STDOUT);
    }

    // Get watch list by index
    WatchListProxy operator[](int idx) { return WatchListProxy(this, idx); }
        
    // Memory address calculations
    uint64_t watchesAddr(int idx) const { return watches_base_addr + idx * sizeof(uint64_t); }
    
    uint64_t allocateNode();
    
    // Read head pointer for a literal
    void readHeadPointer(int lit_idx);
    
    // Write head pointers (for initialization)
    void writeHeadPointers(int start_idx, const std::vector<uint64_t>& ptrs);
    
    // Read a watcher node from memory
    void readNode(uint64_t addr);
    
    // Write a watcher node to memory
    void writeNode(uint64_t addr, const WatcherNode& node);

    void initWatches(size_t watch_count, std::vector<Clause>& clauses);
    
    // Insert a new watcher for a literal
    void insertWatcher(int lit_idx, int clause_idx, Lit blocker);
    
    // Remove a watcher with given clause index
    void removeWatcher(int lit_idx, int clause_idx);

    // Handle memory response
    void handleMem(SST::Interfaces::StandardMem::Request* req);
    
    size_t size() const { return num_watches; }
    void freeNode(uint64_t addr) { free_nodes.push(addr); }
    uint64_t getLastHeadPointer() const { return last_head_ptr; }
    WatcherNode getLastReadNode() const { return last_node; }
    bool isBusy() const { return busy; }

private:
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    uint64_t watches_base_addr;    // Base address of the watches array (head pointers)
    uint64_t nodes_base_addr;      // Base address for watcher nodes
    size_t num_watches;            // Number of watch lists
    uint64_t next_free_node;       // Next free address for node allocation
    bool busy;                     // Flag for ongoing memory operations
    coro_t::push_type** yield_ptr; // Pointer to the yield_ptr in SATSolver
    
    // Free list for recycling nodes
    std::queue<uint64_t> free_nodes;
    
    // Results of memory operations
    uint64_t last_head_ptr;        // Last read head pointer
    WatcherNode last_node;         // Last read watcher node
};

#endif // ASYNC_WATCHES_H
