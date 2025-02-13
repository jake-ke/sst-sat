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

void SATSolver::parseDIMACS(const std::string& content) {
    output.output("Starting DIMACS parsing\n");
    std::istringstream iss(content);
    std::string line;
    
    num_vars = 0;
    num_clauses = 0;
    clauses.clear();
    variables.clear();
    
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
                int literal;
                Clause clause;
                clause.satisfied = false;
                
                while (clause_iss >> literal && literal != 0) {
                    clause.literals.push_back(literal);
                    int var = abs(literal);
                    if (variables.find(var) == variables.end()) {
                        variables[var] = {false, false};
                    }
                }
                
                if (!clause.literals.empty()) {
                    clauses.push_back(clause);
                    std::ostringstream clause_output;
                    clause_output << "Added clause " << clauses.size() - 1 << ":";
                    for (int lit : clause.literals) {
                        clause_output << " " << lit;
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
    
    if (variables.size() > num_vars) {
        output.fatal(CALL_INFO, -1, "Parsing error: Found %zu variables but expected only %u\n", 
            variables.size(), num_vars);
    }
    
    output.output("Successfully parsed %zu clauses with %zu variables\n", 
        clauses.size(), variables.size());
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
        for (const auto& pair : variables) {
            if (pair.second.assigned)
                output.output("x%d=%d ", pair.first, pair.second.value ? 1 : 0);
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

bool SATSolver::unitPropagate() {
    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: Starting unit propagation with %zu queued literals\n", propagationQueue.size());
    
    while (!propagationQueue.empty()) {
        propagationQueue.clear();  // Clear queue since all literals in it are already assigned
        
        // Check all clauses for implications
        for (size_t i = 0; i < clauses.size(); i++) {
            auto& clause = clauses[i];
            if (clause.satisfied) {
                continue;
            }

            // Build clause string for debug
            std::string clauseStr = "( ";
            for (int lit : clause.literals) clauseStr += std::to_string(lit) + " ";
            clauseStr += ")";

            int unassignedLit = 0;
            int unassignedCount = 0;

            // Evaluate clause
            for (int lit : clause.literals) {
                int var = abs(lit);
                if (!variables[var].assigned) {
                    unassignedLit = lit;
                    unassignedCount++;
                } else if ((lit > 0) == variables[var].value) {
                    clause.satisfied = true;
                    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: Clause %zu %s satisfied\n", i, clauseStr.c_str());
                    break;
                }
            }

            if (!clause.satisfied) {
                if (unassignedCount == 0) {
                    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: Clause %zu %s is a conflict\n", i, clauseStr.c_str());
                    return false;
                } else if (unassignedCount == 1) {
                    output.verbose(CALL_INFO, 3, 0, "PROPAGATE: Clause %zu %s forces x%d=%d\n", 
                        i, clauseStr.c_str(), abs(unassignedLit), unassignedLit > 0 ? 1 : 0);
                    addToPropagationQueue(unassignedLit);
                    stat_propagations->addData(1);
                }
            }
        }
    }
    
    return true;
}

void SATSolver::addToPropagationQueue(int literal) {
    propagationQueue.push_back(literal);
    int var = abs(literal);
    variables[var].assigned = true;
    variables[var].value = (literal > 0);
    variables[var].level = decision_stack.size();
    stat_assigned_vars->addData(1);
    output.verbose(CALL_INFO, 3, 0, "ASSIGN: x%d = %d at level %zu\n", var, variables[var].value ? 1 : 0, variables[var].level);
}

bool SATSolver::decide() {
    int var = chooseBranchVariable();
    if (var == 0) {
        output.verbose(CALL_INFO, 2, 0, "DECIDE: No unassigned variables left\n");
        return false;
    }
    
    output.verbose(CALL_INFO, 3, 0, "DECIDE: Making decision: setting x%d = true\n", var);
    decision_stack.push_back(var);
    addToPropagationQueue(var);
    stat_decisions->addData(1);
    return true;
}

bool SATSolver::backtrack() {
    if (decision_stack.empty()) return false;
    
    // Get the last decision variable
    int var = decision_stack.back();
    decision_stack.pop_back();
    size_t backtrackLevel = decision_stack.size();
    
    output.verbose(CALL_INFO, 3, 0, "BACKTRACK: From level %zu to level %zu\n", backtrackLevel + 1, backtrackLevel);
    
    // Only unassign variables at the current level
    for (auto& pair : variables) {
        if (pair.second.assigned && pair.second.level > backtrackLevel) {
            unassignVariable(pair.first);
            output.verbose(CALL_INFO, 3, 0, "BACKTRACK: Unassigning x%d from level %zu\n", pair.first, pair.second.level);
            stat_backtracks->addData(1);
        }
    }
    
    // Update clause status after unassignment
    updateAllClauseStatus();
    
    // Try opposite value of last decision
    output.verbose(CALL_INFO, 3, 0, "BACKTRACK: Trying opposite value: x%d = %d\n", abs(var), var > 0 ? 0 : 1);
    addToPropagationQueue(-var);
    
    return true;
}

// Utility functions
bool SATSolver::isSatisfied() {
    for (const auto& clause : clauses) {
        if (!checkClauseSatisfied(clause)) 
            return false;
    }
    return true;
}

void SATSolver::assignVariable(int literal) {
    int var = abs(literal);
    variables[var].assigned = true;
    variables[var].value = (literal > 0);
    output.verbose(CALL_INFO, 3, 0, "ASSIGN: x%d = %d\n", var, variables[var].value ? 1 : 0);
}

void SATSolver::unassignVariable(int var) {
    variables[var].assigned = false;
    stat_assigned_vars->addData(-1);  // One less variable assigned
    output.verbose(CALL_INFO, 3, 0, "UNASSIGN: x%d\n", var);
}

int SATSolver::chooseBranchVariable() {
    for (const auto& pair : variables) {
        if (!pair.second.assigned) {
            return pair.first; // Return first unassigned variable
        }
    }
    return 0; // No unassigned variables
}

bool SATSolver::checkClauseSatisfied(const Clause& clause) {
    // A clause is a disjunction (OR) of literals
    // It's satisfied if any literal evaluates to true
    for (int lit : clause.literals) {
        int var = abs(lit);
        if (!variables[var].assigned)
            continue;

        // One true literal makes the whole clause true
        if ((lit > 0) == variables[var].value)
            return true;
    }
    return false;  // No literal evaluated to true
}

void SATSolver::updateAllClauseStatus() {
    for (auto& clause : clauses) {
        clause.satisfied = checkClauseSatisfied(clause);
    }

    // for (size_t i = 0; i < clauses.size(); i++) {
    //     auto& clause = clauses[i];
    //     output.verbose(CALL_INFO, 3, 0, "CLAUSE %zu: %s\n", i, clause.satisfied ? "SAT" : "UNSAT");
    // }
}
