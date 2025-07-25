#include <sst/core/sst_config.h> // This include is REQUIRED for all implementation files
#include "satsolver.h"
#include <sst/core/interfaces/stdMem.h>
#include "sst/core/statapi/stathistogram.h"
#include "sst/core/statapi/stataccumulator.h"
#include <algorithm>  // For std::sort
#include <cmath>      // For pow function
#include <fstream>    // For file reading

//-----------------------------------------------------------------------------------
// Component Lifecycle Methods
//-----------------------------------------------------------------------------------

SATSolver::SATSolver(SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id), 
    state(IDLE),
    random_seed(91648253),
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
    yield_ptr(nullptr),
    // Initialize cycle counters
    cycles_propagate(0),
    cycles_analyze(0),
    cycles_minimize(0),
    cycles_backtrack(0),
    cycles_decision(0),
    cycles_reduce(0),
    cycles_restart(0),
    // Initialize cycle tracking
    prev_state(IDLE),
    last_state_change(0) {
    
    // Initialize output
    int verbose = params.find<int>("verbose", 0);
    output.init("MAIN-> ",verbose, 0, SST::Output::STDOUT);

    // Configure clock
    registerClock(params.find<std::string>("clock", "1GHz"),
                  new SST::Clock::Handler2<SATSolver, &SATSolver::clockTick>(this));

    // Get file size parameter
    filesize = params.find<size_t>("filesize", 0);
    if (filesize == 0) {
        output.fatal(CALL_INFO, -1, "File size parameter not provided\n");
    }

    // Get CNF file path
    cnf_file_path = params.find<std::string>("cnf_file", "");
    if (cnf_file_path.empty()) {
        output.fatal(CALL_INFO, -1, "CNF file path not provided\n");
    }

    sort_clauses = params.find<bool>("sort_clauses", true);

    // Initialize activity-related variables
    var_decay = params.find<double>("var_decay", 0.95);
    clause_decay = params.find<double>("clause_decay", 0.999);  // Add clause decay parameter
    random_var_freq = params.find<double>("random_var_freq", 0.0);
    
    // Get heap memory addresses
    heap_base_addr = std::stoull(params.find<std::string>("heap_base_addr", "0x00000000"), nullptr, 0);
    indices_base_addr = std::stoull(params.find<std::string>("indices_base_addr", "0x10000000"), nullptr, 0);
    variables_base_addr = std::stoull(params.find<std::string>("variables_base_addr", "0x20000000"), nullptr, 0);
    watches_base_addr = std::stoull(params.find<std::string>("watches_base_addr", "0x30000000"), nullptr, 0);
    watch_nodes_base_addr = std::stoull(params.find<std::string>("watch_nodes_base_addr", "0x40000000"), nullptr, 0);
    clauses_cmd_base_addr = std::stoull(params.find<std::string>("clauses_cmd_base_addr", "0x50000000"), nullptr, 0);
    clauses_base_addr = std::stoull(params.find<std::string>("clauses_base_addr", "0x60000000"), nullptr, 0);
    var_act_base_addr = std::stoull(params.find<std::string>("var_act_base_addr", "0x70000000"), nullptr, 0);
    
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
    
    // Create Watches object
    watches = Watches(verbose, global_memory, watches_base_addr, watch_nodes_base_addr, &yield_ptr);
    watches.setReorderBuffer(&reorder_buffer);
    
    // Create Clauses object
    clauses = Clauses(verbose, global_memory, clauses_cmd_base_addr, clauses_base_addr, &yield_ptr);
    clauses.setReorderBuffer(&reorder_buffer);
    
    // Load the heap subcomponent
    order_heap = loadUserSubComponent<Heap>("order_heap",
        SST::ComponentInfo::SHARE_PORTS | SST::ComponentInfo::SHARE_STATS,
        global_memory, heap_base_addr, indices_base_addr
    );
    sst_assert(order_heap != nullptr, CALL_INFO, -1, "Unable to load Heap subcomponent\n");
    unstalled_heap = false;
    unstalled_cnt = 0;

    // Configure the link to the heap subcomponent
    heap_link = configureLink("heap_port", 
        new SST::Event::Handler2<SATSolver, &SATSolver::handleHeapResponse>(this));
    sst_assert( heap_link != nullptr, CALL_INFO, -1, "Error: 'heap_port' is not connected to a link\n");
    
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
    stat_para_watchers = registerStatistic<uint64_t>("para_watchers");
    stat_para_vars = registerStatistic<uint64_t>("para_vars");

    // Component should not end simulation until solution is found
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

SATSolver::~SATSolver() {}

void SATSolver::init(unsigned int phase) {
    global_memory->init(phase);

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

        // Untimed data structure initialization
        variables.init(num_vars);
        watches.initWatches(2 * (num_vars + 1), parsed_clauses);
        clauses.initialize(parsed_clauses);

        order_heap->decision = decision;
        order_heap->heap_size = num_vars;
        order_heap->var_inc_ptr = &var_inc;
        order_heap->initHeap();
    }
    output.verbose(CALL_INFO, 3, 0, "SATSolver initialized in phase %u\n", phase);
}

