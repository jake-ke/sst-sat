#include <sst/core/sst_config.h> // This include is REQUIRED for all implementation files
#include "satsolver.h"
#include <sst/core/interfaces/stdMem.h>
#include "sst/core/statapi/stataccumulator.h"
#include <algorithm>  // For std::sort
#include <fstream>    // For reading decision file
#include <cmath>      // For pow function

//-----------------------------------------------------------------------------------
// Component Lifecycle Methods
//-----------------------------------------------------------------------------------

SATSolver::SATSolver(SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id), 
    state(INIT),
    order_heap(VarOrderLt(activity)),
    random_seed(91648253),
    var_inc(1.0),
    cla_inc(1.0),
    learntsize_factor((double)1/(double)3),
    learntsize_inc(1.1),
    learnt_adjust_start_confl(100),
    learnt_adjust_inc(1.5),
    requestPending(false),
    ccmin_mode(2),
    decision_seq_idx(0),
    has_decision_sequence(false),
    luby_restart(true),
    restart_first(100),
    restart_inc(2.0),
    curr_restarts(0),
    conflicts_until_restart(restart_first),
    conflictC(0) {
    
    // Initialize output
    output.init("SATSolver-" + getName() + "-> ", 
                params.find<int>("verbose", 0), 
                0, 
                SST::Output::STDOUT);

    // Configure clock
    registerClock(params.find<std::string>("clock", "1GHz"),
                 new SST::Clock::Handler<SATSolver>(this, &SATSolver::clockTick));

    // Get file size parameter
    filesize = params.find<size_t>("filesize", 0);
    if (filesize == 0) {
        output.fatal(CALL_INFO, -1, "File size parameter not provided\n");
    }

    sort_clauses = params.find<bool>("sort_clauses", true);

    // Initialize activity-related variables
    var_decay = params.find<double>("var_decay", 0.95);
    clause_decay = params.find<double>("clause_decay", 0.999);  // Add clause decay parameter
    random_var_freq = params.find<double>("random_var_freq", 0.0);
    
    // Load decision sequence if provided
    std::string decision_file = params.find<std::string>("decision_file", "");
    if (!decision_file.empty()) {
        output.verbose(CALL_INFO, 1, 0, "Loading decision sequence from %s\n", decision_file.c_str());
        loadDecisionSequence(decision_file);
        has_decision_sequence = true;
    }

    // Configure StandardMem interface
    SST::Interfaces::StandardMem::Handler<SATSolver>* handler = 
        new SST::Interfaces::StandardMem::Handler<SATSolver>(this, &SATSolver::handleMemEvent);
    
    memory = loadUserSubComponent<SST::Interfaces::StandardMem>(
        "memory", 
        SST::ComponentInfo::SHARE_NONE,
        getTimeConverter("1GHz"),  // Time base for memory interface
        handler                    // Event handler
    );

    if (!memory) {
        output.fatal(CALL_INFO, -1, "Unable to load StandardMem SubComponent\n");
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
    
    // Component should not end simulation until solution is found
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

SATSolver::~SATSolver() {}

void SATSolver::init(unsigned int phase) {
    memory->init(phase);
}

void SATSolver::setup() {
    memory->setup();
    
    // Initial memory read moved here from constructor
    if (!requestPending) {
        SST::Interfaces::StandardMem::Request* req;
        // Read exact file size
        req = new SST::Interfaces::StandardMem::Read(0, filesize);  
        memory->send(req);
        requestPending = true;
        output.verbose(CALL_INFO, 1, 0, "Sent memory read request for %zu bytes\n", filesize);
    }
}

void SATSolver::complete(unsigned int phase) {
    memory->complete(phase);
}

void SATSolver::finish() {
    memory->finish();
    
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
    output.output("Variables    : %zu (Total), %lu (Assigned)\n", 
        variables.size() - 1,
        getStatCount(stat_assigns) - getStatCount(stat_unassigns));
    output.output("Clauses      : %zu (Total), %lu (Learned)\n", 
        clauses.size(), 
        getStatCount(stat_learned) - getStatCount(stat_removed));
    output.output("===========================================================================\n");
}

//-----------------------------------------------------------------------------------
// Event Handling Methods
//-----------------------------------------------------------------------------------

void SATSolver::handleMemEvent(SST::Interfaces::StandardMem::Request* req) {
    SST::Interfaces::StandardMem::ReadResp* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req);
    
    if (resp) {
        if (state == INIT) {
            // Convert byte data back to string
            std::vector<uint8_t>& data = resp->data;
            dimacs_content = std::string(data.begin(), data.end());
            // output.output("Raw memory content:\n%s\n", dimacs_content.c_str());
            output.verbose(CALL_INFO, 1, 0,
                "Received %zu bytes from memory\n", resp->data.size());
            state = PARSING;
            requestPending = false;
        }
    }
    delete req;
}

bool SATSolver::clockTick(SST::Cycle_t cycle) {
    switch (state) {
        case INIT:
            // Wait for memory response
            return false;
            
        case PARSING:
            if (!requestPending) {
                parseDIMACS(dimacs_content);
                output.verbose(CALL_INFO, 1, 0,
                    "Parsed %u variables, %u clauses\n", num_vars, num_clauses);
                state = SOLVING;
            }
            return false;
            
        case SOLVING:
            if (solveCDCL()) {
                output.verbose(CALL_INFO, 1, 0, "Solver Done\n");
                state = DONE;
            }
            return false;
            
        case DONE:
            return true;
    }
    return false;
}

//-----------------------------------------------------------------------------------
// Input Processing
//-----------------------------------------------------------------------------------

void SATSolver::parseDIMACS(const std::string& content) {
    output.output("Starting DIMACS parsing\n");
    std::istringstream iss(content);
    std::string line;
    
    num_vars = 0;
    num_clauses = 0;
    clauses.clear();
    variables.clear();
    watches.clear();
    activity.clear();
    polarity.clear();
    decision.clear();
    
    while (std::getline(iss, line)) {
        // Skip empty lines
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
                
                while (clause_iss >> dimacs_lit && dimacs_lit != 0) {
                    Lit lit = toLit(dimacs_lit);
                    clause.literals.push_back(lit);
                    Var v = var(lit);
                    ensureVarCapacity(v);
                }
                
                assert (!clause.literals.empty());
                if (clause.literals.size() == 1) {
                    // Unit clause
                    trailEnqueue(clause.literals[0]);
                    num_clauses--;
                    output.verbose(CALL_INFO, 3, 0,
                        "Unit clause: %d\n",
                        toInt(clause.literals[0]));
                } else {
                    if (sort_clauses) {
                        // Sort literals in the clause
                        std::sort(clause.literals.begin(), clause.literals.end());
                    }
                    clauses.push_back(clause);

                    // debugging outputs
                    std::ostringstream clause_output;
                    clause_output << "Added clause " << clauses.size() - 1 << ":";
                    for (const Lit& lit : clause.literals) {
                        clause_output << " " << toInt(lit);
                    }
                    output.verbose(CALL_INFO, 6, 0, "%s\n",
                        clause_output.str().c_str());
                }
                break;
            }
        }
    }
    
    // Verify parsing results
    if (clauses.size() != num_clauses) {
        output.fatal(CALL_INFO, -1,
            "Parsing error: Expected %u clauses but got %zu\n", 
            num_clauses, clauses.size());
    }
    
    if (variables.size() - 1 > num_vars) {
        output.fatal(CALL_INFO, -1,
            "Parsing error: Found %zu variables but expected only %u\n", 
            variables.size() - 1, num_vars);
    }
    
    // initialization
    qhead = 0;
    seen.resize(variables.size(), 0);
    activity.resize(variables.size(), 0.0);
    polarity.resize(variables.size(), false); // Default phase is false
    decision.resize(variables.size(), true);  // All variables are decision variables
    cls_activity.resize(clauses.size(), 0.0);
    
    // Insert variables into the heap
    for (Var v = 1; v < (Var)variables.size(); v++) insertVarOrder(v);
    
    // Setup watched literals for all clauses
    for (int i = 0; i < clauses.size(); i++) attachClause(i);
    
    // Print watch lists after parsing
    if (output.getVerboseLevel() >= 6) {
        output.output("Watch lists after parsing:\n");
        for (size_t i = 0; i < watches.size(); i++) {
            const std::vector<Watcher>& watchers = watches[i];
            
            if (!watchers.empty()) {
                // Convert watch index back to literal for display
                Lit lit = toLit((i % 2 == 0) ? (i/2) : -(i/2));
                output.output("  Watch list for ~%d, idx %zu, (%zu watchers):",
                    toInt(~lit), i, watchers.size());
                for (const auto& w : watchers) {
                    output.output(" [C%d,b=%d]", w.clause_idx, toInt(w.blocker));
                }
                output.output("\n");
            }
        }
        output.output("\n");
    }
    
    // Initialize learnt clause adjustment parameters
    learnt_adjust_confl = learnt_adjust_start_confl;
    learnt_adjust_cnt = (int)learnt_adjust_confl;
    max_learnts = clauses.size() * learntsize_factor;
    output.verbose(CALL_INFO, 3, 0, "INIT: learnt_adjust_confl %f\n", learnt_adjust_confl);
    output.verbose(CALL_INFO, 3, 0, "INIT: max_learnts %.0f\n", max_learnts);
}

