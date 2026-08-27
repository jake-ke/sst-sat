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
#include "activity_histogram.h"
#include "trace_writer.h"

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
        "satsolver",
        "SATSolver",
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
        {"act_mant", "Emulate reduced-mantissa activity ordering: var_inc advances as an exponent/mantissa code with this many mantissa bits, half-life pinned at 16 conflicts (0 = off, min 4; pipelined heap only, pair with the heap's act_mant)", "0"},
        {"clause_decay", "Clause activity decay factor", "0.999"},
        {"random_var_freq", "Frequency of random decisions", "0.02"},
        {"decision_file", "Path to a file containing decision sequence", ""},
        {"decision_output_file", "Path to output decision sequence", ""},
        {"heap_base_addr", "Base address for heap memory", "0x00000000"},
        {"indices_base_addr", "Base address for indices memory", "0x08000000"},
        {"variables_base_addr", "Base address for variables memory", "0x10000000"},
        {"watches_base_addr", "Base address for watches memory", "0x30000000"},
        {"watch_nodes_base_addr", "Base address for watch nodes memory", "0xC0000000"},
        {"clauses_cmd_base_addr", "Base address for clauses command memory", "0x100000000"},
        {"clauses_base_addr", "Base address for clauses data memory", "0x140000000"},
        {"var_act_base_addr", "Base address for variable activity memory", "0x1C0000000"},
        {"prefetch_enabled", "Enable prefetching", "false"},
        {"enable_speculative", "Enable speculative propagation", "false"},
        {"timeout_cycles", "Maximum solver cycles before timing out (0 = no timeout)", "0"},
        {"max_confl", "Maximum number of conflicts collected per propagation and analyzed (in batches of LEARNERS) per conflict round; -1 = no limit", "8"},
        {"gmc1", "Enable Guarded Multi-Commit +1: on rounds where the harvested conflicts disagree on backtrack level, also learn the lowest-LBD non-winner clause without enqueueing it (default: stock single-commit)", "false"},
        {"profile_2wl", "Enable 2WL clause-access reduction profiling (host-side; counts only original clauses)", "false"},
        {"profile_prop_timing", "Enable per-propagation timing breakdown (cycles_read_headptr/blocks/clauses/insert/polling and spec/normal metrics). Auto-enabled when enable_speculative=true.", "false"},
        {"trace_file", "Path to binary memory-access trace. Empty disables tracing.", ""},
        {"trace_buffer_bytes", "Ring buffer size for trace writer (bytes).", "4194304"},
        {"heap_dist_interval", "Heap activity distribution snapshot interval in conflicts (0 = off; pipelined heap only, host-side measurement with no timing impact)", "0"},
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
        {"midsearch_rebuilds", "Heap rebuilds triggered between restarts by the stale threshold", "count", 1},
        {"rescale_forced_rebuilds", "Heap rebuilds where the rescale runway backstop (E >= 900) was the deciding trigger (staleness alone would not have fired)", "count", 1},
        {"watcher_occ", "Number of watchers residing in watch lists", "count", 1},
        {"watcher_blocks", "Number of blocks visited during watcher insertions", "count", 1},
        {"para_watchers", "Number of watchers inspected per propagation", "count", 1},
        {"para_vars", "Number of variables processed per unitPropagate before conflict", "count", 1},
        {"spec_started", "Total literals started in speculative propagation", "count", 1},
        {"spec_finished", "Total literals finished in speculative propagation", "count", 1},
        {"total_occ", "Sum of occurrence list sizes per propagation", "count", 1},
        {"watcher_traversed", "Sum of watchers traversed per propagation", "count", 1},
        {"learnt_length", "Total length of learnt clauses", "count", 1},
        {"learnt_units", "Number of unit-literal learnt clauses", "count", 1},
        {"learnt_lbd", "Total LBD of learnt clauses", "count", 1},
        {"bt_level", "Total backtrack level", "count", 1},
        {"multi_confl_rounds", "Number of conflict rounds that collected more than one conflict", "count", 1},
        {"bt_level_diff", "Number of multi-conflict rounds whose conflicts disagree on the backtrack level (min < max)", "count", 1},
        {"gmc_extra_learnts", "Number of extra learnt clauses added by gmc1 (one per disagreeing round that has a valid non-winner)", "count", 1},
        {"clause_lock_occ", "Clause-lock table occupancy sampled at each acquire (concurrent locked clauses)", "count", 1},
        {"busy_occ", "Watchlist write-lock occupancy sampled at each insert (concurrent watchlist insertions)", "count", 1},
        {"wl_q_occ", "Total pending watchlist insertions in flight, sampled at each enqueue", "count", 1},
        {"blocked_workers", "Workers simultaneously blocked on a propagation lock, sampled per scheduler round", "count", 1},
        {"clause_conflicts", "Number of stall episodes on a locked clause", "count", 1},
        {"wl_insert_conflicts", "Number of stall episodes on a busy watchlist (insertion)", "count", 1},
        {"wl_process_conflicts", "Number of stall episodes on pending watchlist insertions (processing)", "count", 1},
        {"stalled_per_cycle", "Time-weighted blocked workers (accumulator: Sum=blocked-worker-cycles, Count=cycles; Mean=avg stalled workers per cycle)", "count", 1},
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
    void handleReduceScan(SST::Event* ev);

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
    
    // Decision Heuristics
    Lit chooseBranchVariable();
    Lit peekBranchVariable();
    void insertVarOrder(Var v);   // Insert variable into order heap
    bool heapRebuildDue() const;  // Stale pile worth a rebuild (off-chip only)
    void fireHeapRebuild();       // Wipe + reinsert-unassigned wave
    void varDecayActivity();       // Decay all variable activities
    void varBumpActivity(Var v);   // Bump a variable's activity
    
    // Clause Activity
    void claDecayActivity();
    // Bump a learnt clause's activity given its CURRENT activity (already in
    // hand from the analyze traversal — no clause re-read). Returns true when
    // the bump triggered a global activity rescale, so the caller can rescale
    // any remembered activities it still holds.
    bool claBumpActivity(Cref clause_addr, float act);
    void reduceDB();                 // Reduce the learnt clause database
    void rescaleAllActivities();     // Pipelined x2^-66 sweep over all learnt activities

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
    int learnt_lbd;                             // LBD of learnt clause from conflict analysis
    int round_max_bt;                           // Max backtrack level among conflicts analyzed this round (-1 = none yet)
    std::vector<char> seen;                     // Temporary array for conflict analysis
    // Clauses to bump with their CURRENT activity, captured while the analyze
    // traversal already holds the clause (saves a full re-read per bump).
    std::vector<std::pair<Cref, float>> c_to_bump;
    std::vector<Var> v_to_bump;

    // Guarded multi-commit (gmc1). When enabled, every conflict analyzed in a
    // round records its learnt clause so that — only on rounds whose conflicts
    // disagree on the backtrack level — the lowest-LBD non-winner can be added
    // to the clause DB as one extra learnt (attached + clause-bumped, but not
    // enqueued). bt_pos is the index of a max-earlier-level literal, placed at
    // watch position 1 when the extra clause is committed.
    bool gmc1_enabled = false;                  // gmc1 param
    struct AnalyzeCand {
        int bt_level;
        int lbd;
        int bt_pos;
        std::vector<Lit> learnt;
    };
    std::vector<AnalyzeCand> round_cands;       // one per conflict analyzed this round
    int winner_cand_idx = -1;                   // index in round_cands of committed winner
    std::vector<Lit> extra_learnt;              // prepared extra clause (empty = none this round)
    int extra_lbd = 0;                          // LBD of the extra clause
    int extra_bt = 0;                           // backtrack level of the extra clause

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
    int act_mant;                       // act_mant param: mantissa bits (0 = off)
    uint64_t var_inc_code;              // act_mant: var_inc as [exponent | mantissa] code
    // var_inc's current binary exponent E — the code's signed exponent field
    // under act_mant, ilogb of the double otherwise. Drives the rebuild-folded
    // rescale thresholds (arm at 708, backstop at 900, ceiling assert at
    // 1015); identical constants for both representations.
    int64_t incExp() const {
        return act_mant ? ((int64_t)var_inc_code >> act_mant)
                        : (int64_t)std::ilogb(var_inc);
    }
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
    bool suppress_heap_inserts_;        // Rebuild-restart: skip trail-unwind inserts
    uint64_t heap_dist_interval;        // heap_dist_interval param (0 = off)
    uint64_t heap_dist_ctr_;            // conflicts since the last dist snapshot

    // external memory controller for struct Variable
    Variables variables;                // Replaces std::vector<Variable> variables
    uint64_t variables_base_addr;       // Base address for variables memory
    
    // Clause activity now stored in the Clause memory
    double clause_decay;
    double cla_inc;

    // On-chip running histogram of learnt non-binary clause activities
    // (median oracle for reduceDB; see activity_histogram.h).
    ActivityHistogram cla_hist_;
    SST::Link* reduce_scan_link_;       // Self-link modeling the serial prefix scan
    bool scan_done_;                    // Reduce commits gate on this

    // Memory addresses
    uint64_t clauses_base_addr;         // Base address for clauses
    uint64_t clauses_cmd_base_addr;  // Base address for clause offsets

    // DB reduction parameters
    double learntsize_factor;
    double learntsize_inc;
    double max_learnts;
    int max_confl;                              // Max conflicts collected/analyzed per round (-1 = no limit)
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

    // Glucose-style restart parameters
    bool glucose_restart;               // Whether to use glucose-style LBD-based restarts
    double lbd_ema_fast;                // Fast-moving EMA of recent LBD values
    double lbd_ema_slow;                // Slow-moving EMA of LBD values
    double lbd_ema_fast_alpha;          // EMA smoothing factor for fast
    double lbd_ema_slow_alpha;          // EMA smoothing factor for slow
    int glucose_min_conflicts;          // Min conflicts before glucose restarts kick in

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
    Statistic<uint64_t>* stat_midsearch_rebuilds;
    Statistic<uint64_t>* stat_rescale_forced_rebuilds;
    Statistic<uint64_t>* stat_watcher_occ;
    Statistic<uint64_t>* stat_watcher_blocks;
    Statistic<uint64_t>* stat_para_watchers;
    Statistic<uint64_t>* stat_para_vars;
    Statistic<uint64_t>* stat_spec_started;
    Statistic<uint64_t>* stat_spec_finished;
    Statistic<uint64_t>* stat_total_occ;          // Accumulator: sum of occurrence list sizes per propagation
    Statistic<uint64_t>* stat_watcher_traversed;  // Accumulator: sum of watchers traversed per propagation
    Statistic<uint64_t>* stat_learnt_length;      // Accumulator: total length of learnt clauses
    Statistic<uint64_t>* stat_learnt_units;       // Count of unit-literal learnt clauses
    Statistic<uint64_t>* stat_learnt_lbd;         // Accumulator: total LBD of learnt clauses
    Statistic<uint64_t>* stat_bt_level;           // Accumulator: total backtrack level (destination level)
    Statistic<uint64_t>* stat_bt_distance;        // Accumulator: total backtrack distance (levels jumped)
    Statistic<uint64_t>* stat_multi_confl_rounds; // Count of rounds that collected >1 conflict
    Statistic<uint64_t>* stat_bt_level_diff;      // Count of multi-conflict rounds whose conflicts disagree on bt level
    Statistic<uint64_t>* stat_gmc_extra_learnts;  // Count of extra learnt clauses added by gmc1

    // Propagation synchronization-sizing statistics
    Statistic<uint64_t>* stat_clause_lock_occ;      // Histogram: clause-lock table occupancy at acquire
    Statistic<uint64_t>* stat_busy_occ;             // Histogram: watchlist write-lock occupancy at insert
    Statistic<uint64_t>* stat_wl_q_occ;             // Histogram: total pending watchlist insertions
    Statistic<uint64_t>* stat_blocked_workers;      // Histogram: workers blocked on a lock per scheduler round
    Statistic<uint64_t>* stat_clause_conflicts;     // Accumulator: clause-lock stall episodes
    Statistic<uint64_t>* stat_wl_insert_conflicts;  // Accumulator: watchlist-insert stall episodes
    Statistic<uint64_t>* stat_wl_process_conflicts; // Accumulator: watchlist-process stall episodes
    Statistic<uint64_t>* stat_stalled_per_cycle;    // Accumulator: time-weighted blocked workers (Mean = avg stalled/cycle)
    bool track_stalls = false;                      // True when blocked_workers/stalled_per_cycle is enabled (gates per-round sum)
    uint64_t last_round_cycle = 0;                  // sim cycle at last blocked-worker sample
    uint64_t last_blocked = 0;                      // blocked-worker count at last sample

    std::vector<uint32_t> lit_occ_count;          // Precomputed occurrence count per literal index

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

    // Coprocessor raw statistics for offline cost computation (always collected)
    uint64_t coproc_sf_hw_learning;   // sum(elapsed * sf) for ANALYZE+BTLEVEL
    uint64_t coproc_sf_hw_minimize;   // sum(elapsed * sf) for MINIMIZE
    uint64_t coproc_dep_decision;     // total dependent accesses for DECIDE
    uint64_t coproc_dep_learning;     // total dependent accesses for ANALYZE+BTLEVEL
    uint64_t coproc_dep_minimize;     // total dependent accesses for MINIMIZE
    uint64_t coproc_dep_backtrack;    // total dependent accesses for BACKTRACK+RESTART
    // Trail snapshots (saved before BACKTRACK/RESTART modify trail)
    uint64_t saved_trail_size;
    uint64_t saved_bt_level;

    // Cycle tracking
    SolverState prev_state;
    SST::Cycle_t last_state_change;
    uint64_t progress_countdown_ = 100000;  // ticks until next progress verbose

    // Prefetch support
    bool prefetch_enabled;
    SST::Link* prefetch_link;
    void issuePrefetch(uint64_t addr);

    // 2WL clause-access reduction profiling (host-side, no simulated memory access)
    bool profile_2wl;

    // Per-propagation timing breakdown (gates cycles_read_* counters and
    // speculative_metrics/normal_metrics structs). Auto-enabled when
    // enable_speculative is on.
    bool profile_prop_timing;

    // Binary memory-access trace writer (nullptr when tracing disabled).
    TraceWriter* tracer_ = nullptr;
    uint8_t tracer_phase_cache_ = 0;
    int32_t tracer_level_cache_ = -1;
    uint64_t tracer_events_at_tick_start_ = 0;

    // Propagation timing counters
    uint64_t cycles_read_headptr;        // Time spent reading head pointers
    uint64_t cycles_read_watcher_blocks; // Time spent reading watcher blocks
    uint64_t cycles_read_clauses;        // Time spent reading clauses in subPropagate
    uint64_t cycles_insert_watchers;     // Time spent inserting watchers
    uint64_t cycles_polling;             // Time spent polling for busy watchers
    uint64_t inserts_in_flight;          // Concurrent insertWatcher calls (mirrors Watches::busy occupancy)

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
