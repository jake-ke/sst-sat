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
typedef int Cref;
const Cref ClauseRef_Undef = -1;

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