//-----------------------------------------------------------------------------------
// Core CDCL Algorithm
//-----------------------------------------------------------------------------------

bool SATSolver::solveCDCL() {
    output.verbose(CALL_INFO, 2, 0, "=== New CDCL Solver Step Start ===\n");
    
    // Unit propagation: returns a conflict clause index or ClauseRef_Undef
    int conflict = unitPropagate();
    
    if (conflict != ClauseRef_Undef) {
        output.verbose(CALL_INFO, 2, 0,
            "CONFLICT: Found unsatisfiable clause %d\n", conflict);
        
        conflictC++;
        stat_conflicts->addData(1);
        
        if (trail_lim.empty()) {
            // Conflict at decision level 0 means the formula is UNSAT
            output.output("UNSAT: Formula is unsatisfiable (conflict at level 0)\n");
            primaryComponentOKToEndSim();
            return true;
        }
        
        // Analyze conflict and learn a new clause
        std::vector<Lit> learnt_clause;
        int backtrack_level;
        analyze(conflict, learnt_clause, backtrack_level);
        
        // Backtrack to the appropriate level
        backtrack(backtrack_level);
        
        if (learnt_clause.size() == 1){
            // unit learnt clause will be instantly propagated
            trailEnqueue(learnt_clause[0]);
        }
        else{
            // Add the learned clause
            Clause new_clause(learnt_clause);
            int clause_idx = clauses.size();
            clauses.push_back(new_clause);
            attachClause(clause_idx);  
            trailEnqueue(learnt_clause[0], clause_idx);
            claBumpActivity(clause_idx);
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
        
        return false;
    }

    // Check if we need to restart
    if (conflictC >= conflicts_until_restart) {
        // Restart by backtracking to decision level 0
        backtrack(0);
        conflictC = 0;
        curr_restarts++;
        stat_restarts->addData(1);
        
        // Update the restart limit using Luby sequence or geometric progression
        double rest_base = luby_restart ? luby(restart_inc, curr_restarts) : pow(restart_inc, curr_restarts);
        conflicts_until_restart = rest_base * restart_first;
        
        output.verbose(CALL_INFO, 2, 0, 
            "RESTART: #%d, new conflict limit=%d\n", 
            curr_restarts, conflicts_until_restart);
        return false;
    }

    // Check if we need to reduce the learnt clause database
    if (nLearnts() - nAssigns() >= max_learnts) {
        output.verbose(CALL_INFO, 3, 0, 
            "REDUCEDB: Too many learnt clauses (%d - %d >= %.0f)\n", 
            nLearnts(), nAssigns(), max_learnts);
        reduceDB();
    }

    // Make a new decision
    if (!decide()) {
        output.output("SAT: Formula is satisfiable\n");
        if (output.getVerboseLevel() >= 3) {
            for (Var v = 1; v < (Var)variables.size(); v++) {
                output.output("x%d=%d ", v, variables[v].value ? 1 : 0);
            }
            output.output("\n");
        }
        primaryComponentOKToEndSim();
        return true;
    }

    return false;
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
            if (!variables[next_var].assigned && decision[next_var]) {
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
    
    // If we couldn't use the decision sequence, fall back to normal heuristic
    if (lit == lit_Undef) {
        // Fall back to normal decision heuristic if no sequence or exhausted
        lit = chooseBranchVariable();
        if (lit == lit_Undef) {
            output.verbose(CALL_INFO, 2, 0, "DECIDE: No unassigned variables left\n");
            return false;
        }
    }

    trail_lim.push_back(trail.size());  // new decision level
    trailEnqueue(lit);
 
    return true;
}

//-----------------------------------------------------------------------------------
// unitPropagate
//-----------------------------------------------------------------------------------

int SATSolver::unitPropagate() {
    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: Starting unit propagation\n");
    int conflict = ClauseRef_Undef;

    while (qhead < trail.size()) {
        stat_propagations->addData(1);
        Lit p = trail[qhead++];
        Lit not_p = ~p;
        int watch_idx = toWatchIndex(p);
        if (watch_idx >= (int)watches.size())
            continue;  // No watches for this literal
        
        std::vector<Watcher>& ws = watches[watch_idx];
        
        output.verbose(CALL_INFO, 3, 0,
            "PROPAGATE: Processing %zu watchers for literal %d\n", 
            ws.size(), toInt(p));
        
        size_t i, j;
        for (i = j = 0; i != ws.size();) {
            // Try to avoid inspecting the clause using the blocker
            Lit blocker = ws[i].blocker;
            
            // Debug info for watchers
            output.verbose(CALL_INFO, 4, 0,
                "  Watcher[%zu]: clause %d, blocker %d, j = %zu\n", 
                i, ws[i].clause_idx, toInt(blocker), j);
            
            if (variables[var(blocker)].assigned
                && value(blocker) == true) {
                // Blocker is true, skip to next watcher
                output.verbose(CALL_INFO, 4, 0, "    Blocker is true, skipping\n");
                ws[j++] = ws[i++];
                continue;
            }
            
            // Need to inspect the clause
            int clause_idx = ws[i].clause_idx;
            Clause& c = clauses[clause_idx];
            
            // Print clause for debugging
            output.verbose(CALL_INFO, 4, 0, "    Clause: %s\n", printClause(c).c_str());
            
            // Make sure the false literal (~p) is at position 1
            if (c.literals[0] == not_p) {
                std::swap(c.literals[0], c.literals[1]);
                output.verbose(CALL_INFO, 4, 0, "    Swapped literals 0 and 1\n");
            }
            assert(c.literals[1] == not_p);
            i++;
            
            // If first literal is already true, just update the blocker and continue
            Lit first = c.literals[0];
            Watcher w = Watcher(clause_idx, first);
            
            if (variables[var(first)].assigned && value(first) == true) {
                output.verbose(CALL_INFO, 4, 0,
                    "    First literal %d is true\n", toInt(first));
                ws[j++] = w;
                continue;
            }
            
            // Look for a new literal to watch
            for (size_t k = 2; k < c.size(); k++) {
                Lit lit = c.literals[k];
                if (!variables[var(lit)].assigned || value(lit) == true) {
                    // Swap to position 1 and update watcher
                    std::swap(c.literals[1], c.literals[k]);
                    insert_watch(~c.literals[1], w);
                    output.verbose(CALL_INFO, 4, 0, 
                        "    Found new watch: literal %d at position %zu\n", 
                        toInt(c.literals[1]), k);
                    goto NextClause;  // postponed the deletion of the watcher
                }
            }
                        
            // Did not find a new watch - clause is unit or conflicting
            ws[j++] = w; // Keep watching current literals
            output.verbose(CALL_INFO, 4, 0, "    No new watch found\n");
            
            // Check if first literal is false (conflict) or undefined (unit)
            if (variables[var(first)].assigned && value(first) == false) {
                // Conflict detected
                // j may lag behind i due to a previous to-be-deleted watcher
                // before returning the conflict, need to remove potential watchers
                output.verbose(CALL_INFO, 3, 0,
                    "CONFLICT: Clause %d has all literals false\n", clause_idx);
                qhead = trail.size();
                conflict = clause_idx;
                
                // Copy remaining watchers
                while (i < ws.size())
                    ws[j++] = ws[i++];
            } else {
                // Unit clause found, propagate
                output.verbose(CALL_INFO, 3, 0,
                    "UNIT: Clause %d forces literal %d (to true)\n",
                    clause_idx, toInt(first));
                trailEnqueue(first, clause_idx);
            }
        NextClause:;
        }
        
        ws.resize(j); // Remove deleted watchers
    }
    
    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: no more propagations\n");
    return conflict;
}

//-----------------------------------------------------------------------------------
// analyze
//-----------------------------------------------------------------------------------

void SATSolver::analyze(int confl, std::vector<Lit>& learnt_clause, int& backtrack_level) {
    output.verbose(CALL_INFO, 3, 0,
        "ANALYZE: Starting conflict analysis of clause %d\n", confl);
    
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
        assert(confl != ClauseRef_Undef); // (otherwise should be UIP)
        Clause& c = clauses[confl];
        
        // Bump activity for learnt clauses
        if (isLearnt(confl))
            claBumpActivity(confl);
        
        // Debug print for current clause
        output.verbose(CALL_INFO, 4, 0,
            "ANALYZE: Processing clause: %s\n", printClause(c).c_str());

        // For each literal in the clause
        for (size_t i = (p == lit_Undef) ? 0 : 1; i < c.size(); i++) {
            Lit q = c.literals[i];
            Var v = var(q);
            
            output.verbose(CALL_INFO, 5, 0,
                "ANALYZE:   Examining literal %d (var %d, level %zu)\n", 
                toInt(q), v, variables[v].level);
            
            if (!seen[v] && variables[v].level > 0) {
                varBumpActivity(v);
                
                seen[v] = 1;
                output.verbose(CALL_INFO, 5, 0,
                    "ANALYZE:     Marking var %d as seen\n", v);
                
                if (variables[v].level >= current_level()) {
                    pathC++;  // Count literals at current decision level
                    output.verbose(CALL_INFO, 5, 0,
                        "ANALYZE:     At current level, pathC=%d\n", pathC);
                } else {
                    // Literals from earlier decision levels go directly to the learnt clause
                    learnt_clause.push_back(q);
                    output.verbose(CALL_INFO, 5, 0,
                        "ANALYZE:     Added to learnt clause (earlier level %zu)\n", 
                        variables[v].level);
                }
            }
        }

        // Select next literal to expand from the trail
        while (!seen[var(trail[index--])]);
        p = trail[index+1];
        confl = variables[var(p)].reason;
        seen[var(p)] = 0;
        pathC--;
        
        output.verbose(CALL_INFO, 4, 0,
            "ANALYZE: Selected trail literal %d, index %d, reason=%d, pathC=%d\n", 
            toInt(p), index, confl, pathC);
        
    } while (pathC > 0);
    
    // Add the 1-UIP literal as the first in the learnt clause
    learnt_clause[0] = ~p;
    
    // Keep track of literals to clear
    analyze_toclear.clear();
    analyze_toclear = learnt_clause; 
    // Minimize conflict clause:
    if (ccmin_mode != 0) {
        size_t i, j;
        output.verbose(CALL_INFO, 3, 0,
            "ANALYZE: Minimizing clause (size %zu): %s\n", learnt_clause.size(),
            printClause(learnt_clause).c_str());

        if (ccmin_mode == 2) {
            // Deep minimization (more thorough)
            for (i = j = 1; i < learnt_clause.size(); i++) {
                Var v = var(learnt_clause[i]);
                if (variables[v].reason == ClauseRef_Undef || !litRedundant(learnt_clause[i]))
                    learnt_clause[j++] = learnt_clause[i];
            }
        } else if (ccmin_mode == 1) {
            // Basic minimization (faster but less thorough)
            for (i = j = 1; i < learnt_clause.size(); i++) {
                Var v = var(learnt_clause[i]);
                
                if (variables[v].reason == ClauseRef_Undef)
                    learnt_clause[j++] = learnt_clause[i];
                else {
                    Clause& c = clauses[variables[v].reason];
                    for (size_t k = 1; k < c.size(); k++) {
                        Var l = var(c.literals[k]);
                        if (!seen[l] && variables[l].level > 0) {}
                            learnt_clause[j++] = learnt_clause[i];
                            break; }
                }
            }
        } else
            i = j = learnt_clause.size();

        learnt_clause.resize(j);
        
        // Update statistics - count how many literals were removed
        if (i - j > 0) {
            stat_minimized_literals->addDataNTimes(i - j, 1);
            output.verbose(CALL_INFO, 3, 0, 
                "ANALYZE: Minimization removed %zu literals\n", i - j);
        }
    }

    // Find backtrack level
    if (learnt_clause.size() == 1) {
        // 0 if only one literal in learnt clause
        backtrack_level = 0;
    } else {
        // Find the second highest level in the clause
        int max_i = 1;
        for (size_t i = 2; i < learnt_clause.size(); i++) {
            if (variables[var(learnt_clause[i])].level
                > variables[var(learnt_clause[max_i])].level) {
                max_i = i;
            }
        }
        
        // Swap-in this literal at index 1
        Lit p = learnt_clause[max_i];
        learnt_clause[max_i] = learnt_clause[1];
        learnt_clause[1] = p;
        backtrack_level = variables[var(p)].level;
    }

    // Clear seen vector for next analysis
    for (const Lit& l : analyze_toclear) seen[var(l)] = 0;

    // Print learnt clause for debug
    output.verbose(CALL_INFO, 3, 0, "LEARNT CLAUSE: %s\n", printClause(learnt_clause).c_str());
    output.verbose(CALL_INFO, 3, 0, "backtrack_level = %d\n", backtrack_level);
}

//-----------------------------------------------------------------------------------
// backtrack
//-----------------------------------------------------------------------------------

void SATSolver::backtrack(int backtrack_level) {
    output.verbose(CALL_INFO, 3, 0, "BACKTRACK: From level %d to level %d\n", 
        current_level(), backtrack_level);
    
    // Unassign all variables above backtrack_level using the trail
    for (int i = trail.size() - 1; i >= int(trail_lim[backtrack_level]); i--) {
        Lit p = trail[i];
        Var v = var(p);
        
        polarity[v] = sign(p);
        unassignVariable(v);
        insertVarOrder(v);
        
        output.verbose(CALL_INFO, 4, 0,
            "BACKTRACK: Unassigning x%d from level %zu, saved polarity %s\n", 
            v, variables[v].level, polarity[v] ? "false" : "true");
    }
    
    qhead = trail_lim[backtrack_level];
    trail.resize(trail_lim[backtrack_level]);
    trail_lim.resize(backtrack_level);
}

//-----------------------------------------------------------------------------------
// Trail Management
//-----------------------------------------------------------------------------------

void SATSolver::trailEnqueue(Lit literal, int reason) {
    Var v = var(literal);
    variables[v].assigned = true;
    variables[v].value = !sign(literal);
    variables[v].level = current_level();
    variables[v].reason = reason;
    trail.push_back(literal);  // Add to trail

    stat_assigns->addData(1);
    output.verbose(CALL_INFO, 4, 0,"ASSIGN: x%d = %d at level %d due to clause %d\n", 
        v, variables[v].value ? 1 : 0, current_level(), reason);
}

void SATSolver::unassignVariable(Var var) {
    variables[var].assigned = false;
    stat_unassigns->addData(1);
}

//-----------------------------------------------------------------------------------
// Two-Watched Literals
//-----------------------------------------------------------------------------------

void SATSolver::attachClause(int clause_idx) {
    Clause& c = clauses[clause_idx];
    // Watch the first two literals in the clause
    Lit not_lit0 = ~c.literals[0];
    Lit not_lit1 = ~c.literals[1];
    insert_watch(not_lit0, Watcher(clause_idx, c.literals[1]));
    insert_watch(not_lit1, Watcher(clause_idx, c.literals[0]));
}

void SATSolver::detachClause(int clause_idx) {
    Clause& c = clauses[clause_idx];
    remove_watch(watches[toWatchIndex(~c.literals[0])], clause_idx);
    remove_watch(watches[toWatchIndex(~c.literals[1])], clause_idx);
}

void SATSolver::insert_watch(Lit p, Watcher w) {
    int idx = toWatchIndex(p);
    if (idx >= (int)watches.size()) {
        watches.resize(idx + 1);
    }
    watches[idx].push_back(w);
}

void SATSolver::remove_watch(std::vector<Watcher>& ws, int clause_idx) {
    bool found = false;
    for (size_t i = 0; i < ws.size(); i++) {
        Clause c = clauses[ws[i].clause_idx];
        if (ws[i].clause_idx == clause_idx) {
            ws[i] = ws.back();
            ws.pop_back();
            found = true;
            break;
        }
    }
    assert(found);
}

//-----------------------------------------------------------------------------------
// Decision Heuristics
//-----------------------------------------------------------------------------------

Lit SATSolver::chooseBranchVariable() {
    Var next = var_Undef;
    
    if (!order_heap.empty() && drand(random_seed) < random_var_freq) {
        next = order_heap[irand(random_seed, order_heap.size())];
        if (!variables[next].assigned && decision[next]) {
            output.verbose(CALL_INFO, 3, 0, "DECISION: Random selection of var %d\n", next);
        }
    }
    
    while (next == var_Undef || variables[next].assigned || !decision[next]) {
        if (order_heap.empty()) {
            next = var_Undef;
            break;
        } else
            next = order_heap.removeMin();
    }

    output.verbose(CALL_INFO, 5, 0, "DECISION: Selected var %d (activity=%f)\n",
        next, activity[next]);
    if (next == var_Undef) return lit_Undef;
    return mkLit(next, polarity[next]);
}

void SATSolver::insertVarOrder(Var v) {
    if (!order_heap.inHeap(v) && decision[v]) {
        order_heap.insert(v);
        output.verbose(CALL_INFO, 4, 0, "HEAP: Inserted var %d into order heap\n", v);
    }
}

void SATSolver::varDecayActivity() {
    var_inc *= 1.0 / var_decay;
    output.verbose(CALL_INFO, 4, 0,
        "ACTIVITY: Decayed activity increment to %f\n", var_inc);
}

void SATSolver::varBumpActivity(Var v) {
    if ((activity[v] += var_inc) > 1e100) {
        output.verbose(CALL_INFO, 3, 0, "ACTIVITY: Rescaling all activities\n");
        for (size_t i = 0; i < activity.size(); i++) {
            activity[i] *= 1e-100;
        }
        var_inc *= 1e-100;
    }
    
    if (order_heap.inHeap(v)) {
        order_heap.decrease(v);
    }
    
    output.verbose(CALL_INFO, 4, 0, "ACTIVITY: Bumped var %d to %f\n", v, activity[v]);
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
void SATSolver::claBumpActivity(int clause_idx) {
    Clause& c = clauses[clause_idx];
    
    if ((c.activity += cla_inc) > 1e20) {
        // Rescale all clause activities if they get too large
        output.verbose(CALL_INFO, 3, 0, "ACTIVITY: Rescaling all clause activities\n");
        for (size_t i = nLearnts(); i < clauses.size(); i++) {
            clauses[i].activity *= 1e-20;
        }
        cla_inc *= 1e-20;
    }
    
    output.verbose(CALL_INFO, 4, 0, "ACTIVITY: Bumped clause %d to %f\n", 
        clause_idx, c.activity);
}

// Check if a clause is "locked" - exactly matching MiniSat's implementation
bool SATSolver::locked(int clause_idx) {
    const Clause& c = clauses[clause_idx];
    Var v = var(c.literals[0]);
    if (c.size() == 0) return false;
    
    return variables[v].assigned && 
           value(c.literals[0]) == true &&      // First literal is true
           variables[v].reason == clause_idx;   // This clause is the reason
}

// Remove half of the learnt clauses, 
// minus the clauses locked by the current assignment. 
// Locked clauses are clauses that are reason to some assignment. 
// Binary clauses are never removed.
void SATSolver::reduceDB() {
    output.verbose(CALL_INFO, 2, 0, 
        "REDUCEDB: Starting clause database reduction\n");
    
    // 1. Collect learnt clauses indices
    std::vector<int> learnts;
    learnts.reserve(clauses.size() - num_clauses);
    for (int i = num_clauses; i < clauses.size(); i++) {
        learnts.push_back(i);
    }
    
    // 2. Sort learnt clauses by activity
    std::sort(learnts.begin(), learnts.end(), [&](int i, int j) {
        return clauses[i].size() > 2 && 
              (clauses[j].size() == 2 ||
              clauses[i].activity < clauses[j].activity);
    });
    
    // 3. Extra activity limit for removal
    double extra_lim = learnts.size() > 0 ? cla_inc / learnts.size() : 0;
    
    output.verbose(CALL_INFO, 3, 0, 
        "REDUCEDB: Found %zu learnt clauses, extra_lim = %f\n", 
        learnts.size(), extra_lim);
    
    // 4. First pass: mark clauses for removal
    std::vector<bool> to_remove(clauses.size(), false);
    int removed = 0;
    
    for (size_t i = 0; i < learnts.size(); i++) {
        int idx = learnts[i];
        Clause& c = clauses[idx];
        
        // Only remove non-binary, unlocked clauses
        if (c.size() > 2 && !locked(idx) && 
            (i < learnts.size() / 2 || c.activity < extra_lim)) {
            output.verbose(CALL_INFO, 4, 0, 
                "REDUCEDB: Marking clause %d for removal (size=%d, activity=%.2e)\n", 
                idx, c.size(), c.activity);
                
            // Mark for removal and detach from watch lists
            to_remove[idx] = true;
            detachClause(idx);
            removed++;
        }
    }
    
    // 5. Build clause map (old index -> new index)
    std::vector<int> clause_map(clauses.size());
    int i, j;
    for (i = j = num_clauses; i < clauses.size(); i++) {
        if (!to_remove[i]) {
            clause_map[i] = j++;
        } else {
            clause_map[i] = ClauseRef_Undef;
        }
    }
    
    // 6. Update reasons for assigned variables in trail
    for (int i = 0; i < trail.size(); i++) {
        Var v = var(trail[i]);
        int old_reason = variables[v].reason;
        // skip original clauses and decision variables
        if (old_reason >= num_clauses && old_reason != ClauseRef_Undef) {
            // Fatal error: trying to remove a locked clause referenced by a var
            assert(!to_remove[old_reason]);
            variables[v].reason = clause_map[old_reason];
            output.verbose(CALL_INFO, 5, 0, 
                "REDUCEDB: Updated var %d reason from %d to %d\n", 
                v, old_reason, variables[v].reason);
        }
    }
    
    // 7. Update all watch lists with new indices
    for (size_t i = 0; i < watches.size(); i++) {
        std::vector<Watcher>& ws = watches[i];
        size_t new_size = 0;
        for (size_t k = 0; k < ws.size(); k++) {
            int old_idx = ws[k].clause_idx;
            if (old_idx >= num_clauses) {
                // Fatal error: trying to remove a locked clause referenced by a var
                assert(!to_remove[old_idx]);
                // Update index in watcher and keep it
                ws[k].clause_idx = clause_map[old_idx];
                ws[new_size++] = ws[k];
                
                if (clause_map[old_idx] != old_idx) {
                    output.verbose(CALL_INFO, 6, 0, 
                        "REDUCEDB: Updated watcher reference from %d to %d\n",
                        old_idx, ws[k].clause_idx);
                }
            }
        }
        ws.resize(new_size);
    }
    
    // 8. Finally, compact the clauses vector
    std::vector<Clause> new_clauses;
    new_clauses.reserve(clauses.size() - removed);
    
    for (size_t i = 0; i < clauses.size(); i++) {
        if (!to_remove[i]) {
            new_clauses.push_back(clauses[i]);
        }
    }
    
    clauses.swap(new_clauses);
    
    output.verbose(CALL_INFO, 3, 0, 
        "REDUCEDB: Removed %d learnt clauses, new clause count: %zu\n", 
        removed, clauses.size());
        
    stat_db_reductions->addData(1);
    stat_removed->addDataNTimes(removed, 1);
}

//-----------------------------------------------------------------------------------
// Clause Minimization
//-----------------------------------------------------------------------------------

// Check if 'p' can be removed from a conflict clause
bool SATSolver::litRedundant(Lit p) {
    output.verbose(CALL_INFO, 6, 0, "MIN: Checking if %d is redundant\n", toInt(p));
    enum { seen_undef = 0, seen_source = 1, seen_removable = 2, seen_failed = 3 };
    assert(seen[var(p)] == seen_undef || seen[var(p)] == seen_source);
    assert(variables[var(p)].reason != ClauseRef_Undef);
    
    analyze_stack.clear();
    Clause c = clauses[variables[var(p)].reason];
    
    for (size_t i = 1; ; i++) {
        if (i < c.size()) {
            // Examining the literals in the reason clause
            Lit l = c.literals[i];
            Var v = var(l);
            
            // If variable at level 0 or already marked as source/removable, skip it
            if (variables[v].level == 0 || seen[v] == seen_source || seen[v] == seen_removable) {
                continue;
            }
            
            // Cannot remove if var has no reason or was already marked as failed
            if (variables[v].reason == ClauseRef_Undef || seen[v] == seen_failed) {
                // Mark all variables in stack as failed
                analyze_stack.push_back(ShrinkStackElem(0, p));
                for (size_t j = 0; j < analyze_stack.size(); j++) {
                    if (seen[var(analyze_stack[j].l)] == seen_undef) {
                        seen[var(analyze_stack[j].l)] = seen_failed;
                        analyze_toclear.push_back(analyze_stack[j].l);
                    }
                }
                
                output.verbose(CALL_INFO, 5, 0, "MIN: literal %d undefined or failed\n", toInt(l));
                return false;
            }

            // Recursively check this literal
            analyze_stack.push_back(ShrinkStackElem(i, p));
            i = 0;
            p = l;
            c = clauses[variables[var(p)].reason];
        } else {
            // Finished examining current reason clause
            if (seen[var(p)] == seen_undef) {
                seen[var(p)] = seen_removable;
                analyze_toclear.push_back(p);
                output.verbose(CALL_INFO, 5, 0, "MIN: Marked %d as removable\n", toInt(p));
            }
            
            // If stack is empty, we're done
            if (analyze_stack.empty()) {
                output.verbose(CALL_INFO, 4, 0, "MIN: %d is redundant\n", toInt(p));
                return true;
            }
            
            // Continue with next element from stack
            ShrinkStackElem e = analyze_stack.back();
            analyze_stack.pop_back();
            i = e.i;
            p = e.l;
            c = clauses[variables[var(p)].reason];
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

void SATSolver::ensureVarCapacity(Var v) {
    if (v >= (int)variables.size()) {
        variables.resize(v + 1, Variable());
    }
}

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

std::string SATSolver::printClause(const Clause& c) {
    std::string clause_str = "";
    for (const auto& lit : c.literals)
        clause_str += " " + std::to_string(toInt(lit));
    return clause_str;
}