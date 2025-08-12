#include <sst/core/sst_config.h> // This include is REQUIRED for all implementation files
#include "directedprefetch.h"
#include "sst/core/params.h"
#include "sst/core/statapi/stataccumulator.h"
#include <stdint.h>
#include <vector>
#include <unordered_set>

using namespace SST::MemHierarchy;

DirectedPrefetcher::DirectedPrefetcher(ComponentId_t id, Params& params) : CacheListener(id, params) {
    requireLibrary("memHierarchy");

    blockSize = params.find<uint64_t>("cache_line_size", 64);

    // Set up link to receive prefetch requests
    cmdLink = configureLink("cmd_port", 
        new Event::Handler2<DirectedPrefetcher, &DirectedPrefetcher::handlePrefetchRequest>(this));

    statPrefetchEventsIssued = registerStatistic<uint64_t>("prefetches_issued");
    statPrefetchUsed = registerStatistic<uint64_t>("prefetches_used");
    statPrefetchUnused = registerStatistic<uint64_t>("prefetches_unused");
}

DirectedPrefetcher::~DirectedPrefetcher() {}

void DirectedPrefetcher::handlePrefetchRequest(SST::Event* ev) {
    PrefetchRequestEvent* prefReq = dynamic_cast<PrefetchRequestEvent*>(ev);
    if (prefReq) {
        Addr addr = prefReq->addr;
        Addr lineAddr = addr - (addr % blockSize);
        
        // Check if this address is already cached or already in prefetch table
        if (prefetchTable.find(lineAddr) == prefetchTable.end()) {
            prefetchTable.insert(lineAddr);

            // Issue prefetch request
            std::vector<Event::HandlerBase*>::iterator callbackItr;
            for (callbackItr = registeredCallbacks.begin(); callbackItr != registeredCallbacks.end(); callbackItr++) {
                MemEvent* newEv = new MemEvent(getName(), lineAddr, lineAddr, Command::GetS);
                newEv->setSize(blockSize);
                newEv->setPrefetchFlag(true);
                (*(*callbackItr))(newEv);
            }
            statPrefetchEventsIssued->addData(1);
        }
    }
    delete ev;
}

void DirectedPrefetcher::notifyAccess(const SST::MemHierarchy::CacheListenerNotification& notify) {
    const NotifyAccessType notifyType = notify.getAccessType();
    const NotifyResultType notifyResType = notify.getResultType();
    const uint64_t addr = notify.getPhysicalAddress();
    const uint64_t lineAddr = addr - (addr % blockSize);
    
    if (notifyType == READ || notifyType == WRITE) {
        // Check if this address was prefetched before
        if (prefetchTable.find(lineAddr) != prefetchTable.end()) {
            if (notifyResType == HIT) {
                // printf("addr 0x%lx prefetched\n", addr);
                statPrefetchUsed->addData(1);
            }
            // else printf("addr 0x%lx late\n", addr);
            prefetchTable.erase(lineAddr);
        }
    } else if (notifyType == EVICT) {
        // Check if the evicted line was a prefetch we issued but was never used
        if (prefetchTable.find(lineAddr) != prefetchTable.end()) {
            // printf("addr 0x%lx evicted unused prefetch\n", addr);
            statPrefetchUnused->addData(1);
            prefetchTable.erase(lineAddr);
        }
    }
}

void DirectedPrefetcher::registerResponseCallback(Event::HandlerBase *handler) {
    registeredCallbacks.push_back(handler);
}

uint64_t getCount(Statistic<uint64_t>* stat) {
    AccumulatorStatistic<uint64_t>* accum = dynamic_cast<AccumulatorStatistic<uint64_t>*>(stat);
    if (accum) {
        return accum->getCount();
    }
    return 0; // Return 0 if the cast fails
}

void DirectedPrefetcher::printStats(Output& out) {
    out.output("DirectedPrefetcher Statistics:\n");
    out.output("  Prefetches issued: %" PRIu64 "\n", getCount(statPrefetchEventsIssued));
    out.output("  Prefetches used: %" PRIu64 "\n", getCount(statPrefetchUsed));
    out.output("  Prefetches unused (evicted): %" PRIu64 "\n", getCount(statPrefetchUnused));

    double accuracy = 0.0;
    if (getCount(statPrefetchEventsIssued) > 0) {
        accuracy = static_cast<double>(getCount(statPrefetchUsed)) / getCount(statPrefetchEventsIssued) * 100.0;
    }
    out.output("  Prefetch accuracy: %.2f%%\n", accuracy);
}

void DirectedPrefetcher::serialize_order(SST::Core::Serialization::serializer& ser) {
    CacheListener::serialize_order(ser);

    SST_SER(registeredCallbacks);
    SST_SER(blockSize);
    SST_SER(prefetchTable);
    SST_SER(cmdLink);
    SST_SER(statPrefetchEventsIssued);
    SST_SER(statPrefetchUsed);
    SST_SER(statPrefetchUnused);
}
