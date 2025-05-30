#ifndef SATSOLVER_H
#define SATSOLVER_H

#include <sst/core/component.h>
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <map>
#include <vector>
#include <string>
#include <fstream>    // For reading decision file
#include <boost/coroutine2/all.hpp>
#include "structs.h"
#include "async_heap.h"
#include "async_variables.h"

//-----------------------------------------------------------------------------------
// Type Definitions and Constants
//-----------------------------------------------------------------------------------

// Unified state machine combining high-level states and detailed operations
enum SolverState { 
    IDLE,
    INIT,
    STEP,
    PROPAGATE,
    DECIDE,
    ANALYZE,
    BACKTRACK,
    REDUCE,
    RESTART,
    DONE 
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

//-----------------------------------------------------------------------------------
// Data Structures
//-----------------------------------------------------------------------------------

struct Clause {
    std::vector<Lit> literals;
    double activity;  // Activity score for this clause

    Clause() : activity(0) {}
    Clause(const std::vector<Lit>& lits) 
        : literals(lits), activity(0) {}
    int size() const { return literals.size(); }
};

// Watcher structure for 2WL scheme
struct Watcher {
    int clause_idx;  // Index of the clause in the clauses vector
    Lit blocker;        // Blocker literal (optimization to avoid accessing clause memory)
    
    Watcher() : clause_idx(ClauseRef_Undef), blocker(lit_Undef) {} // Default constructor
    Watcher(int ci, Lit b) : clause_idx(ci), blocker(b) {}
};


//-----------------------------------------------------------------------------------
// Component Class
//-----------------------------------------------------------------------------------

class SATSolver : public SST::Component {

public:
    // SST ELI Registrations
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
        {"filesize", "Size of CNF file to read", "0"},
        {"var_decay", "Variable activity decay factor", "0.95"},
        {"clause_decay", "Clause activity decay factor", "0.999"},
        {"random_var_freq", "Frequency of random decisions", "0.02"},
        {"decision_file", "Path to a file containing decision sequence", ""},
        {"decision_output_file", "Path to output decision sequence", ""},
        {"luby_restart", "Use Luby restart sequence", "true"},
        {"restart_first", "Initial restart limit", "100"},
        {"restart_inc", "Restart limit increase factor", "2.0"},
        {"heap_base_addr", "Base address for heap memory", "0x00000000"},
        {"indices_base_addr", "Base address for indices memory", "0x10000000"},
        {"variables_base_addr", "Base address for variables memory", "0x20000000"},
    )

    SST_ELI_DOCUMENT_STATISTICS(
        {"decisions", "Number of decisions made", "count", 1},
        {"propagations", "Number of propagations", "count", 1},
        {"assigns", "Number of variable assignments", "count", 1},
        {"unassigns", "Number of variable unassignments", "count", 1},
        {"conflicts", "Number of conflicts", "count", 1},
        {"learned", "Number of learnt clauses", "count", 1},
        {"removed", "Number of clauses removed during DB reductions", "count", 1},
        {"db_reductions", "Number of clause database reductions", "count", 1},
        {"minimized_literals", "Number of literals removed by clause minimization", "count", 1},
        {"restarts", "Number of restarts", "count", 1},
    )

    SST_ELI_DOCUMENT_PORTS(
        {"cnf_mem_link", "Connection to CNF memory", {"memHierarchy.MemEventBase"}},
        {"global_mem_link", "Connection to global memory", {"memHierarchy.MemEventBase"}},
        {"heap_port", "Link to external heap subcomponent", {"sst.Event"}}
    )
    
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"cnf_memory", "Memory interface for CNF data", "SST::Interfaces::StandardMem"},
        {"global_memory", "Memory interface for Heap and Variables", "SST::Interfaces::StandardMem"},
        {"order_heap", "ordered heap for VSIDS", "Heap"}
    )

    // Component Lifecycle Methods
    SATSolver(SST::ComponentId_t id, SST::Params& params);
    ~SATSolver();

    virtual void init(unsigned int phase) override;
    virtual void setup() override;
    virtual void complete(unsigned int phase) override;
    virtual void finish() override;

    // Event Handling Methods
    void handleCnfMemEvent(SST::Interfaces::StandardMem::Request* req);
    void handleGlobalMemEvent(SST::Interfaces::StandardMem::Request* req);
    void handleHeapResponse(SST::Event* ev);

    // Top level FSM
    bool clockTick(SST::Cycle_t currentCycle);
    void execPropagate();
    void execAnalyze();
    void execBacktrack();
    void execReduce();
    void execRestart();
    void execDecide();
    
    // Input Processing
    void parseDIMACS(const std::string& content);
    
    // Core CDCL Algorithm
    void initialize();
    bool decide();
    int unitPropagate();
    void analyze();
    void backtrack(int backtrack_level);
    
    // Trail Management
    void trailEnqueue(Lit literal, int reason = ClauseRef_Undef);
    void unassignVariable(Var var);
    int current_level() { return trail_lim.size(); }

    // Two-Watched Literals
    inline int toWatchIndex(Lit p) { return p.x; }
    void attachClause(int clause_idx);
    void detachClause(int clause_idx);
    void insert_watch(Lit p, Watcher w);
    void remove_watch(std::vector<Watcher>& ws, int clause_idx);
    
    // Decision Heuristics
    Lit chooseBranchVariable();
    void insertVarOrder(Var v);   // Insert variable into order heap
    void varDecayActivity();       // Decay all variable activities
    void varBumpActivity(Var v);   // Bump a variable's activity
    
    // Clause Activity
    void claDecayActivity();
    void claBumpActivity(int clause_idx);
    void reduceDB();               // Reduce the learnt clause database
    bool locked(int clause_idx);   // Check if clause is locked (reason for assignment)

    // Clause Minimization
    bool litRedundant(Lit p);

    // Restart helpers
    double luby(double y, int x);  // Calculate Luby sequence value

    // Utility Functions
    inline bool value(Var v) { return var_value[v]; }
    inline bool value(Lit p) { return var_value[var(p)] ^ sign(p); }
    void ensureVarCapacity(Var v);
    double drand(uint64_t& seed);                  // Random number generator
    int irand(uint64_t& seed, int size);           // Integer random in range [0,size-1]
    uint64_t getStatCount(Statistic<uint64_t>* stat);
    inline int nAssigns() const { return trail.size(); }
    inline int nLearnts() const { return clauses.size() - num_clauses; }
    inline bool isLearnt(int clause_idx) const { return clause_idx >= nLearnts(); }
    std::string printClause(const Clause& c);
    void loadDecisionSequence(const std::string& filename);  // user-defined decision sequence
    void dumpDecision(Lit lit);

