#ifndef ASYNC_CLAUSES_H
#define ASYNC_CLAUSES_H

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <vector>
#include <string>
#include "structs.h"

class AsyncClauses {
public:
    // Proxy class for clause access
    class ClauseProxy {
    private:
        AsyncClauses* parent;
        int clause_idx;

    public:
        ClauseProxy(AsyncClauses* p, int idx) : parent(p), clause_idx(idx) {}
        
        // Implicit conversion to Clause reference for reading
        operator Clause&() {
            parent->readClause(clause_idx);
            return parent->getLastRead();
        }
        
        // const version for read-only access
        operator const Clause&() const {
            parent->readClause(clause_idx);
            return parent->getLastRead();
        }
        
        // Access to literals array
        std::vector<Lit>& literals() {
            parent->readClause(clause_idx);
            return parent->getLastRead().literals;
        }

        // Direct size operation
        size_t size() const { 
            parent->readClause(clause_idx);
            return parent->getLastRead().size();
        }

        // Read-only indexing operation
        Lit operator[](size_t i) const {
            parent->readClause(clause_idx);
            return parent->getLastRead().literals[i];
        }
    };

    AsyncClauses(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr, 
                 uint64_t clauses_base_addr = 0, coro_t::push_type** yield_ptr = nullptr);

    // Array-style access
    ClauseProxy operator[](int idx) { return ClauseProxy(this, idx); }

    // Basic accessors
    size_t size() const { return clause_offsets.size(); }
    bool empty() const { return clause_offsets.empty(); }
    bool isBusy() const { return busy; }
    Clause& getLastRead() { return last_read; }
    
    // Core operations
    void initialize(const std::vector<Clause>& clauses, size_t num_original);
    void addClause(const Clause& clause);
    void reduceDB(const std::vector<bool>& to_remove);
    void handleMem(SST::Interfaces::StandardMem::Request* req);
    void swapLiterals(int idx, size_t pos1, size_t pos2);

private:
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    coro_t::push_type** yield_ptr;
    uint64_t clauses_base_addr;

    size_t num_orig_clauses;
    mutable bool busy;
    mutable Clause last_read;               // Last clause read from memory
    mutable std::vector<uint8_t> last_buffer; // Buffer for bulk reads
    size_t next_free_offset;                // Next offset for insertion
    std::vector<size_t> clause_offsets;     // offsets to clauses in memory

    void readClause(int idx) const;         // Read a clause from memory
};

#endif // ASYNC_CLAUSES_H
