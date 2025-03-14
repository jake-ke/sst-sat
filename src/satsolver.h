#ifndef SATSOLVER_H
#define SATSOLVER_H

#include <sst/core/component.h>
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <map>
#include <vector>
#include <string>

enum State { INIT, PARSING, SOLVING, DONE };

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

const Lit lit_Undef = { 0 }; // Special undefined literal

struct Clause {
    std::vector<Lit> literals;
    bool learnt;  // Flag to identify if this is a learnt clause
    
    Clause() : learnt(false) {}
    Clause(const std::vector<Lit>& lits, bool is_learnt = false) 
        : literals(lits), learnt(is_learnt) {}
    int size() const { return literals.size(); }
};

struct Variable {
    bool assigned;
    bool value;
    size_t level;                // Decision level when variable was assigned
    int reason;                  // Index of clause that caused this assignment, or ClauseRef_Undef
    
    Variable() : assigned(false), value(false), level(0), reason(ClauseRef_Undef) {}
};

// Watcher structure for 2WL scheme
struct Watcher {
    int clause_idx;  // Index of the clause in the clauses vector
    Lit blocker;        // Blocker literal (optimization to avoid accessing clause memory)
    
    Watcher() : clause_idx(ClauseRef_Undef), blocker(lit_Undef) {} // Default constructor
    Watcher(int ci, Lit b) : clause_idx(ci), blocker(b) {}
};

class SATSolver : public SST::Component {

public:
    SST_ELI_REGISTER_COMPONENT(
        SATSolver,
        "satsolver",
        "SATSolver",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "SAT Solver Component",
        COMPONENT_CATEGORY_PROCESSOR
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"clock", "Clock frequency", "1GHz"},
        {"verbose", "Verbosity level", "0"},
        {"filesize", "Size of CNF file to read", "0"}
    )

    SST_ELI_DOCUMENT_STATISTICS(
        {"decisions", "Number of decisions made", "count", 1},
        {"propagations", "Number of propagations", "count", 1},
        {"backtracks", "Number of backtracks", "count", 1},
        {"assigned_vars", "Current number of assigned variables", "count", 1}
    )

    SST_ELI_DOCUMENT_PORTS(
        {"mem_link", "Connection to HBM", {"memHierarchy.MemEventBase"}}
    )

    SATSolver(SST::ComponentId_t id, SST::Params& params);
    ~SATSolver();

    virtual void init(unsigned int phase) override;
    virtual void setup() override;
    virtual void complete(unsigned int phase) override;
    virtual void finish() override;

    bool clockTick(SST::Cycle_t currentCycle);
    void handleMemEvent(SST::Interfaces::StandardMem::Request* req);
    void parseDIMACS(const std::string& content);
    bool solveCDCL();  // Renamed from solveDPLL to solveCDCL

private:
    State state;
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    std::string dimacs_content;
    bool requestPending;
    SST::Cycle_t currentCycle;

    // Parsing state
    size_t filesize;
    uint32_t num_vars;
    uint32_t num_clauses;
    
    // SAT solver state
    std::vector<Clause> clauses;
    std::vector<Variable> variables;  // Indexed by variable number
    
    // Trail for recording assignment order
    uint qhead;
    std::vector<Lit> trail;           // Sequence of assignments in chronological order
    std::vector<uint> trail_lim;       // Indices in trail for the first literal at each decision level
    
    // Clause learning
    std::vector<char> seen;  // Temporary array for conflict analysis

    // Two Watched Literals implementation
    std::vector<std::vector<Watcher>> watches;  // Indexed by literal encoding
    
    Statistic<uint64_t>* stat_decisions;
    Statistic<uint64_t>* stat_propagations;
    Statistic<uint64_t>* stat_backtracks;
    Statistic<uint64_t>* stat_assigned_vars;

    // CDCL helper functions
    int unitPropagate();  // returns conflict clause index or ClauseRef_Undef if no conflict
    bool decide();
    void backtrack(int backtrack_level);
    void analyze(int conflict, std::vector<Lit>& learnt_clause, int& backtrack_level);
    
    // Utility functions for CDCL
    void trailEnqueue(Lit literal, int reason = ClauseRef_Undef);
    void unassignVariable(Var var);
    Var chooseBranchVariable();
    int current_level() { return trail_lim.size(); }  // Changed to use trail_lim
    
    // Two Watched Literals helper functions
    inline int toWatchIndex(Lit p) { return p.x; }
    void attachClause(int clause_idx);
    void insert_watch(Lit p, Watcher w);
    void ensureVarCapacity(Var v);
};

#endif // SATSOLVER_H
