#ifndef SATSOLVER_H
#define SATSOLVER_H

#include <sst/core/component.h>
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <map>
#include <vector>
#include <string>

enum State { INIT, PARSING, SOLVING, DONE };

struct Clause {
    std::vector<int> literals;
    bool satisfied;
};

struct Variable {
    bool assigned;
    bool value;
    size_t level;  // Add level tracking to know when variable was assigned
};

class SATSolver : public SST::Component {

public:
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
        {"filesize", "Size of CNF file to read", "0"}
    )

    SST_ELI_DOCUMENT_STATISTICS(
        {"decisions", "Number of decisions made", "count", 1},
        {"propagations", "Number of propagations", "count", 1},
        {"backtracks", "Number of backtracks", "count", 1},
        {"assigned_vars", "Current number of assigned variables", "count", 1}
    )

    SST_ELI_DOCUMENT_PORTS(
        {"mem_link", "Connection to HBM", {"memHierarchy.MemEventBase"}}
    )

    SATSolver(SST::ComponentId_t id, SST::Params& params);
    ~SATSolver();

    virtual void init(unsigned int phase) override;
    virtual void setup() override;
    virtual void complete(unsigned int phase) override;
    virtual void finish() override;

    bool clockTick(SST::Cycle_t currentCycle);
    void handleMemEvent(SST::Interfaces::StandardMem::Request* req);
    void parseDIMACS(const std::string& content);
    bool solveDPLL();

private:
    State state;
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    std::string dimacs_content;
    bool requestPending;
    SST::Cycle_t currentCycle;

    // Parsing state
    size_t filesize;
    uint32_t num_vars;
    uint32_t num_clauses;
    
    // SAT solver state
    std::vector<Clause> clauses;
    std::map<int, Variable> variables;
    std::vector<int> decision_stack;
    std::vector<int> propagationQueue;  // Queue of literals to propagate

    Statistic<uint64_t>* stat_decisions;
    Statistic<uint64_t>* stat_propagations;
    Statistic<uint64_t>* stat_backtracks;
    Statistic<uint64_t>* stat_assigned_vars;

    // DPLL helper functions
    bool unitPropagate();
    bool decide();
    bool backtrack();
    
    // Utility functions for DPLL
    void addToPropagationQueue(int literal);
    bool isSatisfied();
    void assignVariable(int literal);
    void unassignVariable(int var);
    int chooseBranchVariable();
    bool checkClauseSatisfied(const Clause& clause);
    void updateAllClauseStatus();
};

#endif // SATSOLVER_H
