#ifndef PIPELINED_HEAP_TEST_H
#define PIPELINED_HEAP_TEST_H

#include <sst/core/component.h>
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include "pipelined_heap.h"
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PipelinedHeapTest : public SST::Component {
public:
    PipelinedHeapTest(SST::ComponentId_t id, SST::Params& params);
    ~PipelinedHeapTest() {}

    // SST Component API
    virtual void init(unsigned int phase) override;
    virtual void setup() override;
    virtual void complete(unsigned int phase) override;
    virtual void finish() override;
    bool tick(SST::Cycle_t cycle);
    void handleGlobalMemEvent(SST::Interfaces::StandardMem::Request* req);

    
    // SST Component Registration Info
    SST_ELI_REGISTER_COMPONENT(
        PipelinedHeapTest,
        "satsolver",
        "PipelinedHeapTest",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Test component for PipelinedHeap",
        COMPONENT_CATEGORY_UNCATEGORIZED
    )
    
    SST_ELI_DOCUMENT_PARAMS(
        {"verbose", "Verbosity level (0-10)", "1"},
        {"clock", "Clock frequency", "1GHz"},
        {"var_inc", "Value added to activity on bump operations", "1.0"},
        {"script_path", "Path to external script describing heap operations", ""}
    )
    
    SST_ELI_DOCUMENT_PORTS(
        {"global_mem_link", "Connection to global memory", {"memHierarchy.MemEventBase"}},
        {"heap_port", "Port to communicate with heap", {"HeapReqEvent"}}
    )
    
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"global_memory", "Memory interface for Heap and Variables", "SST::Interfaces::StandardMem"},
        {"heap", "Pipelined heap subcomponent", "PipelinedHeap"}
    )
    
private:
    struct Step {
        enum class Type { Insert, Remove, Bump };
        Type type;
        int var;
    };

    enum class ResponseKind { Insert, Remove };

    // Output for logging
    SST::Output output;
    int verbose;
    SST::Interfaces::StandardMem* global_memory;
    PipelinedHeap* heap;
    SST::Link* heap_link;

    // Verification state
    std::string script_path;
    std::vector<Step> script;
    size_t script_index;
    std::queue<ResponseKind> pending_responses;
    int pending_insert_responses;
    std::unordered_map<int, double> activities;
    std::unordered_set<int> active_vars;
    std::vector<int> tracked_vars;
    double var_inc_value;
    uint64_t var_mem_base_addr;
    bool script_completed;
    bool sim_finish_requested;
    uint64_t resp_cnt;

    // Statistics
    uint64_t stat_successful_ops;
    uint64_t stat_failed_ops;

    // Helpers
    void handleHeapResponse(SST::Event* ev);
    void executeStep(const Step& step);
    void issueInsert(int var);
    void issueBump(int var);
    void issueRemove();
    void finalizeIfDone();
    void loadScriptFromFile(const std::string& path);
};

#endif // PIPELINED_HEAP_TEST_H
