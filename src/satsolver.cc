#include <sst/core/sst_config.h> // This include is REQUIRED for all implementation files
#include "satsolver.h"
#include <sst/core/interfaces/stdMem.h>
#include "sst/core/statapi/stathistogram.h"
#include "sst/core/statapi/stataccumulator.h"
#include <algorithm>  // For std::sort
#include <cmath>      // For pow function
#include <deque>      // For reduceDB dispatch/free queues
#include <fstream>    // For file reading
#include "directedprefetch.h" // Include for PrefetchRequestEvent
#include <sst/core/realtimeAction.h>  // For current simulation time

//-----------------------------------------------------------------------------------
// Component Lifecycle Methods
//-----------------------------------------------------------------------------------

SATSolver::SATSolver(SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id), 
    state(IDLE),
    var_inc(1.0),
    cla_inc(1.0),
    learntsize_factor((double)1/(double)3),
    learntsize_inc(1.1),
    learnt_adjust_start_confl(100),
    learnt_adjust_inc(1.5),
    ccmin_mode(2),
    decision_seq_idx(0),
    has_decision_sequence(false),
    luby_restart(true),
    restart_first(100),
    restart_inc(2.0),
    curr_restarts(0),
    conflicts_until_restart(restart_first),
    conflictC(0),
    glucose_restart(false),
    lbd_ema_fast(0.0),
    lbd_ema_slow(0.0),
    lbd_ema_fast_alpha(1.0 / 50.0),
    lbd_ema_slow_alpha(1.0 / 5000.0),
    glucose_min_conflicts(100),
    yield_ptr(nullptr),
    // Initialize cycle counters
    cycles_propagate(0),
    cycles_analyze(0),
    cycles_minimize(0),
    cycles_backtrack(0),
    cycles_decision(0),
    cycles_reduce(0),
    cycles_restart(0),
    cycles_heap_bump(0),
    cycles_heap_insert(0),
    // Initialize coprocessor raw statistics
    coproc_sf_hw_learning(0),
    coproc_sf_hw_minimize(0),
    coproc_dep_decision(0),
    coproc_dep_learning(0),
    coproc_dep_minimize(0),
    coproc_dep_backtrack(0),
    saved_trail_size(0),
    saved_bt_level(0),
    // Initialize cycle tracking
    prev_state(IDLE),
    last_state_change(0),
    // Initialize propagation timing counters
    cycles_read_headptr(0),
    cycles_read_watcher_blocks(0),
    cycles_read_clauses(0),
    cycles_insert_watchers(0),
    cycles_polling(0),
    inserts_in_flight(0),
    main_active(false),
    spec_literal(lit_Undef),
    spec_active(false),
    spec_conflicts(0),
    spec_coroutine(nullptr),
    spec_yield_ptr(nullptr) {
    
    // Initialize output
    int verbose = params.find<int>("verbose", 0);
    output.init("MAIN-> ",verbose, 0, SST::Output::STDOUT);

    // Configure clock
    SST::TimeConverter* clock_tc =
        registerClock(params.find<std::string>("clock", "1GHz"),
                  new SST::Clock::Handler2<SATSolver, &SATSolver::clockTick>(this));

    // Self-link modeling the serial on-chip histogram prefix scan at reduce
    // time (one cycle per bucket; commits gate on its arrival).
    reduce_scan_link_ = configureSelfLink("reduce_scan", *clock_tc,
        new SST::Event::Handler2<SATSolver, &SATSolver::handleReduceScan>(this));
    scan_done_ = true;

    // Print build-time/static configuration from structs.h
    output.output("==================[ SATSolver Configuration (structs.h) ]==================\n");
    output.output("PARA_LITS           : %d\n", PARA_LITS);
    output.output("PROPAGATORS         : %d\n", PROPAGATORS);
    output.output("LEARNERS            : %d\n", LEARNERS);
    output.output("HEAPLANES           : %d\n", HEAPLANES);
    output.output("MINIMIZERS          : %d\n", MINIMIZERS);
    output.output("REDUCE_WORKERS      : %d\n", REDUCE_WORKERS);
    output.output("OVERLAP_HEAP_INSERT : %s\n", OVERLAP_HEAP_INSERT ? "true" : "false");
    output.output("OVERLAP_HEAP_BUMP   : %s\n", OVERLAP_HEAP_BUMP ? "true" : "false");
    output.output("WRITE_BUFFER        : %s\n", WRITE_BUFFER ? "true" : "false");
    output.output("PRE_WATCHERS        : %d\n", PRE_WATCHERS);
    output.output("USE_FREE_LIST       : %d\n", USE_FREE_LIST);
    output.output("FREE_IDX_BITS       : %d\n", FREE_IDX_BITS);
    output.output("=============================================================================\n");

    // Get CNF file path
    cnf_file_path = params.find<std::string>("cnf_file", "");
    if (cnf_file_path.empty()) {
        output.fatal(CALL_INFO, -1, "CNF file path not provided\n");
    }

    random_seed = params.find<uint64_t>("random_seed", 8888);
    sort_clauses = params.find<bool>("sort_clauses", true);

    // Initialize activity-related variables
    var_decay = params.find<double>("var_decay", 0.95);
    clause_decay = params.find<double>("clause_decay", 0.999);  // Add clause decay parameter
    random_var_freq = params.find<double>("random_var_freq", 0.0);
    
    // Get heap memory addresses (defaults mirror the 8GiB map in tests/test_two_level.py;
    // region ORDER must stay heap < indices < variables < watches < watch_nodes <
    // clauses_cmd < clauses < var_act — the >= routing cascades depend on it)
    heap_base_addr = std::stoull(params.find<std::string>("heap_base_addr", "0x00000000"), nullptr, 0);
    indices_base_addr = std::stoull(params.find<std::string>("indices_base_addr", "0x08000000"), nullptr, 0);
    variables_base_addr = std::stoull(params.find<std::string>("variables_base_addr", "0x10000000"), nullptr, 0);
    watches_base_addr = std::stoull(params.find<std::string>("watches_base_addr", "0x30000000"), nullptr, 0);
    watch_nodes_base_addr = std::stoull(params.find<std::string>("watch_nodes_base_addr", "0xC0000000"), nullptr, 0);
    clauses_cmd_base_addr = std::stoull(params.find<std::string>("clauses_cmd_base_addr", "0x100000000"), nullptr, 0);
    clauses_base_addr = std::stoull(params.find<std::string>("clauses_base_addr", "0x140000000"), nullptr, 0);
    var_act_base_addr = std::stoull(params.find<std::string>("var_act_base_addr", "0x1C0000000"), nullptr, 0);
    
    // Coprocessor mode

    // Load decision sequence if provided
    std::string decision_file = params.find<std::string>("decision_file", "");
    if (!decision_file.empty()) {
        output.verbose(CALL_INFO, 1, 0, "Loading decision sequence from %s\n", decision_file.c_str());
        loadDecisionSequence(decision_file);
        has_decision_sequence = true;
    }

    // Configure global memory interface for heap and variables
    global_memory = loadUserSubComponent<SST::Interfaces::StandardMem>(
        "global_memory", 
        SST::ComponentInfo::SHARE_NONE,
        getTimeConverter("1GHz"),  // Time base for memory interface
        new SST::Interfaces::StandardMem::Handler2<SATSolver, &SATSolver::handleGlobalMemEvent>(this)
    );

    if (!global_memory) {
        output.fatal(CALL_INFO, -1, "Unable to load StandardMem SubComponent for global memory\n");
    }

    // Create Variables object by passing point of yield_ptr
    variables = Variables(verbose, global_memory, variables_base_addr, &yield_ptr);
    variables.setReorderBuffer(&reorder_buffer);
    
    // Create Watches object. Node addresses are packed into uint32 fields
    // (head_ptr/free_head/next_block), so the node region is bounded by the
    // next region base or the 4GiB pointer limit, whichever is lower.
    uint64_t watch_nodes_region_end = std::min(clauses_cmd_base_addr, (uint64_t)1 << 32);
    watches = Watches(verbose, global_memory, watches_base_addr, watch_nodes_base_addr,
                      &yield_ptr, watch_nodes_region_end);
    watches.setReorderBuffer(&reorder_buffer);

    // Create Clauses object. The allocator manages [clauses_base, var_act_base),
    // capped below 2GiB because Cref offsets are signed 32-bit.
    uint64_t clauses_region_size = std::min(var_act_base_addr - clauses_base_addr,
                                            (uint64_t)0x7FFFFFF0);
    clauses = Clauses(verbose, global_memory, clauses_cmd_base_addr, clauses_base_addr,
                      &yield_ptr, clauses_region_size);
    clauses.setReorderBuffer(&reorder_buffer);
    
    // Load the selected heap subcomponent depending on build flag
#ifdef USE_CLASSIC_HEAP
    order_heap = loadUserSubComponent<Heap>("order_heap",
        SST::ComponentInfo::SHARE_PORTS | SST::ComponentInfo::SHARE_STATS,
        global_memory, heap_base_addr, indices_base_addr);
#else
    // The pipelined heap owns its memory interface (dedicated activity
    // cache); it reads var_act_base_addr from its own params.
    order_heap = loadUserSubComponent<PipelinedHeap>("order_heap",
        SST::ComponentInfo::SHARE_PORTS | SST::ComponentInfo::SHARE_STATS);
    if (order_heap) order_heap->setAssignedFlagsRef(&var_assigned);
#endif
    sst_assert(order_heap != nullptr, CALL_INFO, -1, "Unable to load Heap subcomponent\n");
    in_decision = false;
    heap_resp_cnt = 0;
    suppress_heap_inserts_ = false;

    // Configure the link to the heap subcomponent
    heap_link = configureLink("heap_port", 
        new SST::Event::Handler2<SATSolver, &SATSolver::handleHeapResponse>(this));
    sst_assert( heap_link != nullptr, CALL_INFO, -1, "Error: 'heap_port' is not connected to a link\n");

    prefetch_enabled = params.find<bool>("prefetch_enabled", false);
    if (prefetch_enabled) {
        prefetch_link = configureLink("prefetch_port");
        sst_assert(prefetch_link != nullptr, CALL_INFO, -1, "Error: 'prefetch_port' is not connected to a link\n");
    }

    enable_speculative = params.find<bool>("enable_speculative", false);
    timeout_cycles = params.find<uint64_t>("timeout_cycles", 0);
    max_confl = params.find<int>("max_confl", 8);
    output.output("MAX_CONFL           : %d\n", max_confl);
    profile_2wl = params.find<bool>("profile_2wl", false);
    profile_prop_timing = params.find<bool>("profile_prop_timing", false);
    // Speculative profiling depends on the same per-literal cycle data, so
    // force the timing breakdown on whenever speculation is enabled.
    if (enable_speculative) profile_prop_timing = true;
    if (profile_prop_timing) {
        output.output("Per-propagation timing breakdown enabled\n");
    }
    glucose_restart = params.find<bool>("glucose_restart", false);
    if (glucose_restart) {
        output.output("Glucose-style LBD-based restarts enabled\n");
    }

    // Open decision output file if specified
    std::string decision_output_file = params.find<std::string>("decision_output_file", "");
    if (!decision_output_file.empty()) {
        decision_output_stream.open(decision_output_file);
        if (!decision_output_stream.is_open()) {
            output.fatal(CALL_INFO, -1, "Could not open decision output file: %s\n", decision_output_file.c_str());
        }
        // Write header
        decision_output_stream << "# Decision sequence generated by SATSolver\n";
        decision_output_stream << "# Format: <var> <value> (where value is 0 for false, 1 for true)\n";
    }

    // Register statistics
    stat_decisions = registerStatistic<uint64_t>("decisions");
    stat_propagations = registerStatistic<uint64_t>("propagations");
    stat_assigns = registerStatistic<uint64_t>("assigns");
    stat_unassigns = registerStatistic<uint64_t>("unassigns");
    stat_conflicts = registerStatistic<uint64_t>("conflicts");
    stat_learned = registerStatistic<uint64_t>("learned");
    stat_removed = registerStatistic<uint64_t>("removed");
    stat_db_reductions = registerStatistic<uint64_t>("db_reductions");
    stat_minimized_literals = registerStatistic<uint64_t>("minimized_literals");
    stat_restarts = registerStatistic<uint64_t>("restarts");
    stat_midsearch_rebuilds = registerStatistic<uint64_t>("midsearch_rebuilds");
    stat_watcher_occ = registerStatistic<uint64_t>("watcher_occ");
    stat_watcher_blocks = registerStatistic<uint64_t>("watcher_blocks");
    stat_para_watchers = registerStatistic<uint64_t>("para_watchers");
    stat_para_vars = registerStatistic<uint64_t>("para_vars");
    stat_spec_started = registerStatistic<uint64_t>("spec_started");
    stat_spec_finished = registerStatistic<uint64_t>("spec_finished");
    stat_total_occ = registerStatistic<uint64_t>("total_occ");
    stat_watcher_traversed = registerStatistic<uint64_t>("watcher_traversed");
    stat_learnt_length = registerStatistic<uint64_t>("learnt_length");
    stat_learnt_units = registerStatistic<uint64_t>("learnt_units");
    stat_learnt_lbd = registerStatistic<uint64_t>("learnt_lbd");
    stat_bt_level = registerStatistic<uint64_t>("bt_level");
    stat_bt_distance = registerStatistic<uint64_t>("bt_distance");
    stat_multi_confl_rounds = registerStatistic<uint64_t>("multi_confl_rounds");
    stat_bt_level_diff = registerStatistic<uint64_t>("bt_level_diff");

    // Propagation synchronization-sizing statistics
    stat_clause_lock_occ = registerStatistic<uint64_t>("clause_lock_occ");
    stat_busy_occ = registerStatistic<uint64_t>("busy_occ");
    stat_wl_q_occ = registerStatistic<uint64_t>("wl_q_occ");
    stat_blocked_workers = registerStatistic<uint64_t>("blocked_workers");
    stat_clause_conflicts = registerStatistic<uint64_t>("clause_conflicts");
    stat_wl_insert_conflicts = registerStatistic<uint64_t>("wl_insert_conflicts");
    stat_wl_process_conflicts = registerStatistic<uint64_t>("wl_process_conflicts");
    stat_stalled_per_cycle = registerStatistic<uint64_t>("stalled_per_cycle");
    // Gate the per-scheduler-round blocked-worker sum so disabled runs pay nothing.
    track_stalls = !stat_blocked_workers->isNullStatistic()
                || !stat_stalled_per_cycle->isNullStatistic();

    // Binary memory-access trace writer (opt-in).
    std::string trace_file = params.find<std::string>("trace_file", "");
    if (!trace_file.empty()) {
        tracer_ = new TraceWriter();
        size_t trace_buf = params.find<size_t>("trace_buffer_bytes", 4*1024*1024);
        if (!tracer_->open(trace_file, trace_buf)) {
            output.fatal(CALL_INFO, -1, "trace_file: could not open %s\n", trace_file.c_str());
        }
        // The DsMap classifies addresses by bits 28-31 only, which requires all
        // eight bases to live in distinct 256MiB slots below 4GiB. The 8GiB map
        // violates that (e.g. clauses_cmd at 0x100000000 aliases nibble 0), so
        // refuse to trace rather than emit silently misclassified events.
        const uint64_t bases[8] = { heap_base_addr, indices_base_addr, variables_base_addr,
                                    watches_base_addr, watch_nodes_base_addr,
                                    clauses_cmd_base_addr, clauses_base_addr, var_act_base_addr };
        uint16_t nibble_seen = 0;
        for (int bi = 0; bi < 8; bi++) {
            uint16_t bit = 1u << ((bases[bi] >> 28) & 0xF);
            if (bases[bi] >= ((uint64_t)1 << 32) || (nibble_seen & bit)) {
                output.fatal(CALL_INFO, -1,
                    "trace_file: DsMap needs distinct 256MiB-aligned region bases below "
                    "4GiB; base 0x%lx does not qualify. Tracing is unsupported with this "
                    "address map.\n", bases[bi]);
            }
            nibble_seen |= bit;
        }
        TraceWriter::DsMap m;
        m.nibble[(heap_base_addr        >> 28) & 0xF] = TraceWriter::DS_HEAP;
        m.nibble[(indices_base_addr     >> 28) & 0xF] = TraceWriter::DS_INDICES;
        m.nibble[(variables_base_addr   >> 28) & 0xF] = TraceWriter::DS_VARIABLES;
        m.nibble[(watches_base_addr     >> 28) & 0xF] = TraceWriter::DS_WATCHES;
        m.nibble[(watch_nodes_base_addr >> 28) & 0xF] = TraceWriter::DS_WATCH_NODES;
        m.nibble[(clauses_cmd_base_addr >> 28) & 0xF] = TraceWriter::DS_CLAUSES_CMD;
        m.nibble[(clauses_base_addr     >> 28) & 0xF] = TraceWriter::DS_CLAUSES;
        m.nibble[(var_act_base_addr     >> 28) & 0xF] = TraceWriter::DS_VAR_ACT;
        tracer_->setDsMap(m);

        variables.setTracer(tracer_, TraceWriter::DS_VARIABLES);
        watches  .setTracer(tracer_, TraceWriter::DS_WATCHES);
        clauses  .setTracer(tracer_, TraceWriter::DS_CLAUSES);
        order_heap->setTracer(tracer_, TraceWriter::DS_HEAP);
        output.output("Binary trace writer enabled: %s (buffer=%zu B)\n",
                      trace_file.c_str(), trace_buf);
    }

    // Component should not end simulation until solution is found
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

SATSolver::~SATSolver() {}

void SATSolver::init(unsigned int phase) {
    global_memory->init(phase);
#ifndef USE_CLASSIC_HEAP
    order_heap->init(phase);
#endif

    // Only parse the file in phase 0
    if (phase == 0) {
        output.output("Reading CNF file: %s\n", cnf_file_path.c_str());
        
        parseDIMACS(cnf_file_path);
        output.verbose(CALL_INFO, 1, 0, "Parsed %u variables, %u clauses\n", num_vars, num_clauses);
        
        state = INIT;

        qhead = 0;
        seen.resize(num_vars + 1, 0);
        polarity.resize(num_vars + 1, false); // Default phase is false
        decision.resize(num_vars + 1, true);  // All variables are decision variables
        var_assigned.resize(num_vars + 1, false);
        var_value.resize(num_vars + 1);
        resetSpecState();

        // The variables array is the only per-var region without its own
        // bounds check; overrunning it would silently clobber watch metadata.
        // (initWatches and the clause allocator guard their own regions.)
        uint64_t variables_end = variables_base_addr
                               + (uint64_t)(num_vars + 1) * sizeof(Variable);
        sst_assert(variables_end <= watches_base_addr, CALL_INFO, -1,
            "Variables region (%u vars, 0x%lx-0x%lx) overruns watches_base_addr 0x%lx\n",
            num_vars, variables_base_addr, variables_end, watches_base_addr);

        // Untimed data structure initialization
        variables.init(num_vars);
        watches.initWatches(2 * (num_vars + 1), parsed_clauses);
        clauses.initialize(parsed_clauses);

        // Precompute occurrence list sizes for each literal (original clauses only).
        // Conservative under-count: learnt clauses are not tracked, so the
        // "naive" occurrence cost reflects the original CNF only.
        if (profile_2wl) {
            lit_occ_count.resize(2 * (num_vars + 1), 0);
            for (auto& c : parsed_clauses) {
                for (auto& lit : c.literals) {
                    lit_occ_count[toWatchIndex(~lit)]++;
                }
            }
        }

        order_heap->setDecisionFlags(decision);
        order_heap->setHeapSize(num_vars);
        order_heap->setVarIncPtr(&var_inc);
        order_heap->initHeap(random_seed);
    }
    output.verbose(CALL_INFO, 3, 0, "SATSolver initialized in phase %u\n", phase);
}

void SATSolver::setup() {
    global_memory->setup();
    
    // Get cache line size from memory interface
    size_t line_size = global_memory->getLineSize();
    line_size = std::max(line_size, (size_t)64); // Ensure minimum line size
    output.verbose(CALL_INFO, 1, 0, "Cache line size: %zu bytes\n", line_size);
    
    // Propagate cache line size to all memory-using components
    watches.setLineSize(line_size);
    clauses.setLineSize(line_size);
#ifdef USE_CLASSIC_HEAP
    order_heap->setLineSize(line_size);
#else
    order_heap->setup();  // pipelined heap derives line size from its own interface
#endif

    // Write the trace file header now that num_vars/num_clauses are known
    // (populated by init(phase=0) before setup runs). No trace events have
    // been emitted yet because init paths use writeUntimed which is skipped.
    if (tracer_) {
        tracer_->writeHeader(cnf_file_path, random_seed, num_vars, num_clauses);
        tracer_->emitPhase((uint8_t)state);
        tracer_->emitLevel(0);
        tracer_phase_cache_ = (uint8_t)state;
        tracer_level_cache_ = 0;
    }
}

void SATSolver::complete(unsigned int phase) {
    global_memory->complete(phase);
#ifndef USE_CLASSIC_HEAP
    order_heap->complete(phase);
#endif
}

void SATSolver::finish() {
    global_memory->finish();
#ifndef USE_CLASSIC_HEAP
    order_heap->finish();
#endif

    // Flush and close the binary trace writer (if enabled).
    if (tracer_) {
        tracer_->close(total_cycles);
        delete tracer_;
        tracer_ = nullptr;
    }

    // Close decision output file if open
    if (decision_output_stream.is_open()) {
        decision_output_stream.close();
        output.verbose(CALL_INFO, 1, 0, "Closed decision output file\n");
    }
    
    // Print solver statistics
    output.output("============================[ Solver Statistics ]============================\n");
    output.output("Decisions    : %lu\n", getStatCount(stat_decisions));
    output.output("Propagations : %lu\n", getStatCount(stat_propagations));
    output.output("Conflicts    : %lu\n", getStatCount(stat_conflicts));
    output.output("Learned      : %lu\n", getStatCount(stat_learned));
    output.output("Removed      : %lu\n", getStatCount(stat_removed));
    output.output("DB_Reductions: %lu\n", getStatCount(stat_db_reductions));
    output.output("Assigns      : %lu\n", getStatCount(stat_assigns));
    output.output("UnAssigns    : %lu\n", getStatCount(stat_unassigns));
    output.output("Minimized    : %lu\n", getStatCount(stat_minimized_literals));
    output.output("Restarts     : %lu\n", getStatCount(stat_restarts));
    output.output("MultiConfl   : %lu\n", getStatCount(stat_multi_confl_rounds));
    output.output("BtLevelDiff  : %lu\n", getStatCount(stat_bt_level_diff));
    // Speculative propagation statistics
    output.output("Spec Started : %lu\n", getStatCount(stat_spec_started));
    output.output("Spec Finished: %lu\n", getStatCount(stat_spec_finished));
    output.output("Variables    : %u (Total), %lu (Assigned)\n", num_vars,
        getStatCount(stat_assigns) - getStatCount(stat_unassigns));
    output.output("Clauses      : %lu (Total), %lu (Learned)\n",
        clauses.size(),
        getStatCount(stat_learned) - getStatCount(stat_removed));
    output.output("===========================================================================\n");

    // Conflict learning statistics
    {
        uint64_t learned_count = getStatCount(stat_learned) + getStatCount(stat_learnt_units);
        uint64_t total_length = getStatCount(stat_learnt_length);
        uint64_t unit_count = getStatCount(stat_learnt_units);
        uint64_t total_lbd = getStatCount(stat_learnt_lbd);
        uint64_t total_bt_level = getStatCount(stat_bt_level);
        uint64_t total_bt_distance = getStatCount(stat_bt_distance);
        output.output("===================[ Conflict Learning Statistics ]========================\n");
        output.output("Total Learnt Clause Length : %lu\n", total_length);
        output.output("Avg Learnt Clause Length   : %.2f\n",
            learned_count > 0 ? (double)total_length / learned_count : 0.0);
        output.output("Unit Learnt Clauses        : %lu\n", unit_count);
        output.output("Avg LBD                    : %.2f\n",
            learned_count > 0 ? (double)total_lbd / learned_count : 0.0);
        output.output("Avg Backtrack Level        : %.2f\n",
            learned_count > 0 ? (double)total_bt_level / learned_count : 0.0);
        output.output("Avg Backtrack Distance     : %.2f\n",
            learned_count > 0 ? (double)total_bt_distance / learned_count : 0.0);
        output.output("===========================================================================\n");
    }

    // Only print histograms if they have data
    if (auto* hist_stat_occ = dynamic_cast<HistogramStatistic<uint64_t>*>(stat_watcher_occ)) {
        if (hist_stat_occ->getCollectionCount() > 0) {
            output.output("=========================[ Watchers Occupancy Histogram ]=================\n");
            printHist(stat_watcher_occ);
            output.output("===========================================================================\n");
        }
    }

    if (auto* hist_stat_blocks = dynamic_cast<HistogramStatistic<uint64_t>*>(stat_watcher_blocks)) {
        if (hist_stat_blocks->getCollectionCount() > 0) {
            output.output("=========================[ Watcher Blocks Visited Histogram ]=============\n");
            printHist(stat_watcher_blocks);
            output.output("===========================================================================\n");
        }
    }
    
    if (auto* hist_stat_para = dynamic_cast<HistogramStatistic<uint64_t>*>(stat_para_watchers)) {
        if (hist_stat_para->getCollectionCount() > 0) {
            output.output("=========================[ Parallel Watchers Histogram ]=================\n");
            printHist(stat_para_watchers);
            output.output("===========================================================================\n");
        }
    }
    
    if (auto* hist_stat_vars = dynamic_cast<HistogramStatistic<uint64_t>*>(stat_para_vars)) {
        if (hist_stat_vars->getCollectionCount() > 0) {
            output.output("=========================[ Parallel Variables Histogram ]================\n");
            printHist(stat_para_vars);
            output.output("===========================================================================\n");
        }
    }
    
    // Reduced clause access statistics (only meaningful when profile_2wl is enabled).
    // Note: lit_occ_count tracks the original CNF only, so total_occ_sum is a
    // conservative under-count of what a naive occurrence-list propagator would do.
    if (profile_2wl) {
        uint64_t total_occ_sum = getStatCount(stat_total_occ);
        uint64_t watcher_sum = getStatCount(stat_watcher_traversed);
        uint64_t reduced = total_occ_sum - watcher_sum;
        double reduction_pct = (total_occ_sum > 0)
            ? (double)reduced / total_occ_sum * 100.0 : 0.0;
        output.output("=================[ Reduced Clause Access Statistics ]===================\n");
        output.output("Full Occurrence List (naive, original clauses only) : %lu\n", total_occ_sum);
        output.output("2WL Watchers Traversed                              : %lu\n", watcher_sum);
        output.output("Reduced Clause Accesses                             : %lu (%.1f%%)\n", reduced, reduction_pct);
        output.output("===========================================================================\n");
    }

    // output.output("=========================[ Clauses Fragmentation ]=========================\n");
    // clauses.printFragStats();
    // output.output("===========================================================================\n");

    output.output("=========================[ Binary Clause Region ]=========================\n");
    clauses.printBinaryStats();
    output.output("===========================================================================\n");

    uint64_t total_counted = cycles_propagate + cycles_analyze + cycles_minimize +
                            cycles_backtrack + cycles_decision + cycles_reduce + cycles_restart +
                            cycles_heap_insert + cycles_heap_bump;

    // Calculate percentages (avoid division by zero)
    double pct_propagate = (double)cycles_propagate * 100.0 / total_cycles;
    double pct_analyze = (double)cycles_analyze * 100.0 / total_cycles;
    double pct_minimize = (double)cycles_minimize * 100.0 / total_cycles;
    double pct_backtrack = (double)cycles_backtrack * 100.0 / total_cycles;
    double pct_decision = (double)cycles_decision * 100.0 / total_cycles;
    double pct_reduce = (double)cycles_reduce * 100.0 / total_cycles;
    double pct_restart = (double)cycles_restart * 100.0 / total_cycles;
    double pct_heap_insert = (double)cycles_heap_insert * 100.0 / total_cycles;
    double pct_heap_bump = (double)cycles_heap_bump * 100.0 / total_cycles;

    // Print cycle count statistics with percentages
    output.output("===========================[ Cycle Statistics ]============================\n");
    output.output("Propagate    : %.2f%% \t(%lu cycles)\n", pct_propagate, cycles_propagate);
    output.output("Analyze      : %.2f%% \t(%lu cycles)\n", pct_analyze, cycles_analyze);
    output.output("Minimize     : %.2f%% \t(%lu cycles)\n", pct_minimize, cycles_minimize);
    output.output("Backtrack    : %.2f%% \t(%lu cycles)\n", pct_backtrack, cycles_backtrack);
    output.output("Decision     : %.2f%% \t(%lu cycles)\n", pct_decision, cycles_decision);
    output.output("Reduce DB    : %.2f%% \t(%lu cycles)\n", pct_reduce, cycles_reduce);
    output.output("Restart      : %.2f%% \t(%lu cycles)\n", pct_restart, cycles_restart);
    output.output("Heap Insert  : %.2f%% \t(%lu cycles)\n", pct_heap_insert, cycles_heap_insert);
    output.output("Heap Bump    : %.2f%% \t(%lu cycles)\n", pct_heap_bump, cycles_heap_bump);
    output.output("Total Counted: %lu cycles\n", total_counted);
    output.output("===========================================================================\n");
    
    // Add speculative propagation profiling (gated by profile_prop_timing)
    if (profile_prop_timing) {
        output.output("===================[ Speculative Propagation Profiling ]===================\n");
        if (normal_metrics.count > 0) {
            double avg_normal_headptr = (double)normal_metrics.read_headptr_cycles / normal_metrics.count;
            double avg_normal_blocks = (double)normal_metrics.read_watcher_blocks_cycles / normal_metrics.count;
            double avg_normal_clauses = (double)normal_metrics.read_clauses_cycles / normal_metrics.count;
            double avg_normal_total = avg_normal_headptr + avg_normal_blocks + avg_normal_clauses;

            output.output("Normal Propagations (count: %lu):\n", normal_metrics.count);
            output.output("  Avg cycles to read head pointer   : %.2f\n", avg_normal_headptr);
            output.output("  Avg cycles to read watcher blocks : %.2f\n", avg_normal_blocks);
            output.output("  Avg cycles to read clauses        : %.2f\n", avg_normal_clauses);
            output.output("  Avg total memory latency          : %.2f\n", avg_normal_total);
        } else {
            output.output("Normal Propagations: No data collected\n");
        }

        if (speculative_metrics.count > 0) {
            double avg_spec_headptr = (double)speculative_metrics.read_headptr_cycles / speculative_metrics.count;
            double avg_spec_blocks = (double)speculative_metrics.read_watcher_blocks_cycles / speculative_metrics.count;
            double avg_spec_clauses = (double)speculative_metrics.read_clauses_cycles / speculative_metrics.count;
            double avg_spec_total = avg_spec_headptr + avg_spec_blocks + avg_spec_clauses;

            output.output("Speculative Propagations (count: %lu):\n", speculative_metrics.count);
            output.output("  Avg cycles to read head pointer   : %.2f\n", avg_spec_headptr);
            output.output("  Avg cycles to read watcher blocks : %.2f\n", avg_spec_blocks);
            output.output("  Avg cycles to read clauses        : %.2f\n", avg_spec_clauses);
            output.output("  Avg total memory latency          : %.2f\n", avg_spec_total);

            // Calculate speedup if both have data
            if (normal_metrics.count > 0) {
                double avg_normal_total = (double)(normal_metrics.read_headptr_cycles +
                                                   normal_metrics.read_watcher_blocks_cycles +
                                                   normal_metrics.read_clauses_cycles) / normal_metrics.count;
                double speedup = avg_normal_total / avg_spec_total;
                double reduction = (1.0 - (avg_spec_total / avg_normal_total)) * 100.0;
                output.output("Speedup: %.2fx (%.1f%% latency reduction)\n", speedup, reduction);
            }
        } else {
            output.output("Speculative Propagations: No data collected\n");
        }
        output.output("===========================================================================\n");
    }
    
    // Add cache line statistics for speculative propagations
    if (!spec_prop_cache_lines.empty()) {
        output.output("==============[ Speculative Propagation Cache Line Stats ]================\n");
        
        // Calculate all statistics in a single pass
        uint64_t total_cache_lines = 0;
        uint64_t min_cache_lines = spec_prop_cache_lines[0];
        uint64_t max_cache_lines = spec_prop_cache_lines[0];
        
        for (uint64_t cache_lines : spec_prop_cache_lines) {
            total_cache_lines += cache_lines;
            if (cache_lines < min_cache_lines) min_cache_lines = cache_lines;
            if (cache_lines > max_cache_lines) max_cache_lines = cache_lines;
        }
        
        double avg_cache_lines = (double)total_cache_lines / spec_prop_cache_lines.size();
        
        output.output("Number of speculative propagations: %zu\n", spec_prop_cache_lines.size());
        output.output("Total cache lines brought in: %lu\n", total_cache_lines);
        output.output("Average cache lines per propagation: %.2f\n", avg_cache_lines);
        output.output("Min cache lines per propagation: %lu\n", min_cache_lines);
        output.output("Max cache lines per propagation: %lu\n", max_cache_lines);
        output.output("===========================================================================\n");
    }
    
    // Add new detailed propagation statistics (gated by profile_prop_timing)
    if (profile_prop_timing) {
        output.output("======================[ Propagation Detail Statistics ]===================\n");
        double pct_read_headptr = (double)cycles_read_headptr * 100.0 / total_counted;
        double pct_read_watcher_blocks = (double)cycles_read_watcher_blocks * 100.0 / total_counted;
        double pct_read_clauses = (double)cycles_read_clauses * 100.0 / total_counted;
        double pct_insert_watchers = (double)cycles_insert_watchers * 100.0 / total_counted;
        double pct_polling = (double)cycles_polling * 100.0 / total_counted;

        output.output("Read Head Pointers : %.2f%% \t(%lu cycles)\n", pct_read_headptr, cycles_read_headptr);
        output.output("Read Watcher Blocks: %.2f%% \t(%lu cycles)\n", pct_read_watcher_blocks, cycles_read_watcher_blocks);
        output.output("Read Clauses       : %.2f%% \t(%lu cycles)\n", pct_read_clauses, cycles_read_clauses);
        output.output("Insert Watchers    : %.2f%% \t(%lu cycles)\n", pct_insert_watchers, cycles_insert_watchers);
        output.output("Polling for Busy   : %.2f%% \t(%lu cycles)\n", pct_polling, cycles_polling);
        uint64_t propagation_detail_sum = cycles_read_headptr + cycles_read_watcher_blocks +
                                          cycles_read_clauses + cycles_insert_watchers + cycles_polling;
        output.output("Detail Sum         : %lu cycles\n", propagation_detail_sum);
        output.output("===========================================================================\n");
    }

    // Coprocessor mode: raw statistics for offline computation
    {
        output.output("=============[ Coprocessor Raw Statistics ]=============\n");
        output.output("sf_hw_learning    = %lu\n", coproc_sf_hw_learning);
        output.output("sf_hw_minimize    = %lu\n", coproc_sf_hw_minimize);
        output.output("dep_decision      = %lu\n", coproc_dep_decision);
        output.output("dep_learning      = %lu\n", coproc_dep_learning);
        output.output("dep_minimize      = %lu\n", coproc_dep_minimize);
        output.output("dep_backtrack     = %lu\n", coproc_dep_backtrack);
        output.output("========================================================\n");
    }
}

//-----------------------------------------------------------------------------------
// Input Processing
//-----------------------------------------------------------------------------------

void SATSolver::parseDIMACS(const std::string& filename) {
    output.output("Starting DIMACS parsing from file: %s\n", filename.c_str());
    
    // Open file safely for direct reading
    std::ifstream file(filename);
    if (!file.is_open()) {
        output.fatal(CALL_INFO, -1, "Failed to open CNF file: %s\n", filename.c_str());
    }
    
    std::string line;
    Clause pending_clause;  // accumulates literals across lines until 0-terminator
    while (std::getline(file, line)) {
        // Skip empty lines
        if (line.empty()) continue;
        
        // Remove any trailing carriage returns or whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        
        // Skip empty lines after cleaning
        if (line.empty()) continue;
        
        // Skip whitespace at start
        size_t firstChar = line.find_first_not_of(" \t");
        if (firstChar == std::string::npos) continue;
        
        // Process based on first character
        switch (line[firstChar]) {
            case 'c':  // Comment line
                output.verbose(CALL_INFO, 8, 0,
                    "Comment: %s\n", line.substr(firstChar + 1).c_str());
                break;
                
            case 'p': {  // Problem line
                std::istringstream pline(line);
                std::string p, cnf;
                pline >> p >> cnf;
                if (cnf != "cnf") {
                    output.fatal(CALL_INFO, -1,
                        "Invalid DIMACS format: expected 'cnf' but got '%s'\n",
                        cnf.c_str());
                }
                pline >> num_vars >> num_clauses;
                output.verbose(CALL_INFO, 1, 0,
                    "Problem: vars=%u clauses=%u\n", num_vars, num_clauses);
                break;
            }
                
            default: {  // Clause line
                std::istringstream clause_iss(line);
                int dimacs_lit;

                // Validate the line contains only valid DIMACS literals
                bool valid_clause = true;
                for (char c : line) {
                    if (!std::isdigit(c) && c != '-' && c != ' ' && c != '\t' && c != '0') {
                        valid_clause = false;
                        break;
                    }
                }

                if (!valid_clause) {
                    output.verbose(CALL_INFO, 4, 0, "Skipping invalid clause line: %s\n", line.c_str());
                    continue;
                }

                // Accumulate literals into pending_clause; emit when 0 is seen.
                // This handles clauses that span multiple lines.
                while (clause_iss >> dimacs_lit) {
                    if (dimacs_lit == 0) {
                        // Clause terminator: emit the accumulated clause
                        if (pending_clause.literals.empty()) continue;

                        if (pending_clause.literals.size() == 1) {  // Unit clause
                            if (std::find(initial_units.begin(), initial_units.end(),
                                          pending_clause.literals[0]) == initial_units.end()) {
                                initial_units.push_back(pending_clause.literals[0]);
                            }
                            num_clauses--;
                            output.verbose(CALL_INFO, 3, 0,
                                "Unit clause: %d\n", toInt(pending_clause.literals[0]));
                        } else {
                            if (sort_clauses) {
                                std::sort(pending_clause.literals.begin(), pending_clause.literals.end());
                            }

                            pending_clause.literals.erase(
                                std::unique(pending_clause.literals.begin(), pending_clause.literals.end()),
                                pending_clause.literals.end());

                            pending_clause.num_lits = pending_clause.literals.size();
                            parsed_clauses.push_back(pending_clause);

                            output.verbose(CALL_INFO, 6, 0, "Added clause %lu: %s\n",
                                           parsed_clauses.size() - 1, printClause(pending_clause.literals).c_str());
                        }
                        pending_clause = Clause();
                    } else {
                        Lit lit = toLit(dimacs_lit);
                        pending_clause.literals.push_back(lit);
                    }
                }
                // No break: if line had no 0, pending_clause carries over to next line
                break;
            }
        }
    }
    
    // Flush any remaining pending clause (file ended without trailing 0)
    if (!pending_clause.literals.empty()) {
        if (pending_clause.literals.size() == 1) {
            if (std::find(initial_units.begin(), initial_units.end(),
                          pending_clause.literals[0]) == initial_units.end()) {
                initial_units.push_back(pending_clause.literals[0]);
            }
            num_clauses--;
        } else {
            if (sort_clauses) {
                std::sort(pending_clause.literals.begin(), pending_clause.literals.end());
            }
            pending_clause.literals.erase(
                std::unique(pending_clause.literals.begin(), pending_clause.literals.end()),
                pending_clause.literals.end());
            pending_clause.num_lits = pending_clause.literals.size();
            parsed_clauses.push_back(pending_clause);
        }
    }

    // Close the file explicitly to ensure clean up
    file.close();

    sst_assert(parsed_clauses.size() == num_clauses, CALL_INFO, -1,
        "Parsing error: Expected %u clauses but got %zu\n",
        num_clauses, parsed_clauses.size());
    
    // Initialize learnt clause adjustment parameters
    learnt_adjust_confl = learnt_adjust_start_confl;
    learnt_adjust_cnt = (int)learnt_adjust_confl;
    max_learnts = parsed_clauses.size() * learntsize_factor;
    output.verbose(CALL_INFO, 3, 0, "learnt_adjust_confl %f\n", learnt_adjust_confl);
    output.verbose(CALL_INFO, 3, 0, "max_learnts %.0f\n", max_learnts);
}

//-----------------------------------------------------------------------------------
// Event Handling Methods
//-----------------------------------------------------------------------------------

void SATSolver::handleCnfMemEvent(SST::Interfaces::StandardMem::Request* req) {
    SST::Interfaces::StandardMem::ReadResp* resp = 
        dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req);
    
    if (resp) {
        // Convert byte data back to string
        std::vector<uint8_t>& data = resp->data;
        dimacs_content = std::string(data.begin(), data.end());
        output.verbose(CALL_INFO, 1, 0,
            "Received %zu bytes from memory\n", resp->data.size());
        parseDIMACS(dimacs_content);
        output.verbose(CALL_INFO, 1, 0,
            "Parsed %u variables, %u clauses\n\n", num_vars, num_clauses);
        state = INIT;
    }
    delete resp;
}

