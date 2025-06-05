#ifndef CLAUSES_H
#define CLAUSES_H

#include "async_base.h"


class Clauses : public AsyncBase {
public:
    // Proxy class for clause access
    class ClauseProxy {
    private:
        Clauses* parent;
        int clause_idx;

    public:
        ClauseProxy(Clauses* p, int idx) : parent(p), clause_idx(idx) {}
        
        // Implicit conversion to Clause reference for reading
        operator Clause&() {
            parent->readClause(clause_idx);
            return parent->last_read;
        }
        
        // const version for read-only access
        operator const Clause&() const {
            parent->readClause(clause_idx);
            return parent->last_read;
        }
        
        // Direct size operation
        size_t size() const { 
            parent->getMetaData(clause_idx);
            return parent->last_metadata.size;
        }
    };

    Clauses(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr, 
            uint64_t clauses_cmd_base_addr = 0, uint64_t clauses_base_addr = 0, 
            coro_t::push_type** yield_ptr = nullptr);

    // Array-style access
    ClauseProxy operator[](int idx) { return ClauseProxy(this, idx); }
    
    // Core operations
    void initialize(const std::vector<Clause>& clauses);
    void addClause(const Clause& clause);
    void swapLiterals(int idx, size_t pos1, size_t pos2);
    void reduceDB(const std::vector<bool>& rm);

private:
    uint64_t clauses_cmd_base_addr;
    uint64_t clauses_base_addr;

    size_t num_orig_clauses;
    size_t next_free_offset;
    Clause last_read;
    ClauseMetaData last_metadata;

    // Memory operations
    uint64_t cmdAddr(int idx) const {
        return clauses_cmd_base_addr + idx * sizeof(ClauseMetaData); 
    }
    void readClause(int idx);
    void writeClause(uint64_t offset, const Clause& c);
    void getMetaData(int idx);
    void writeMetaData(int idx, const ClauseMetaData& metadata);
};

#endif // CLAUSES_H
