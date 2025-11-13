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
// Heap implementations (select via USE_PIPELINED_HEAP build flag)
#include "async_heap.h"         // classic external memory heap (Heap)
#include "pipelined_heap.h"     // pipelined heap (PipelinedHeap)
#include "async_variables.h"
#include "async_watches.h"
#include "async_clauses.h"
#include "async_activity.h"

//-----------------------------------------------------------------------------------
// Type Definitions and Constants
//-----------------------------------------------------------------------------------

// Unified state machine combining high-level states and detailed operations
enum SolverState { 
    IDLE,       // 0
    INIT,       // 1
    STEP,       // 2
    PROPAGATE,  // 3
    DECIDE,     // 4
    ANALYZE,    // 5
    MINIMIZE,   // 6
    BTLEVEL,    // 7
    BACKTRACK,  // 8
    REDUCE,     // 9
    RESTART,    // 10
    WAIT_HEAP,  // 11
    DONE,       // 12
};

//-----------------------------------------------------------------------------------
// Component Class
//-----------------------------------------------------------------------------------

class SATSolver : public SST::Component {

public:
    // SST ELI Registrations
    SST_ELI_REGISTER_COMPONENT(
        SATSolver,
        "satsolver-opt-final",
        "SATSolver-opt-final",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "SAT Solver Component",
        COMPONENT_CATEGORY_PROCESSOR
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"clock", "Clock frequency", "1GHz"},
        {"verbose", "Verbosity level", "0"},
        {"cnf_file", "Path to the CNF file to solve", ""},
        {"random_seed", "Random seed for decision making", "8888"},
        {"sort_clauses", "Sort clauses by activity", "true"},
        {"var_decay", "Variable activity decay factor", "0.95"},
        {"clause_decay", "Clause activity decay factor", "0.999"},
        {"random_var_freq", "Frequency of random decisions", "0.02"},
        {"decision_file", "Path to a file containing decision sequence", ""},
        {"decision_output_file", "Path to output decision sequence", ""},
        {"heap_base_addr", "Base address for heap memory", "0x00000000"},
        {"indices_base_addr", "Base address for indices memory", "0x10000000"},
        {"variables_base_addr", "Base address for variables memory", "0x20000000"},
        {"watches_base_addr", "Base address for watches memory", "0x30000000"},
        {"clauses_cmd_base_addr", "Base address for clauses command memory", "0x40000000"},
        {"clauses_data_base_addr", "Base address for clauses data memory", "0x50000000"},
        {"watch_nodes_base_addr", "Base address for watch nodes memory", "0x60000000"},
        {"var_act_base_addr", "Base address for variable activity memory", "0x70000000"},
        {"prefetch_enabled", "Enable prefetching", "false"},
        {"enable_speculative", "Enable speculative propagation", "false"},
        {"timeout_cycles", "Maximum solver cycles before timing out (0 = no timeout)", "0"}
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
        {"watcher_occ", "Number of watchers residing in watch lists", "count", 1},
        {"watcher_blocks", "Number of blocks visited during watcher insertions", "count", 1},
        {"para_watchers", "Number of watchers inspected per propagation", "count", 1},
        {"para_vars", "Number of variables processed per unitPropagate before conflict", "count", 1},
        {"spec_started", "Total literals started in speculative propagation", "count", 1},
        {"spec_finished", "Total literals finished in speculative propagation", "count", 1},
    )

    SST_ELI_DOCUMENT_PORTS(
        {"global_mem_link", "Connection to global memory", {"memHierarchy.MemEventBase"}},
        {"heap_port", "Link to external heap subcomponent", {"sst.Event"}},
        {"prefetch_port", "Port to send prefetch requests", {"SST::Event"}}
    )
    
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
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
    void parseDIMACS(const std::string& filename);
    
    // Core CDCL Algorithm
    void initialize();
    bool decide();
    void unitPropagate();
    void propagateLiteral(Lit p, int lit_worker_id, 
                          uint64_t& read_headptr_cycles, 
                          uint64_t& read_watcher_blocks_cycles,
                          uint64_t& read_clauses_cycles,
                          uint64_t& insert_watchers_cycles,
                          uint64_t& polling_cycles);
    void propagateWatchers(int watcher_i, Lit not_p, bool& block_modified, WatcherBlock& block, 
                           int lit_worker_id, int worker_id,
                           uint64_t& read_clauses_cycles, uint64_t& insert_watchers_cycles, 
                           uint64_t& polling_cycles);
    void analyze(Cref conflict, int worker_id = 0);
    void findBtLevel();
    void backtrack(int backtrack_level);
    
    // Trail Management
    void trailEnqueue(Lit literal, int reason = ClauseRef_Undef);
    void unassignVariable(Var var);
    int current_level() { return trail_lim.size(); }

    // Two-Watched Literals
    void attachClause(int clause_idx, const Clause& c);
    void detachClause(int clause_idx);
    
    // Decision Heuristics
    Lit chooseBranchVariable();
    Lit peekBranchVariable();
    void insertVarOrder(Var v);   // Insert variable into order heap
    void varDecayActivity();       // Decay all variable activities
    void varBumpActivity(Var v);   // Bump a variable's activity
    
    // Clause Activity
    void claDecayActivity();
    void claBumpActivity(Cref clause_addr, float act);
    void reduceDB();               // Reduce the learnt clause database
    bool locked(Cref clause_addr);   // Check if clause is locked (reason for assignment)

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
    inline int nAssigns() const { return trail.size(); }
    inline int nLearnts() const { return clauses.size() - num_clauses; }
    uint64_t getStatCount(Statistic<uint64_t>* stat);
    std::string printClause(const std::vector<Lit>& literals);
    void printHist(Statistic<uint64_t>* stat_hist);
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
    SolverState state, next_state, saved_state;
    SST::Output output;
    SST::Interfaces::StandardMem* global_memory; // For heap and variables operations
    std::string dimacs_content;
    SST::Cycle_t currentCycle;
    uint64_t timeout_cycles;           // timeout parameter, 0 means no timeout
    int heap_resp;

    // Parsing state
    std::string cnf_file_path;         // Path to CNF file
    uint32_t num_vars;
    uint32_t num_clauses;
    bool sort_clauses;
    std::vector<Lit> initial_units;             // Initial unit clauses from DIMACS
    std::vector<Clause> parsed_clauses;         // Temporary storage during parsing
    
    // SAT solver state
    Clauses clauses;                    // all clauses stored in external memory
    std::vector<bool> var_assigned;     // Whether each variable is assigned
    std::vector<bool> var_value;        // Value of each variable
    
    // Implication graph
    uint qhead;
    std::vector<Lit> trail;            // Sequence of assignments in chronological order
    std::vector<uint> trail_lim;       // Indices in trail for the first literal at each decision level
    
    // Clause learning
    std::vector<Cref> conflicts;                // Conflict clauses from propagation
    std::vector<Lit> learnt_clause;             // Learnt clause from conflict analysis
    int bt_level;                               // Backtrack level from conflict analysis
    std::vector<char> seen;                     // Temporary array for conflict analysis
    std::vector<Cref> c_to_bump;
    std::vector<Var> v_to_bump;

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
    double var_inc;                     // Amount to bump variable activity by
    double var_decay;                   // Variable activity decay factor
    double random_var_freq;             // Frequency of random decisions
    uint64_t random_seed;               // Seed for random number generation
    SST::Link* heap_link;               // Link to async heap
    uint64_t heap_base_addr;            // Base address for heap memory
    uint64_t indices_base_addr;         // Base address for indices memory
    uint64_t var_act_base_addr;         // Base address for variable activity array
    
    // external heap
    // Selected heap implementation pointer (pipelined by default)