void SATSolver::handleGlobalMemEvent(SST::Interfaces::StandardMem::Request* req) {
    sst_assert(req != nullptr, CALL_INFO, -1, "Received null request in handleGlobalMemEvent\n");
    if (auto* read_resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        uint64_t addr = read_resp->pAddr;

        // only used if it is not heap's response, because heap has its own reorder buffer
        int worker_id = -1;

        // Route the request to the appropriate handler based on address range.
        // Pipelined-heap build: activity traffic uses the heap's own
        // interface, so no var_act (or heap/indices) responses arrive here.
#ifdef USE_CLASSIC_HEAP
        if (addr >= var_act_base_addr) {  // Variable activity request
            order_heap->handleMem(req);
        } else
#endif
        if (addr >= clauses_cmd_base_addr) {  // Clauses request
            worker_id = reorder_buffer.lookUpWorkerId(read_resp->getID());
            clauses.handleMem(req);
            if (worker_id >= 0 && state != STEP) {
                saved_state = state;
                state = STEP;
            }
        } else if (addr >= watches_base_addr) {  // Watches request
            worker_id = reorder_buffer.lookUpWorkerId(read_resp->getID());
            watches.handleMem(req);
            if (worker_id >= 0 && state != STEP) {
                saved_state = state;
                state = STEP;
            }
        } else if (addr >= variables_base_addr) {  // Variables request
            worker_id = reorder_buffer.lookUpWorkerId(read_resp->getID());
            variables.handleMem(req);
            if (worker_id >= 0 && state != STEP) {
                saved_state = state;
                state = STEP;
            }
        }
#ifdef USE_CLASSIC_HEAP
        else order_heap->handleMem(req);  // Heap request
#else
        else sst_assert(false, CALL_INFO, -1,
            "Unexpected memory response for 0x%lx on the solver interface\n", addr);
#endif

        // Activate appropriate workers based on worker_id range.
        // Two-part guard:
        //   (1) spec_coroutine != nullptr — don't classify as spec when no
        //       spec coroutine exists.
        //   (2) worker_id >= SPEC_WORKER_BASE — SPEC_WORKER_BASE is strictly
        //       greater than every main-side worker_id (unitPropagate /
        //       execAnalyze / execMinimize), so it's a sufficient
        //       discriminator when spec *is* running.
        if (worker_id >= 0) {
            if (spec_coroutine != nullptr && worker_id >= SPEC_WORKER_BASE) {
                int spec_worker = worker_id - SPEC_WORKER_BASE;
                assert(spec_worker <= (int)spec_active_workers.size());
                if (spec_active_workers.size() > 0)
                    spec_active_workers[spec_worker] = true;
                spec_active = true;
            } else {
                assert(worker_id <= (int)active_workers.size());
                if (active_workers.size() > 0)
                    active_workers[worker_id] = true;
                main_active = true;
            }
        }
        output.verbose(CALL_INFO, 8, 0, "handleGlobalMemEvent received for 0x%lx, worker %d\n", addr, worker_id);
    } else if (auto* write_resp = dynamic_cast<SST::Interfaces::StandardMem::WriteResp*>(req)) {
        if (WRITE_BUFFER) {
            // for popping write queue
            uint64_t addr = write_resp->pAddr;
#ifdef USE_CLASSIC_HEAP
            if (addr >= var_act_base_addr) {  // Variable activity request
                order_heap->handleMem(req);
            } else
#endif
            if (addr >= clauses_cmd_base_addr) {  // Clauses request
                clauses.handleMem(req);
            } else if (addr >= watches_base_addr) {  // Watches request
                watches.handleMem(req);
            } else if (addr >= variables_base_addr) {  // Variables request
                variables.handleMem(req);
            }
#ifdef USE_CLASSIC_HEAP
            else order_heap->handleMem(req);  // Heap request
#endif
        }
    }
    delete req;
}