void SATSolver::setup() {
    global_memory->setup();
    
    // Get cache line size from memory interface
    size_t line_size = global_memory->getLineSize();
    output.verbose(CALL_INFO, 1, 0, "Cache line size: %zu bytes\n", line_size);
    
    // Propagate cache line size to all memory-using components
    watches.setLineSize(line_size);
    clauses.setLineSize(line_size);
    order_heap->setLineSize(line_size);
}

void SATSolver::complete(unsigned int phase) {
    global_memory->complete(phase);
}

void SATSolver::finish() {
    global_memory->finish();
    
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
    output.output("Variables    : %u (Total), %lu (Assigned)\n", num_vars,
        getStatCount(stat_assigns) - getStatCount(stat_unassigns));
    output.output("Clauses      : %lu (Total), %lu (Learned)\n", 
        clauses.size(), 
        getStatCount(stat_learned) - getStatCount(stat_removed));
    output.output("===========================================================================\n");
    
    output.output("=========================[ Parallel Watchers Histogram ]=================\n");
    printHist(stat_para_watchers);
    output.output("===========================================================================\n");
    output.output("=========================[ Parallel Variables Histogram ]================\n");
    printHist(stat_para_vars);
    output.output("===========================================================================\n");
    output.output("=========================[ Clauses Fragmentation ]=========================\n");
    clauses.printFragStats();
    output.output("===========================================================================\n");
    
    uint64_t total_counted = cycles_propagate + cycles_analyze + cycles_minimize +
                            cycles_backtrack + cycles_decision + cycles_reduce + cycles_restart;
    
    // Calculate percentages (avoid division by zero)
    double pct_propagate = (double)cycles_propagate * 100.0 / total_cycles;
    double pct_analyze = (double)cycles_analyze * 100.0 / total_cycles;
    double pct_minimize = (double)cycles_minimize * 100.0 / total_cycles;
    double pct_backtrack = (double)cycles_backtrack * 100.0 / total_cycles;
    double pct_decision = (double)cycles_decision * 100.0 / total_cycles;
    double pct_reduce = (double)cycles_reduce * 100.0 / total_cycles;
    double pct_restart = (double)cycles_restart * 100.0 / total_cycles;
    
    // Print cycle count statistics with percentages
    output.output("===========================[ Cycle Statistics ]============================\n");
    output.output("Propagate    : %.2f%% \t(%lu cycles)\n", pct_propagate, cycles_propagate);
    output.output("Analyze      : %.2f%% \t(%lu cycles)\n", pct_analyze, cycles_analyze);
    output.output("Minimize     : %.2f%% \t(%lu cycles)\n", pct_minimize, cycles_minimize);
    output.output("Backtrack    : %.2f%% \t(%lu cycles)\n", pct_backtrack, cycles_backtrack);
    output.output("Decision     : %.2f%% \t(%lu cycles)\n", pct_decision, cycles_decision);
    output.output("Reduce DB    : %.2f%% \t(%lu cycles)\n", pct_reduce, cycles_reduce);
    output.output("Restart      : %.2f%% \t(%lu cycles)\n", pct_restart, cycles_restart);
    output.output("Total Counted: %lu cycles\n", total_counted);
    output.output("===========================================================================\n");
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
                output.verbose(CALL_INFO, 4, 0,
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
                Clause clause;
                
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
                
                while (clause_iss >> dimacs_lit && dimacs_lit != 0) {
                    Lit lit = toLit(dimacs_lit);
                    clause.literals.push_back(lit);
                }
                
                // Skip empty clauses (can happen with corrupted data)
                if (clause.literals.empty()) {
                    output.verbose(CALL_INFO, 4, 0, "Skipping empty clause line\n");
                    continue;
                }
                
                if (clause.literals.size() == 1) {  // Unit clause
                    initial_units.push_back(clause.literals[0]);
                    num_clauses--;
                    output.verbose(CALL_INFO, 3, 0,
                        "Unit clause: %d\n", toInt(clause.literals[0]));
                } else {
                    if (sort_clauses) {
                        // Sort literals in the clause
                        std::sort(clause.literals.begin(), clause.literals.end());
                    }

                    // remove duplicates
                    clause.literals.erase(
                        std::unique(clause.literals.begin(), clause.literals.end()),
                        clause.literals.end());

                    clause.num_lits = clause.literals.size();
                    parsed_clauses.push_back(clause);

                    // debugging outputs
                    output.verbose(CALL_INFO, 6, 0, "Added clause %lu: %s\n",
                                   parsed_clauses.size() - 1, printClause(clause.literals).c_str());
                }
                break;
            }
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

        // Route the request to the appropriate handler based on address range
        if (addr >= var_act_base_addr) {
            order_heap->handleMem(req);
        } else if (addr >= clauses_cmd_base_addr) {  // Clauses request
            worker_id = reorder_buffer.lookUpWorkerId(read_resp->getID());
            clauses.handleMem(req);
            state = STEP;
        } else if (addr >= watches_base_addr) {  // Watches request
            worker_id = reorder_buffer.lookUpWorkerId(read_resp->getID());
            watches.handleMem(req);
            state = STEP;
        } else if (addr >= variables_base_addr) {  // Variables request
            worker_id = reorder_buffer.lookUpWorkerId(read_resp->getID());
            variables.handleMem(req);
            state = STEP;
        } else order_heap->handleMem(req);  // Heap request

        if (active_workers.size() > 0 && worker_id >= 0 && worker_id < (int)active_workers.size()) {
            active_workers[worker_id] = true;
        }
        output.verbose(CALL_INFO, 8, 0, "handleGlobalMemEvent received for 0x%lx, worker %d\n", addr, worker_id);
    }
    delete req;  // assuming no write responses are sent back
}

