#ifndef CLAUSES_H
#define CLAUSES_H

#include "async_base.h"


class Clauses : public AsyncBase {
public:
    Clauses(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr, 
            uint64_t clauses_cmd_base_addr = 0, uint64_t clauses_base_addr = 0, 
            coro_t::push_type** yield_ptr = nullptr);

    // Core operations
    Clause readClause(const ClauseMetaData& cmd, int worker_id = 0);
    Clause readClause(int idx, int worker_id = 0);
    ClauseMetaData getMetaData(int idx, int worker_id = 0);
    void writeClause(uint64_t offset, const Clause& c);
    size_t getSize(int idx, int worker_id = 0);
    void initialize(const std::vector<Clause>& clauses);
    void addClause(const Clause& clause);
    void reduceDB(const std::vector<bool>& rm, int worker_id = 0);

private:
    uint64_t clauses_cmd_base_addr;
    uint64_t clauses_base_addr;

    size_t num_orig_clauses;
    size_t next_free_offset;

    // Memory operations
    uint64_t cmdAddr(int idx) const {
        return clauses_cmd_base_addr + idx * sizeof(ClauseMetaData); 
    }
    uint64_t clauseAddr(uint64_t offset) const {
        return clauses_base_addr + offset;
    }
    void writeMetaData(int idx, const ClauseMetaData& metadata);
};

#endif // CLAUSES_H
