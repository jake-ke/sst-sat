#include <sst/core/sst_config.h>
#include "cache_profiler.h"
#include <sst/core/statapi/stataccumulator.h>
#include <unordered_set> // Add this for the unordered_set

using namespace SST;
using namespace SST::MemHierarchy;
using namespace SST::SATSolver;

CacheProfiler::CacheProfiler(ComponentId_t id, Params& params) : CacheListener(id, params) {
    output.init("CacheProfiler -> ", 
                 params.find<int>("verbose", 0),
                 0,
                 Output::STDOUT);

    cache_level = params.find<std::string>("cache_level", "unknown");
    
    // Get parameter for excluding cold misses
    exclude_cold_misses = params.find<bool>("exclude_cold_misses", false);
    output.verbose(CALL_INFO, 2, 0, "Exclude cold misses: %s\n", 
                   exclude_cold_misses ? "true" : "false");

    // Get base addresses for each data structure from the solver
    heap_base_addr = std::stoull(params.find<std::string>("heap_base_addr", "0x00000000"), nullptr, 0);
    variables_base_addr = std::stoull(params.find<std::string>("variables_base_addr", "0x20000000"), nullptr, 0);
    watches_base_addr = std::stoull(params.find<std::string>("watches_base_addr", "0x30000000"), nullptr, 0);
    clauses_cmd_base_addr = std::stoull(params.find<std::string>("clauses_cmd_base_addr", "0x50000000"), nullptr, 0);
    var_act_base_addr = std::stoull(params.find<std::string>("var_act_base_addr", "0x70000000"), nullptr, 0);

    // Register statistics
    heap_hits = registerStatistic<uint64_t>("heap_hits");
    heap_misses = registerStatistic<uint64_t>("heap_misses");
    variables_hits = registerStatistic<uint64_t>("variables_hits");
    variables_misses = registerStatistic<uint64_t>("variables_misses");
    watches_hits = registerStatistic<uint64_t>("watches_hits");
    watches_misses = registerStatistic<uint64_t>("watches_misses");
    clauses_hits = registerStatistic<uint64_t>("clauses_hits");
    clauses_misses = registerStatistic<uint64_t>("clauses_misses");
    var_activity_hits = registerStatistic<uint64_t>("var_activity_hits");
    var_activity_misses = registerStatistic<uint64_t>("var_activity_misses");
}

void CacheProfiler::notifyAccess(const CacheListenerNotification& notify) {
    const NotifyAccessType notifyType = notify.getAccessType();
    const NotifyResultType notifyResType = notify.getResultType();
    Addr addr = notify.getPhysicalAddress();

    // if (notifyType == EVICT && addr < variables_base_addr) {
    //     printf("Cache eviction at physical 0x%lx, result type %d\n", addr, notifyResType);
    // }

    if (notifyType != READ && notifyType != WRITE) {
        return; // Only handle read and write accesses
    }
    
    // Track if this is a cold miss (first access to this address)
    bool is_cold_miss = false;
    if (exclude_cold_misses && notifyResType == MISS && accessed_addresses.find(addr) == accessed_addresses.end()) {
        is_cold_miss = true;
        accessed_addresses.insert(addr); // Mark as accessed for future reference
    } else if (exclude_cold_misses) {
        // If we're excluding cold misses, make sure we mark any address we see
        accessed_addresses.insert(addr);
    }

    // Identify which data structure this access belongs to and update statistics
    if (addr >= var_act_base_addr) {
        // Variable activity
        if (notifyResType == HIT) 
            var_activity_hits->addData(1);
        else if (!exclude_cold_misses || !is_cold_miss)
            var_activity_misses->addData(1);
    } else if (addr >= clauses_cmd_base_addr) {
        // Clauses
        if (notifyResType == HIT) 
            clauses_hits->addData(1);
        else if (!exclude_cold_misses || !is_cold_miss)
            clauses_misses->addData(1);
    } else if (addr >= watches_base_addr) {
        // Watches
        if (notifyResType == HIT) 
            watches_hits->addData(1);
        else if (!exclude_cold_misses || !is_cold_miss)
            watches_misses->addData(1);
    } else if (addr >= variables_base_addr) {
        // Variables
        if (notifyResType == HIT) 
            variables_hits->addData(1);
        else if (!exclude_cold_misses || !is_cold_miss)
            variables_misses->addData(1);
    } else if (addr >= heap_base_addr) {
        // Heap
        // if (notifyType == WRITE) printf("CACHE write: 0x%lx, physical 0x%lx, notifyResType %d\n", notify.getTargetAddress(), addr, notifyResType);
        // else printf("CACHE read: 0x%lx, physical 0x%lx, notifyResType %d\n", notify.getTargetAddress(), addr, notifyResType);
        if (notifyResType == HIT) 
            heap_hits->addData(1);
        else if (!exclude_cold_misses || !is_cold_miss)
            heap_misses->addData(1);
    } else output.fatal(CALL_INFO, -1, "Unknown address 0x%lx\n", addr);
}


void CacheProfiler::printStats(Output& output) {
    output.output("============================ %s Cache Profiler Statistics ====================\n", cache_level.c_str());

    // Calculate statistics for each data structure
    uint64_t total_hits = 0;
    uint64_t total_misses = 0;
    
    // Helper function to get the count from a statistic
    auto getStatCount = [](Statistic<uint64_t>* stat) -> uint64_t {
        AccumulatorStatistic<uint64_t>* accum = dynamic_cast<AccumulatorStatistic<uint64_t>*>(stat);
        if (accum) {
            return accum->getCount();
        }
        return 0; // Return 0 if the cast fails
    };
    
    // Helper lambda to print stats for a data structure
    auto printStats = [&](const std::string& name, Statistic<uint64_t>* hits, Statistic<uint64_t>* misses) {
        uint64_t h = getStatCount(hits);
        uint64_t m = getStatCount(misses);
        uint64_t total = h + m;
        double miss_rate = (total > 0) ? (double)m / total * 100.0 : 0.0;
        
        output.output("  %-12s: %10lu hits, %10lu misses, %10lu total, %6.2f%% miss rate\n",
               name.c_str(), h, m, total, miss_rate);
        
        total_hits += h;
        total_misses += m;
    };
    
    // Print stats for each data structure
    printStats("Heap", heap_hits, heap_misses);
    printStats("Variables", variables_hits, variables_misses);
    printStats("Watches", watches_hits, watches_misses);
    printStats("Clauses", clauses_hits, clauses_misses);
    printStats("VarActivity", var_activity_hits, var_activity_misses);
    
    // Print total stats
    uint64_t total = total_hits + total_misses;
    double overall_miss_rate = (total > 0) ? (double)total_misses / total * 100.0 : 0.0;
    output.output("  %-12s: %10lu hits, %10lu misses, %10lu total, %6.2f%% miss rate\n",
           "TOTAL", total_hits, total_misses, total, overall_miss_rate);
    output.output("==============================================================================\n");
}