void SATSolver::handleHeapResponse(SST::Event* ev) {
    HeapRespEvent* resp = dynamic_cast<HeapRespEvent*>(ev);
    sst_assert(resp != nullptr, CALL_INFO, -1, "Invalid heap response event\n");
    output.verbose(CALL_INFO, 8, 0, "HandleHeapResponse: response %d\n", resp->result);
    heap_resp = resp->result;
    if (!unstalled_heap) state = STEP;
    else unstalled_cnt--;
    // printf("HandleHeap: unstalled_cnt is now %d, size is %lu\n", unstalled_cnt, order_heap->heap_size);
    delete resp;
}

bool SATSolver::clockTick(SST::Cycle_t cycle) {
    // Calculate elapsed cycles since last state change if we're not in IDLE or STEP
    if (state != IDLE && state != STEP && state != WAIT_HEAP && prev_state != state) {
        // Update cycle counts based on previous state
        uint64_t elapsed = cycle - last_state_change;
        output.verbose(CALL_INFO, 8, 0,
            "DEBUG: previous state %d, current state %d, elapsed cycles %lu\n", 
            prev_state, state, cycle - last_state_change);
        switch (prev_state) {
            case PROPAGATE:
                cycles_propagate += elapsed;
                output.verbose(CALL_INFO, 8, 0, "DEBUG: Propagate cycles %lu\n", cycles_propagate);
                break;
            case ANALYZE:
                cycles_analyze += elapsed;
                output.verbose(CALL_INFO, 8, 0, "DEBUG: Analyze cycles %lu\n", cycles_analyze);
                break;
            case MINIMIZE:
                cycles_minimize += elapsed;
                output.verbose(CALL_INFO, 8, 0, "DEBUG: Minimize cycles %lu\n", cycles_minimize);
                break;
            case BTLEVEL:
            case BACKTRACK:
                cycles_backtrack += elapsed;
                output.verbose(CALL_INFO, 8, 0, "DEBUG: Backtrack cycles %lu\n", cycles_backtrack);
                break;
            case DECIDE:
                cycles_decision += elapsed;
                output.verbose(CALL_INFO, 8, 0, "DEBUG: Decision cycles %lu\n", cycles_decision);
                break;
            case REDUCE:
                cycles_reduce += elapsed;
                output.verbose(CALL_INFO, 8, 0, "DEBUG: Reduce cycles %lu\n", cycles_reduce);
                break;
            case RESTART:
                cycles_restart += elapsed;
                output.verbose(CALL_INFO, 8, 0, "DEBUG: Restart cycles %lu\n", cycles_restart);
                break;
            default:
                break;
        }
    
        // Record the new state change
        prev_state = state;
        last_state_change = cycle;
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
            (*coroutine)();
            if (*coroutine) {
                output.verbose(CALL_INFO, 8, 0, "coroutine paused\n");
                state = IDLE;  // Continue coroutine later
            } else {
                output.verbose(CALL_INFO, 8, 0, "coroutine completed\n");
                delete coroutine;
                coroutine = nullptr;  // coroutine will set the next state
                yield_ptr = nullptr;  // Clear yield pointer when coroutine completes
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
            state = IDLE;
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
            state = IDLE;
            break;
        case BTLEVEL:
            if (learnt_clause.size() == 1) {
                bt_level = 0;
                if (OVERLAP_HEAP_BUMP) {
                    state = WAIT_HEAP;
                    next_state = BACKTRACK;
                } else state = BACKTRACK;
            } else {
                coroutine = new coro_t::pull_type(
                    [this](coro_t::push_type &yield) {
                        yield_ptr = &yield;
                        findBtLevel(); 
                    });
                state = IDLE;
            }
            break;
        case BACKTRACK:
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
            state = IDLE;
            break;
        case RESTART:
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
            assert(unstalled_cnt >= 0);
            if (unstalled_cnt == 0) {
                unstalled_heap = false;
                state = next_state;
            }
            break;
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
    conflict = unitPropagate();
    
    // if not parallelizing propagation and heap insertions
    if (conflict != ClauseRef_Undef) {
        if (decision_output_stream.is_open()) decision_output_stream << "#Conflict" << std::endl;
        output.verbose(CALL_INFO, 2, 0, "CONFLICT: clause 0x%x\n", conflict);
        conflictC++;
        stat_conflicts->addData(1);
        
        if (trail_lim.empty()) {
            output.output("UNSATISFIABLE: conflict at level 0\n");
            state = DONE;
            return;
        }
        state = ANALYZE;  // learn from the conflict
    } else if (conflictC >= conflicts_until_restart) state = RESTART;
    else if (nLearnts() - nAssigns() >= max_learnts) state = REDUCE;
    else state = DECIDE;

    if (OVERLAP_HEAP_INSERT) {
        next_state = state;
        state = WAIT_HEAP;
    }
}

void SATSolver::execAnalyze() {
    analyze();
    if (OVERLAP_HEAP_BUMP) state = MINIMIZE;
    else {
        state = WAIT_HEAP;
        next_state = MINIMIZE;
    }
}

void SATSolver::execMinimize() {
    // Keep track of literals to clear
    analyze_toclear.clear();
    analyze_toclear = learnt_clause; 

    // Minimize conflict clause:
    int i, j;
    output.verbose(CALL_INFO, 3, 0,
        "ANALYZE: Minimizing clause (size %zu): %s\n", learnt_clause.size(),
        printClause(learnt_clause).c_str());

    if (ccmin_mode == 2) {
        // Deep minimization (more thorough)
        parent_yield_ptr = yield_ptr;
        int workers = std::min(MINIMIZERS, (int)learnt_clause.size() - 1);
        active_workers.resize(workers, false);
        coroutines.resize(workers);
        yield_ptrs.resize(workers);
        std::vector<bool> redundant(learnt_clause.size(), false);

        // spawn sub-coroutines for each literal
        for (int worker_id = 0; worker_id < workers; worker_id++) {
            coroutines[worker_id] = new coro_t::pull_type(
                [this, worker_id, &redundant](coro_t::push_type &yield) {
                    yield_ptr = &yield;
                    yield_ptrs[worker_id] = yield_ptr;
                    minimizeL2_sub(redundant, worker_id);
                });
        }
        (*parent_yield_ptr)();  // yield back to IDLE

        // stepping sub-coroutines
        bool done = false;
        while (!done) {
            done = true;
            // Check if any worker is active
            for (int worker_id = 0; worker_id < workers; worker_id++) {
                if (active_workers[worker_id]) {
                    yield_ptr = yield_ptrs[worker_id];
                    (*coroutines[worker_id])();
                    active_workers[worker_id] = false;
                    if ((*coroutines[worker_id])) {
                        done = false;
                    } else {
                        delete coroutines[worker_id];
                        coroutines[worker_id] = nullptr;
                        yield_ptrs[worker_id] = nullptr;
                    }
                } else if (coroutines[worker_id]) done = false;
            }

            if (!done) (*parent_yield_ptr)();  // yield back to IDLE
        }
        
        // finished all sub-coroutines
        active_workers.clear();
        coroutines.clear();
        yield_ptrs.clear();
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
        output.verbose(CALL_INFO, 3, 0, 
            "MINIMIZE: removed %d literals\n", i - j);
        output.verbose(CALL_INFO, 3, 0, "MINIMIZE: Final minimized clause: %s\n",
            printClause(learnt_clause).c_str());
    }

    state = BTLEVEL;
}

void SATSolver::execBacktrack() {
    backtrack(bt_level);
    
    if (learnt_clause.size() == 1) {
        // Unit learnt clause will be instantly propagated
        trailEnqueue(learnt_clause[0]);
    } else {
        // Add the learned clause
        Clause new_clause(learnt_clause, cla_inc);
        Cref addr = clauses.addClause(new_clause);
        output.verbose(CALL_INFO, 3, 0, 
            "Added learnt clause 0x%x: %s\n", 
            addr, printClause(new_clause.literals).c_str());
        attachClause(addr);
        trailEnqueue(learnt_clause[0], addr);
        stat_learned->addData(1);
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

    if (OVERLAP_HEAP_INSERT) state = PROPAGATE;
    else {
        state = WAIT_HEAP;
        next_state = PROPAGATE;
    }
}

void SATSolver::execReduce() {
    output.verbose(CALL_INFO, 3, 0, "REDUCE: %d - %d >= %.0f\n", 
        nLearnts(), nAssigns(), max_learnts);
    reduceDB();
    state = DECIDE;
}

void SATSolver::execRestart() {
    output.verbose(CALL_INFO, 2, 0, "RESTART: Executing restart #%d\n", curr_restarts);
    backtrack(0);
    conflictC = 0;
    curr_restarts++;
    stat_restarts->addData(1);
    
    // Update the restart limit using Luby sequence or geometric progression
    double rest_base = luby_restart ? luby(restart_inc, curr_restarts) : pow(restart_inc, curr_restarts);
    conflicts_until_restart = rest_base * restart_first;
    
    output.verbose(CALL_INFO, 2, 0, "RESTART: #%d, new limit=%d\n", 
        curr_restarts, conflicts_until_restart);

    if (OVERLAP_HEAP_INSERT) state = PROPAGATE;
    else {
        state = WAIT_HEAP;
        next_state = PROPAGATE;
    }
}

void SATSolver::execDecide() {
    if (!decide()) {
        state = DONE;
        output.output("SATISFIABLE: All variables assigned\n");
        for (Var v = 1; v <= (Var)num_vars; v++) {
            output.output("x%d=%d ", v, var_value[v] ? 1 : 0);
        }
        output.output("\n");
        return;
    }
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
                output.verbose(CALL_INFO, 2, 0, 
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
    
    // If couldn't use the decision sequence, fall back to normal heuristic
    if (lit == lit_Undef) {
        lit = chooseBranchVariable();
        if (lit == lit_Undef) {
            output.verbose(CALL_INFO, 2, 0, "DECIDE: No unassigned variables left\n");
            return false;
        }
    }

    if (decision_output_stream.is_open()) dumpDecision(lit);
    trail_lim.push_back(trail.size());  // new decision level
    trailEnqueue(lit);
    return true;
}

//-----------------------------------------------------------------------------------
// unitPropagate
//-----------------------------------------------------------------------------------

int SATSolver::unitPropagate() {
    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: Starting unit propagation\n");
    conflict = ClauseRef_Undef;
    
    // Track the current batch of variables that can be processed in parallel
    size_t batch_start = qhead;
    size_t batch_end = trail.size();
    
    while (qhead < trail.size()) {
        stat_propagations->addData(1);
        Lit p = trail[qhead++];
        Lit not_p = ~p;
        int watch_idx = toWatchIndex(p);
        uint64_t head_addr = watches.readHeadPointer(watch_idx);

        output.verbose(CALL_INFO, 3, 0,
            "PROPAGATE: Processing watchers for literal %d\n", toInt(p));

        // if (head_addr == 0) continue; // Empty watch list
        uint64_t curr_addr = head_addr;
        uint64_t prev_addr = 0;
        WatcherBlock prev_block;
        
        // Counter for watchers inspected in this propagation
        uint64_t para_watchers = 0;
        
        // Traverse the linked list
        while (curr_addr != 0) {
            // Read current block
            WatcherBlock curr_block = watches.readBlock(curr_addr);

            uint64_t next_addr = curr_block.next_block;
            bool block_modified = false;
            
            // spawn sub-coroutines
            parent_yield_ptr = yield_ptr;
            
            // Collect valid nodes that need processing
            std::vector<int> valid_nodes;
            for (int i = 0; i < watches.getNodesPerBlock(); i++) {
                assert(curr_block.valid_mask != 0 && "Invalid watch block with no valid nodes");
                // Skip invalid nodes
                if ((curr_block.valid_mask & (1 << i)) == 0) continue;

                Lit blocker = curr_block.nodes[i].blocker;
                if (var_assigned[var(blocker)] && value(blocker) == true) {
                    // Blocker is true, skip to next watcher
                    output.verbose(CALL_INFO, 4, 0,
                        "  Watch block[%d]: clause 0x%x, blocker %d = True, skipping\n", 
                        i, curr_block.nodes[i].clause_addr, toInt(blocker));
                    continue;
                }
                
                valid_nodes.push_back(i);
            }
            
            // Count the watchers inspected in this block
            para_watchers += valid_nodes.size();
            
            // Process valid nodes in batches, limited by PROPAGATORS
            int workers = std::min(PROPAGATORS, (int)valid_nodes.size());
            for (int worker_id = 0; worker_id < workers; worker_id++) {
                active_workers.push_back(false);
                polling.push_back(false);
                coroutines.push_back(new coro_t::pull_type(
                    [this, worker_id, &valid_nodes, not_p, &block_modified, &curr_block](coro_t::push_type &yield) {
                        yield_ptr = &yield;
                        yield_ptrs.push_back(yield_ptr);
                        // Process nodes assigned to this worker
                        for (size_t node_idx = worker_id; node_idx < valid_nodes.size(); node_idx += PROPAGATORS) {
                            int i = valid_nodes[node_idx];
                            subPropagate(i, not_p, block_modified, curr_block, worker_id);
                            if (conflict != ClauseRef_Undef) break; // Early exit on conflict
                        }
                    }));
            }
            if (!active_workers.empty()) (*parent_yield_ptr)();  // yield back to IDLE

            // stepping sub-coroutines
            bool done = false;
            while (!done) {
                done = true;
                // Check if any worker is active
                for (size_t j = 0; j < active_workers.size(); j++) {
                    output.verbose(CALL_INFO, 8, 0, "Worker %zu: active=%d, polling=%d, done=%d\n",
                        j, (bool)active_workers[j], (bool)polling[j], (bool)(coroutines[j] == nullptr));
                    if (active_workers[j]) {
                        yield_ptr = yield_ptrs[j];
                        (*coroutines[j])();
                        active_workers[j] = false;
                        if ((*coroutines[j])) {
                            done = false;
                        } else {
                            delete coroutines[j];
                            coroutines[j] = nullptr;
                            yield_ptrs[j] = nullptr;
                        }
                    } else if (coroutines[j]) done = false;
                }

                // since the polling workers never get triggered,
                // we need to check them after completing the active workers
                for (size_t j = 0; j < polling.size(); j++) {
                    if (polling[j]) {
                        yield_ptr = yield_ptrs[j];
                        (*coroutines[j])();
                    }
                }

                if (!done) (*parent_yield_ptr)();  // yield back to IDLE
            }

            // finished all sub-coroutines
            active_workers.clear();
            polling.clear();
            coroutines.clear();
            yield_ptrs.clear();
            yield_ptr = parent_yield_ptr;
            output.verbose(CALL_INFO, 4, 0, "  Finished processing a watch block\n");
            
            // After processing all nodes in the block, check if we need to write it back
            if (block_modified)
                watches.updateBlock(watch_idx, prev_addr, curr_addr, prev_block, curr_block);

            if (conflict != ClauseRef_Undef) {
                // Record watchers inspected up to the conflict point
                stat_para_watchers->addData(para_watchers);
                stat_para_vars->addData(qhead - batch_start);

                qhead = trail.size();
                return conflict;
            }
            
            // the current block is deleted if it has no valid nodes left
            if (curr_block.valid_mask != 0) {
                prev_addr = curr_addr;
                prev_block = curr_block;
            }

            // Move to next block
            curr_addr = next_addr;
            block_modified = false;
        }
        
        // Record total watchers inspected for this literal (entire linked list traversed)
        if (para_watchers > 0) stat_para_watchers->addData(para_watchers);
        
        // Check if the current batch of vars has been fully processed
        if (qhead == batch_end) {
            stat_para_vars->addData(batch_end - batch_start);
            // Start tracking a new batch
            batch_end = trail.size();
            batch_start = qhead;
        }
    }
    
    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: no more propagations\n");
    return conflict;
}

void SATSolver::subPropagate(
    int i,
    Lit not_p,
    bool& block_modified,
    WatcherBlock& curr_block,
    int worker_id
) {
    // Need to inspect the clause
    Cref clause_addr = curr_block.nodes[i].clause_addr;
    Clause c = clauses.readClause(clause_addr, worker_id);

    // Print clause for debugging
    output.verbose(CALL_INFO, 4, 0,
        "  Watch block[%d]: blocker:%d, clause 0x%x: %s\n",
        i, toInt(curr_block.nodes[i].blocker),
        curr_block.nodes[i].clause_addr, printClause(c.literals).c_str());

    // Make sure the false literal (~p) is at position 1
    if (c[0] == not_p) {
        std::swap(c.literals[0], c.literals[1]);
        clauses.writeClause(clause_addr, c);
        output.verbose(CALL_INFO, 4, 0, "    [W%d]Swapped literals 0 and 1\n", worker_id);
    }
    assert(c[1] == not_p);
    
    // If first literal is already true, just update the blocker and continue
    Lit first = c[0];
    if (var_assigned[var(first)] && value(first) == true) {
        output.verbose(CALL_INFO, 4, 0,
            "    [W%d]First literal %d is true\n", worker_id, toInt(first));
        curr_block.nodes[i].blocker = first;
        block_modified = true;
        return;
    }
    
    // Look for a new literal to watch
    for (size_t k = 2; k < c.litSize(); k++) {
        Lit lit = c[k];
        if (!var_assigned[var(lit)] || value(lit) == true) {
            // Swap to position 1 and update watcher
            std::swap(c.literals[1], c.literals[k]);
            clauses.writeClause(clause_addr, c);
            output.verbose(CALL_INFO, 4, 0, 
                "    [W%d]Found new watch: literal %d at position %zu\n", 
                worker_id, toInt(c[1]), k);
            
            while (watches.isBusy(toWatchIndex(~c[1]))) {
                polling[worker_id] = true;
                (*yield_ptr)();  // Yield to allow other workers to process
            }
            polling[worker_id] = false;

            output.verbose(CALL_INFO, 4, 0, "    [W%d]Start watchlist insertion\n", worker_id);
            watches.insertWatcher(toWatchIndex(~c[1]), clause_addr, first, worker_id);
            
            // Mark this node as invalid in the current block
            curr_block.valid_mask &= ~(1 << i);
            block_modified = true;
            return;
        }
    }
    
    // Did not find a new watch - clause is unit or conflicting
    output.verbose(CALL_INFO, 4, 0, "    [W%d]No new watch found\n", worker_id);

    // Check if first literal is false (conflict) or undefined (unit)
    if (var_assigned[var(first)] && value(first) == false) {
        // Conflict detected
        output.verbose(CALL_INFO, 3, 0,
            "    [W%d]Conflict: Clause 0x%x has all literals false\n", worker_id, conflict);
        conflict = clause_addr;
    } else {
        // Unit clause found, propagate
        output.verbose(CALL_INFO, 3, 0,
            "    [W%d]forces literal %d (to true)\n", worker_id, toInt(first));
        trailEnqueue(first, clause_addr);
    }
}


//-----------------------------------------------------------------------------------
// analyze
//-----------------------------------------------------------------------------------

void SATSolver::analyze() {
    output.verbose(CALL_INFO, 3, 0,
        "ANALYZE: Starting conflict analysis of clause 0x%x\n", conflict);
    
    // Debug print for trail
    if (output.getVerboseLevel() >= 3) {
        int j = 0;
        output.verbose(CALL_INFO, 3, 0, "Trail (%zu):", trail.size());
        for (int i = 0; i < trail.size(); i++) {
            if (i == trail_lim[j]) {
                output.output("\n    dec=%d: ",j);
                j++;
            }
            output.output(" %d", toInt(trail[i]));
        }
        output.output("\n");
    }

    // First UIP scheme
    learnt_clause.clear();
    learnt_clause.resize(1);  // Reserve space for the asserting literal
    
    int pathC = 0;  // Counter for literals at current decision level
    Lit p = lit_Undef;
    int index = trail.size() - 1;
    
    // Add literals from conflict clause to learnt clause
    do {
        assert(conflict != ClauseRef_Undef); // (otherwise should be UIP)
        const Clause& c = clauses.readClause(conflict);
        
        // Bump activity for learnt clauses
        if (clauses.isLearnt(conflict))
            claBumpActivity(conflict, c.act());
        
        // Debug print for current clause
        output.verbose(CALL_INFO, 5, 0, "ANALYZE: current clause (0x%x): %s\n",
            conflict, printClause(c.literals).c_str());

        // For each literal in the clause
        for (size_t i = (p == lit_Undef) ? 0 : 1; i < c.litSize(); i++) {
            Lit q = c[i];
            Var v = var(q);
            output.verbose(CALL_INFO, 5, 0,
                "ANALYZE: Processing literal %d\n", toInt(q));
            
            // Read variable data individually for each variable in the conflict clause
            Variable v_data = variables.readVar(v);

            if (!seen[v] && v_data.level > 0) {
                order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::BUMP, v));
                unstalled_heap = true;
                unstalled_cnt++;

                seen[v] = 1;
                output.verbose(CALL_INFO, 5, 0,
                    "ANALYZE:     Marking var %d as seen\n", v);
                
                if (v_data.level >= current_level()) {
                    pathC++;  // Count literals at current decision level
                    output.verbose(CALL_INFO, 5, 0,
                        "ANALYZE:     At current level, pathC=%d\n", pathC);
                } else {
                    // Literals from earlier decision levels go directly to the learnt clause
                    learnt_clause.push_back(q);
                    output.verbose(CALL_INFO, 5, 0,
                        "ANALYZE:     Added to learnt clause (earlier level %zu)\n", 
                        v_data.level);
                }
            }
        }

        // Select next literal to expand from the trail
        while (!seen[var(trail[index--])]);
        p = trail[index+1];
        conflict = variables.getReason(var(p));

        seen[var(p)] = 0;
        pathC--;
        
        output.verbose(CALL_INFO, 5, 0,
            "ANALYZE: Selected trail literal %d, index %d, reason=0x%x, pathC=%d\n",
            toInt(p), index, conflict, pathC);
        
    } while (pathC > 0);
    
    // Add the 1-UIP literal as the first in the learnt clause
    learnt_clause[0] = ~p;

    // Print learnt clause for debug
    output.verbose(CALL_INFO, 3, 0, "ANALYZE: learnt: %s\n",
        printClause(learnt_clause).c_str());
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

    output.verbose(CALL_INFO, 3, 0, "Backtrack Level = %d\n", bt_level);
    output.verbose(CALL_INFO, 3, 0, "Final learnt clause: %s\n",
        printClause(learnt_clause).c_str());
    if (OVERLAP_HEAP_BUMP) {
        state = WAIT_HEAP;
        next_state = BACKTRACK;
    } else state = BACKTRACK;
}

//-----------------------------------------------------------------------------------
// backtrack
//-----------------------------------------------------------------------------------

void SATSolver::backtrack(int backtrack_level) {
    output.verbose(CALL_INFO, 3, 0, "BACKTRACK From level %d to level %d\n", 
        current_level(), backtrack_level);
    
    unstalled_heap = true;
    // Unassign all variables above backtrack_level using the trail
    for (int i = trail.size() - 1; i >= int(trail_lim[backtrack_level]); i--) {
        Lit p = trail[i];
        Var v = var(p);
        
        polarity[v] = sign(p);
        unassignVariable(v);

        insertVarOrder(v);
        unstalled_cnt++;
        
        output.verbose(CALL_INFO, 5, 0,
            "BACKTRACK: Unassigning x%d, saved polarity %s\n", 
            v, polarity[v] ? "false" : "true");
    }
    
    qhead = trail_lim[backtrack_level];
    trail.resize(trail_lim[backtrack_level]);
    trail_lim.resize(backtrack_level);
    output.verbose(CALL_INFO, 4, 0, "Insert %d vars in heap in parallel\n", unstalled_cnt);
}

//-----------------------------------------------------------------------------------
// clause deletion
//-----------------------------------------------------------------------------------
// Remove half of the learnt clauses, 
// minus the clauses locked by the current assignment. 
// Locked clauses are clauses that are reason to some assignment. 
// Binary clauses are never removed.
void SATSolver::reduceDB() {
    output.verbose(CALL_INFO, 3, 0, "REDUCEDB: Starting clause database reduction\n");
    
    size_t nl = nLearnts();
    std::vector<Cref> learnts_addr = clauses.readAllAddr();
    std::vector<float> activities = clauses.readAllAct(learnts_addr);

    // Create pairs of (idx, activity) for sorting
    std::vector<std::pair<Cref, float>> learnts(nl);
    for (uint32_t i = 0; i < nl; i++) {
        learnts[i] = std::make_pair(learnts_addr[i], activities[i]);
    }
    
    // 2. Sort learnt clauses by activity
    std::sort(learnts.begin(), learnts.end(), [&](const auto& a, const auto& b) {
        Cref i = a.first, j = b.first;
        return clauses.getClauseSize(i) > 2 && (clauses.getClauseSize(j) == 2 || a.second < b.second);
    });
    
    // 3. Extra activity limit for removal
    double extra_lim = learnts.size() > 0 ? cla_inc / learnts.size() : 0;
    
    output.verbose(CALL_INFO, 3, 0, 
        "REDUCEDB: Found %zu learnt clauses, extra_lim = %f\n", 
        learnts.size(), extra_lim);
    
    // 4. mark for removal
    std::vector<Cref> to_keep;
    int removed = 0;
    for (size_t i = 0; i < learnts.size(); i++) {
        Cref addr = learnts[i].first;
        float act = learnts[i].second;
        uint32_t cls_size = clauses.getClauseSize(addr);

        // Only remove non-binary, unlocked clauses
        if (cls_size > 2 && !locked(addr) && (i < learnts.size() / 2 || act < extra_lim)) {
            output.verbose(CALL_INFO, 4, 0, 
                "REDUCEDB: Marking clause 0x%x for removal\n", addr);

            // remove watchers
            detachClause(addr);
            clauses.freeClause(addr, cls_size);
            removed++;
        }
        else to_keep.push_back(addr);
    }

    // 5. Compact clauses by moving non-removed learnt clauses forward
    clauses.reduceDB(to_keep);

    output.verbose(CALL_INFO, 3, 0, 
        "REDUCEDB: Removed %d learnt clauses, new clause count: %zu\n", 
        removed, clauses.size());
        
    stat_db_reductions->addData(1);
    stat_removed->addDataNTimes(removed, 1);
}

//-----------------------------------------------------------------------------------
// Trail Management
//-----------------------------------------------------------------------------------

void SATSolver::trailEnqueue(Lit literal, int reason) {
    Var v = var(literal);
    var_assigned[v] = true;
    var_value[v] = !sign(literal);
    
    Variable var_data;
    var_data.level = current_level();
    var_data.reason = reason;
    variables[v] = var_data;
    
    // Add to trail
    trail.push_back(literal);
    stat_assigns->addData(1);
    output.verbose(CALL_INFO, 5, 0,"ASSIGN: x%d = %d at level %d due to clause %d\n", 
        v, var_value[v] ? 1 : 0, current_level(), reason);
}

void SATSolver::unassignVariable(Var v) {
    var_assigned[v] = false;
    stat_unassigns->addData(1);
}

//-----------------------------------------------------------------------------------
// Two-Watched Literals
//-----------------------------------------------------------------------------------

void SATSolver::attachClause(Cref clause_addr) {
    const Clause& c = clauses.readClause(clause_addr);
    // Watch the first two literals in the clause, use each other as a blocker
    output.verbose(CALL_INFO, 5, 0, "ATTACH: clause 0x%x with literals %d and %d\n",
        clause_addr, toInt(c[0]), toInt(c[1]));
    watches[toWatchIndex(~c[0])].insert(clause_addr, c[1]);
    watches[toWatchIndex(~c[1])].insert(clause_addr, c[0]);
}

void SATSolver::detachClause(Cref clause_addr) {
    const Clause& c = clauses.readClause(clause_addr);
    output.verbose(CALL_INFO, 6, 0, "DETACH: clause 0x%x from watcher %d and %d\n",
        clause_addr, toInt(~c[0]), toInt(~c[1]));
    watches[toWatchIndex(~c[0])].remove(clause_addr);
    watches[toWatchIndex(~c[1])].remove(clause_addr);
}


//-----------------------------------------------------------------------------------
// Decision Heuristics
//-----------------------------------------------------------------------------------
Lit SATSolver::chooseBranchVariable() {
    Var next = var_Undef;
    if (!order_heap->empty() && drand(random_seed) < random_var_freq) {
        int rand_idx = irand(random_seed, order_heap->size());
        order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::READ, rand_idx));
        (*yield_ptr)();
        next = heap_resp;

        if (!var_assigned[next] && decision[next]) {
            output.verbose(CALL_INFO, 3, 0, "DECISION: Random selection of var %d\n", next);
        }
    }
    
    while (next == var_Undef || var_assigned[next] || !decision[next]) {
        if (order_heap->empty()) {
            next = var_Undef;
            break;
        }
        order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::REMOVE_MIN));
        (*yield_ptr)();
        next = heap_resp;
        assert(next != var_Undef);
    }

    output.verbose(CALL_INFO, 3, 0, "DECISION: Selected lit %d \n", toInt(mkLit(next, polarity[next])));
    if (next == var_Undef) return lit_Undef;
    return mkLit(next, polarity[next]);
}

void SATSolver::insertVarOrder(Var v) {
    if (decision[v]) {
        order_heap->handleRequest(new HeapReqEvent(HeapReqEvent::INSERT, v));
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

// Bump activity for a specific clause
void SATSolver::claBumpActivity(Cref clause_addr, float act) {
    clauses.writeAct(clause_addr, act + cla_inc);

    if ((act + cla_inc) > 1e20) {
        // Rescale all clause activities if they get too large
        output.verbose(CALL_INFO, 3, 0, "ACTIVITY: Rescaling all clause activities\n");
        clauses.rescaleAllAct(1e-20);
        cla_inc *= 1e-20;
    }

    output.verbose(CALL_INFO, 4, 0, "ACTIVITY: Bumped clause 0x%x\n", clause_addr);
}

// Check if a clause is "locked" -- cannot be removed
bool SATSolver::locked(Cref clause_addr) {
    const Clause& c = clauses.readClause(clause_addr);
    assert(c.litSize() != 0);
    Var v = var(c[0]);
    int reason = variables.getReason(v);
    
    return var_assigned[v] && 
           value(c[0]) == true &&   // First literal is true
           reason == clause_addr;   // This clause is the reason
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
