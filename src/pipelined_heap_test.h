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
        {"heap_port", "Port to communicate with heap", {"HeapReqEvent"}}
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"heap", "Pipelined heap subcomponent (owns its memory interface)", "PipelinedHeap"}
    )

private:
    struct Step {
        enum class Type { Insert, Remove, Bump, Debug, Wait, Rebuild, Hint };
        Type type;
        int var;   // var id for Insert/Bump; wait cycles for Wait
    };

    enum class ResponseKind { Remove, Debug };

    // Output for logging
    SST::Output output;
    int verbose;
    PipelinedHeap* heap;
    SST::Link* heap_link;

    // Script state
    std::string script_path;
    std::vector<Step> script;
    size_t script_index;
    uint64_t wait_cycles;
    std::queue<ResponseKind> pending_responses;
    std::vector<int> tracked_vars;
    double var_inc_value;
    bool script_completed;
    bool sim_finish_requested;
    uint64_t resp_cnt;

    // Golden model mirroring the inheap-bit design: a multiset of
    // (var, activity-at-insert) copies plus a per-var inheap bit. The heap
    // may autonomously drop stale (bit=0) copies at any time (tail-trim /
    // root-peek purge), so REMOVE checks accept any result r whose max
    // stored copy dominates every remaining FRESH copy; stale copies above
    // it are reconciled as purged.
    std::unordered_map<int, double> activities;          // authoritative act per var
    std::vector<std::pair<int, double>> golden_copies;   // (var, stored act)
    std::unordered_set<int> golden_inheap;

    // Statistics
    uint64_t stat_successful_ops;
    uint64_t stat_failed_ops;

    // Helpers
    void handleHeapResponse(SST::Event* ev);
    void executeStep(const Step& step);
    void issueInsert(int var);
    void issueBump(int var);
    void issueRemove();
    void issueDebug();
    void checkRemoveResponse(int result);
    void finalizeIfDone();
    void loadScriptFromFile(const std::string& path);
};

#endif // PIPELINED_HEAP_TEST_H
