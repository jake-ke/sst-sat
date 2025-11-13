#ifndef structs_h
#define structs_h

#include <boost/coroutine2/all.hpp>
#include <vector>
#include <cstddef>

using coro_t = boost::coroutines2::coroutine<void>;

// const int PARA_LITS = 8;  // Number of parallel literals to propagate
// const int PROPAGATORS = 7;  // Number of watchers to propagate
// const int MAX_CONFL = 8;  // Maximum number of learned clauses
// const int LEARNERS = 8;  // Number of learners for clause learning
// const int HEAPLANES = 8;  // Number of heap lanes for parallel execution
// const int MINIMIZERS = 4;  // Number of minimizers
// const bool OVERLAP_HEAP_INSERT = true;  // overlaps heap insertions (backtracking) with propagation
// const bool OVERLAP_HEAP_BUMP = true;  // overlaps heap bumping with clause minimization and find bt level
// const bool WRITE_BUFFER = true;  // enables write request buffering for improved performance
// const int PRE_WATCHERS = 7;  // Number of pre-watchers to store in metadata
// const int USE_FREE_LIST = 1;  // Use free list for watcher insertion

const int PARA_LITS = 8;  // Number of parallel literals to propagate
const int PROPAGATORS = 7;  // Number of watchers to propagate
const int MAX_CONFL = 1;  // Maximum number of learned clauses
const int LEARNERS = 1;  // Number of learners for clause learning
const int HEAPLANES = 1;  // Number of heap lanes for parallel execution
const int MINIMIZERS = 1;  // Number of minimizers
const bool OVERLAP_HEAP_INSERT = true;  // overlaps heap insertions (backtracking) with propagation
const bool OVERLAP_HEAP_BUMP = true;  // overlaps heap bumping with clause minimization and find bt level
const bool WRITE_BUFFER = true;  // enables write request buffering for improved performance
const int PRE_WATCHERS = 0;  // Number of pre-watchers to store in metadata
const int USE_FREE_LIST = 0;  // Use free list for watcher insertion

// helpers
const int FREE_IDX_BITS = pow(2, ceil(log(PROPAGATORS)/log(2)));  // next power of 2

// Define types for variables and literals
typedef int Var;
const Var var_Undef = 0;

// Define a constant for undefined clause reference
typedef int Cref;
const Cref ClauseRef_Undef = 0;

struct Lit {
    int x;
    
    bool operator == (const Lit& other) const { return x == other.x; }
    bool operator != (const Lit& other) const { return x != other.x; }
    bool operator <  (const Lit& other) const { return x < other.x; }
};
const Lit lit_Undef = { 0 }; // Special undefined literal

struct Variable {
    size_t level;   // Decision level when variable was assigned
    Cref reason;     // Index of clause that caused this assignment
    
    Variable() : level(0), reason(ClauseRef_Undef) {}
};

const int CLAUSE_MEMBER_SIZE = 4;  // bytes, union of size of num_lits, activity, and Lit
struct Clause {
    uint32_t num_lits;  // Number of literals in the clause
    float activity;     // Activity score for the clause
    std::vector<Lit> literals;

    Clause() : activity(0.0) {}
    
    Clause(std::vector<Lit> lits, float act = 0.0) :
        num_lits(lits.size()), literals(std::move(lits)), activity(act) {}
    
    Clause(std::vector<Lit> lits) :
        num_lits(lits.size()), literals(std::move(lits)), activity(0.0) {}
    
    Clause(uint32_t s) : num_lits(s), activity(0.0) { literals.resize(s); }
    
    // number of literals
    uint32_t litSize() const { assert(num_lits == literals.size()); return num_lits; }
    // number of bytes
    uint32_t size() const { return CLAUSE_MEMBER_SIZE * 2 + litSize() * sizeof(Lit); }
    float act() const { return activity; }
    Lit operator[] (size_t i) const { return literals[i]; }
};

// Store queue entry for Write->Read ordering
struct StoreQueueEntry {
    uint64_t addr;                // Memory address
    size_t size;                  // Size of data in bytes
    std::vector<uint8_t> data;    // Data to be written
    StoreQueueEntry(uint64_t a, size_t s, const std::vector<uint8_t>& d)
        : addr(a), size(s), data(d) {}
};

// for parallel literal propagation
class WatchListQueue {
private:
    // number of items waiting to be inserted into watchlist
    std::unordered_map<int, int> counts;

public:
    void add(int item) { counts[item]++; }

    void remove(int item) {
        if (counts.find(item) != counts.end()) {
            counts[item]--;
            if (counts[item] <= 0) counts.erase(item);
        }
    }

    int count(int item) const {
        auto it = counts.find(item);
        return (it != counts.end()) ? it->second : 0;
    }
};

// Events for heap operations
class HeapReqEvent : public SST::Event {
public:
    enum OpType { INSERT, REMOVE_MAX, READ, BUMP, DEBUG_HEAP };
    OpType op;
    int arg;
    HeapReqEvent() : op(HeapReqEvent::INSERT), arg(0) {}
    HeapReqEvent(OpType o, int a = 0)
        : op(o), arg(a) {}
    
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        Event::serialize_order(ser);
        SST_SER(op);
        SST_SER(arg);
    }
    ImplementSerializable(HeapReqEvent);
};

class HeapRespEvent : public SST::Event {
public:
    int result;
    HeapRespEvent() : result(0) {}
    HeapRespEvent(int r) : result(r) {}
    
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        Event::serialize_order(ser);
        SST_SER(result);
    }
    ImplementSerializable(HeapRespEvent);
};

// Helper functions for literals
inline Lit mkLit(Var var, bool sign = false) { Lit p; p.x = var + var + (int)sign; return p; }
inline Lit operator ~(Lit p) { Lit q; q.x = p.x ^ 1; return q; }
inline bool sign(Lit p) { return p.x & 1; }
inline int var(Lit p) { return p.x >> 1; }
inline Lit toLit(int dimacs_lit) { 
    int var = abs(dimacs_lit);
    return dimacs_lit > 0 ? mkLit(var, false) : mkLit(var, true);
}
inline int toInt(Lit p) { return sign(p) ? -var(p) : var(p); }
inline int toWatchIndex(Lit p) { return p.x; }

#endif // structs_h