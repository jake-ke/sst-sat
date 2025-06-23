#ifndef SATSOLVER_H
#define SATSOLVER_H

#include <sst/core/component.h>
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <vector>
#include <string>
#include <fstream>    // For reading decision file
#include <boost/coroutine2/all.hpp>
#include "structs.h"
#include "async_heap.h"
#include "async_variables.h"
#include "async_watches.h"
#include "async_clauses.h"
#include "async_activity.h"

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
    MINIMIZE,
    BTLEVEL,
    BACKTRACK,
    REDUCE,
    RESTART,
    DONE 
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
        {"cnf_file", "Path to the CNF file to solve", ""},
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
        {"var_act_base_addr", "Base address for variable activity memory", "0x70000000"},
        {"clause_act_base_addr", "Base address for clause activity memory", "0x80000000"},
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
    void execMinimize();
    void execBtLevel();
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
    void subPropagate(int i, Lit not_p, bool& block_modified, WatcherBlock& block, int worker_id);
    void analyze();
    void findBtLevel();
    void backtrack(int backtrack_level);
    
    // Trail Management
    void trailEnqueue(Lit literal, int reason = ClauseRef_Undef);
    void unassignVariable(Var var);
    int current_level() { return trail_lim.size(); }

    // Two-Watched Literals
    void attachClause(int clause_idx);
    void detachClause(int clause_idx);
    
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
    void minimizeL2_sub(std::vector<bool>& redundant, int worker_id = 0);  // coroutine function
    bool litRedundant(Lit p, int worker_id = 0);  // Check if literal can be removed

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
    inline bool isLearnt(int clause_idx) const { return clause_idx >= num_clauses; }
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
    int heap_resp;

    // Parsing state
    std::string cnf_file_path;         // Path to CNF file
    size_t filesize;
    uint32_t num_vars;
    uint32_t num_clauses;
    bool sort_clauses;
    std::vector<Lit> initial_units;             // Initial unit clauses from DIMACS
    std::vector<Clause> parsed_clauses;         // Temporary storage during parsing
    
    // SAT solver state
    Clauses clauses;                    // Replaces std::vector<Clause> clauses
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

    // Clause minimization
    int ccmin_mode;                             // Conflict clause minimization mode
    std::vector<Lit> analyze_toclear;           // Literals to clear after analysis

    // Two Watched Literals implementation
    Watches watches;  // Replaces std::vector<std::vector<Watcher>> watches
    uint64_t watches_base_addr;      // Base address for watches array
    uint64_t watch_nodes_base_addr;  // Base address for watch nodes
    
    // Variable related
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
    uint64_t var_act_base_addr;         // Base address for variable activity array

    // external memory controller for struct Variable
    Variables variables;                // Replaces std::vector<Variable> variables
    uint64_t variables_base_addr;       // Base address for variables memory
    
    // Clause activity
    Activity cla_activity;              // Replace std::vector<double> cla_activity
    uint64_t clause_act_base_addr;      // Base address for clause activity
    double clause_decay;
    double cla_inc;

    // Memory addresses
    uint64_t clauses_base_addr;         // Base address for clauses
    uint64_t clauses_cmd_base_addr;  // Base address for clause offsets

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

    // simulating parallel execution support
    ReorderBuffer reorder_buffer;                   // Reorder buffer for managing parallel read requests
    coro_t::pull_type* coroutine;                   // coroutine in the top level FSM
    coro_t::push_type* yield_ptr;                   // current yield pointer
    coro_t::push_type* parent_yield_ptr;            // saves parent (fsm) yield pointer
    std::vector<coro_t::pull_type*> coroutines;     // sub coroutines for parallel tasks
    std::vector<coro_t::push_type*> yield_ptrs;     // yield pointers for parallel tasks
    std::vector<bool> active_workers;               // Track completion of sub coroutines
    std::vector<bool> polling;


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
