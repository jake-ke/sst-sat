#include <sst/core/sst_config.h> // This include is REQUIRED for all implementation files
#include "satsolver.h"
#include <sst/core/interfaces/stdMem.h>

SATSolver::SATSolver(SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id), 
    state(INIT),
    requestPending(false) {
    
    // Initialize output
    output.init("SATSolver-" + getName() + "-> ", 
                params.find<int>("verbose", 0), 
                0, 
                SST::Output::STDOUT);

    // Configure clock
    registerClock(params.find<std::string>("clock", "1GHz"),
                 new SST::Clock::Handler<SATSolver>(this, &SATSolver::clockTick));

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
    stat_backtracks = registerStatistic<uint64_t>("backtracks");
    stat_assigned_vars = registerStatistic<uint64_t>("assigned_vars");
    
    // Component should not end simulation until solution is found
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();

    // Get file size parameter
    filesize = params.find<size_t>("filesize", 0);
    if (filesize == 0) {
        output.fatal(CALL_INFO, -1, "File size parameter not provided\n");
    }
}

SATSolver::~SATSolver() {}

// Add lifecycle implementations
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
}

void SATSolver::handleMemEvent(SST::Interfaces::StandardMem::Request* req) {
    SST::Interfaces::StandardMem::ReadResp* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req);
    
    if (resp) {
        if (state == INIT) {
            // Convert byte data back to string
            std::vector<uint8_t>& data = resp->data;
            dimacs_content = std::string(data.begin(), data.end());
            // output.output("Raw memory content:\n%s\n", dimacs_content.c_str());
            output.verbose(CALL_INFO, 1, 0, "Received %zu bytes from memory\n", resp->data.size());
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
                output.verbose(CALL_INFO, 1, 0, "Parsed %u variables, %u clauses\n", num_vars, num_clauses);
                state = SOLVING;
            }
            return false;
            
        case SOLVING:
            if (solveCDCL()) {  // Changed from solveDPLL to solveCDCL
                output.verbose(CALL_INFO, 1, 0, "Solver Done\n");
                state = DONE;
            }
            return false;
            
        case DONE:
            return true;
    }
    return false;
}

void SATSolver::parseDIMACS(const std::string& content) {
    output.output("Starting DIMACS parsing\n");
    std::istringstream iss(content);
    std::string line;
    
    num_vars = 0;
    num_clauses = 0;
    clauses.clear();
    variables.clear();
    watches.clear();
    
    while (std::getline(iss, line)) {
        // Skip empty lines
        if (line.empty()) continue;
        
        // Skip whitespace at start
        size_t firstChar = line.find_first_not_of(" \t");
        if (firstChar == std::string::npos) continue;
        
        // Process based on first character
        switch (line[firstChar]) {
            case 'c':  // Comment line
                output.verbose(CALL_INFO, 4, 0, "Comment: %s\n", line.substr(firstChar + 1).c_str());
                break;
                
            case 'p': {  // Problem line
                std::istringstream pline(line);
                std::string p, cnf;
                pline >> p >> cnf;
                if (cnf != "cnf") {
                    output.fatal(CALL_INFO, -1, "Invalid DIMACS format: expected 'cnf' but got '%s'\n", cnf.c_str());
                }
                pline >> num_vars >> num_clauses;
                output.verbose(CALL_INFO, 1, 0, "Problem: vars=%u clauses=%u\n", num_vars, num_clauses);
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
                
                if (!clause.literals.empty()) {
                    clauses.push_back(clause);
                    std::ostringstream clause_output;
                    clause_output << "Added clause " << clauses.size() - 1 << ":";
                    for (const Lit& lit : clause.literals) {
                        clause_output << " " << toInt(lit);
                    }
                    output.verbose(CALL_INFO, 4, 0, "%s\n", clause_output.str().c_str());
                }
                break;
            }
        }
    }
    
    // Verify parsing results
    if (clauses.size() != num_clauses) {
        output.fatal(CALL_INFO, -1, "Parsing error: Expected %u clauses but got %zu\n", 
            num_clauses, clauses.size());
    }
    
    if (variables.size() > num_vars + 1) {
        output.fatal(CALL_INFO, -1, "Parsing error: Found %zu variables but expected only %u\n", 
            variables.size(), num_vars);
    }
    
    output.output("Successfully parsed %zu clauses with %zu variables\n", 
        clauses.size(), variables.size());
    
    // Setup watched literals for all clauses
    for (int i = 0; i < clauses.size(); i++) {
        attachClause(i);
    }
    
    // Print watch lists after parsing
    if (output.getVerboseLevel() >= 4) {
        output.output("Watch lists after parsing:\n");
        for (size_t i = 0; i < watches.size(); i++) {
            const std::vector<Watcher>& watchers = watches[i];
            
            if (!watchers.empty()) {
                // Convert watch index back to literal for display
                Lit lit = toLit((i % 2 == 0) ? (i/2) : -(i/2));
                output.output("  Watch list for ~%d, idx %zu, (%zu watchers):", toInt(~lit), i, watchers.size());
                for (const auto& w : watchers) {
                    output.output(" [C%d,b=%d]", w.clause_idx, toInt(w.blocker));
                }
                output.output("\n");
            }
        }
        output.output("\n");
    }

    // initialization
    qhead = 0;
    seen.resize(variables.size(), 0);
}