void SATSolver::handleHeapResponse(SST::Event* ev) {
    HeapRespEvent* resp = dynamic_cast<HeapRespEvent*>(ev);
    sst_assert(resp != nullptr, CALL_INFO, -1, "Invalid heap response event\n");
    output.verbose(CALL_INFO, 8, 0, "HandleHeapResponse: response %d\n", resp->result);
    heap_resp = resp->result;
    heap_resp_cnt--;
    if (in_decision && heap_resp_cnt == 0) {
        state = STEP;
        main_active = true;
    }
    assert(heap_resp_cnt >= 0);
    delete resp;
}

// The histogram prefix scan finished: unblock reduce commits and wake the
// reduce parent coroutine (same wake protocol as a memory response).
void SATSolver::handleReduceScan(SST::Event* ev) {
    delete ev;
    output.verbose(CALL_INFO, 4, 0, "REDUCEDB: threshold scan complete\n");
    scan_done_ = true;
    main_active = true;
    if (state != STEP) {
        saved_state = state;
        state = STEP;
    }
}

bool SATSolver::clockTick(SST::Cycle_t cycle) {
    // Check for timeout before doing any work. If exceeded, terminate simulation.
    if (timeout_cycles > 0 && cycle >= timeout_cycles && state != DONE) {
        output.output("====================[ Timeout Reached ]====================\n");
        output.output("Cycle %lu >= timeout limit %lu. Terminating early.\n", (uint64_t)cycle, timeout_cycles);
        output.output("===========================================================\n");
        state = DONE;
        total_cycles = cycle;
        primaryComponentOKToEndSim();
        return true; // signal done
    }

    // Trace-side: snapshot phase/level/cycle at the top of every tick so
    // memory events emitted during this tick inherit the latest labels.
    // STEP is a transient reply-handling state set by handleGlobalMemEvent;
    // attribute events to the pre-STEP phase (saved_state).
    if (tracer_) {
        uint8_t phase = (state == STEP) ? (uint8_t)saved_state : (uint8_t)state;
        if (phase != tracer_phase_cache_) {
            tracer_->emitPhase(phase);
            tracer_phase_cache_ = phase;
        }
        int32_t lvl = (int32_t)trail_lim.size();
        if (lvl != tracer_level_cache_) {
            tracer_->emitLevel(lvl);
            tracer_level_cache_ = lvl;
        }
        if (tracer_->events() != tracer_events_at_tick_start_) {
            tracer_->emitTick((uint64_t)cycle);
            tracer_events_at_tick_start_ = tracer_->events();
        }
    }

    // Calculate elapsed cycles since last state change if we're not in IDLE or STEP
    if (state != IDLE && state != STEP && prev_state != state) {
        // Update cycle counts based on previous state
        uint64_t elapsed = cycle - last_state_change;
        output.verbose(CALL_INFO, 8, 0,
            "DEBUG: previous state %d, current state %d, elapsed cycles %lu\n", 
            prev_state, state, cycle - last_state_change);
        switch (prev_state) {
            case PROPAGATE:
                cycles_propagate += elapsed;
                output.verbose(CALL_INFO, 4, 0, "DEBUG: Propagate cycles %lu\n", cycles_propagate);
                break;
            case ANALYZE:
                cycles_analyze += elapsed;
                output.verbose(CALL_INFO, 4, 0, "DEBUG: Analyze cycles %lu\n", cycles_analyze);
                break;
            case BTLEVEL:  // count towards minimize
            case MINIMIZE:
                cycles_minimize += elapsed;
                output.verbose(CALL_INFO, 4, 0, "DEBUG: Minimize cycles %lu\n", cycles_minimize);
                break;
            case BACKTRACK:
                cycles_backtrack += elapsed;
                output.verbose(CALL_INFO, 4, 0, "DEBUG: Backtrack cycles %lu\n", cycles_backtrack);
                break;
            case DECIDE:
                cycles_decision += elapsed;
                output.verbose(CALL_INFO, 4, 0, "DEBUG: Decision cycles %lu\n", cycles_decision);
                break;
            case REDUCE:
                cycles_reduce += elapsed;
                output.verbose(CALL_INFO, 4, 0, "DEBUG: Reduce cycles %lu\n", cycles_reduce);
                break;
            case RESTART:
                cycles_restart += elapsed;
                output.verbose(CALL_INFO, 4, 0, "DEBUG: Restart cycles %lu\n", cycles_restart);
                break;
            case WAIT_HEAP:
                if (state == BACKTRACK) cycles_heap_bump += elapsed;
                else cycles_heap_insert += elapsed;
                break;
            default:
                break;
        }
    
        // Accumulate coprocessor raw statistics for offline computation
        {
            const int LITS_PER_CL = 16;
            switch (prev_state) {
                case ANALYZE:
                case BTLEVEL: {
                    double sf = std::min((int)conflicts.size(), LEARNERS);
                    if (sf < 1.0) sf = 1.0;
                    coproc_sf_hw_learning += (uint64_t)(elapsed * sf);
                    uint64_t trail_entries = 0;
                    if (bt_level < (int)trail_lim.size())
                        trail_entries = trail.size() - trail_lim[bt_level];
                    coproc_dep_learning += (trail_entries + LITS_PER_CL - 1) / LITS_PER_CL;
                    break;
                }
                case MINIMIZE:
                    coproc_sf_hw_minimize += (uint64_t)(elapsed *
                        std::max(1.0, (double)std::min((int)learnt_clause.size() - 1, MINIMIZERS)));
                    coproc_dep_minimize += (learnt_clause.size() + LITS_PER_CL - 1) / LITS_PER_CL;
                    break;
                case DECIDE:
                    coproc_dep_decision += 2 * (uint64_t)std::ceil(
                        std::log2(std::max((size_t)2, order_heap->size())));
                    break;
                case BACKTRACK: {
                    uint64_t vars = (saved_bt_level < (uint64_t)trail_lim.size()) ?
                        saved_trail_size - trail_lim[saved_bt_level] : 0;
                    coproc_dep_backtrack += 2 * ((vars + LITS_PER_CL - 1) / LITS_PER_CL);
                    break;
                }
                case RESTART:
                    coproc_dep_backtrack += 2 * ((saved_trail_size + LITS_PER_CL - 1) / LITS_PER_CL);
                    break;
                default: break;
            }
        }

        // Record the new state change
        prev_state = state;
        last_state_change = cycle;
    }

    if (--progress_countdown_ == 0) {  // every 100000 cycles; avoids a 64-bit modulo per tick
        progress_countdown_ = 100000;
        output.verbose(CALL_INFO, 2, 0, "Propagations: %lu, Conflicts: %lu, Decisions: %lu, Learnt: %lu\n",
            getStatCount(stat_propagations), getStatCount(stat_conflicts),
            getStatCount(stat_decisions), getStatCount(stat_learned));
    }

    switch (state) {
        case IDLE: return false; // skip prints
        case INIT: 
            coroutine = new coro_t::pull_type(
                [this](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    initialize();
                });
            if (!(*coroutine)) {
                output.verbose(CALL_INFO, 8, 0, "Coroutine never paused but completed\n");
                delete coroutine;
                coroutine = nullptr;
                yield_ptr = nullptr;
            } else state = IDLE;
            break;
        case STEP: {
            sst_assert(main_active || spec_active, CALL_INFO, -1, "STEP state entered without active coroutines\n");
            state = saved_state;
            if (main_active) {
                (*coroutine)();
                if (*coroutine) {
                    output.verbose(CALL_INFO, 8, 0, "coroutine paused\n");
                    state = IDLE;  // Continue coroutine later
                } else {
                    output.verbose(CALL_INFO, 8, 0, "coroutine completed\n");
                    assert(state != STEP);
                    delete coroutine;
                    coroutine = nullptr;  // coroutine will set the next state
                    yield_ptr = nullptr;  // Clear yield pointer when coroutine completes
                }
                main_active = false;
            }

            if (spec_active) {
                assert(state != STEP);  // does not modify state
                coro_t::push_type* saved_yield_ptr = yield_ptr;
                yield_ptr = spec_yield_ptr;
                (*spec_coroutine)();
                if (*spec_coroutine) {
                    output.verbose(CALL_INFO, 8, 0, "speculative coroutine paused\n");
                } else {
                    output.verbose(CALL_INFO, 8, 0, "speculative coroutine completed\n");
                    delete spec_coroutine;
                    spec_coroutine = nullptr;  // coroutine will set the next state
                    spec_yield_ptr = nullptr;  // Clear yield pointer when coroutine completes
                }
                spec_active = false;
                yield_ptr = saved_yield_ptr;  // Restore original yield_ptr
            }
            break;
        }
        case PROPAGATE:
            coroutine = new coro_t::pull_type(
                [this](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    execPropagate(); 
                });
            if (!(*coroutine)) {
                delete coroutine;
                coroutine = nullptr;
                yield_ptr = nullptr;
            } else state = IDLE;
            
            // Launch speculative propagation coroutine if spec_literal is defined
            if (spec_literal != lit_Undef && spec_coroutine == nullptr) {
                coro_t::push_type* saved_yield_ptr = yield_ptr;
                resetSpecState();
                spec_coroutine = new coro_t::pull_type(
                    [this](coro_t::push_type &yield) {
                        yield_ptr = &yield;
                        spec_yield_ptr = &yield;
                        speculativePropagate();
                    });
                if (!(*spec_coroutine)) {
                    delete spec_coroutine;
                    spec_coroutine = nullptr;
                    spec_yield_ptr = nullptr;
                } else state = IDLE;
                yield_ptr = saved_yield_ptr; // Restore original yield_ptr
            }
            break;
        case DECIDE:
            coroutine = new coro_t::pull_type(
                [this](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    execDecide(); 
                });
            if (!(*coroutine)) {
                delete coroutine;
                coroutine = nullptr;
                yield_ptr = nullptr;
            } else state = IDLE;
            break;
        case ANALYZE:
            coroutine = new coro_t::pull_type(
                [this](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    execAnalyze(); 
                });
            if (!(*coroutine)) {
                delete coroutine;
                coroutine = nullptr;
                yield_ptr = nullptr;
            } else state = IDLE;
            break;
        case MINIMIZE:
            if (ccmin_mode == 0 || learnt_clause.size() <= 1) {
                state = BTLEVEL;
                break;
            }

            coroutine = new coro_t::pull_type(
                [this](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    execMinimize(); 
                });
            if (!(*coroutine)) {
                delete coroutine;
                coroutine = nullptr;
                yield_ptr = nullptr;
            } else state = IDLE;
            break;
        case BTLEVEL:
            if (learnt_clause.size() == 1) {
                bt_level = 0;
                state = BACKTRACK;
#ifdef USE_CLASSIC_HEAP
                if (OVERLAP_HEAP_BUMP) {
                    state = WAIT_HEAP;
                    next_state = BACKTRACK;
                } else state = BACKTRACK;
#endif
            } else {
                coroutine = new coro_t::pull_type(
                    [this](coro_t::push_type &yield) {
                        yield_ptr = &yield;
                        findBtLevel(); 
                    });
                if (!(*coroutine)) {
                    delete coroutine;
                    coroutine = nullptr;
                    yield_ptr = nullptr;
                } else state = IDLE;
            }
            break;
        case BACKTRACK:
            saved_trail_size = trail.size();
            saved_bt_level = bt_level;
            coroutine = new coro_t::pull_type(
                [this](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    execBacktrack();
                });
            if (!(*coroutine)) {
                delete coroutine;
                coroutine = nullptr;
                yield_ptr = nullptr;
            } else state = IDLE;
            break;
        case REDUCE:
            coroutine = new coro_t::pull_type(
                [this](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    execReduce(); 
                });
            if (!(*coroutine)) {
                delete coroutine;
                coroutine = nullptr;
                yield_ptr = nullptr;
            } else state = IDLE;
            break;
        case RESTART:
            saved_trail_size = trail.size();
            coroutine = new coro_t::pull_type(
                [this](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    execRestart(); 
                });
            if (!(*coroutine)) {
                delete coroutine;
                coroutine = nullptr;
                yield_ptr = nullptr;
            } else state = IDLE;
            break;
        case WAIT_HEAP:
            if (heap_resp_cnt == 0) state = next_state;
            return false;
        case DONE: total_cycles = cycle; primaryComponentOKToEndSim(); return true;
        default: output.fatal(CALL_INFO, -1, "Invalid state: %d\n", state);
    }
    output.verbose(CALL_INFO, 7, 0, "=== Clock Tick %ld === State: %d\n", cycle, state);
    return false;
}

