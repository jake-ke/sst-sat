#ifndef CLAUSES_H
#define CLAUSES_H

#include <unordered_map>
#include "async_base.h"


class Clauses : public AsyncBase {
public:
    Clauses(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr, 
            uint64_t clauses_cmd_base_addr = 0, uint64_t clauses_base_addr = 0, 
            coro_t::push_type** yield_ptr = nullptr);

    // Core operations
    Clause readClause(Cref addr, int worker_id = 0);
    void writeClause(Cref addr, const Clause& c);
    uint32_t getClauseSize(Cref addr, int worker_id = 0);
    void initialize(const std::vector<Clause>& clauses);
    Cref addClause(const Clause& clause);
    bool isLearnt(Cref addr) const { return addr >= learnt_offset; }
    void writeAct(Cref addr, float act);
    std::vector<Cref> readAllAddr(int worker_id = 0);
    std::vector<float> readAllAct(const std::vector<Cref>& addr, int worker_id = 0);
    void rescaleAllAct(float factor);
    std::unordered_map<Cref, Cref> reduceDB(const std::vector<Cref>& to_keep);

private:
    uint64_t clauses_cmd_base_addr;
    uint64_t clauses_base_addr;

    size_t num_orig_clauses;
    Cref learnt_offset;
    Cref next_free_offset;

    // Memory operations
    uint64_t cmdAddr(int idx) const {
        return clauses_cmd_base_addr + idx * sizeof(Cref); 
    }
    uint64_t clauseAddr(uint32_t offset) const {
        return clauses_base_addr + offset;
    }
    void writeAddr(uint32_t idx, const Cref& addr);
};

#endif // CLAUSES_H