private:
    // Structure for clause minimization
    struct ShrinkStackElem {
        size_t i;
        Lit l;
        ShrinkStackElem(size_t _i, Lit _l) : i(_i), l(_l) {}
    };
  
    // State Variables
    SolverState state, next_state;
    SST::Output output;
    SST::Interfaces::StandardMem* cnf_memory;    // For CNF data
    SST::Interfaces::StandardMem* global_memory; // For heap and variables operations
    std::string dimacs_content;
    SST::Cycle_t currentCycle;
    coro_t::pull_type* coroutine; // Coroutine for async operations
    coro_t::push_type* yield_ptr; // Pointer to current yield object
    int heap_resp;

    // Parsing state
    size_t filesize;
    uint32_t num_vars;
    uint32_t num_clauses;
    bool sort_clauses;
    std::vector<Lit> initial_units;     // Initial unit clauses from DIMACS
    
    // SAT solver state
    std::vector<Clause> clauses;
    std::vector<bool> var_assigned;     // Whether each variable is assigned
    std::vector<bool> var_value;        // Value of each variable
    
    // Implication graph
    uint qhead;
    std::vector<Lit> trail;            // Sequence of assignments in chronological order
    std::vector<uint> trail_lim;       // Indices in trail for the first literal at each decision level
    
    // Clause learning
    int conflict;                               // Conflict clause index from propagation
    std::vector<Lit> learnt_clause;             // Learnt clause from conflict analysis
    int bt_level;                               // Backtrack level from conflict analysis
    std::vector<char> seen;                     // Temporary array for conflict analysis
    int ccmin_mode;                             // Conflict clause minimization mode
    std::vector<ShrinkStackElem> analyze_stack; // Stack for clause minimization
    std::vector<Lit> analyze_toclear;           // Literals to clear after analysis

    // Two Watched Literals implementation
    std::vector<std::vector<Watcher>> watches;  // Indexed by literal encoding
    
    // Variable activity for VSIDS
    std::vector<double> activity;       // Activity score for each variable
    std::vector<bool> polarity;         // Saved phase (polarity) for each variable
    std::vector<bool> decision;         // Whether variable is eligible for decisions
    Heap* order_heap;                   // Heap of variables ordered by activity
    double var_inc;                     // Amount to bump variable activity by
    double var_decay;                   // Variable activity decay factor
    double random_var_freq;             // Frequency of random decisions
    uint64_t random_seed;               // Seed for random number generation
    SST::Link* heap_link;               // Link to async heap
    uint64_t heap_base_addr;            // Base address for heap memory
    uint64_t indices_base_addr;         // Base address for indices memory

    // Variables instance for asynchronous memory access to variable data
    Variables variables;
    uint64_t variables_base_addr;
    
    // Clause activity
    double clause_decay;
    double cla_inc;

    // DB reduction parameters
    double learntsize_factor;
    double learntsize_inc;
    double max_learnts;
    int learnt_adjust_start_confl;
    double learnt_adjust_inc;
    double learnt_adjust_confl;
    int learnt_adjust_cnt;

    // Restart parameters
    bool luby_restart;                  // Whether to use Luby sequence for restarts
    int restart_first;                  // Initial restart limit
    double restart_inc;                 // Factor to increase restart limit
    int curr_restarts;                  // Number of restarts performed
    int conflicts_until_restart;        // Number of conflicts to trigger next restart
    int conflictC;                      // Number of conflicts since last restart

    // Statistics
    Statistic<uint64_t>* stat_decisions;
    Statistic<uint64_t>* stat_propagations;
    Statistic<uint64_t>* stat_assigns;
    Statistic<uint64_t>* stat_unassigns;
    Statistic<uint64_t>* stat_conflicts;
    Statistic<uint64_t>* stat_learned;
    Statistic<uint64_t>* stat_removed;
    Statistic<uint64_t>* stat_db_reductions;
    Statistic<uint64_t>* stat_minimized_literals;
    Statistic<uint64_t>* stat_restarts;

    // User-defined decision sequence
    std::vector<std::pair<Var, bool>> decision_sequence; // (variable, sign) pairs
    size_t decision_seq_idx;                             // Current position in sequence
    bool has_decision_sequence;                          // Whether a decision sequence was provided
    // Decision output
    std::string decision_output_file;
    std::ofstream decision_output_stream;
};

#endif // SATSOLVER_H