void SATSolver::attachClause(int clause_idx) {
    Clause& c = clauses[clause_idx];
    if (c.literals.size() > 1) {
        // Watch the first two literals in the clause
        Lit not_lit0 = ~c.literals[0];
        Lit not_lit1 = ~c.literals[1];
        insert_watch(not_lit0, Watcher(clause_idx, c.literals[1]));
        insert_watch(not_lit1, Watcher(clause_idx, c.literals[0]));
    } else if (c.literals.size() == 1) {
        // Unit clause - immediately assign the variable at level 0
        Var v = var(c.literals[0]);
        variables[v].assigned = true;
        variables[v].value = !sign(c.literals[0]);
        variables[v].level = 0;
        variables[v].reason = ClauseRef_Undef;  // Changed from -1
        stat_assigned_vars->addData(1);
        output.verbose(CALL_INFO, 3, 0, "ASSIGN: x%d = %d at level %zu due to unit clause %d\n", 
            v, variables[v].value ? 1 : 0, variables[v].level, clause_idx);
    }
}

// Updated unit propagation using 2WL scheme - returns conflict clause index or ClauseRef_Undef if no conflict
int SATSolver::unitPropagate() {
    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: Starting unit propagation\n");
    int conflict = ClauseRef_Undef;

    while (qhead < trail.size()) {
        Lit p = trail[qhead++];
        
        // Process the watched clauses containing ~p
        Lit not_p = ~p;
        int watch_idx = toWatchIndex(p);
        
        if (watch_idx >= (int)watches.size())
            continue;  // No watches for this literal
        
        std::vector<Watcher>& ws = watches[watch_idx];
        
        output.verbose(CALL_INFO, 3, 0, "PROPAGATE: Processing %zu watchers for literal %d (to 1)\n", 
            ws.size(), toInt(p));
        
        size_t i = 0;
        size_t j = 0;
        
        for (i = j = 0; i != ws.size();) {
            // Try to avoid inspecting the clause using the blocker
            Lit blocker = ws[i].blocker;
            Var blocker_var = var(blocker);
            
            // Debug info for watchers
            output.verbose(CALL_INFO, 4, 0, "  Watcher[%zu]: clause %d, blocker %d\n", 
                i, ws[i].clause_idx, toInt(blocker));
            
            if (variables[blocker_var].assigned && variables[blocker_var].value == !sign(blocker)) {
                // Blocker is true, skip to next watcher
                output.verbose(CALL_INFO, 4, 0, "    Blocker is true, skipping\n");
                ws[j++] = ws[i++];
                continue;
            }
            
            // Need to inspect the clause
            int clause_idx = ws[i].clause_idx;
            Clause& c = clauses[clause_idx];
            
            // Print clause for debugging
            if (output.getVerboseLevel() >= 5) {
                std::string clause_str = "    Clause: ";
                for (const auto& lit : c.literals) {
                    clause_str += std::to_string(toInt(lit)) + " ";
                }
                output.verbose(CALL_INFO, 5, 0, "%s\n", clause_str.c_str());
            }
            
            // Make sure the false literal (~p) is at position 1
            if (c.literals[0] == not_p) {
                std::swap(c.literals[0], c.literals[1]);
                output.verbose(CALL_INFO, 5, 0, "    Swapped literals 0 and 1\n");
            }
            assert(c.literals[1] == not_p);
            i++;
            
            // If first literal is already true, just update the blocker and continue
            Lit first = c.literals[0];
            Var first_var = var(first);
            Watcher w = Watcher(clause_idx, first);
            
            if (variables[first_var].assigned && variables[first_var].value == !sign(first)) {
                output.verbose(CALL_INFO, 4, 0, "    First literal %d is true\n", toInt(first));
                ws[j++] = w;
                continue;
            }
            
            // Look for a new literal to watch
            for (size_t k = 2; k < c.size(); k++) {
                Lit lit = c.literals[k];
                Var lit_var = var(lit);
                if (!variables[lit_var].assigned || variables[lit_var].value == !sign(lit)) {
                    // Swap to position 1 and update watcher
                    std::swap(c.literals[1], c.literals[k]);
                    insert_watch(~c.literals[1], w);
                    output.verbose(CALL_INFO, 4, 0, "    Found new watch: literal %d at position %zu\n", 
                        toInt(c.literals[1]), k);
                    goto NextClause;  // we have postponed the deletion of the watcher
                }
            }
                        
            // Did not find a new watch - clause is unit or conflicting
            ws[j++] = w; // Keep watching current literals
            output.verbose(CALL_INFO, 4, 0, "    No new watch found\n");
            
            // Check if first literal is false (conflict) or undefined (unit)
            if (variables[first_var].assigned && variables[first_var].value == sign(first)) {
                // Conflict detected
                // j may lag behind i due to a previous to-be-deleted watcher
                // before returning the conflict, need to remove potential watchers
                output.verbose(CALL_INFO, 3, 0, "CONFLICT: Clause %d has all literals false\n", clause_idx);
                qhead = trail.size();
                conflict = clause_idx;
                
                // Copy remaining watchers
                while (i < ws.size())
                    ws[j++] = ws[i++];
            } else {
                // Unit clause found, propagate
                output.verbose(CALL_INFO, 3, 0, "UNIT: Clause %d forces literal %d (to 1)\n", clause_idx, toInt(first));
                trailEnqueue(first, clause_idx);
                stat_propagations->addData(1);
            }
        NextClause:;
        }
        
        ws.resize(ws.size() - (i-j)); // Remove deleted watchers
    }
    
    return conflict;
}

