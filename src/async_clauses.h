#ifndef CLAUSES_H
#define CLAUSES_H

#include <unordered_map>
#include "async_base.h"
#include "memory_allocator.h"

// First 16 bytes of a stored clause: everything reduceDB needs to make a
// keep/remove decision (activity), check locked (l0) and detach (l0, l1)
// in a single pipelined read.
struct ClauseHead {
    uint32_t num_lits;
    float activity;
    Lit l0, l1;
};

class Clauses : public AsyncBase {
public:
    Clauses(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr,
            uint64_t clauses_cmd_base_addr = 0, uint64_t clauses_base_addr = 0,
            coro_t::push_type** yield_ptr = nullptr,
            uint64_t clauses_region_size = 0x0FFFFFFF);

    void setReorderBuffer(ReorderBuffer* rb) override {
        reorder_buffer = rb;
        allocator.setReorderBuffer(rb);
    }
    void printFragStats() const { allocator.printFragStats(); }
    void printBinaryStats() const;

    // Core operations
    Clause readClause(Cref addr, int worker_id = 0);
    void writeClause(Cref addr, const Clause& c);
    void writeLiteral(Cref addr, const Lit& lit, int idx);
    uint32_t getClauseSize(Cref addr, int worker_id = 0);
    ClauseHead readClauseHead(Cref addr, int worker_id = 0);
    float readAct(Cref addr, int worker_id = 0);
    void initialize(const std::vector<Clause>& clauses);
    Cref addClause(const Clause& clause);
    bool isLearnt(Cref addr) const { return addr >= learnt_offset; }
    // Binary learnt clauses live in a bump-down region at the top of the
    // clause region (grown chunk-wise by lowering the allocator ceiling), so
    // membership is a single range check against the current ceiling.
    bool isBinaryLearnt(Cref addr) const { return addr >= (Cref)allocator.capacity(); }
    void writeAct(Cref addr, float act);
    size_t numLearnts() const { return size_ - num_orig_clauses; }
    size_t numOrig() const { return num_orig_clauses; }

    // reduceDB streaming support: read a chunk of the learnt pointer array,
    // write one surviving pointer during in-place compaction, then shrink.
    std::vector<Cref> readAddrChunk(size_t learnt_start, size_t count, int worker_id = 0);
    void compactKeep(size_t keep_idx, Cref addr);
    void finishReduce(size_t kept);
    void freeClause(Cref addr, uint32_t cls_size, int worker_id = 0);

private:
    uint64_t clauses_cmd_base_addr;
    uint64_t clauses_base_addr;

    size_t num_orig_clauses;
    Cref learnt_offset;

    // Binary-clause bump region: allocations go DOWN from the top of the
    // clause region; when it runs out, the allocator ceiling is lowered by
    // BINARY_CHUNK (only possible while the top of the heap is free).
    static const uint32_t BINARY_CHUNK = 65536;
    Cref binary_next_;        // next (lowest) binary allocation goes just below this
    Cref region_size_;        // full clause region size (binary region top)
    size_t num_binary_;       // binary learnt clauses ever allocated (never freed)

    // Memory allocator
    MemoryAllocator allocator;

    // Memory operations
    uint64_t cmdAddr(int idx) const {
        return clauses_cmd_base_addr + idx * sizeof(Cref);
    }

    uint64_t clauseAddr(uint32_t offset) const {
        // Learnt clauses managed by the allocator carry a block-header tag;
        // original clauses and bump-allocated binary clauses do not.
        bool tagged = offset >= (uint32_t)learnt_offset && !isBinaryLearnt((Cref)offset);
        return clauses_base_addr + offset + (tagged ? TAG_SIZE : 0);
    }

    void writeAddr(uint32_t idx, const Cref& addr);
};

#endif // CLAUSES_H
