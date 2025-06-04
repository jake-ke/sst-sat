#ifndef EXTERNAL_HEAP_H
#define EXTERNAL_HEAP_H

#include <sst/core/subcomponent.h>
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/event.h>
#include <boost/coroutine2/all.hpp>
#include "structs.h"
#include "async_var_activity.h"

// Events for heap operations
class HeapReqEvent : public SST::Event {
public:
    enum OpType { INIT, INSERT, REMOVE_MIN, IN_HEAP, READ, BUMP };
    OpType op;
    int arg;
    double var_inc;
    HeapReqEvent() : op(HeapReqEvent::READ), arg(0) {}
    HeapReqEvent(OpType o, int a = 0, double b = 0.0)
        : op(o), arg(a), var_inc(b) {}
    
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        Event::serialize_order(ser);
        ser & op;
        ser & arg;
        ser & var_inc;
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
    // Update API to remove VarOrderLt parameter
    SST_ELI_REGISTER_SUBCOMPONENT_API(Heap, SST::Interfaces::StandardMem*, uint64_t, uint64_t)
    
    SST_ELI_REGISTER_SUBCOMPONENT(
        Heap, "satsolver", "Heap", SST_ELI_ELEMENT_VERSION(1,0,0),
        "External-memory-based heap", Heap
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"clock", "Clock frequency", "1GHz"},
        {"heap_addr", "Base address for heap array", "0x10000000"},
        {"indices_addr", "Base address for indices array", "0x20000000"},
        {"var_act_base_addr", "Base address for variable activity array", "0x70000000"}
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"memory", "Memory interface (shared from parent)", "SST::Interfaces::StandardMem"}
    )

    SST_ELI_DOCUMENT_PORTS(
        {"response", "Response port to parent", {"sst.Event"}}
    )

    Heap(SST::ComponentId_t id, SST::Params& params,
         SST::Interfaces::StandardMem* mem, uint64_t heap_base_addr, uint64_t indices_base_addr);
    
    size_t heap_size;
    std::vector<bool> decision;         // Whether variable is eligible for decisions

    bool tick(SST::Cycle_t cycle);
    void handleMem(SST::Interfaces::StandardMem::Request* req);
    void handleRequest(HeapReqEvent* req);

    size_t size() const { return heap_size; }
    bool empty() const { return heap_size == 0; }
    bool busy() const { return state != IDLE; }
    
    enum State { IDLE, WAIT, START, STEP };
    State state;
    int outstanding_mem_requests;  // Track outstanding memory requests
    
private:
    SST::Output output;
    SST::Link* response_port;
    SST::Interfaces::StandardMem* memory;
    uint64_t heap_addr, indices_addr;
    HeapReqEvent::OpType current_op;
    coro_t::pull_type* heap_source;
    coro_t::push_type* heap_sink_ptr;  // Pointer to current coroutine sink
    Var key;
    int idx, read_data;
    double var_inc;
    
    VarActivity var_activity;
    uint64_t var_act_base_addr;  // Base address for variable activity array
    bool lt(Var x, Var y);  // Comparison method using var_activity directly

    // Helper methods
    inline int parent(int i) { return (i - 1) >> 1; }
    inline int left(int i) { return (i << 1) + 1; }
    inline int right(int i) { return (i << 1) + 2; }
    inline uint64_t heapAddr(int i) { return heap_addr + i*sizeof(Var); }
    inline uint64_t indexAddr(int i) { return indices_addr + i*sizeof(Var); }
    
    Var read(uint64_t addr);
    void write(uint64_t addr, Var val);
    void complete(int res);

    void percolateUp(int i);
    void percolateDown(int i);
    void initHeap();
    void readHeap();
    void inHeap();
    void insert();
    void decrease();
    void removeMin();
    void varBump();
};

#endif