// Updated to track reason for propagation and add to trail
void SATSolver::trailEnqueue(Lit literal, int reason) {
    trail.push_back(literal);  // Add to trail
    Var v = var(literal);
    variables[v].assigned = true;
    variables[v].value = !sign(literal);
    variables[v].level = current_level();
    variables[v].reason = reason;
    
    stat_assigned_vars->addData(1);
    output.verbose(CALL_INFO, 3, 0, "ASSIGN: x%d = %d at level %d due to clause %d\n", 
        v, variables[v].value ? 1 : 0, current_level(), reason);
}

// Core CDCL implementation
bool SATSolver::solveCDCL() {
    output.verbose(CALL_INFO, 2, 0, "=== New CDCL Solver Step Start ===\n");
    
    // Unit propagation - now returns a conflict clause index or ClauseRef_Undef
    int conflict = unitPropagate();
    
    if (conflict != ClauseRef_Undef) {  // Changed from (size_t)-1
        // Conflict detected during propagation
        output.verbose(CALL_INFO, 2, 0, "CONFLICT: Found unsatisfiable clause %d\n", conflict);
        
        if (trail_lim.empty()) {
            // Conflict at decision level 0 means the formula is UNSAT
            output.output("UNSAT: Formula is unsatisfiable - conflict at decision level 0\n");
            primaryComponentOKToEndSim();
            return true;
        }
        
        // Analyze conflict and learn a new clause
        std::vector<Lit> learnt_clause;
        int backtrack_level;
        analyze(conflict, learnt_clause, backtrack_level);
        
        // Backtrack to the appropriate level
        backtrack(backtrack_level);
        
        // Add the learned clause
        Clause new_clause(learnt_clause, true);
        int clause_idx = clauses.size();
        clauses.push_back(new_clause);
        attachClause(clause_idx);
        
        trailEnqueue(learnt_clause[0], clause_idx);
        return false;
    }

    // Make a new decision
    if (!decide()) {
        output.output("SAT: Formula is satisfiable\nSolution: ");
        for (Var v = 1; v < (Var)variables.size(); v++) {
            output.output("x%d=%d ", v, variables[v].value);
        }
        output.output("\n");
        primaryComponentOKToEndSim();
        return true;
    }

    return false;
}

bool SATSolver::decide() {
    Var v = chooseBranchVariable();
    if (v == 0) {
        output.verbose(CALL_INFO, 2, 0, "DECIDE: No unassigned variables left\n");
        return false;
    }

    // Make a positive literal for the decision
    Lit lit = mkLit(v, false);
    trail_lim.push_back(trail.size());
    trailEnqueue(lit);
 
    output.verbose(CALL_INFO, 3, 0, "DECIDE: setting x%d = true at level %d\n", v, current_level());
    stat_decisions->addData(1);
        return true;
}