void SATSolver::initialize() {
    // Enqueue unit clauses from the input DIMACS
    output.verbose(CALL_INFO, 3, 0, "Enqueuing initial unit clauses\n");
    for (int i = 0; i < initial_units.size(); i++) {
        trailEnqueue(initial_units[i]); 
    }
    
    output.verbose(CALL_INFO, 1, 0, "Initialization complete\n");
    state = PROPAGATE;
}

void SATSolver::execPropagate() {
    unitPropagate();

    // If we have conflicts
    if (!conflicts.empty()) {
        conflictC ++;  // for restart
        stat_conflicts->addDataNTimes(conflicts.size(), 1);
        if (decision_output_stream.is_open()) decision_output_stream << "#Conflict" << std::endl;
        
        if (trail_lim.empty()) {
            output.output("UNSATISFIABLE: conflict at level 0\n");
            state = DONE;
            return;
        }
        state = ANALYZE;  // learn from the conflicts
    } else if (glucose_restart ?
        (conflictC > glucose_min_conflicts && lbd_ema_fast * 0.8 > lbd_ema_slow) :
        (conflictC >= conflicts_until_restart)) state = RESTART;
    else if (nLearnts() - nAssigns() >= max_learnts) state = REDUCE;
    else state = DECIDE;

#ifdef USE_CLASSIC_HEAP
    if (OVERLAP_HEAP_INSERT) {
        next_state = state;
        state = WAIT_HEAP;
    }
#endif
}

void SATSolver::execAnalyze() {
    // Debug print for trail
    if (output.getVerboseLevel() >= 4) {
        int j = 0;
        output.verbose(CALL_INFO, 4, 0, "Trail (%zu, level=%d):", trail.size(), current_level());
        for (int i = 0; i < trail.size(); i++) {
            if (j < (int)trail_lim.size() && i == trail_lim[j]) {
                output.output("\n    dec=%d: ",j);
                j++;
            }
            output.output(" %d", toInt(trail[i]));
        }
        output.output("\n");
    }

    coro_t::push_type* parent_yield_ptr = yield_ptr;
    int total_confl = (int)conflicts.size();
    bt_level = std::numeric_limits<int>::max();
    round_max_bt = -1;

    // Analyze all collected conflicts in batches of LEARNERS hardware lanes.
    // bt_level (min) and round_max_bt (max) persist across batches so the single
    // best learnt clause is kept and the bt-level spread can be measured.
    for (int batch_start = 0; batch_start < total_confl; batch_start += LEARNERS) {
        int workers = std::min(LEARNERS, total_confl - batch_start);
        active_workers.assign(workers, false);
        std::vector<coro_t::pull_type*> coroutines(workers);
        std::vector<coro_t::push_type*> yield_ptrs(workers);
        bool done = true;

        // spawn sub-coroutines, one hardware lane per conflict in this batch
        for (int worker_id = 0; worker_id < workers; worker_id++) {
            int conflict_idx = batch_start + worker_id;
            coroutines[worker_id] = new coro_t::pull_type(
                [this, worker_id, conflict_idx, &yield_ptrs](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    yield_ptrs[worker_id] = yield_ptr;
                    analyze(conflicts[conflict_idx], worker_id);
                });
            if (*coroutines[worker_id]) done = false;  // may finish without yielding
        }
        if (!done) (*parent_yield_ptr)();  // yield back to IDLE

        // stepping sub-coroutines
        while (!done) {
            done = true;
            // Check if any worker is active
            for (int worker_id = 0; worker_id < workers; worker_id++) {
                if (active_workers[worker_id]) {
                    yield_ptr = yield_ptrs[worker_id];
                    (*coroutines[worker_id])();
                    active_workers[worker_id] = false;
                    if (*coroutines[worker_id]) {
                        done = false;
                    } else {
                        delete coroutines[worker_id];
                        coroutines[worker_id] = nullptr;
                        yield_ptrs[worker_id] = nullptr;
                    }
                // waiting workers
                } else if (coroutines[worker_id] != nullptr) done = false;
            }

            if (!done) (*parent_yield_ptr)();  // yield back to IDLE
        }
    }

    // finished all sub-coroutines
    active_workers.clear();
    yield_ptr = parent_yield_ptr;

    // Measure the opportunity for multi-conflict learning: how often a round
    // collects >1 conflict, and how often those conflicts disagree on the
    // backtrack level (so selecting among them can actually change behavior).
    // round_max_bt is the max bt level seen; bt_level is the min (selected).
    if (total_confl > 1) {
        stat_multi_confl_rounds->addData(1);
        if (round_max_bt > bt_level) stat_bt_level_diff->addData(1);
    }

    for (const Var& v : v_to_bump) {
        order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::BUMP, v));
#ifdef USE_CLASSIC_HEAP
        heap_resp_cnt++;
#endif
    }
    for (size_t bi = 0; bi < c_to_bump.size(); bi++) {
        if (claBumpActivity(c_to_bump[bi].first, c_to_bump[bi].second)) {
            // A rescale fired mid-loop: the remaining remembered activities
            // are stale by exactly the rescale factor. Apply the same float
            // transform the DRAM sweep applied (power-of-two, exact).
            for (size_t bj = bi + 1; bj < c_to_bump.size(); bj++)
                c_to_bump[bj].second =
                    ActivityHistogram::rescaleValue(c_to_bump[bj].second);
        }
    }

    output.verbose(CALL_INFO, 3, 0, "Final learnt: %s\n",
        printClause(learnt_clause).c_str());

    state = MINIMIZE;
#ifdef USE_CLASSIC_HEAP
    if (OVERLAP_HEAP_BUMP) state = MINIMIZE;
    else {
        state = WAIT_HEAP;
        next_state = MINIMIZE;
    }
#endif
}

void SATSolver::execMinimize() {
    // Keep track of literals to clear
    analyze_toclear.clear();
    analyze_toclear = learnt_clause; 

    // Minimize conflict clause:
    int i, j;
    output.verbose(CALL_INFO, 4, 0,
        "ANALYZE: Minimizing clause (size %zu): %s\n", learnt_clause.size(),
        printClause(learnt_clause).c_str());

    if (ccmin_mode == 2) {
        // Deep minimization (more thorough)
        coro_t::push_type* parent_yield_ptr = yield_ptr;
        int workers = std::min(MINIMIZERS, (int)learnt_clause.size() - 1);
        active_workers.resize(workers, false);
        std::vector<coro_t::pull_type*> coroutines(workers);
        std::vector<coro_t::push_type*> yield_ptrs(workers);
        std::vector<bool> redundant(learnt_clause.size(), false);
        bool done = true;

        // spawn sub-coroutines for each literal
        for (int worker_id = 0; worker_id < workers; worker_id++) {
            coroutines[worker_id] = new coro_t::pull_type(
                [this, worker_id, &redundant, &yield_ptrs](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    yield_ptrs[worker_id] = yield_ptr;
                    minimizeL2_sub(redundant, worker_id);
                });
            if (*coroutines[worker_id]) done = false;  // may finish without yielding
        }
        if (!done) (*parent_yield_ptr)();  // yield back to IDLE

        // stepping sub-coroutines
        while (!done) {
            done = true;
            // Check if any worker is active
            for (int worker_id = 0; worker_id < workers; worker_id++) {
                if (active_workers[worker_id]) {
                    yield_ptr = yield_ptrs[worker_id];
                    (*coroutines[worker_id])();
                    active_workers[worker_id] = false;
                    if (*coroutines[worker_id]) {
                        done = false;
                    } else {
                        delete coroutines[worker_id];
                        coroutines[worker_id] = nullptr;
                        yield_ptrs[worker_id] = nullptr;
                    }
                // waiting workers
                } else if (coroutines[worker_id] != nullptr) done = false;
            }

            if (!done) (*parent_yield_ptr)();  // yield back to IDLE
        }
        
        // finished all sub-coroutines
        active_workers.clear();
        yield_ptr = parent_yield_ptr;

        for (i = j = 1; i < learnt_clause.size(); i++) {
            if (!redundant[i]) learnt_clause[j++] = learnt_clause[i];
        }

    } else if (ccmin_mode == 1) {
        // Basic minimization (faster but less thorough)
        for (i = j = 1; i < learnt_clause.size(); i++) {
            Variable var_data = variables.readVar(var(learnt_clause[i]));

            if (var_data.reason == ClauseRef_Undef)
                learnt_clause[j++] = learnt_clause[i];
            else {
                Clause c = clauses.readClause(var_data.reason);
                for (size_t k = 1; k < c.litSize(); k++) {
                    Var l = var(c[k]);
                    if (!seen[l] && var_data.level > 0) {
                        learnt_clause[j++] = learnt_clause[i];
                        break; }
                }
            }
        }
    } else i = j = learnt_clause.size();

    learnt_clause.resize(j);
    
    // Clear seen vector for next analysis
    for (const Lit& l : analyze_toclear) seen[var(l)] = 0;
    
    // Update statistics - count how many literals were removed
    if (i - j > 0) {
        stat_minimized_literals->addDataNTimes(i - j, 1);
        output.verbose(CALL_INFO, 4, 0, 
            "MINIMIZE: removed %d literals\n", i - j);
        output.verbose(CALL_INFO, 3, 0, "MINIMIZE: Final minimized clause: %s\n",
            printClause(learnt_clause).c_str());
    }

    state = BTLEVEL;
}

void SATSolver::execBacktrack() {
    // Conflict-site rebuild check: stales are minted only by the bump waves
    // conflict analysis enqueues, so checking once per conflict is a complete
    // cover (between conflicts staleCount only falls; overshoot is bounded by
    // the one just-enqueued wave, caught at the next conflict). Deciding
    // BEFORE the unwind lets its inserts fold into the post-wipe wave.
    bool rebuild_heap = heapRebuildDue();
    if (rebuild_heap) suppress_heap_inserts_ = true;

    backtrack(bt_level);

    if (rebuild_heap) {
        fireHeapRebuild();
        stat_midsearch_rebuilds->addData(1);
        output.verbose(CALL_INFO, 3, 0, "BACKTRACK: mid-search heap rebuild triggered\n");
    }

    // Conflict learning statistics
    stat_learnt_length->addDataNTimes(learnt_clause.size(), 1);
    stat_learnt_lbd->addDataNTimes(learnt_lbd, 1);

    // Update glucose EMA trackers
    if (glucose_restart) {
        lbd_ema_fast = lbd_ema_fast * (1.0 - lbd_ema_fast_alpha) + learnt_lbd * lbd_ema_fast_alpha;
        lbd_ema_slow = lbd_ema_slow * (1.0 - lbd_ema_slow_alpha) + learnt_lbd * lbd_ema_slow_alpha;
    }
    stat_bt_level->addDataNTimes(bt_level, 1);
    stat_bt_distance->addDataNTimes(current_level() - bt_level, 1);
    if (learnt_clause.size() == 1) stat_learnt_units->addData(1);

    if (learnt_clause.size() == 1) {
        // Unit learnt clause will be instantly propagated
        if (tracer_) tracer_->emitLearn(learnt_lbd, (int)learnt_clause.size(), bt_level, 0);
        trailEnqueue(learnt_clause[0]);
    } else {
        // Add the learned clause
        Clause new_clause(learnt_clause, cla_inc);
        Cref addr = clauses.addClause(new_clause);
        // Non-binary learnts enter the activity histogram (binaries are never
        // removable, so the median oracle excludes them).
        if (new_clause.litSize() > 2) cla_hist_.add(new_clause.act());
        output.verbose(CALL_INFO, 3, 0,
            "Added learnt clause 0x%x: %s\n",
            addr, printClause(new_clause.literals).c_str());
        if (tracer_) tracer_->emitLearn(learnt_lbd, (int)learnt_clause.size(), bt_level, (int)addr);
        attachClause(addr, new_clause);
        trailEnqueue(learnt_clause[0], addr);
        stat_learned->addData(1);
    }

    // terminate speculative propagation if conflict after backtracking
    if (spec_literal != lit_Undef
        && var_assigned[var(spec_literal)] 
        && var_value[var(spec_literal)] != !sign(spec_literal)) {
        if (spec_coroutine != nullptr) terminateSpecPropagate();
        insertVarOrder(var(spec_literal));  // decide with spec_literal
        spec_literal = lit_Undef;
    }

    // do not redo speculative propagation if completed
    if (spec_literal != lit_Undef
        && spec_coroutine == nullptr) {
        insertVarOrder(var(spec_literal));  // decide with spec_literal
        spec_literal = lit_Undef;
    }

    varDecayActivity();
    claDecayActivity();
    
    // Periodically adjust learntsize limits
    if (--learnt_adjust_cnt == 0) {
        learnt_adjust_confl *= learnt_adjust_inc;
        learnt_adjust_cnt = (int)learnt_adjust_confl;
        max_learnts *= learntsize_inc;
        output.verbose(CALL_INFO, 3, 0, 
            "LEARN: Adjusted learnt_adjust_confl to %.0f\n", learnt_adjust_confl);
        output.verbose(CALL_INFO, 3, 0, 
            "LEARN: Adjusted max_learnts to %.0f\n", max_learnts);
    }

    state = PROPAGATE;
#ifdef USE_CLASSIC_HEAP
    if (OVERLAP_HEAP_INSERT) state = PROPAGATE;
    else {
        state = WAIT_HEAP;
        next_state = PROPAGATE;
    }
#endif
}

void SATSolver::execReduce() {
    output.verbose(CALL_INFO, 3, 0, "REDUCE: %d - %d >= %.0f\n", 
        nLearnts(), nAssigns(), max_learnts);
    reduceDB();
    state = DECIDE;
}

void SATSolver::execRestart() {
    output.verbose(CALL_INFO, 4, 0, "RESTART: Executing restart #%d\n", curr_restarts);
    if (tracer_) tracer_->emitRestart(curr_restarts);

    bool rebuild_heap = heapRebuildDue();
    if (rebuild_heap) suppress_heap_inserts_ = true;

    backtrack(0);
    conflictC = 0;
    curr_restarts++;
    stat_restarts->addData(1);

    // terminate speculative propagation because we are ready to choose a new decision
    if (spec_literal != lit_Undef) {
        if (spec_coroutine != nullptr) terminateSpecPropagate();
        insertVarOrder(var(spec_literal));  // decide with spec_literal
        spec_literal = lit_Undef;
    }

    if (rebuild_heap) {
        fireHeapRebuild();
        output.verbose(CALL_INFO, 3, 0, "RESTART: heap rebuild triggered\n");
    }

    // Update the restart limit using Luby sequence or geometric progression
    double rest_base = luby_restart ? luby(restart_inc, curr_restarts) : pow(restart_inc, curr_restarts);
    conflicts_until_restart = rest_base * restart_first;
    
    output.verbose(CALL_INFO, 3, 0, "RESTART: #%d executed, new limit=%d\n", 
        curr_restarts - 1, conflicts_until_restart);

    state = PROPAGATE;
#ifdef USE_CLASSIC_HEAP
    if (OVERLAP_HEAP_INSERT) state = PROPAGATE;
    else {
        state = WAIT_HEAP;
        next_state = PROPAGATE;
    }
#endif
}

void SATSolver::execDecide() {
    // Finish any previous speculative propagation early
    if (spec_coroutine != nullptr) terminateSpecPropagate();

    in_decision = true;
    if (!decide()) {
        state = DONE;
        output.output("SATISFIABLE: All variables assigned\n");
        for (Var v = 1; v <= (Var)num_vars; v++) {
            output.output("x%d=%d ", v, var_value[v] ? 1 : 0);
            sst_assert(var_assigned[v], CALL_INFO, -1, "Variable %d not assigned at end\n", v);
        }
        output.output("\n");
        return;
    }
    in_decision = false;
    state = PROPAGATE;
}

//-----------------------------------------------------------------------------------
// decision
//-----------------------------------------------------------------------------------

