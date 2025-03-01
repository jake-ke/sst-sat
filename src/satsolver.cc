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
    
    // Initialize counters

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
            if (solveDPLL()) {
                output.verbose(CALL_INFO, 1, 0, "Solver Done\n");
                state = DONE;
            }
            return false;
            
        case DONE:
            return true;
    }
    return false;
}

// Helper functions for vector-based watches
void SATSolver::ensureWatchSizeForLiteral(Lit p) {
    int idx = toWatchIndex(p);
    if (idx >= (int)watches.size()) {
        watches.resize(idx + 1);
    }
}

void SATSolver::ensureVarCapacity(Var v) {
    if (v >= (int)variables.size()) {
        variables.resize(v + 1, {false, false, 0});
    }
}

void SATSolver::parseDIMACS(const std::string& content) {
    output.output("Starting DIMACS parsing\n");
    std::istringstream iss(content);
    std::string line;
    
    num_vars = 0;
    num_clauses = 0;
    clauses.clear();
    variables.clear();
    watches.clear(); // Clear watches when parsing a new problem
    
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
    for (size_t i = 0; i < clauses.size(); i++) {
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
                    output.output(" [C%zu,b=%d]", w.clause_idx, toInt(w.blocker));
                }
                output.output("\n");
            }
        }
        output.output("\n");
    }
}

void SATSolver::attachClause(size_t clause_idx) {
    Clause& c = clauses[clause_idx];
    if (c.literals.size() > 1) {
        // Watch the first two literals in the clause
        Lit not_lit0 = ~c.literals[0];
        Lit not_lit1 = ~c.literals[1];
        
        ensureWatchSizeForLiteral(not_lit0);
        ensureWatchSizeForLiteral(not_lit1);
        
        watches[toWatchIndex(not_lit0)].push_back(Watcher(clause_idx, c.literals[1]));
        watches[toWatchIndex(not_lit1)].push_back(Watcher(clause_idx, c.literals[0]));
    } else if (c.literals.size() == 1) {
        // Unit clause - immediately add to propagation queue
        addToPropagationQueue(c.literals[0]);
        output.verbose(CALL_INFO, 3, 0, "Found unit clause %zu with literal %d\n", 
            clause_idx, toInt(c.literals[0]));
    }
}

// Detach clause from watcher lists
void SATSolver::detachClause(size_t clause_idx) {
    Clause& c = clauses[clause_idx];
    if (c.literals.size() <= 1) return;
    
    // Remove watchers for this clause from the watch lists
    Lit not_lit0 = ~c.literals[0];
    Lit not_lit1 = ~c.literals[1];
    
    int idx0 = toWatchIndex(not_lit0);
    int idx1 = toWatchIndex(not_lit1);
    
    if (idx0 < (int)watches.size()) {
        std::vector<Watcher>& ws1 = watches[idx0];
        for (size_t i = 0; i < ws1.size(); i++) {
            if (ws1[i].clause_idx == clause_idx) {
                ws1[i] = ws1.back();
                ws1.pop_back();
                break;
            }
        }
    }
    
    if (idx1 < (int)watches.size()) {
        std::vector<Watcher>& ws2 = watches[idx1];
        for (size_t i = 0; i < ws2.size(); i++) {
            if (ws2[i].clause_idx == clause_idx) {
                ws2[i] = ws2.back();
                ws2.pop_back();
                break;
            }
        }
    }
}

// Updated unit propagation using 2WL scheme with vector-based watches
bool SATSolver::unitPropagate() {
    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: Starting unit propagation with %zu queued literals\n", propagationQueue.size());
    bool conflict = false;

    while (!propagationQueue.empty()) {
        Lit p = propagationQueue.back();
        propagationQueue.pop_back();
        
        // Process the watched clauses containing ~p
        Lit not_p = ~p;
        int watch_idx = toWatchIndex(p);
        
        if (watch_idx >= (int)watches.size()) {
            continue;  // No watches for this literal
        }
        
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
            output.verbose(CALL_INFO, 4, 0, "  Watcher[%zu]: clause %zu, blocker %d\n", 
                i, ws[i].clause_idx, toInt(blocker));
            
            if (variables[blocker_var].assigned && variables[blocker_var].value == !sign(blocker)) {
                // Blocker is true, skip to next watcher
                output.verbose(CALL_INFO, 4, 0, "    Blocker is true, skipping\n");
                ws[j++] = ws[i++];
                continue;
            }
            
            // Need to inspect the clause
            size_t clause_idx = ws[i].clause_idx;
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
            for (size_t k = 2; k < c.literals.size(); k++) {
                Lit lit = c.literals[k];
                Var lit_var = var(lit);
                if (!variables[lit_var].assigned || variables[lit_var].value == !sign(lit)) {
                    // Swap to position 1 and update watcher
                    std::swap(c.literals[1], c.literals[k]);
                    ensureWatchSizeForLiteral(~c.literals[1]);
                    watches[toWatchIndex(~c.literals[1])].push_back(w);
                    output.verbose(CALL_INFO, 4, 0, "    Found new watch: literal %d at position %zu\n", 
                        toInt(c.literals[1]), k);
                    goto NextClause;
                }
            }
                        
            // Did not find a new watch - clause is unit or conflicting
            ws[j++] = w; // Keep watching current literals
            output.verbose(CALL_INFO, 4, 0, "    No new watch found\n");
            
            // Check if first literal is false (conflict) or undefined (unit)
            if (variables[first_var].assigned && variables[first_var].value == sign(first)) {
                // Conflict detected
                output.verbose(CALL_INFO, 3, 0, "CONFLICT: Clause %zu has all literals false\n", clause_idx);
                
                // Copy remaining watchers
                while (i < ws.size()) {
                    ws[j++] = ws[i++];
                }
                propagationQueue.clear();
                conflict = true;
            } else {
                // Unit clause found, propagate
                output.verbose(CALL_INFO, 3, 0, "UNIT: Clause %zu forces literal %d (to 1)\n", clause_idx, toInt(first));
                addToPropagationQueue(first);
                stat_propagations->addData(1);
            }
NextClause:;
        }
        
        ws.resize(ws.size() - (i-j)); // Remove deleted watchers
    }
    
    return !conflict;
}

