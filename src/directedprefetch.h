#ifndef _H_SST_DIRECTED_PREFETCH
#define _H_SST_DIRECTED_PREFETCH

#include <vector>
#include <unordered_set>

#include <sst/core/event.h>
#include <sst/core/sst_types.h>
#include <sst/core/component.h>
#include <sst/core/link.h>
#include <sst/core/timeConverter.h>
#include <sst/elements/memHierarchy/memEvent.h>
#include <sst/elements/memHierarchy/cacheListener.h>


// Event class for prefetch requests
class PrefetchRequestEvent : public SST::Event {
public:
    uint64_t addr;  // Address to prefetch

    PrefetchRequestEvent() : addr(0) {}
    PrefetchRequestEvent(uint64_t addr) : addr(addr) {}

    void serialize_order(SST::Core::Serialization::serializer &ser) override {
        Event::serialize_order(ser);
        SST_SER(addr);
    }
    ImplementSerializable(PrefetchRequestEvent);
};

class DirectedPrefetcher : public SST::MemHierarchy::CacheListener {
public:
    DirectedPrefetcher(ComponentId_t id, Params& params);
    ~DirectedPrefetcher();

    void notifyAccess(const SST::MemHierarchy::CacheListenerNotification& notify) override;
    void registerResponseCallback(Event::HandlerBase *handler) override;
    void printStats(Output& out) override;
    
    // Handle prefetch request events from solver
    void handlePrefetchRequest(SST::Event* ev);
    
    SST_ELI_REGISTER_SUBCOMPONENT(
        DirectedPrefetcher,
        "satsolver",
        "DirectedPrefetcher",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Directed Prefetcher that receives explicit prefetch requests",
        SST::MemHierarchy::CacheListener
    )

    SST_ELI_DOCUMENT_PARAMS(
        { "cache_line_size", "Size of the cache line the prefetcher is attached to", "64" }
    )
    
    SST_ELI_DOCUMENT_PORTS(
        {"cmd_port", "Port to receive prefetch requests", {"SST::Event"}}
    )

    SST_ELI_DOCUMENT_STATISTICS(
        { "prefetches_issued", "Number of prefetch requests issued", "prefetches", 1 },
        { "prefetches_used", "Number of prefetch requests that were used", "prefetches", 1 },
        { "prefetches_unused", "Number of prefetch requests that were evicted unused", "prefetches", 1 },
    )

    // Serialization support
    DirectedPrefetcher() : SST::MemHierarchy::CacheListener() {} // For serialization
    void serialize_order(SST::Core::Serialization::serializer& ser) override;   
    ImplementSerializable(DirectedPrefetcher)

private:
    std::vector<Event::HandlerBase*> registeredCallbacks;
    uint64_t blockSize;
    std::unordered_set<uint64_t> prefetchTable;  // Track addresses that were prefetched
    SST::Link* cmdLink;  // Link to receive prefetch requests

    Statistic<uint64_t>* statPrefetchEventsIssued;
    Statistic<uint64_t>* statPrefetchUsed;
    Statistic<uint64_t>* statPrefetchUnused;
};

#endif
