#ifndef structs_h
#define structs_h

#include <boost/coroutine2/all.hpp>
#include <vector>
#include <cstddef>

using coro_t = boost::coroutines2::coroutine<void>;

const int MINIMIZERS = 2;  // Number of minimizers
const int PROPAGATORS = 7;  // Number of propagators
const int HEAPLANES = 8;  // Number of heap lanes for parallel execution
const bool OVERLAP_HEAP_INSERT = false;  // overlaps heap insertions (backtracking) with propagation
const bool OVERLAP_HEAP_BUMP = false;  // overlaps heap bumping with clause minimization and find bt level

// Define types for variables and literals
typedef int Var;
const Var var_Undef = 0;

// Define a constant for undefined clause reference
const int ClauseRef_Undef = -1;

struct Lit {
    int x;
    
    bool operator == (const Lit& other) const { return x == other.x; }
    bool operator != (const Lit& other) const { return x != other.x; }
    bool operator <  (const Lit& other) const { return x < other.x; }
};
const Lit lit_Undef = { 0 }; // Special undefined literal

struct Variable {
    size_t level;   // Decision level when variable was assigned
    int reason;     // Index of clause that caused this assignment
    
    Variable() : level(0), reason(ClauseRef_Undef) {}
};

struct Clause {
    std::vector<Lit> literals;

    Clause() {}
    Clause(const std::vector<Lit>& lits) : literals(lits) {}
    int size() const { return literals.size(); }
    Lit operator[] (size_t i) const { return literals[i]; }
};

// New struct for efficient clause metadata storage
struct ClauseMetaData {
    uint64_t offset;  // address of clause data
    uint64_t size;    // Number of literals in the clause
    
    ClauseMetaData() : offset(0), size(0) {}
    ClauseMetaData(uint64_t o, uint64_t s) : offset(o), size(s) {}
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