void SATSolver::addToPropagationQueue(Lit literal) {
    propagationQueue.push_back(literal);
    Var v = var(literal);
    ensureVarCapacity(v);
    variables[v].assigned = true;
    variables[v].value = !sign(literal);
    variables[v].level = decision_stack.size();
    stat_assigned_vars->addData(1);
    output.verbose(CALL_INFO, 3, 0, "ASSIGN: x%d = %d at level %zu\n", v, variables[v].value ? 1 : 0, variables[v].level);
}

// Core DPLL implementation
bool SATSolver::solveDPLL() {
    output.verbose(CALL_INFO, 2, 0, "=== New DPLL Solver Step Start ===\n");
    
    // Unit propagation
    if (!unitPropagate()) {
        output.verbose(CALL_INFO, 2, 0, "CONFLICT: Found unsatisfiable clause\n");
        if (decision_stack.empty()) {
            output.output("UNSAT: Formula is unsatisfiable - conflict at decision level 0\n");
            primaryComponentOKToEndSim();
            return true;
        }
        output.verbose(CALL_INFO, 2, 0, "BACKTRACK: Conflict at decision level %zu\n", decision_stack.size());
        backtrack();
        return false;
    }

    // Check if satisfied
    if (isSatisfied()) {
        output.output("SAT: Formula is satisfiable\nSolution: ");
        for (Var v = 1; v < (Var)variables.size(); v++) {
            output.output("x%d=%d ", v, variables[v].value);
        }
        output.output("\n");
        primaryComponentOKToEndSim();
        return true;
    }

    // Make a new decision
    output.verbose(CALL_INFO, 2, 0, "DECIDE: Current assignment incomplete, making new decision\n");
    if (!decide()) {
        output.fatal(CALL_INFO, -1, "Error: No decision possible but formula not SAT/UNSAT\n");
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
    output.verbose(CALL_INFO, 3, 0, "DECIDE: Making decision: setting x%d = true\n", v);
    decision_stack.push_back(lit);
    addToPropagationQueue(lit);
    stat_decisions->addData(1);
    return true;
}

bool SATSolver::backtrack() {
    if (decision_stack.empty()) return false;
    
    // Get the last decision literal
    Lit lit = decision_stack.back();
    decision_stack.pop_back();
    Var v = var(lit);
    size_t backtrackLevel = decision_stack.size();
    
    output.verbose(CALL_INFO, 3, 0, "BACKTRACK: From level %zu to level %zu\n", backtrackLevel + 1, backtrackLevel);
    
    // Only unassign variables at the current level
    for (Var i = 0; i < (Var)variables.size(); i++) {
        if (variables[i].assigned && variables[i].level > backtrackLevel) {
            unassignVariable(i);
            output.verbose(CALL_INFO, 3, 0, "BACKTRACK: Unassigning x%d from level %zu\n", i, variables[i].level);
            stat_backtracks->addData(1);
        }
    }
    
    // Try opposite value of last decision
    output.verbose(CALL_INFO, 3, 0, "BACKTRACK: Trying opposite value: x%d = %d\n", v, sign(lit) ? 0 : 1);
    addToPropagationQueue(~lit);
    
    return true;
}

// Utility functions
bool SATSolver::isSatisfied() {
    for (Var v = 1; v < (Var)variables.size(); v++) {
        if (!variables[v].assigned) {
            return false;
        }
    }
    return true;
}

void SATSolver::unassignVariable(int var) {
    variables[var].assigned = false;
    stat_assigned_vars->addData(-1);  // One less variable assigned
    output.verbose(CALL_INFO, 3, 0, "UNASSIGN: x%d\n", var);
}

int SATSolver::chooseBranchVariable() {
    for (Var v = 1; v < (Var)variables.size(); v++) {
        if (!variables[v].assigned) {
            return v; // Return first unassigned variable
        }
    }
    return 0; // No unassigned variables
}