bool SATSolver::decide() {
    stat_decisions->addData(1);
    Lit lit = lit_Undef;
    
    // Use decision sequence if available and not exhausted
    if (has_decision_sequence && decision_seq_idx < decision_sequence.size()) {
        while (decision_seq_idx < decision_sequence.size() && lit == lit_Undef) {
            Var next_var = decision_sequence[decision_seq_idx].first;
            bool next_sign = decision_sequence[decision_seq_idx].second;
            decision_seq_idx++;
            
            // Check if variable can be decided on
            if (!var_assigned[next_var] && decision[next_var]) {
                lit = mkLit(next_var, !next_sign); // Note: mkLit's sign is negated in the API
                output.verbose(CALL_INFO, 3, 0, 
                    "DECISION: Using predefined decision %zu: var %d = %s\n", 
                    decision_seq_idx, next_var, next_sign ? "true" : "false");
            } else {
                output.output(
                    "WARNING: Skipping predefined decision %zu (var %d), assigned/not decidable\n", 
                    decision_seq_idx-1, next_var);
            }
        }

        if (decision_seq_idx >= decision_sequence.size()) {
            output.verbose(CALL_INFO, 1, 0,
                "DECISION: Exhausted decision sequence after %ld decisions\n",
                getStatCount(stat_decisions));
            has_decision_sequence = false;
        }
    }

    // decide with spec_literal
    if (spec_literal != lit_Undef && !var_assigned[var(spec_literal)]) {
        lit = spec_literal;
        output.verbose(CALL_INFO, 2, 0, "DECISION: Using speculative lit %d\n", toInt(lit));
    }

    // If couldn't use the decision sequence and no speculative literal, remove from heap
    if (lit == lit_Undef) {
        lit = chooseBranchVariable();
        output.verbose(CALL_INFO, 2, 0, "DECISION: Selected lit %d \n", toInt(lit));
        if (lit == lit_Undef) {
            output.verbose(CALL_INFO, 2, 0, "DECISION: No unassigned variables left\n");
            return false;
        }
    }

    if (decision_output_stream.is_open()) dumpDecision(lit);
    trail_lim.push_back(trail.size());  // new decision level
    if (tracer_) tracer_->emitDecision(var(lit), sign(lit), current_level());
    trailEnqueue(lit);

    // Extract speculative literal for next decision
    if (enable_speculative) {
        spec_literal = chooseBranchVariable();
        // spec_literal = peekBranchVariable();
        output.verbose(CALL_INFO, 2, 0, "Selected next speculative lit %d \n", toInt(spec_literal));
    }

    return true;
}

//-----------------------------------------------------------------------------------
// unitPropagate
//-----------------------------------------------------------------------------------

void SATSolver::unitPropagate() {
    output.verbose(CALL_INFO, 4, 0, "PROPAGATE: Starting unit propagation\n");
    conflicts.clear();

    // Track the current batch of variables that can be processed in parallel
    size_t batch_start = qhead;
    size_t batch_end = trail.size();

    // Clear any existing clause locks from previous propagations
    clause_locks.clear();

    while (qhead < trail.size()) {
        // Process literals in parallel batches of PARA_LITS
        coro_t::push_type* parent_yield_ptr = yield_ptr;
        int workers = std::min(PARA_LITS, int(trail.size() - qhead));
        std::vector<coro_t::pull_type*> coroutines(PARA_LITS, nullptr);
        std::vector<coro_t::push_type*> yield_ptrs(PARA_LITS, nullptr);
        active_workers.resize(PARA_LITS * PROPAGATORS, false);
        polling.resize(PARA_LITS * PROPAGATORS, false);

        // Track times for each literal worker
        std::vector<uint64_t> lit_read_headptr(PARA_LITS, 0);
        std::vector<uint64_t> lit_read_watcher_blocks(PARA_LITS, 0);
        std::vector<uint64_t> lit_read_clauses(PARA_LITS, 0);
        std::vector<uint64_t> lit_insert_watchers(PARA_LITS, 0);
        std::vector<uint64_t> lit_polling(PARA_LITS, 0);
        int last_worker = -1;

        // Spawn coroutines for each literal in this batch
        bool done = true;
        // printf("Prop %lu, Cycle %lu\n", getStatCount(stat_propagations), getCurrentSimCycle()/1000);
        output.verbose(CALL_INFO, 4, 0, "PROPAGATE: spawning literal coroutine (%d/%lu)\n",
            workers, trail.size() - qhead);
        for (int lit_idx = 0; lit_idx < workers; lit_idx++) {
            // track max literal parallelism
            if (qhead == batch_end) {
                stat_para_vars->addData(batch_end - batch_start);
                // Start tracking a new batch
                batch_start = batch_end;
                batch_end = trail.size();
            }

            // when watcher coroutines are not launched, assume lit coroutine uses the start
            coro_t::pull_type* lit_coro = new coro_t::pull_type(
                [this, lit_idx, &lit_read_headptr, &lit_read_watcher_blocks, &lit_read_clauses, 
                 &lit_insert_watchers, &lit_polling, &yield_ptrs]
                (coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    yield_ptrs[lit_idx] = yield_ptr;
                    propagateLiteral(trail[qhead++], lit_idx,
                                     lit_read_headptr[lit_idx], 
                                     lit_read_watcher_blocks[lit_idx],
                                     lit_read_clauses[lit_idx],
                                     lit_insert_watchers[lit_idx],
                                     lit_polling[lit_idx]);
                });
            coroutines[lit_idx] = lit_coro;
            stat_propagations->addData(1);
            if (*lit_coro) done = false; // May finish without yielding
        }

        // Initial yield if any coroutines are active
        if (!done) (*parent_yield_ptr)();

        // Process all coroutines until completion
        while (!done) {
            // Sample how many workers are simultaneously blocked on a propagation
            // lock this scheduler round. Low => parallelism hides the stalls;
            // spikes toward PARA_LITS*PROPAGATORS => the locks are serializing.
            // Also accumulate the time-weighted average: the interval since the
            // last sample held 'last_blocked' workers blocked, so weight it by
            // the elapsed cycles (addDataNTimes => Mean = avg stalled per cycle).
            if (track_stalls) {
                uint64_t now = getCurrentSimCycle() / 1000;
                uint64_t delta = now - last_round_cycle;
                if (delta > 0) stat_stalled_per_cycle->addDataNTimes(delta, last_blocked);
                uint64_t blocked = 0;
                for (size_t w = 0; w < polling.size(); w++) if (polling[w]) blocked++;
                stat_blocked_workers->addData(blocked);
                last_blocked = blocked;
                last_round_cycle = now;
            }

            // Check for active workers within each lit coroutine
            for (int j = 0; j < PARA_LITS; j++) {
                for (int jj = 0; jj < PROPAGATORS; jj++) {
                    if (active_workers[j * PROPAGATORS + jj]) {
                        yield_ptr = yield_ptrs[j];
                        (*coroutines[j])();
                        active_workers[j * PROPAGATORS + jj] = false;
                        break;
                    }
                }
            }

            for (int j = 0; j < PARA_LITS; j++) {
                for (int jj = 0; jj < PROPAGATORS; jj++) {
                    if (polling[j * PROPAGATORS + jj]) {
                        yield_ptr = yield_ptrs[j];
                        (*coroutines[j])();
                        break;
                    }
                }
            }

            // launch new lit coroutines if there are empty slots
            for (int j = 0; j < PARA_LITS; j++) {
                bool lit_done = true;
                if (coroutines[j] != nullptr)
                    if (*coroutines[j])
                        lit_done = false;

                if (lit_done && qhead < trail.size() 
                    && !(max_confl >= 0 && (int)conflicts.size() >= max_confl)) {
                    // printf("Prop %lu, Cycle %lu\n", getStatCount(stat_propagations), getCurrentSimCycle()/1000);
                    // track max literal parallelism
                    if (qhead == batch_end) {
                        stat_para_vars->addData(batch_end - batch_start);
                        // Start tracking a new batch
                        batch_start = batch_end;
                        batch_end = trail.size();
                    }
                    // start a new worker right away if available
                    output.verbose(CALL_INFO, 4, 0,
                        "PROPAGATE: spawning literal coroutine (1/%lu) at L%d\n",
                        trail.size() - qhead, j);
                    delete coroutines[j];
                    coro_t::pull_type* lit_coro = new coro_t::pull_type(
                        [this, j, &lit_read_headptr, &lit_read_watcher_blocks, &lit_read_clauses,
                         &lit_insert_watchers, &lit_polling, &yield_ptrs]
                        (coro_t::push_type &yield) {
                            yield_ptr = &yield;
                            yield_ptrs[j] = yield_ptr;
                            propagateLiteral(trail[qhead++], j,
                                             lit_read_headptr[j], 
                                             lit_read_watcher_blocks[j],
                                             lit_read_clauses[j],
                                             lit_insert_watchers[j],
                                             lit_polling[j]);
                        });
                    coroutines[j] = lit_coro;
                    stat_propagations->addData(1);
                }
            }

            // Check if any workers are still active
            // we can just check the parent coroutines
            done = true;
            for (int j = 0; j < PARA_LITS; j++) {
                if (coroutines[j] != nullptr) {
                    if (*coroutines[j]) done = false;
                    else {
                        last_worker = j;
                        delete coroutines[j];
                        coroutines[j] = nullptr;
                        yield_ptrs[j] = nullptr;
                    }
                }
            }

            // If not done, yield back to IDLE
            if (!done) (*parent_yield_ptr)();
        }

        // Cleanup any remaining coroutines
        for (auto* coro : coroutines) {
            if (coro) delete coro;
        }

        // Cleanup the shared coroutine structures
        active_workers.clear();
        polling.clear();
        yield_ptr = parent_yield_ptr;

        // Accumulate timing data of the last finished worker (gated)
        if (profile_prop_timing) {
            // Accumulate timing data of the last finished worker.
            // last_worker stays -1 only in the degenerate case where every
            // spawned lit coroutine completed synchronously without yielding
            // (the while(!done) body above is skipped). Guard to avoid UB
            // from vector[-1] indexing — mirrors the guard in propagateLiteral.
            if (last_worker >= 0 && last_worker < PARA_LITS) {
                cycles_read_headptr += lit_read_headptr[last_worker];
                cycles_read_watcher_blocks += lit_read_watcher_blocks[last_worker];
                cycles_read_clauses += lit_read_clauses[last_worker];
                cycles_insert_watchers += lit_insert_watchers[last_worker];
                cycles_polling += lit_polling[last_worker];
            }
        }

        // Stop if we've reached max_confl conflicts (unless max_confl is -1, meaning no limit)
        if (max_confl >= 0 && (int)conflicts.size() >= max_confl) {
            output.verbose(CALL_INFO, 3, 0, "PROPAGATE: max_confl reached, stop\n");
            qhead = trail.size();
        }
    }
    
    // Flush the final propagation interval, then zero the blocked count so the
    // gap until the next propagation phase is time-weighted as 0 stalled.
    if (track_stalls) {
        uint64_t now = getCurrentSimCycle() / 1000;
        uint64_t delta = now - last_round_cycle;
        if (delta > 0) stat_stalled_per_cycle->addDataNTimes(delta, last_blocked);
        last_blocked = 0;
        last_round_cycle = now;
    }

    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: no more propagations\n");
    return;
}

void SATSolver::propagateLiteral(
    Lit p,
    int lit_worker_id,
    uint64_t& read_headptr_cycles,
    uint64_t& read_watcher_blocks_cycles,
    uint64_t& read_clauses_cycles,
    uint64_t& insert_watchers_cycles,
    uint64_t& polling_cycles
) {
    // Check if this literal's variable was PROPAGATED (not just assigned) during speculative propagation
    // Check both current and previous speculative propagations with matching values
    Var v = var(p);
    bool is_speculative = false;
    if (spec_var_propagated[v] && spec_var_value[v] == var_value[v]) {
        is_speculative = true;
    } else if (prev_spec_var_propagated[v] && prev_spec_var_value[v] == var_value[v]) {
        is_speculative = true;
    }

    if (is_speculative)
        output.verbose(CALL_INFO, 2, 0, "lit %d was speculatively propagated\n", toInt(p));
    
    // assuming we are using base_worker_id when watcher coroutines are not launched
    int base_worker_id = lit_worker_id * PROPAGATORS;
    Lit not_p = ~p;
    int watch_idx = toWatchIndex(p);

    // Wait for any previous watchlist insertion
    if (wl_q.count(watch_idx) > 0) stat_wl_process_conflicts->addData(1);
    while (wl_q.count(watch_idx) > 0) {
        polling[base_worker_id] = true;
        (*yield_ptr)();  // Yield to allow other workers to process
    }
    polling[base_worker_id] = false;

    output.verbose(CALL_INFO, 2, 0,
        "PROPAGATE[L%d]: Processing watchers for literal %d\n", 
        lit_worker_id, toInt(not_p));

    // Measure time to read head pointer
    SST::Cycle_t start_headptr = profile_prop_timing ? (getCurrentSimCycle() / 1000) : 0;
    WatchMetaData wmd = watches.readMetaData(watch_idx, base_worker_id);
    if (profile_prop_timing) {
        SST::Cycle_t end_headptr = getCurrentSimCycle() / 1000;
        read_headptr_cycles += (end_headptr - start_headptr);
    }

    // Prefetch the next watch metadata if available
    if (qhead < trail.size()) {
        issuePrefetch(watches.watchesAddr(toWatchIndex(trail[qhead])));
    }

    bool do_prewatch = PRE_WATCHERS > 0;
    uint32_t curr_addr = wmd.head_ptr;
    uint32_t prev_addr = 0;
    WatcherBlock prev_block;

    uint64_t para_watchers = 0;  // watchers inspected in this propagation
    uint64_t watcher_occ = 0;    // number of watchers residing in watch lists

    // Rebuilt (singly linked) free list head, reconstructed from scratch as we
    // walk every block below. This literal's watchlist is exclusively ours for
    // the whole traversal (no other worker inserts into a literal that is being
    // propagated), so we publish the new free_head with a single write at the end.
    uint32_t rebuilt_free_head = 0;

    // Traverse the linked list
    while (curr_addr != 0 || do_prewatch) {
        bool block_modified = false;
        WatcherBlock curr_block;
        if (do_prewatch) {
            curr_block.setNextBlock(curr_addr);
            for (int i = 0; i < PRE_WATCHERS; i++) {
                curr_block.nodes[i] = wmd.pre_watchers[i];
            }

            if (curr_addr != 0) issuePrefetch(curr_addr);
        } else {
            // Read current block with optional timing
            SST::Cycle_t start_block = profile_prop_timing ? (getCurrentSimCycle() / 1000) : 0;
            curr_block = watches.readBlock(curr_addr, base_worker_id);
            if (profile_prop_timing) {
                SST::Cycle_t end_block = getCurrentSimCycle() / 1000;
                read_watcher_blocks_cycles += (end_block - start_block);
            }

            if (curr_block.getNextBlock() != 0) issuePrefetch(curr_block.getNextBlock());
        }

        // Collect valid nodes that need processing
        std::vector<int> valid_nodes;
        for (int i = 0; i < PROPAGATORS; i++) {
            if (!curr_block.nodes[i].valid) continue;
            watcher_occ++;

            Lit blocker = curr_block.nodes[i].blocker;
            if (var_assigned[var(blocker)] && value(blocker) == true) {
                // Blocker is true, skip to next watcher
                output.verbose(CALL_INFO, 4, 0,
                    "PROPAGATE[L%d]: Watch block[%d]: clause 0x%x, blocker %d = True, skipping\n", 
                    lit_worker_id, i, curr_block.nodes[i].getClauseAddr(), toInt(blocker));
                continue;
            }

            valid_nodes.push_back(i);
        }
        para_watchers += valid_nodes.size();
        
        // Propagate watchers in parallel batches
        coro_t::push_type* parent_yield_ptr = yield_ptr;
        int workers = std::min(PROPAGATORS, (int)valid_nodes.size());
        std::vector<coro_t::pull_type*> coroutines(workers);
        std::vector<coro_t::push_type*> yield_ptrs(workers);
        bool done = true;
        int last_worker = -1;

        // Track cycles for worker operations
        std::vector<uint64_t> worker_read_clauses(PROPAGATORS, 0);
        std::vector<uint64_t> worker_insert_watchers(PROPAGATORS, 0);
        std::vector<uint64_t> worker_polling(PROPAGATORS, 0);
        output.verbose(CALL_INFO, 4, 0, "PROPAGATE[L%d]: spawning %d watcher coroutines\n",
            lit_worker_id, workers);
        // Create watcher coroutines
        for (int worker_id = 0; worker_id < workers; worker_id++) {
            coroutines[worker_id] = new coro_t::pull_type(
                [this, worker_id, lit_worker_id, &valid_nodes, not_p,
                 &block_modified, &curr_block, &yield_ptrs,
                 &worker_read_clauses, &worker_insert_watchers, &worker_polling
                ](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    yield_ptrs[worker_id] = yield_ptr;
                    propagateWatchers(valid_nodes[worker_id], not_p, block_modified, curr_block,
                                      lit_worker_id, worker_id,
                                      worker_read_clauses[worker_id],
                                      worker_insert_watchers[worker_id],
                                      worker_polling[worker_id]);
                });
            if (*coroutines[worker_id]) done = false;  // may finish without yielding
        }

        if (!done) (*parent_yield_ptr)();  // yield back to IDLE

        // stepping sub-coroutines
        while (!done) {
            // Check if any worker is active
            for (int j = 0; j < workers; j++) {
                // printf("Worker %d: active=%d, polling=%d, done=%d\n",
                //     j, (bool)active_workers[base_worker_id + j], (bool)polling[base_worker_id + j], (bool)(coroutines[j] == nullptr));
                if (active_workers[base_worker_id + j]) {
                    yield_ptr = yield_ptrs[j];
                    (*coroutines[j])();
                    active_workers[base_worker_id + j] = false;
                }
            }

            // since the polling workers never get triggered,
            // we need to check them after completing the active workers
            // polling status may also change after processing active workers
            for (int j = 0; j < workers; j++) {
                if (polling[base_worker_id + j]) {
                    polling[base_worker_id + j] = false;
                    yield_ptr = yield_ptrs[j];
                    (*coroutines[j])();
                }
            }

            // check for done
            done = true;
            for (int j = 0; j < workers; j++) {
                if (coroutines[j] != nullptr) {
                    if (*coroutines[j]) done = false;
                    else {
                        last_worker = j; // Track the last worker to complete
                        delete coroutines[j];
                        coroutines[j] = nullptr;
                        yield_ptrs[j] = nullptr;
                    }
                }
            }

            if (!done) (*parent_yield_ptr)();  // yield back to IDLE
        }
        // finished all sub-coroutines
        yield_ptr = parent_yield_ptr;
        output.verbose(CALL_INFO, 4, 0, "PROPAGATE[L%d]: Finished a watch block\n", lit_worker_id);

        // After all workers finished, accumulate timing data to the literal-level counters
        // Only accumulate if we had valid workers (last_worker will be 0 for single worker, -1 if none completed)
        if (profile_prop_timing && last_worker >= 0 && last_worker < workers) {
            // Accumulate to literal-level counters (passed by reference from unitPropagate)
            read_clauses_cycles += worker_read_clauses[last_worker];
            insert_watchers_cycles += worker_insert_watchers[last_worker];
            polling_cycles += worker_polling[last_worker];
        }
        
        // After all workers finished, write back the pre-watchers or the block.
        if (do_prewatch) {
            // Pre-watchers live in metadata and never join the free list.
            if (block_modified) watches.writePreWatchers(watch_idx, curr_block.nodes);
        } else {
            // Rebuild the free list for kept blocks; emptied blocks are freed
            // by updateBlock and simply not prepended. This is authoritative
            // over free_index: any stale value from a prior traversal is
            // overwritten here.
            bool block_freed = curr_block.countValidNodes() == 0;
            if (USE_FREE_LIST && !block_freed) {
                int slot = curr_block.firstFreeSlot();
                if (slot != -1) {
                    // Prepend this block: its linkage node points at the running head.
                    curr_block.nodes[slot] = WatcherNode(rebuilt_free_head);
                    curr_block.free_index = slot;
                    rebuilt_free_head = curr_addr | slot;
                    block_modified = true;
                } else if (curr_block.free_index != PROPAGATORS) {
                    // Full block: clear any stale free-list membership.
                    curr_block.free_index = PROPAGATORS;
                    block_modified = true;
                }
            }

            if (block_modified)
                watches.updateBlock(watch_idx, prev_addr, curr_addr, prev_block, curr_block, wmd);

            // the current block is deleted if it has no valid nodes left
            if (!block_freed) {
                prev_addr = curr_addr;
                prev_block = curr_block;
            }
        }

        if (max_confl >= 0 && (int)conflicts.size() >= max_confl) break;

        // Move to next block
        curr_addr = curr_block.getNextBlock();
        block_modified = false;
        do_prewatch = false;
    }

    // Publish the rebuilt free list. On an early break above this reflects only
    // the blocks visited so far; unvisited blocks are simply not referenced by
    // free_head (their stale linkage is inert and gets rebuilt next time).
    if (USE_FREE_LIST && wmd.free_head != rebuilt_free_head) {
        watches.writeFreeHead(watch_idx, rebuilt_free_head);
        wmd.free_head = rebuilt_free_head;
    }

    stat_para_watchers->addData(para_watchers);
    stat_watcher_occ->addData(watcher_occ);
    if (profile_2wl) {
        stat_total_occ->addDataNTimes(lit_occ_count[watch_idx], 1);
        stat_watcher_traversed->addDataNTimes(watcher_occ, 1);
    }

    // Track metrics based on whether this literal was speculative (gated)
    if (profile_prop_timing) {
        if (is_speculative) {
            // This variable was assigned during speculative propagation
            speculative_metrics.read_headptr_cycles += read_headptr_cycles;
            speculative_metrics.read_watcher_blocks_cycles += read_watcher_blocks_cycles;
            speculative_metrics.read_clauses_cycles += read_clauses_cycles;
            speculative_metrics.count++;
        } else {
            // Normal (non-speculative) propagation
            normal_metrics.read_headptr_cycles += read_headptr_cycles;
            normal_metrics.read_watcher_blocks_cycles += read_watcher_blocks_cycles;
            normal_metrics.read_clauses_cycles += read_clauses_cycles;
            normal_metrics.count++;
        }
    }
}

