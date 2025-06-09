#ifndef _H_SST_CACHE_PROFILER
#define _H_SST_CACHE_PROFILER

#include <sst/core/event.h>
#include <sst/core/sst_types.h>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/timeConverter.h>
#include <sst/core/output.h>
#include <sst/elements/memHierarchy/memEvent.h>
#include <sst/elements/memHierarchy/cacheListener.h>
#include <string>

using namespace SST;
using namespace SST::MemHierarchy;

namespace SST {
namespace SATSolver {

class CacheProfiler : public SST::MemHierarchy::CacheListener {
public:
    CacheProfiler(ComponentId_t id, Params& params);
    ~CacheProfiler() {};

    void notifyAccess(const CacheListenerNotification& notify);
    void printStats(Output& out);
    
    SST_ELI_REGISTER_SUBCOMPONENT(
        CacheProfiler,
        "satsolver",
        "CacheProfiler",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Cache profiler for SAT solver data structures",
        SST::MemHierarchy::CacheListener
    )

    SST_ELI_DOCUMENT_PARAMS(
        { "cache_level", "Cache level (L1, L2, L3) for reporting", "unknown" },
        { "heap_base_addr", "Base address for heap data", "0x00000000" },
        { "variables_base_addr", "Base address for variables data", "0x20000000" },
        { "watches_base_addr", "Base address for watches data", "0x30000000" },
        { "clauses_cmd_base_addr", "Base address for clauses command data", "0x50000000" },
        { "var_act_base_addr", "Base address for variable activity data", "0x70000000" },
        { "clause_act_base_addr", "Base address for clause activity data", "0x80000000" },
        { "verbose", "Verbosity level", "0" }
    )

    SST_ELI_DOCUMENT_STATISTICS(
        { "heap_hits", "Number of heap hits", "count", 1 },
        { "heap_misses", "Number of heap misses", "count", 1 },
        { "variables_hits", "Number of variables hits", "count", 1 },
        { "variables_misses", "Number of variables misses", "count", 1 },
        { "watches_hits", "Number of watches hits", "count", 1 },
        { "watches_misses", "Number of watches misses", "count", 1 },
        { "clauses_hits", "Number of clauses hits", "count", 1 },
        { "clauses_misses", "Number of clauses misses", "count", 1 },
        { "var_activity_hits", "Number of variable activity hits", "count", 1 },
        { "var_activity_misses", "Number of variable activity misses", "count", 1 },
        { "cla_activity_hits", "Number of clause activity hits", "count", 1 },
        { "cla_activity_misses", "Number of clause activity misses", "count", 1 },
    )

private:
    SST::Output output;
    std::string cache_level;

    // Address ranges for different data structures
    uint64_t heap_base_addr;
    uint64_t variables_base_addr;
    uint64_t watches_base_addr; 
    uint64_t clauses_cmd_base_addr;
    uint64_t var_act_base_addr;
    uint64_t clause_act_base_addr;

    // Statistics
    Statistic<uint64_t>* heap_hits;
    Statistic<uint64_t>* heap_misses;
    Statistic<uint64_t>* variables_hits;
    Statistic<uint64_t>* variables_misses;
    Statistic<uint64_t>* watches_hits;
    Statistic<uint64_t>* watches_misses;
    Statistic<uint64_t>* clauses_hits;
    Statistic<uint64_t>* clauses_misses;
    Statistic<uint64_t>* var_activity_hits;
    Statistic<uint64_t>* var_activity_misses;
    Statistic<uint64_t>* cla_activity_hits;
    Statistic<uint64_t>* cla_activity_misses;
    Statistic<uint64_t>* other_hits;
    Statistic<uint64_t>* other_misses;
};

} // namespace SATSolver
} // namespace SST

#endif // _H_SST_CACHE_PROFILER
