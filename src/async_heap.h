#ifndef EXTERNAL_HEAP_H
#define EXTERNAL_HEAP_H

#include <sst/core/subcomponent.h>
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/event.h>
#include <boost/coroutine2/all.hpp>
#include "structs.h"

// Events for heap operations
class HeapReqEvent : public SST::Event {
public:
    enum OpType { INIT, INSERT, REMOVE_MIN, DECREASE, IN_HEAP, READ };
    OpType op;
    int arg;
    HeapReqEvent() : op(HeapReqEvent::READ), arg(0) {}
    HeapReqEvent(OpType o, int a = 0) : op(o), arg(a) {}
    
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        Event::serialize_order(ser);
        ser & op;
        ser & arg;
    }
    ImplementSerializable(HeapReqEvent);
};

class HeapRespEvent : public SST::Event {
public:
    int result;
    HeapRespEvent() : result(0) {}
    HeapRespEvent(int r) : result(r) {}
    
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        Event::serialize_order(ser);
        ser & result;
    }
    ImplementSerializable(HeapRespEvent);
};


class Heap : public SST::SubComponent {
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(Heap, VarOrderLt, SST::Interfaces::StandardMem*, uint64_t, uint64_t)
    
    SST_ELI_REGISTER_SUBCOMPONENT(
        Heap, "satsolver", "Heap", SST_ELI_ELEMENT_VERSION(1,0,0),
        "External-memory-based heap", Heap
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"clock", "Clock frequency", "1GHz"},
        {"heap_addr", "Base address for heap array", "0x10000000"},
        {"indices_addr", "Base address for indices array", "0x20000000"}
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"memory", "Memory interface (shared from parent)", "SST::Interfaces::StandardMem"}
    )

    SST_ELI_DOCUMENT_PORTS(
        {"response", "Response port to parent", {"sst.Event"}}
    )

    Heap(SST::ComponentId_t id, SST::Params& params, const VarOrderLt& c, 
         SST::Interfaces::StandardMem* mem, uint64_t heap_base_addr, uint64_t indices_base_addr);
    
    size_t heap_size;
    std::vector<bool> decision;         // Whether variable is eligible for decisions

    bool tick(SST::Cycle_t cycle);
    void handleMem(SST::Interfaces::StandardMem::Request* req);
    void handleRequest(HeapReqEvent* req);

    size_t size() const { return heap_size; }
    bool empty() const { return heap_size == 0; }
    bool busy() const { return state != IDLE; }
    
private:
    enum State { IDLE, WAIT, START, STEP };
    SST::Output output;
    SST::Link* response_port;
    SST::Interfaces::StandardMem* memory;
    VarOrderLt lt;
    uint64_t heap_addr, indices_addr;
    State state;
    HeapReqEvent::OpType current_op;
    coro_t::pull_type* heap_source;
    Var key;
    int idx, read_data;
    int outstanding_mem_requests;  // Track outstanding memory requests

    // Helper methods
    inline int parent(int i) { return (i - 1) >> 1; }
    inline int left(int i) { return (i << 1) + 1; }
    inline int right(int i) { return (i << 1) + 2; }
    inline uint64_t heapAddr(int i) { return heap_addr + i*sizeof(Var); }
    inline uint64_t indexAddr(int i) { return indices_addr + i*sizeof(Var); }
    void read(uint64_t addr);
    void write(uint64_t addr, Var val);
    void complete(int res);
    void percolateUp(int i, coro_t::push_type &heap_sink);
    void percolateDown(int i, coro_t::push_type &heap_sink);

    void initHeap(coro_t::push_type &heap_sink);
    void readHeap(coro_t::push_type &heap_sink);
    void inHeap(coro_t::push_type &heap_sink);
    void insert(coro_t::push_type &heap_sink);
    void decrease(coro_t::push_type &heap_sink);
    void removeMin(coro_t::push_type &heap_sink);
};


#endif