void SATSolver::propagateWatchers(
    int watcher_i,
    Lit not_p,
    bool& block_modified,
    WatcherBlock& curr_block,
    int lit_worker_id,
    int worker_id,
    uint64_t& read_clauses_cycles,
    uint64_t& insert_watchers_cycles,
    uint64_t& polling_cycles
) {
    // Calculate the global worker ID for reporting
    int global_worker_id = lit_worker_id * PROPAGATORS + worker_id;

    // Need to inspect the clause
    Cref clause_addr = curr_block.nodes[watcher_i].getClauseAddr();

    // Check if the clause is already being processed by another worker
    SST::Cycle_t start_poll = profile_prop_timing ? (getCurrentSimCycle() / 1000) : 0;
    if (clause_locks.count(clause_addr) > 0) stat_clause_conflicts->addData(1);
    while (clause_locks.count(clause_addr) > 0) {
        polling[global_worker_id] = true;
        (*yield_ptr)();  // Yield to allow other workers to process
    }
    if (profile_prop_timing) {
        SST::Cycle_t end_poll = getCurrentSimCycle() / 1000;
        polling_cycles += (end_poll - start_poll);
    }

    // Lock the clause
    clause_locks.insert(clause_addr);
    stat_clause_lock_occ->addData(clause_locks.size());

    // Time the reading of clauses (gated)
    SST::Cycle_t start_read = profile_prop_timing ? (getCurrentSimCycle() / 1000) : 0;
    Clause c = clauses.readClause(clause_addr, global_worker_id);
    if (profile_prop_timing) {
        SST::Cycle_t end_read = getCurrentSimCycle() / 1000;
        read_clauses_cycles += (end_read - start_read);
    }

    // Print clause for debugging
    output.verbose(CALL_INFO, 4, 0,
        "[L%d-W%d] Watch block[%d]: blocker:%d, clause 0x%x: %s\n",
        lit_worker_id, worker_id, watcher_i, toInt(curr_block.nodes[watcher_i].blocker),
        clause_addr, printClause(c.literals).c_str());

    // Make sure the false literal (~p) is at position 1
    if (c[0] == not_p) {
        std::swap(c.literals[0], c.literals[1]);
        clauses.writeLiteral(clause_addr, c[0], 0);
        clauses.writeLiteral(clause_addr, c[1], 1);
        output.verbose(CALL_INFO, 4, 0, "  Swapped literals 0 and 1\n");
    }
    sst_assert(c[1] == not_p, CALL_INFO, -1, "Second literal %d is not %d", toInt(c[1]), toInt(not_p));

    // If first literal is already true, just update the blocker and continue
    Lit first = c[0];
    if (var_assigned[var(first)] && value(first) == true) {
        output.verbose(CALL_INFO, 4, 0,
            "  First literal %d is true\n", toInt(first));
        curr_block.nodes[watcher_i].blocker = first;
        block_modified = true;

        // Release the lock on this clause
        clause_locks.erase(clause_addr);
        return;
    }

    // Look for a new literal to watch
    for (size_t k = 2; k < c.litSize(); k++) {
        Lit lit = c[k];
        if (!var_assigned[var(lit)] || value(lit) == true) {
            // Swap to position 1 and update watcher
            std::swap(c.literals[1], c.literals[k]);
            clauses.writeLiteral(clause_addr, c[1], 1);
            clauses.writeLiteral(clause_addr, c[k], k);
            output.verbose(CALL_INFO, 4, 0, 
                "  Found new watch: literal %d at position %zu\n", toInt(c[1]), k);

            wl_q.add(toWatchIndex(~c[1]));
            stat_wl_q_occ->addData(wl_q.total());

            // Time spent polling for busy watches (gated)
            SST::Cycle_t start_poll2 = profile_prop_timing ? (getCurrentSimCycle() / 1000) : 0;
            if (watches.isBusy(toWatchIndex(~c[1]))) stat_wl_insert_conflicts->addData(1);
            while (watches.isBusy(toWatchIndex(~c[1]))) {
                polling[global_worker_id] = true;
                (*yield_ptr)();  // Yield to allow other workers to process
            }
            if (profile_prop_timing) {
                SST::Cycle_t end_poll2 = getCurrentSimCycle() / 1000;
                polling_cycles += (end_poll2 - start_poll2);
            }

            output.verbose(CALL_INFO, 5, 0, "  [L%d-W%d]Start watchlist insertion\n",
                lit_worker_id, worker_id);

            // Occupancy of the watchlist write-lock table during this insert.
            // inserts_in_flight mirrors Watches::busy.size() (one distinct
            // watchlist per in-flight insertWatcher call).
            inserts_in_flight++;
            stat_busy_occ->addData(inserts_in_flight);

            // Time spent inserting watchers (gated)
            SST::Cycle_t start_insert = profile_prop_timing ? (getCurrentSimCycle() / 1000) : 0;
            int block_visits = watches.insertWatcher(toWatchIndex(~c[1]), clause_addr, first, global_worker_id);
            if (profile_prop_timing) {
                SST::Cycle_t end_insert = getCurrentSimCycle() / 1000;
                insert_watchers_cycles += (end_insert - start_insert);
            }
            inserts_in_flight--;

            // Record block visits statistics
            stat_watcher_blocks->addData(block_visits);

            // Mark this node as invalid in the current block
            curr_block.nodes[watcher_i].valid = 0;
            block_modified = true;

            // Release the lock on this clause
            clause_locks.erase(clause_addr);
            wl_q.remove(toWatchIndex(~c[1]));
            return;
        }
    }

    // Did not find a new watch - clause is unit or conflicting
    output.verbose(CALL_INFO, 5, 0, "  No new watch found\n");

    // Check if first literal is false (conflict) or undefined (unit)
    if (var_assigned[var(first)] && value(first) == false) {
        // Conflict detected
        if (std::find(conflicts.begin(), conflicts.end(), clause_addr) == conflicts.end()
            && (max_confl < 0 || (int)conflicts.size() < max_confl)) {
            conflicts.push_back(clause_addr);
            if (tracer_) tracer_->emitConflict((int)clause_addr);
            output.verbose(CALL_INFO, 3, 0,
                "  Conflict #%zu: Clause 0x%x has all literals false\n",
                conflicts.size(), clause_addr);
        } else { output.verbose(CALL_INFO, 3, 0, "  Conflict, but ignored\n"); }
    } else {
        // Unit clause found, propagate
        output.verbose(CALL_INFO, 4, 0, "  forces literal %d (to true)\n", toInt(first));
        if (conflicts.empty()) trailEnqueue(first, clause_addr);
    }

    // Release the lock on this clause
    clause_locks.erase(clause_addr);
}

// Add a new method to issue prefetches
void SATSolver::issuePrefetch(uint64_t addr) {
    if (prefetch_enabled) {
        output.verbose(CALL_INFO, 4, 0, "Issuing prefetch for address 0x%lx\n", addr);
        prefetch_link->send(new PrefetchRequestEvent(addr));
    }
}

//-----------------------------------------------------------------------------------
// analyze
//-----------------------------------------------------------------------------------

void SATSolver::analyze(Cref conflict, int worker_id) {
    output.verbose(CALL_INFO, 4, 0,
        "ANALYZE[%d]: Starting conflict analysis of clause 0x%x\n", worker_id, conflict);

    // First UIP scheme
    std::vector<Lit> tmp_learnt;
    tmp_learnt.resize(1);  // Reserve space for the asserting literal
    std::vector<char> tmp_seen;
    tmp_seen.resize(num_vars + 1, 0);
    int tmp_btlevel = 0;
    int tmp_lbd = 0;
    std::vector<bool> lbd_levels(current_level() + 1, false);
    std::vector<std::pair<Cref, float>> tmp_c_to_bump;
    std::vector<Var> tmp_v_to_bump;

    int pathC = 0;  // Counter for literals at current decision level
    Lit p = lit_Undef;
    int index = trail.size() - 1;
    
    // Add literals from conflict clause to learnt clause
    do {
        sst_assert(conflict != ClauseRef_Undef, CALL_INFO, -1,  // (otherwise should be UIP)
            "ANALYZE[%d]: conflict clause is undefined\n", worker_id);
        const Clause& c = clauses.readClause(conflict, worker_id);
        
        // Bump activity for learnt clauses. The clause is in hand, so remember
        // its current activity too — the bump loop then needs no re-read.
        if (clauses.isLearnt(conflict)) tmp_c_to_bump.push_back({conflict, c.act()});

        // Debug print for current clause
        output.verbose(CALL_INFO, 5, 0, "ANALYZE[%d]: current clause (0x%x): %s\n",
            worker_id, conflict, printClause(c.literals).c_str());

        // For each literal in the clause
        for (size_t i = (p == lit_Undef) ? 0 : 1; i < c.litSize(); i++) {
            Lit q = c[i];
            Var v = var(q);
            output.verbose(CALL_INFO, 5, 0,
                "ANALYZE[%d]: Processing literal %d\n", worker_id, toInt(q));

            // Read variable data individually for each variable in the conflict clause
            Variable v_data = variables.readVar(v, worker_id);

            if (!tmp_seen[v] && v_data.level > 0) {
                // Bump activity for seen variables
                tmp_v_to_bump.push_back(v);

                tmp_seen[v] = 1;
                lbd_levels[v_data.level] = true;  // Track distinct levels for LBD
                output.verbose(CALL_INFO, 5, 0,
                    "ANALYZE[%d]:     Marking var %d as seen\n", worker_id, v);

                if (v_data.level >= current_level()) {
                    pathC++;  // Count literals at current decision level
                    output.verbose(CALL_INFO, 5, 0,
                        "ANALYZE[%d]:     At current level, pathC=%d\n", worker_id, pathC);
                } else {
                    if (v_data.level > tmp_btlevel) tmp_btlevel = v_data.level;
                    // Literals from earlier decision levels go directly to the learnt clause
                    tmp_learnt.push_back(q);
                    output.verbose(CALL_INFO, 5, 0,
                        "ANALYZE[%d]:     Added to learnt clause (earlier level %zu)\n",
                        worker_id, v_data.level);
                }
            }
        }

        // Select next literal to expand from the trail
        // HACK: may need to check the entire trail if trail is not in order
        // index = trail.size() - 1;
        while (!tmp_seen[var(trail[index--])]);
        p = trail[index+1];
        conflict = variables.getReason(var(p), worker_id);

        tmp_seen[var(p)] = 0;
        pathC--;
        
        output.verbose(CALL_INFO, 5, 0,
            "ANALYZE[%d]: Selected trail literal %d, index %d, reason=0x%x, pathC=%d\n",
            worker_id, toInt(p), index, conflict, pathC);

    } while (pathC > 0);
    
    // Add the 1-UIP literal as the first in the learnt clause
    tmp_learnt[0] = ~p;

    // Compute LBD (number of distinct decision levels in learnt clause)
    for (size_t i = 0; i < lbd_levels.size(); i++) {
        if (lbd_levels[i]) tmp_lbd++;
    }

    // Print learnt clause for debug
    output.verbose(CALL_INFO, 4, 0, "ANALYZE[%d]: learnt: %s, bt_level=%d, lbd=%d\n",
        worker_id, printClause(tmp_learnt).c_str(), tmp_btlevel, tmp_lbd);

    // Track the spread of backtrack levels across the conflicts analyzed this
    // round, so we can measure whether multiple conflicts ever disagree (and
    // thus whether selecting among them can change the backtrack target).
    if (tmp_btlevel > round_max_bt) round_max_bt = tmp_btlevel;

    // Keep the single best candidate: lowest backtrack level, then smallest
    // clause. Ties keep whichever candidate was selected first.
    if (tmp_btlevel < bt_level || (tmp_btlevel == bt_level
        && tmp_learnt.size() < learnt_clause.size())) {
        bt_level = tmp_btlevel;
        learnt_lbd = tmp_lbd;
        learnt_clause = std::move(tmp_learnt);
        seen = std::move(tmp_seen);
        c_to_bump = std::move(tmp_c_to_bump);
        v_to_bump = std::move(tmp_v_to_bump);
    }
    // order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::DEBUG_HEAP, 0));
}

//-----------------------------------------------------------------------------------
// find backtrack level
//-----------------------------------------------------------------------------------

void SATSolver::findBtLevel() {
    // Find backtrack level
    if (learnt_clause.size() == 1) {
        // 0 if only one literal in learnt clause
        bt_level = 0;
    } else {
        // Find the second highest level in the clause
        int max_i = 1;
        int max_level = variables.getLevel(var(learnt_clause[1]));

        for (int i = 2; i < learnt_clause.size(); i++) {
            int level_i = variables.getLevel(var(learnt_clause[i]));

            if (level_i > max_level) {
                max_i = i;
                max_level = level_i;
            }
        }
        
        // Swap-in this literal at index 1
        Lit p = learnt_clause[max_i];
        learnt_clause[max_i] = learnt_clause[1];
        learnt_clause[1] = p;
        bt_level = variables.getLevel(var(p));
    }

    output.verbose(CALL_INFO, 4, 0, "Backtrack Level = %d\n", bt_level);
    output.verbose(CALL_INFO, 4, 0, "Final learnt clause: %s\n",
        printClause(learnt_clause).c_str());
    state = BACKTRACK;
#ifdef USE_CLASSIC_HEAP
    if (OVERLAP_HEAP_BUMP) {
        state = WAIT_HEAP;
        next_state = BACKTRACK;
    } else state = BACKTRACK;
#endif
}

//-----------------------------------------------------------------------------------
// backtrack
//-----------------------------------------------------------------------------------

void SATSolver::backtrack(int backtrack_level) {
    output.verbose(CALL_INFO, 4, 0, "BACKTRACK From level %d to level %d\n",
        current_level(), backtrack_level);
    if (tracer_) tracer_->emitBacktrack(current_level(), backtrack_level);
    
    // Unassign all variables above backtrack_level using the trail
    for (int i = trail.size() - 1; i >= int(trail_lim[backtrack_level]); i--) {
        Lit p = trail[i];
        Var v = var(p);
        
        polarity[v] = sign(p);
        unassignVariable(v);
        
        // peek version
        // insertVarOrder(v);

        // decide with spec_literal
        if (v != var(spec_literal)) {
            insertVarOrder(v);
        }
        
        output.verbose(CALL_INFO, 5, 0,
            "BACKTRACK: Unassigning x%d, saved polarity %s\n", 
            v, polarity[v] ? "false" : "true");
    }
    
    qhead = trail_lim[backtrack_level];
    trail.resize(trail_lim[backtrack_level]);
    trail_lim.resize(backtrack_level);
}