#ifdef USE_CLASSIC_HEAP
    Heap* order_heap;                   // Classic external memory heap
#else
    PipelinedHeap* order_heap;          // Pipelined implementation
#endif
    bool in_decision;                // Whether the heap has been unstalled
    int heap_resp_cnt;                  // Number of unstalled heap responses to receive

    // external memory controller for struct Variable
    Variables variables;                // Replaces std::vector<Variable> variables
    uint64_t variables_base_addr;       // Base address for variables memory
    
    // Clause activity now stored in the Clause memory
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
    bool main_active;
    std::vector<bool> active_workers;               // Track completion of sub coroutines
    std::vector<bool> polling;                      // Track workers in polling state
    std::unordered_set<Cref> clause_locks;          // Track locked clauses during parallel propagation
    WatchListQueue wl_q;                            // Track locked watchlists during parallel propagation


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
    Statistic<uint64_t>* stat_watcher_occ;
    Statistic<uint64_t>* stat_watcher_blocks;
    Statistic<uint64_t>* stat_para_watchers;
    Statistic<uint64_t>* stat_para_vars;
    Statistic<uint64_t>* stat_spec_started;
    Statistic<uint64_t>* stat_spec_finished;

    // User-defined decision sequence
    std::vector<std::pair<Var, bool>> decision_sequence; // (variable, sign) pairs
    size_t decision_seq_idx;                             // Current position in sequence
    bool has_decision_sequence;                          // Whether a decision sequence was provided
    // Decision output
    std::string decision_output_file;
    std::ofstream decision_output_stream;

    // Cycle counters for performance profiling
    uint64_t cycles_propagate;
    uint64_t cycles_analyze;
    uint64_t cycles_minimize;
    uint64_t cycles_backtrack;   // includes BTLEVEL state
    uint64_t cycles_decision;
    uint64_t cycles_reduce;
    uint64_t cycles_restart;
    uint64_t cycles_heap_bump;
    uint64_t cycles_heap_insert;
    uint64_t total_cycles;
    
    // Cycle tracking
    SolverState prev_state;
    SST::Cycle_t last_state_change;

    // Prefetch support
    bool prefetch_enabled;
    SST::Link* prefetch_link;
    void issuePrefetch(uint64_t addr);

    // Propagation timing counters
    uint64_t cycles_read_headptr;        // Time spent reading head pointers
    uint64_t cycles_read_watcher_blocks; // Time spent reading watcher blocks
    uint64_t cycles_read_clauses;        // Time spent reading clauses in subPropagate
    uint64_t cycles_insert_watchers;     // Time spent inserting watchers
    uint64_t cycles_polling;             // Time spent polling for busy watchers

    // Structure for tracking propagation metrics
    struct PropagationMetrics {
        uint64_t read_headptr_cycles;
        uint64_t read_watcher_blocks_cycles;
        uint64_t read_clauses_cycles;
        uint64_t count;  // number of literals propagated

        PropagationMetrics() : read_headptr_cycles(0), read_watcher_blocks_cycles(0),
                               read_clauses_cycles(0), count(0) {}
    };

    PropagationMetrics normal_metrics;      // Non-speculative propagations
    PropagationMetrics speculative_metrics; // Speculative propagations that became actual
    std::vector<uint64_t> spec_prop_cache_lines; // Number of cache lines each spec propagation brought in

    // Speculative Propagation
    bool enable_speculative;
    Lit spec_literal;                    // Next decision literal for speculative propagation
    bool spec_active;                    // Whether speculative propagation is active
    std::vector<Lit> spec_trail;         // Temporary trail for speculative assignments
    std::vector<bool> spec_var_assigned; // Temporary assignments for speculative propagation
    std::vector<bool> spec_var_value;    // Temporary values for speculative propagation
    std::vector<bool> prev_spec_var_assigned; // Copy of spec_var_assigned before reset
    std::vector<bool> prev_spec_var_value;    // Copy of spec_var_value before reset
    std::vector<bool> spec_var_propagated;    // Track which variables have been speculatively PROPAGATED
    std::vector<bool> prev_spec_var_propagated; // Copy before reset
    int spec_conflicts;                  // Number of conflicts in speculative propagation
    coro_t::pull_type* spec_coroutine;   // Coroutine for speculative propagation
    coro_t::push_type* spec_yield_ptr;   // current yield pointer
    std::vector<bool> spec_active_workers; // Track completion of spec propagation workers
    std::vector<coro_t::pull_type*> spec_sub_coroutines;
    std::vector<coro_t::push_type*> spec_sub_yield_ptrs;

    // Speculative propagation methods
    void terminateSpecPropagate();
    void speculativePropagate();
    void resetSpecState();
    bool isSpecAssigned(Var v) const;
    bool getSpecValue(Var v) const;
    bool getSpecValue(Lit p) const;
};

#endif // SATSOLVER_H