// Updated backtracking with non-chronological backtrack support
void SATSolver::backtrack(int backtrack_level) {
    output.verbose(CALL_INFO, 3, 0, "BACKTRACK: From level %d to level %d\n", 
        current_level(), backtrack_level);
    
    // Unassign all variables above backtrack_level using the trail
    for (int i = trail.size() - 1; i >= int(trail_lim[backtrack_level]); i--) {
        Var v = var(trail[i]);
        unassignVariable(v);
        output.verbose(CALL_INFO, 3, 0, "BACKTRACK: Unassigning x%d from level %zu\n", 
            v, variables[v].level);
        stat_backtracks->addData(1);
    }
    
    qhead = trail_lim[backtrack_level];
    trail.resize(trail_lim[backtrack_level]);
    trail_lim.resize(backtrack_level);
}

// Utility functions
void SATSolver::unassignVariable(int var) {
    variables[var].assigned = false;
    stat_assigned_vars->addData(-1);  // One less variable assigned
}

int SATSolver::chooseBranchVariable() {
    for (Var v = 1; v < (Var)variables.size(); v++) {
        if (!variables[v].assigned) {
            return v; // Return first unassigned variable
        }
    }
    return 0; // No unassigned variables
}

void SATSolver::insert_watch(Lit p, Watcher w) {
    int idx = toWatchIndex(p);
    if (idx >= (int)watches.size()) {
        watches.resize(idx + 1);
    }
    watches[idx].push_back(w);
}

void SATSolver::ensureVarCapacity(Var v) {
    if (v >= (int)variables.size()) {
        variables.resize(v + 1, Variable());
    }
}

// Conflict analysis function to learn from conflicts
void SATSolver::analyze(int conflict_idx, std::vector<Lit>& learnt_clause, int& backtrack_level) {
    output.verbose(CALL_INFO, 3, 0, "ANALYZE: Starting conflict analysis of clause %d\n", conflict_idx);
    
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
        Clause& c = clauses[conflict_idx];
        
        // Debug print for current clause
        if (output.getVerboseLevel() >= 4) {
            std::string clause_str = "ANALYZE: Processing clause " + std::to_string(conflict_idx) + ":";
            for (const auto& lit : c.literals) {
                clause_str += " " + std::to_string(toInt(lit));
            }
            output.verbose(CALL_INFO, 4, 0, "%s\n", clause_str.c_str());
        }

        // For each literal in the clause
        for (size_t i = (p == lit_Undef) ? 0 : 1; i < c.size(); i++) {
            Lit q = c.literals[i];
            Var v = var(q);
            
            output.verbose(CALL_INFO, 5, 0, "ANALYZE:   Examining literal %d (var %d, level %zu)\n", 
                toInt(q), v, variables[v].level);
            
            if (!seen[v] && variables[v].level > 0) {
                seen[v] = 1;
                output.verbose(CALL_INFO, 5, 0, "ANALYZE:     Marking var %d as seen\n", v);
                
                if (variables[v].level >= current_level()) {
                    pathC++;  // Count literals at current decision level
                    output.verbose(CALL_INFO, 5, 0, "ANALYZE:     At current level, pathC=%d\n", pathC);
                } else {
                    // Literals from earlier decision levels go directly to the learnt clause
                    learnt_clause.push_back(q);
                    output.verbose(CALL_INFO, 5, 0, "ANALYZE:     Added to learnt clause (earlier level %zu)\n", 
                        variables[v].level);
                }
            }
        }

        // Select next literal to expand from the trail
        int old_index = index;
        while (!seen[var(trail[index--])]);
        p = trail[index+1];
        conflict_idx = variables[var(p)].reason;
        seen[var(p)] = 0;
        pathC--;
        
        output.verbose(CALL_INFO, 4, 0, "ANALYZE: Selected trail literal %d (var %d), moved from index %d to %d, reason=%d, pathC=%d\n", 
            toInt(p), var(p), old_index, index, conflict_idx, pathC);
        
    } while (pathC > 0);
    
    // Add the 1-UIP literal as the first in the learnt clause
    learnt_clause[0] = ~p;
    
    // Find backtrack level
    if (learnt_clause.size() == 1) {
        // 0 if only one literal in learnt clause
        backtrack_level = 0;
    } else {
        // Find the second highest level in the clause
        int max_i = 1;
        for (size_t i = 2; i < learnt_clause.size(); i++) {
            if (variables[var(learnt_clause[i])].level > variables[var(learnt_clause[max_i])].level) {
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
    for (size_t j = 0; j < learnt_clause.size(); j++) {
        seen[var(learnt_clause[j])] = 0;
    }
    
    // Print learnt clause for debug
    if (output.getVerboseLevel() >= 3) {
        std::string clause_str = "LEARNT CLAUSE:";
        for (const auto& lit : learnt_clause) {
                        clause_str += " " + std::to_string(toInt(lit));
        }
        output.verbose(CALL_INFO, 3, 0, "%s, backtrack_level=%d\n", 
            clause_str.c_str(), backtrack_level);
    }
}