//-----------------------------------------------------------------------------------
// clause deletion
//-----------------------------------------------------------------------------------
// Remove half of the learnt clauses,
// minus the clauses locked by the current assignment.
// Locked clauses are clauses that are reason to some assignment.
// Binary clauses are never removed (skipped by address range, zero reads).
//
// Streaming design: the on-chip activity histogram provides the median
// threshold without any sorting or activity sweep. One pass over the learnt
// pointer array (double-buffered stream) fans clauses out to REDUCE_WORKERS
// coroutines, each reading only the 16 B clause head; decisions commit in
// index order so survivors compact in place (no O(N) to_keep buffer). Frees
// funnel through a single free-engine coroutine (shared allocator state).
void SATSolver::reduceDB() {
    output.verbose(CALL_INFO, 4, 0, "REDUCEDB: Starting clause database reduction\n");

    const size_t nl = nLearnts();
    if (nl == 0) return;

    // --- Threshold select (on-chip prefix scan; latency modeled below) ---
    // MiniSat removes the bottom half of the activity-sorted learnt list;
    // binaries sort to the top, so the bottom half is non-binary as long as
    // binaries are a minority (the clamp handles the degenerate case).
    uint64_t target = std::min((uint64_t)(nl / 2), cla_hist_.total());
    uint64_t below = 0;
    int t_bucket = cla_hist_.selectThreshold(target, below);
    uint64_t quota = target - below;  // tie-bucket allowance, consumed in commit order
    double extra_lim = cla_inc / nl;  // Extra activity limit for removal

    output.verbose(CALL_INFO, 4, 0,
        "REDUCEDB: %zu learnts, target %lu, t_bucket %d, quota %lu, extra_lim = %f\n",
        nl, target, t_bucket, quota, extra_lim);

    // Model the serial NUM_BUCKETS-cycle scan: commits gate on scan_done_.
    scan_done_ = false;
    reduce_scan_link_->send(ActivityHistogram::NUM_BUCKETS, new ReduceScanEvent());

    // --- Streaming pass ---
    const int W = REDUCE_WORKERS;
    const int STREAMER = W;
    const int FREEER = W + 1;
    const int TOTAL = W + 2;
    const size_t CHUNK_CREFS = 64;  // 256 B of pointer stream per fetch (4 lines)

    coro_t::push_type* parent_yield_ptr = yield_ptr;
    active_workers.assign(TOTAL, false);
    polling.assign(TOTAL, false);

    std::deque<std::pair<size_t, Cref>> dispatch;  // (learnt idx, clause addr)
    std::deque<std::pair<Cref, uint32_t>> free_q;  // (clause addr, num_lits)
    std::unordered_set<int> wl_busy;               // watchlists under surgery
    size_t stream_pos = 0;      // next learnt idx the streamer fetches
    bool stream_done = false;
    size_t next_commit = 0;     // in-order commit cursor
    size_t write_idx = 0;       // in-place compaction write position
    int workers_running = W;
    int removed = 0;

    std::vector<coro_t::pull_type*> coros(TOTAL, nullptr);
    std::vector<coro_t::push_type*> yptrs(TOTAL, nullptr);

    // Pointer-array streamer: double-buffered chunk fetches. While a chunk is
    // in flight the workers keep draining the previous one — the streamer's
    // blocked time is the overlap.
    auto streamer_body = [&]() {
        while (stream_pos < nl) {
            while (dispatch.size() >= CHUNK_CREFS) {
                polling[STREAMER] = true;
                (*yield_ptr)();
            }
            polling[STREAMER] = false;
            size_t cnt = std::min(CHUNK_CREFS, nl - stream_pos);
            std::vector<Cref> addrs = clauses.readAddrChunk(stream_pos, cnt, STREAMER);
            for (size_t k = 0; k < cnt; k++)
                dispatch.push_back({stream_pos + k, addrs[k]});
            stream_pos += cnt;
        }
        stream_done = true;
    };

    // Clause worker: one 16 B head read per clause (activity + both watched
    // literals), speculative reason pre-read for potential candidates, then a
    // memory-free in-order commit; watchlist surgery happens post-commit.
    auto worker_body = [&](int w) {
        while (true) {
            if (dispatch.empty()) {
                if (stream_done) break;
                polling[w] = true;
                (*yield_ptr)();
                polling[w] = false;
                continue;
            }
            size_t idx = dispatch.front().first;
            Cref addr = dispatch.front().second;
            dispatch.pop_front();

            bool is_bin = clauses.isBinaryLearnt(addr);
            float act = 0.0f;
            Lit c0 = lit_Undef, c1 = lit_Undef;
            uint32_t nlits = 0;
            int b = 0;
            bool reason_read = false;
            Cref reason = ClauseRef_Undef;

            if (!is_bin) {
                ClauseHead h = clauses.readClauseHead(addr, w);
                act = h.activity; c0 = h.l0; c1 = h.l1; nlits = h.num_lits;
                b = ActivityHistogram::bucketOf(act);
                // Locked short-circuit: the reason read is issued only for a
                // potential removal candidate whose first literal is
                // currently assigned true (sparse at reduce time).
                bool candidate_possible = (b <= t_bucket) || ((double)act < extra_lim);
                if (candidate_possible && var_assigned[var(c0)] && value(c0)) {
                    reason = variables.getReason(var(c0), w);
                    reason_read = true;
                }
            }

            // In-order, memory-free commit; also gated on the threshold scan.
            while (!(next_commit == idx && scan_done_)) {
                polling[w] = true;
                (*yield_ptr)();
            }
            polling[w] = false;

            bool remove = false;
            if (!is_bin) {
                bool bottom_half = (b < t_bucket) || (b == t_bucket && quota > 0);
                if (b == t_bucket && quota > 0) quota--;  // consumed by encounter
                remove = bottom_half || ((double)act < extra_lim);
                if (remove && reason_read && reason == addr) remove = false;  // locked
            }

            if (remove) {
                output.verbose(CALL_INFO, 4, 0,
                    "REDUCEDB: Marking clause 0x%x for removal\n", addr);
                cla_hist_.remove(act);
                removed++;
                next_commit++;
                // Watchlist surgery after releasing the commit cursor. Both
                // lists are acquired atomically (no yield between test and
                // set), so concurrent removals cannot deadlock.
                int l0 = toWatchIndex(~c0), l1 = toWatchIndex(~c1);
                while (wl_busy.count(l0) || wl_busy.count(l1)) {
                    polling[w] = true;
                    (*yield_ptr)();
                }
                polling[w] = false;
                wl_busy.insert(l0);
                wl_busy.insert(l1);
                watches.removeWatcher(l0, addr, w);
                watches.removeWatcher(l1, addr, w);
                wl_busy.erase(l0);
                wl_busy.erase(l1);
                free_q.push_back({addr, nlits});
            } else {
                clauses.compactKeep(write_idx, addr);  // fire-and-forget write
                write_idx++;
                next_commit++;
            }
        }
        workers_running--;
    };

    // Free engine: drains removals into the allocator. Frees mutate shared
    // free-list state, so they stay sequential but pipeline behind the
    // removal workers instead of blocking them.
    auto freeer_body = [&]() {
        while (true) {
            if (free_q.empty()) {
                if (workers_running == 0) break;
                polling[FREEER] = true;
                (*yield_ptr)();
                polling[FREEER] = false;
                continue;
            }
            Cref addr = free_q.front().first;
            uint32_t nlits = free_q.front().second;
            free_q.pop_front();
            clauses.freeClause(addr, nlits, FREEER);
        }
    };

    // Spawn all sub-coroutines (streamer, W workers, free engine).
    for (int t = 0; t < TOTAL; t++) {
        coros[t] = new coro_t::pull_type(
            [this, t, W, STREAMER, &yptrs, &streamer_body, &worker_body, &freeer_body]
            (coro_t::push_type& yield) {
                yield_ptr = &yield;
                yptrs[t] = yield_ptr;
                if (t == STREAMER) streamer_body();
                else if (t < W) worker_body(t);
                else freeer_body();
            });
        if (!(*coros[t])) {
            delete coros[t];
            coros[t] = nullptr;
        }
    }

    // Stepping loop: run rounds until a full round makes no progress, then
    // yield for the next external event (memory response or the scan event).
    // Progress-based re-rounds let in-order commits cascade without waiting
    // for another event per commit.
    bool all_done = false;
    while (!all_done) {
        bool progress = true;
        while (progress) {
            progress = false;
            size_t snap = next_commit + stream_pos + write_idx
                        + (size_t)removed + free_q.size() + dispatch.size();
            for (int t = 0; t < TOTAL; t++) {
                if (coros[t] == nullptr) continue;
                if (active_workers[t] || polling[t]) {
                    active_workers[t] = false;
                    yield_ptr = yptrs[t];
                    (*coros[t])();
                    if (!(*coros[t])) {
                        delete coros[t];
                        coros[t] = nullptr;
                        polling[t] = false;
                        progress = true;
                    }
                }
            }
            size_t snap2 = next_commit + stream_pos + write_idx
                         + (size_t)removed + free_q.size() + dispatch.size();
            if (snap2 != snap) progress = true;
        }
        all_done = true;
        for (int t = 0; t < TOTAL; t++) {
            if (coros[t] != nullptr) all_done = false;
        }
        if (!all_done) (*parent_yield_ptr)();
    }

    // finished all sub-coroutines
    active_workers.clear();
    polling.clear();
    yield_ptr = parent_yield_ptr;

    sst_assert(next_commit == nl && (size_t)removed + write_idx == nl,
        CALL_INFO, -1, "REDUCEDB: commit bookkeeping mismatch (%zu/%zu/%d)\n",
        next_commit, write_idx, removed);
    clauses.finishReduce(write_idx);

    output.verbose(CALL_INFO, 4, 0,
        "REDUCEDB: Removed %d learnt clauses, new clause count: %zu\n",
        removed, clauses.size());

    stat_db_reductions->addData(1);
    stat_removed->addDataNTimes(removed, 1);
    if (tracer_) tracer_->emitReduce(removed, (int)write_idx);
}

//-----------------------------------------------------------------------------------
// Trail Management
//-----------------------------------------------------------------------------------

void SATSolver::trailEnqueue(Lit literal, int reason) {
    Var v = var(literal);
    if (tracer_) tracer_->emitEnqueue(v, sign(literal), reason);
    var_assigned[v] = true;
    var_value[v] = !sign(literal);
    
    Variable var_data;
    var_data.level = current_level();
    var_data.reason = reason;
    variables[v] = var_data;
    
    // Add to trail
    trail.push_back(literal);
    stat_assigns->addData(1);
    output.verbose(CALL_INFO, 6, 0,"ASSIGN: x%d = %d at level %d due to clause %d\n", 
        v, var_value[v] ? 1 : 0, current_level(), reason);
}

void SATSolver::unassignVariable(Var v) {
    var_assigned[v] = false;
    stat_unassigns->addData(1);
}

//-----------------------------------------------------------------------------------
// Two-Watched Literals
//-----------------------------------------------------------------------------------

void SATSolver::attachClause(Cref clause_addr, const Clause& c) {
    // Watch the first two literals in the clause, use each other as a blocker
    output.verbose(CALL_INFO, 5, 0, "ATTACH: clause 0x%x with literals %d and %d\n",
        clause_addr, toInt(c[0]), toInt(c[1]));
    watches.insertWatcher(toWatchIndex(~c[0]), clause_addr, c[1]);
    watches.insertWatcher(toWatchIndex(~c[1]), clause_addr, c[0]);
}

// Pipelined x2^-66 rescale of every learnt clause activity (binaries
// included — they are bumped like any learnt clause, just not histogrammed).
// W workers each stream their own chunks of the pointer array and RMW the
// 4 B activity of each clause; denormal results are flushed to zero so the
// stored floats match the histogram's bucket-0 collapse exactly.
void SATSolver::rescaleAllActivities() {
    const size_t nl = nLearnts();
    if (nl == 0) return;
    const int W = REDUCE_WORKERS;
    const size_t CHUNK_CREFS = 64;
    const size_t num_chunks = (nl + CHUNK_CREFS - 1) / CHUNK_CREFS;

    coro_t::push_type* parent_yield_ptr = yield_ptr;
    active_workers.assign(W, false);

    std::vector<coro_t::pull_type*> coros(W, nullptr);
    std::vector<coro_t::push_type*> yptrs(W, nullptr);

    auto sweep_body = [&](int w) {
        for (size_t chunk = w; chunk < num_chunks; chunk += W) {
            size_t start = chunk * CHUNK_CREFS;
            size_t cnt = std::min(CHUNK_CREFS, nl - start);
            std::vector<Cref> addrs = clauses.readAddrChunk(start, cnt, w);
            for (size_t k = 0; k < cnt; k++) {
                float act = clauses.readAct(addrs[k], w);
                clauses.writeAct(addrs[k], ActivityHistogram::rescaleValue(act));
            }
        }
    };

    bool done = true;
    for (int w = 0; w < W; w++) {
        coros[w] = new coro_t::pull_type(
            [this, w, &yptrs, &sweep_body](coro_t::push_type& yield) {
                yield_ptr = &yield;
                yptrs[w] = yield_ptr;
                sweep_body(w);
            });
        if (*coros[w]) done = false;
        else { delete coros[w]; coros[w] = nullptr; }
    }
    if (!done) (*parent_yield_ptr)();

    while (!done) {
        done = true;
        for (int w = 0; w < W; w++) {
            if (coros[w] == nullptr) continue;
            if (active_workers[w]) {
                active_workers[w] = false;
                yield_ptr = yptrs[w];
                (*coros[w])();
                if (!(*coros[w])) {
                    delete coros[w];
                    coros[w] = nullptr;
                }
            }
            if (coros[w] != nullptr) done = false;
        }
        if (!done) (*parent_yield_ptr)();
    }

    active_workers.clear();
    yield_ptr = parent_yield_ptr;
}


//-----------------------------------------------------------------------------------
// Decision Heuristics
//-----------------------------------------------------------------------------------
Lit SATSolver::chooseBranchVariable() {
    // Bring-up probe: verify heap invariants before every decision (slow;
    // one full activity-array burst per decision). Note heap_resp_cnt++ is
    // required or handleHeapResponse underflows the counter.
    // order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::DEBUG_HEAP));
    // heap_resp_cnt++;
    // (*yield_ptr)();
    // sst_assert(heap_resp == 0, CALL_INFO, -1,
    //     "Heap corrupted: DEBUG_HEAP reported %d errors\n", heap_resp);

    // Skip the random probe while a rebuild is still queued: the probe
    // indexes by the solver-side (pre-wipe) size and would race the wipe.
    Var next = var_Undef;
    if (!order_heap->rebuildQueued() && !order_heap->empty()
        && drand(random_seed) < random_var_freq) {
        int rand_idx = irand(random_seed, order_heap->size());
        order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::READ, rand_idx));
        heap_resp_cnt++;
        (*yield_ptr)();
        next = heap_resp;

        if (!var_assigned[next] && decision[next]) {
            output.verbose(CALL_INFO, 3, 0, "DECISION: Random selection of var %d\n", next);
        }
    }

    while (next == var_Undef || var_assigned[next] || !decision[next]) {
        order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::REMOVE_MAX));
        heap_resp_cnt++;
        (*yield_ptr)();
        next = heap_resp;
        if (next == var_Undef && order_heap->empty()) break;
    }

    if (next == var_Undef) return lit_Undef;
    return mkLit(next, polarity[next]);
}

Lit SATSolver::peekBranchVariable() {
    if (order_heap->empty()) return lit_Undef;

    Var next = var_Undef;
    int idx = 1;
    while (next == var_Undef) {
        order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::READ, idx));
        heap_resp_cnt++;
        (*yield_ptr)();
        if (!var_assigned[heap_resp] && decision[heap_resp]) next = heap_resp;
        if (idx >= order_heap->size()) break;
        idx++;
    }

    return mkLit(next, polarity[next]);
}

// A rebuild pays only when the heap extends off-chip (there are crossings and
// deep stale-discard pops to remove); on-chip stale copies cost at most one
// extra percolation level, never worth a full rebuild's tie-break reshuffle.
// staleCount > num_vars preserves the amortization: a rebuild costs ~num_vars
// inserts and can only fire after >= num_vars stale creations, i.e. <= ~1
// cycle per stale-creating op at any instance size. rebuildQueued() blocks
// re-fires while a queued wipe is undispatched (solver-side staleCount()
// reads the pre-wipe value until then). Inert for the classic heap, whose
// in-place updates never create stales.
bool SATSolver::heapRebuildDue() const {
    return !order_heap->rebuildQueued()
        && order_heap->staleCount() > (size_t)num_vars
        && order_heap->isOffChip();
}

// Completes a rebuild armed before a backtrack: ends the suppression window
// (the unwind's inserts fold into the wave), wipes, and reinserts the
// now-current unassigned set; trail vars re-enter through later backtracks
// with their bumped activities. Heap FIFO order plus the REMOVE_MAX drain
// gate make the rebuild synchronous with the next decision, and the wave
// drains under the intervening propagation.
void SATSolver::fireHeapRebuild() {
    suppress_heap_inserts_ = false;
    order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::REBUILD));
    for (Var v = 1; v <= (Var)num_vars; v++)
        if (!var_assigned[v]) insertVarOrder(v);
}

void SATSolver::insertVarOrder(Var v) {
    // Suppressed during a rebuild's trail unwind: the post-REBUILD wave
    // covers every unassigned decision var.
    if (suppress_heap_inserts_) return;
    if (decision[v]) {
        order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::INSERT, v));
#ifdef USE_CLASSIC_HEAP
        heap_resp_cnt++;
#endif
        output.verbose(CALL_INFO, 7, 0, "Insert var %d into order heap\n", v);
    }
}

void SATSolver::varDecayActivity() {
    var_inc *= 1.0 / var_decay;
    output.verbose(CALL_INFO, 4, 0,
        "ACTIVITY: Decayed var activity increment to %f\n", var_inc);
}

//-----------------------------------------------------------------------------------
// Clause Activity
//-----------------------------------------------------------------------------------

// Decay all clause activities
void SATSolver::claDecayActivity() {
    cla_inc *= (1.0 / clause_decay);
    output.verbose(CALL_INFO, 4, 0,
        "ACTIVITY: Decayed clause activity increment to %f\n", cla_inc);
}

// Bump activity for a specific clause. `act` is the clause's current
// activity, already in hand from the analyze traversal (no clause re-read).
// Returns true when the bump triggered a global rescale.
bool SATSolver::claBumpActivity(Cref clause_addr, float act) {
    float new_act = (float)(act + cla_inc);
    clauses.writeAct(clause_addr, new_act);
    // Histogram bump: dec old bucket, inc new (binaries are not tracked).
    if (!clauses.isBinaryLearnt(clause_addr)) cla_hist_.move(act, new_act);

    if ((act + cla_inc) > 1e20) {
        // Rescale all clause activities if they get too large. The factor is
        // a power of two (2^-66) so the histogram update is an exact bucket
        // shift; values that go denormal are flushed to zero by the sweep and
        // collapse into bucket 0.
        output.verbose(CALL_INFO, 3, 0, "ACTIVITY: Rescaling all clause activities\n");
        rescaleAllActivities();
        cla_inc *= 0x1p-66;
        cla_hist_.rescaleShift();
        return true;
    }

    output.verbose(CALL_INFO, 4, 0, "ACTIVITY: Bumped clause 0x%x\n", clause_addr);
    return false;
}

//-----------------------------------------------------------------------------------
// Clause Minimization
//-----------------------------------------------------------------------------------

void SATSolver::minimizeL2_sub(std::vector<bool>& redundant, int worker_id) {
    for (size_t i = worker_id + 1; i < learnt_clause.size(); i += MINIMIZERS) {
        output.verbose(CALL_INFO, 5, 0, 
            "MIN[%d]: Checking literal %d at position %zu\n", 
            worker_id, toInt(learnt_clause[i]), i);
        
        redundant[i] = litRedundant(learnt_clause[i], worker_id);
    }
}

// Check if 'p' can be removed from the learnt clause
bool SATSolver::litRedundant(Lit p, int worker_id) {
    enum { seen_undef = 0, seen_source = 1, seen_removable = 2, seen_failed = 3 };
    int reason = variables.getReason(var(p), worker_id);

    if (reason == ClauseRef_Undef) {
        output.verbose(CALL_INFO, 5, 0, "MIN[%d] literal %d not redundant, reason undefined\n", worker_id, toInt(p));
        return false;
    }

    assert(seen[var(p)] == seen_undef || seen[var(p)] == seen_source);
    
    std::vector<ShrinkStackElem> analyze_stack; // Stack for clause minimization
    Clause c = clauses.readClause(reason, worker_id);
    
    for (size_t i = 1; ; i++) {
        if (i < c.litSize()) {
            // Examining the literals in the reason clause
            Lit l = c[i];
            Var v = var(l);
            Variable v_data = variables.readVar(v, worker_id);
            
            // If variable at level 0 or already marked as source/removable, skip it
            if (v_data.level == 0 || seen[v] == seen_source || seen[v] == seen_removable) {
                continue;
            }
            
            // Cannot remove if var has no reason or was already marked as failed
            if (v_data.reason == ClauseRef_Undef || seen[v] == seen_failed) {
                // Mark all variables in stack as failed
                analyze_stack.push_back(ShrinkStackElem(0, p));
                for (size_t j = 0; j < analyze_stack.size(); j++) {
                    if (seen[var(analyze_stack[j].l)] == seen_undef) {
                        seen[var(analyze_stack[j].l)] = seen_failed;
                        analyze_toclear.push_back(analyze_stack[j].l);
                    }
                }

                output.verbose(CALL_INFO, 5, 0, "MIN[%d]: literal %d undefined or failed\n", worker_id, toInt(l));
                return false;
            }

            // Recursively check this literal
            analyze_stack.push_back(ShrinkStackElem(i, p));
            i = 0;
            p = l;
            c = clauses.readClause(v_data.reason, worker_id);
        } else {
            // Finished examining current reason clause
            if (seen[var(p)] == seen_undef) {
                seen[var(p)] = seen_removable;
                analyze_toclear.push_back(p);
                output.verbose(CALL_INFO, 7, 0, "MIN[%d]: Marked %d as removable\n", worker_id, toInt(p));
            }
            
            // If stack is empty, we're done
            if (analyze_stack.empty()) {
                output.verbose(CALL_INFO, 5, 0, "MIN[%d]: %d is redundant\n", worker_id, toInt(p));
                return true;
            }
            
                       
            // Continue with next element from stack
            ShrinkStackElem e = analyze_stack.back();
            analyze_stack.pop_back();
            i = e.i;
            p = e.l;
            c = clauses.readClause(variables.getReason(var(p), worker_id), worker_id);
        }
    }
}

//-----------------------------------------------------------------------------------
// Restart Helpers
//-----------------------------------------------------------------------------------

// Calculate the value of the Luby sequence at position x
double SATSolver::luby(double y, int x) {
    // Find the finite subsequence that contains index 'x', and the
    // size of that subsequence:
    int size, seq;
    for (size = 1, seq = 0; size < x+1; seq++, size = 2*size+1);

    while (size-1 != x) {
        size = (size-1)>>1;
        seq--;
        x = x % size;
    }

    return pow(y, seq);
}

//-----------------------------------------------------------------------------------
// Utility Functions
//-----------------------------------------------------------------------------------

double SATSolver::drand(uint64_t& seed) {
    seed = seed * 1389796 % 2147483647;
    return ((double)seed / 2147483647);
}

int SATSolver::irand(uint64_t& seed, int size) {
    return (int)(drand(seed) * size);
}

uint64_t SATSolver::getStatCount(Statistic<uint64_t>* stat) {
    AccumulatorStatistic<uint64_t>* accum = dynamic_cast<AccumulatorStatistic<uint64_t>*>(stat);
    if (accum) {
        return accum->getCount();
    }
    return 0; // Return 0 if the cast fails
}

std::string SATSolver::printClause(const std::vector<Lit>& literals) {
    // All call sites pass the result to output.verbose at level >= 3; skip the
    // string build entirely when it cannot be printed (hot path: called per
    // watcher block during propagation).
    if (output.getVerboseLevel() < 3) return std::string();
    std::string clause_str = "";
    for (const auto& lit : literals) {
        clause_str += " " + std::to_string(toInt(lit));
    }
    return clause_str;
}

void SATSolver::printHist(Statistic<uint64_t>* stat_hist) {
    if (auto* hist_stat = dynamic_cast<HistogramStatistic<uint64_t>*>(stat_hist)) {
        uint64_t total_count = hist_stat->getCollectionCount();
        uint64_t total_binned = hist_stat->getItemsBinnedCount();
        uint64_t bin_width = hist_stat->getBinWidth();
        uint64_t num_bins = hist_stat->getNumBins();
        uint64_t min_value = hist_stat->getBinsMinValue();

        output.output("Total samples: %lu\n", total_count);
        for (uint64_t bin = 0; bin < num_bins; bin++) {
            uint64_t bin_start = min_value + (bin * bin_width);
            uint64_t bin_end = bin_start + bin_width - 1;
            uint64_t bin_count = hist_stat->getBinCountByBinStart(bin_start);
            double percentage = total_count > 0 ? (double)bin_count * 100.0 / total_count : 0.0;
            
            if (bin_count > 0) {  // Only print non-empty bins
                output.output("Bin [%2lu-%2lu]: %8lu samples (%.2f%%)\n", 
                    bin_start, bin_end, bin_count, percentage);
            }
        }
        if (total_count - total_binned > 0) {
            output.output("Out of bounds: %6lu samples (%.2f%%)\n", 
                total_count - total_binned, 
                (double)(total_count - total_binned) * 100.0 / total_count);
        }
    }
}

void SATSolver::loadDecisionSequence(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        output.fatal(CALL_INFO, -1, "Could not open decision file: %s\n", filename.c_str());
    }
    
    decision_sequence.clear();
    std::string line;
    int line_number = 0, var, sign;
    
    while (std::getline(file, line)) {
        line_number++;
        if (line.empty() || line[0] == '#' || line[0] == 'c') continue;
            
        std::istringstream iss(line);
        if (!(iss >> var >> sign) || var <= 0 || (sign != 0 && sign != 1)) {
            output.fatal(CALL_INFO, -1, "Error in decision file at line %d\n", line_number);
        }
        
        decision_sequence.push_back(std::make_pair(var, sign == 1));
        output.verbose(CALL_INFO, 5, 0, "Added decision: var %d = %s\n", 
            var, (sign == 1) ? "true" : "false");
    }
    
    output.verbose(CALL_INFO, 1, 0, "Loaded %zu decisions from file\n", decision_sequence.size());
}

void SATSolver::dumpDecision(Lit lit) {
    Var v = var(lit);
    // Value is 1 for true, 0 for false
    // Note: sign(lit) is inverted because in the solver, sign true means negative
    int value = sign(lit) ? 0 : 1;
    decision_output_stream << v << " " << value << std::endl;
}

//-----------------------------------------------------------------------------------
// Speculative Propagation
//-----------------------------------------------------------------------------------

// Reset speculative propagation state
void SATSolver::terminateSpecPropagate() {
    output.verbose(CALL_INFO, 2, 0, "Terminated previous speculative propagation\n");
    for (auto corot : spec_sub_coroutines) {
        if (corot != nullptr) delete corot;
    }
    spec_sub_coroutines.clear();
    spec_sub_yield_ptrs.clear();
    spec_active_workers.clear();
    // Terminate the speculative coroutine
    delete spec_coroutine;
    spec_coroutine = nullptr;
    spec_active = false;
    spec_trail.clear();
    // clear any pending requests
    reorder_buffer.reset();
    clauses.reset();
    watches.reset();
    variables.reset();
}

void SATSolver::resetSpecState() {
    // Save the current speculative assignments before resetting
    prev_spec_var_assigned = spec_var_assigned;
    prev_spec_var_value = spec_var_value;
    prev_spec_var_propagated = spec_var_propagated;
    
    spec_trail.clear();
    
    // Initialize or reset speculative assignment tracking
    if (spec_var_assigned.size() != num_vars + 1) {
        spec_var_assigned.resize(num_vars + 1, false);
        spec_var_value.resize(num_vars + 1, false);
        spec_var_propagated.resize(num_vars + 1, false);
        prev_spec_var_assigned.resize(num_vars + 1, false);
        prev_spec_var_value.resize(num_vars + 1, false);
        prev_spec_var_propagated.resize(num_vars + 1, false);
    } else {
        spec_var_assigned.assign(num_vars + 1, false);
        spec_var_value.assign(num_vars + 1, false);
        spec_var_propagated.assign(num_vars + 1, false);
    }
    
    spec_conflicts = 0;
}

// Check if a variable is assigned in speculative propagation
bool SATSolver::isSpecAssigned(Var v) const {
    return var_assigned[v] || spec_var_assigned[v];
}

// Get the value of a variable (considering both main and speculative assignments)
bool SATSolver::getSpecValue(Var v) const {
    if (var_assigned[v]) return var_value[v];
    return spec_var_value[v];
}

// Get the value of a literal (considering both main and speculative assignments)
bool SATSolver::getSpecValue(Lit p) const {
    return getSpecValue(var(p)) ^ sign(p);
}

// Speculative propagation - reads watchlists and clauses without making writes
void SATSolver::speculativePropagate() {
    if (spec_literal == lit_Undef) return;

    // Add the speculative literal to the temporary trail
    spec_trail.push_back(spec_literal);
    Var v = var(spec_literal);
    spec_var_assigned[v] = true;
    spec_var_value[v] = !sign(spec_literal);
    output.verbose(CALL_INFO, 2, 0, "SPEC: propagate %d (decision)\n", toInt(spec_literal));

    // Initialize cache line counter for this speculativePropagate() call
    // Push immediately so it's preserved even if speculation terminates early
    spec_prop_cache_lines.push_back(0);
    uint64_t& cache_lines_read = spec_prop_cache_lines.back();

    uint spec_qhead = 0;
    while (spec_qhead < spec_trail.size() && (max_confl < 0 || spec_conflicts < max_confl)) {
        Lit p = spec_trail[spec_qhead];
        Lit not_p = ~p;
        int watch_idx = toWatchIndex(p);
        
        // Mark this variable as speculatively propagated (not just assigned)
        spec_var_propagated[var(p)] = true;
        
        output.verbose(CALL_INFO, 2, 0, "SPEC: Processing literal %d\n", toInt(not_p));
        
        // Use dedicated worker IDs starting from SPEC_WORKER_BASE so they
        // never collide with any main-side phase's worker_id range.
        int base_worker_id = SPEC_WORKER_BASE;
        
        // Read watch metadata - count as 1 cache line read
        cache_lines_read++;
        WatchMetaData wmd = watches.readMetaData(watch_idx, base_worker_id);
        stat_spec_started->addData(1);
        // if (var_assigned[var(spec_literal)]) {
        //     output.verbose(CALL_INFO, 4, 0, "Spec Literal propagated by main, early exit speculation\n");
        //     return;
        // }
        
        bool do_prewatch = PRE_WATCHERS > 0;
        uint32_t curr_addr = wmd.head_ptr;
        
        // Traverse the watchlist (prewatchers and blocks)
        while (curr_addr != 0 || do_prewatch) {
            WatcherBlock curr_block;
            if (do_prewatch) {
                curr_block.setNextBlock(curr_addr);
                for (int i = 0; i < PRE_WATCHERS; i++) {
                    curr_block.nodes[i] = wmd.pre_watchers[i];
                }
                do_prewatch = false;
            } else {
                // Read watcher block - count as 1 cache line read
                cache_lines_read++;
                curr_block = watches.readBlock(curr_addr, base_worker_id);
                // if (var_assigned[var(spec_literal)]) {
                //     output.verbose(CALL_INFO, 4, 0, "Spec Literal propagated by main, early exit speculation\n");
                //     return;
                // }
            }
            
            // Collect valid nodes
            std::vector<int> valid_nodes;
            for (int i = 0; i < PROPAGATORS; i++) {
                if (!curr_block.nodes[i].valid) continue;

                Lit blocker = curr_block.nodes[i].blocker;
                if (isSpecAssigned(var(blocker)) && getSpecValue(blocker)) {
                    // Blocker is true, skip to next watcher
                    output.verbose(CALL_INFO, 4, 0,
                        "SPEC: Watch block[%d]: clause 0x%x, blocker %d = True, skipping\n", 
                        i, curr_block.nodes[i].getClauseAddr(), toInt(blocker));
                    continue;
                }

                valid_nodes.push_back(i);
            }
            
            // Process watchers in parallel batches
            coro_t::push_type* parent_yield_ptr = yield_ptr;
            int workers = std::min(PROPAGATORS, (int)valid_nodes.size());
            spec_sub_coroutines.resize(workers);
            spec_sub_yield_ptrs.resize(workers);
            assert(spec_active_workers.size() == 0);
            spec_active_workers.resize(workers, false);
            bool done = true;
            
            output.verbose(CALL_INFO, 4, 0, "SPEC: spawning %d watcher coroutines\n", workers);
            
            // Create watcher coroutines
            for (int worker_id = 0; worker_id < workers; worker_id++) {
                int watcher_i = valid_nodes[worker_id];
                spec_sub_coroutines[worker_id] = new coro_t::pull_type(
                    [this, watcher_i, not_p, &curr_block, worker_id, base_worker_id, &cache_lines_read]
                    (coro_t::push_type &yield) {

                    spec_sub_yield_ptrs[worker_id] = &yield;
                    yield_ptr = &yield;

                    Cref clause_addr = curr_block.nodes[watcher_i].getClauseAddr();
                    int global_worker_id = base_worker_id + worker_id;
                    
                    // Read clause - 1 line for size if not terminated
                    cache_lines_read++;
                    Clause c = clauses.readClause(clause_addr, global_worker_id);

                    // additional lines if clause spans multiple cache lines
                    cache_lines_read += std::ceil(c.size() / 64) - 1;

                    output.verbose(CALL_INFO, 4, 0, "SPEC[W%d]: blocker %d, clause 0x%x: %s\n",
                        worker_id, toInt(curr_block.nodes[watcher_i].blocker),
                        clause_addr, printClause(c.literals).c_str());

                    // Make sure the false literal is at position 1
                    if (c[0] == not_p) std::swap(c.literals[0], c.literals[1]);

                    Lit first = c[0];
                    // If first literal is satisfied, skip
                    if (isSpecAssigned(var(first)) && getSpecValue(first)) {
                        return;
                    }
                    
                    // Look for a new watch
                    bool found_new_watch = false;
                    for (size_t k = 2; k < c.litSize(); k++) {
                        Lit lit = c[k];
                        if (!isSpecAssigned(var(lit)) || getSpecValue(lit)) {
                            found_new_watch = true;
                            break;
                        }
                    }
                    
                    // If no new watch found, check for propagation or conflict
                    if (!found_new_watch) {
                        if (isSpecAssigned(var(first)) && (getSpecValue(first) == false)) {
                            // Conflict
                            spec_conflicts++;
                            output.verbose(CALL_INFO, 4, 0,
                                "SPEC: conflict found, count=%d\n", spec_conflicts);
                        } else {
                            // Propagate
                            spec_trail.push_back(first);
                            spec_var_assigned[var(first)] = true;
                            spec_var_value[var(first)] = !sign(first);
                            output.verbose(CALL_INFO, 4, 0, "SPEC: propagate %d\n", toInt(first));
                        }
                    }
                });

                if (*spec_sub_coroutines[worker_id]) done = false;
            }
            
            if (!done) (*parent_yield_ptr)();  // yield back to main
            
            // Step sub-coroutines
            while (!done) {
                done = true;
                for (int worker_id = 0; worker_id < workers; worker_id++) {
                    if (spec_active_workers[worker_id]) {
                        (*spec_sub_coroutines[worker_id])();
                        spec_active_workers[worker_id] = false;
                        if (!(*spec_sub_coroutines[worker_id])) {
                            delete spec_sub_coroutines[worker_id];
                            spec_sub_coroutines[worker_id] = nullptr;
                            spec_sub_yield_ptrs[worker_id] = nullptr;
                        } else {
                            done = false;
                        }
                    } else if (spec_sub_coroutines[worker_id] != nullptr)
                        done = false;
                }
                if (!done) (*parent_yield_ptr)();
            }

            // Cleanup coroutines
            spec_sub_coroutines.clear();
            spec_sub_yield_ptrs.clear();
            spec_active_workers.clear();
            yield_ptr = parent_yield_ptr;

            // early stop
            // if (var_assigned[var(spec_literal)]) {
            //     output.verbose(CALL_INFO, 4, 0, "Spec Literal propagated by main, early exit speculation\n");
            //     return;
            // }

            // Stop if too many conflicts
            if (max_confl >= 0 && spec_conflicts >= max_confl) break;

            // Move to next block
            curr_addr = curr_block.getNextBlock();
        }
        spec_qhead++;
    }

    // Cache line count already stored in vector via reference
    output.verbose(CALL_INFO, 2, 0, 
        "SPEC: speculativePropagate() processed %zu literals and brought in %lu cache lines\n",
        spec_trail.size(), cache_lines_read);

    stat_spec_finished->addDataNTimes(spec_trail.size(), 1);
    output.verbose(CALL_INFO, 2, 0,
        "Speculative propagation finished: trail_size=%zu, conflicts=%d\n", 
        spec_trail.size(), spec_conflicts);
}
