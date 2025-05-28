#ifndef EXTERNAL_HEAP_H
#define EXTERNAL_HEAP_H

#include <sst/core/subcomponent.h>
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/event.h>
#include "structs.h"

// Events for heap operations
class HeapReqEvent : public SST::Event {
public:
    enum OpType { INSERT, REMOVE_MIN, DECREASE, IN_HEAP, READ };
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
    bool success;
    int result;
    HeapRespEvent() : success(false), result(0) {}
    HeapRespEvent(bool s, int r = 0) : success(s), result(r) {}
    
    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        Event::serialize_order(ser);
        ser & success;
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

    // Simplified state machine
    enum State { IDLE, WAIT, STEP };

    Heap(SST::ComponentId_t id, SST::Params& params, const VarOrderLt& c, 
         SST::Interfaces::StandardMem* mem, uint64_t heap_base_addr, uint64_t indices_base_addr) 
        : SST::SubComponent(id), lt(c), memory(mem), state(IDLE), op_state(0), heap_size(0),
          heap_addr(heap_base_addr), indices_addr(indices_base_addr) {
        
        output.init("HEAP-" + getName() + "-> ",
            params.find<int>("verbose", 0), 0, SST::Output::STDOUT);

        registerClock(params.find<std::string>("clock", "1GHz"),
            new SST::Clock::Handler<Heap>(this, &Heap::tick));
        
        response_port = configureLink("response");
        sst_assert( response_port != nullptr, CALL_INFO, -1, "Error: 'response_port' is not connected to a link\n");
    }

    void handleRequest(HeapReqEvent* req) {
        // output.verbose(CALL_INFO, 7, 0, "Heap::handleRequest: op %d, arg %d\n", req->op, req->arg);
        // output.verbose(CALL_INFO, 7, 0, "state %d, op_state %d\n", state, op_state);
        assert(outstanding_mem_requests == 0);
        assert(state == IDLE);
        
        current_op = req->op;
        key = req->arg;
        op_state = 0;
        idx = key;
        parent_idx = 0;
        child_idx = 0;
        
        // Start operation on next tick
        state = STEP;
    }

    bool tick(SST::Cycle_t cycle) {
        // output.verbose(CALL_INFO, 7, 0, "Heap::tick %lu: state %d, current_op %d, op_state %d\n", cycle, state, current_op, op_state);
        if (state != STEP) return false;
        switch (current_op) {
            case HeapReqEvent::INSERT:
                stepInsert();
                break;
            case HeapReqEvent::REMOVE_MIN:
                stepRemoveMin();
                break;
            case HeapReqEvent::DECREASE:
                stepDecrease();
                break;
            case HeapReqEvent::IN_HEAP:
                stepInHeap();
                break;
            case HeapReqEvent::READ:
                stepRead();
                break;
        }
        return false;
    }

    void handleMem(SST::Interfaces::StandardMem::Request* req) {
        if (auto resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
            memcpy(&result, resp->data.data(), sizeof(Var));
        }
        outstanding_mem_requests--;
        state = STEP;  // Continue operation
        delete req;
    }

    size_t size() const { return heap_size; }
    bool empty() const { return heap_size == 0; }
    bool busy() const { return state != IDLE; }
    
private:
    SST::Output output;
    VarOrderLt lt;
    SST::Interfaces::StandardMem* memory;
    SST::Link* response_port;
    
    HeapReqEvent::OpType current_op;
    State state;
    int op_state;   // Tracks the step within each operation
    
    size_t heap_size;
    Var key, result, min_val, left_val, right_val;
    int idx, parent_idx, child_idx;
    uint64_t heap_addr, indices_addr;
    
    int outstanding_mem_requests = 0;  // Track outstanding memory requests

    // Helper methods
    inline int parent(int i) { return (i - 1) >> 1; }
    inline int left(int i) { return (i << 1) + 1; }
    inline int right(int i) { return (i << 1) + 2; }
    inline uint64_t heapAddr(int i) { return heap_addr + i*sizeof(Var); }
    inline uint64_t indexAddr(int i) { return indices_addr + i*sizeof(Var); }
    
    void read(uint64_t addr) { 
        memory->send(new SST::Interfaces::StandardMem::Read(addr, sizeof(Var)));
        outstanding_mem_requests++;
        state = WAIT;  // Wait for response
    }
    
    void write(uint64_t addr, Var val) { 
        std::vector<uint8_t> data(sizeof(Var));
        memcpy(data.data(), &val, sizeof(Var));
        memory->send(new SST::Interfaces::StandardMem::Write(addr, sizeof(Var), data));
        outstanding_mem_requests++;
        state = WAIT;  // Wait for response 
    }

    void stepInsert() {
        output.verbose(CALL_INFO, 7, 0, "Heap::stepInsert: op_state %d\n", op_state);
        switch (op_state) {
            case 0:  // Write the key's position in index array
                idx = heap_size;
                write(indexAddr(key), idx);
                output.verbose(CALL_INFO, 7, 0, "stepInsert case 0: indices[%d]=%d\n", key, idx);
                op_state = 1;
                break;
                
            case 1:  // Write the element to heap
                output.verbose(CALL_INFO, 7, 0, "stepInsert case 1: heap[%d]=%d\n", idx, key);
                write(heapAddr(idx), key);
                op_state = 2;
                break;
                
            case 2:  // Check if percolate up needed
                output.verbose(CALL_INFO, 7, 0, "stepInsert case 2: idx=%d, heap_size=%lu\n", idx, heap_size);
                if (idx == 0) { // Element at root, no percolate needed
                    heap_size++;
                    completeOperation(true);
                    return;
                }
                // Start percolate up
                parent_idx = parent(idx);
                output.verbose(CALL_INFO, 7, 0, "stepInsert case 2: read parent_idx=%d\n", parent_idx);
                read(heapAddr(parent_idx));
                op_state = 3;
                break;
                
            case 3:  // Compare with parent
                output.verbose(CALL_INFO, 7, 0, "stepInsert case 3: comparing key %d with parent %d\n", key, result);
                if (lt(key, result)) { // New key has higher priority
                    // Swap with parent
                    output.verbose(CALL_INFO, 7, 0, "stepInsert case 3: heap[%d]=%d\n", idx, result);
                    write(heapAddr(idx), result);
                    op_state = 4;
                } else {
                    // heap_size++;
                    // completeOperation(true);
                    output.verbose(CALL_INFO, 7, 0, "stepInsert case 3: finished percolate, heap[%d]=%d\n", idx, key);
                    write(heapAddr(idx), key);
                    op_state = 6;
                }
                break;
                
            case 4:  // Update parent's index
                output.verbose(CALL_INFO, 7, 0, "stepInsert case 4: indices[%d]=%d\n", result, idx);
                write(indexAddr(result), idx);
                op_state = 5;
                break;
                
            case 5:  // Continue percolate up
                idx = parent_idx;
                if (idx <= 0) { // Reached root
                    output.verbose(CALL_INFO, 7, 0, "stepInsert case 5: reached root, heap[%d]=%d\n", idx, key); 
                    write(heapAddr(0), key);
                    op_state = 6;
                } else {
                    parent_idx = parent(idx);
                    read(heapAddr(parent_idx));
                    output.verbose(CALL_INFO, 7, 0, "stepInsert case 5: continuing upward, read parent_idx=%d\n", parent_idx);
                    op_state = 3; // Loop back to comparison
                }
                break;
                
            case 6:  // Update key's index
                output.verbose(CALL_INFO, 7, 0, "stepInsert case 6: indices[%d] = idx %d\n", key, idx);
                write(indexAddr(key), idx);
                heap_size++;
                op_state = 7;
                break;
                
            case 7:
                completeOperation(true);
                break;
        }
    }
    
    void stepRemoveMin() {
        output.verbose(CALL_INFO, 7, 0, "Heap::stepRemoveMin: op_state %d\n", op_state);
        switch (op_state) {
            case 0:
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 0: heap_size=%lu\n", heap_size);
                if (heap_size == 0) {
                    completeOperation(false);
                    assert(false);
                    return;
                }
                read(heapAddr(0));  // Read root (min) element
                op_state = 1;
                break;
                
            case 1:
                min_val = result;
                if (heap_size == 1) {
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 1: indices[%d]=-1\n", min_val);
                    // Mark min_val as not in the heap
                    write(indexAddr(min_val), -1);
                    heap_size--;
                    idx = 0;
                    op_state = 5;
                    break;
                }
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 1: reading last element\n");
                read(heapAddr(heap_size-1));  // Read last element
                op_state = 2;
                break;
                
            case 2:  // Move last element to root
                key = result;
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 2: indices[%d]=0\n", key);
                write(indexAddr(key), 0);
                op_state = 3;
                break;
                
            case 3:  // Move last element to root
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 3: heap[0]=%d\n", key);
                write(heapAddr(0), key);
                op_state = 4;
                break;
                
            case 4:  // Mark min_val as not in the heap
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 4: indices[%d]=-1\n", min_val);
                write(indexAddr(min_val), -1);
                heap_size--;
                op_state = 5;
                idx = 0;  // Start percolate down from root
                break;
                
            case 5:  // Find appropriate child
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 5: idx=%d, left=%d, right=%d, heap_size=%lu\n", 
                       idx, left(idx), right(idx), heap_size);
                // Check if we have children
                if (left(idx) >= heap_size) {
                    write(heapAddr(idx), key);  // No swap needed, write key back
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 5: no swap needed, heap[%d]=%d\n", idx, key);
                    op_state = 10;
                    return;
                }
                
                child_idx = left(idx);
                if (right(idx) < heap_size) {
                    // Have both children, read left first
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 5: two children, read left\n");
                    read(heapAddr(left(idx)));
                    op_state = 6;
                } else {
                    // Only have left child
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 5: only left child\n");
                    read(heapAddr(child_idx));
                    op_state = 8;
                }
                break;
                
            case 6:  // Read left child, then right child
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 6: read right child\n");
                left_val = result;
                read(heapAddr(right(idx)));
                op_state = 7;
                break;
                
            case 7:  // Select smaller child
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 7: comparing children - left=%d, right=%d\n", left_val, result);
                right_val = result;
                
                // Select child with higher priority and continue
                if (lt(right_val, left_val)) {
                    child_idx = right(idx);
                    right_val = right_val; // Use right value
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 7: right child has higher priority\n");
                } else {
                    child_idx = left(idx);
                    right_val = left_val;  // Use left value
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 7: left child has higher priority\n");
                }
                // Fall through to compare with key
                if (lt(right_val, key)) {
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 7: child (val=%d) has higher priority than key=%d, swapping\n", right_val, key);
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 7: writing heap[%d]=%d\n", idx, right_val);
                    write(heapAddr(idx), right_val);  //heap[i] = heap[child];
                    op_state = 9;
                } else {
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 7: key=%d has higher priority than child=%d, no swap needed\n", key, right_val);
                    write(heapAddr(idx), key);  // No swap needed, write key back
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 8: no swap needed, heap[%d]=%d\n", idx, key);
                    op_state = 10;
                }
                break;
                
            case 8:  // Single child case - compare with key
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 8: comparing single child val=%d with key=%d\n", result, key);
                right_val = result;
                if (lt(right_val, key)) {
                    write(heapAddr(idx), right_val);
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 8: heap[%d]=%d\n", idx, right_val);
                    op_state = 9;
                } else {
                    write(heapAddr(idx), key);  // No swap needed, write key back
                    output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 8: no swap needed, heap[%d]=%d\n", idx, key);
                    op_state = 10;
                }
                break;
                
            case 9:  // Update child's index and continue downward
                write(indexAddr(right_val), idx);  //indices[heap[i]] = i;
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 9: indices[%d]=%d\n", right_val, idx);
                idx = child_idx;
                op_state = 5;  // Continue percolate down
                break;
            
            case 10:
                write(indexAddr(key), idx);  // write key's index back
                op_state = 11;
                output.verbose(CALL_INFO, 7, 0, "stepRemoveMin case 10: indices[%d]=%d\n", key, idx);
                break;

            case 11:
                completeOperation(true, min_val);  // Complete operation with min value
                break;
        }
    }
    
    void stepInHeap() {
        output.verbose(CALL_INFO, 7, 0, "Heap::stepInHeap: op_state %d\n", op_state);
        if (op_state == 0) {
            read(indexAddr(key));
            op_state = 1;
        } else {
            output.verbose(CALL_INFO, 7, 0, "stepInHeap case 1: result %d, in_heap %d\n", result, (int(result) >= 0));
            completeOperation(true, (int(result) >= 0));
        }
    }
    
    void stepRead() {
        output.verbose(CALL_INFO, 7, 0, "Heap::stepRead: op_state %d\n", op_state);
        if (op_state == 0) {
            read(heapAddr(idx));
            op_state = 1;
        } else {
            output.verbose(CALL_INFO, 7, 0, "stepRead case 1: result %d\n", result);
            completeOperation(true, result);
        }
    }
    
    void stepDecrease() {
        output.verbose(CALL_INFO, 7, 0, "Heap::stepDecrease: op_state %d\n", op_state);
        switch (op_state) {
            case 0:
                output.verbose(CALL_INFO, 7, 0, "stepDecrease case 0: reading index for key %d\n", key);
                read(indexAddr(key));
                op_state = 1;
                break;
                
            case 1:
                output.verbose(CALL_INFO, 7, 0, "stepDecrease case 1: result=%d\n", result);
                if (int(result) < 0) {  // Key not in heap
                    output.verbose(CALL_INFO, 7, 0, "stepDecrease case 1: key not in heap\n");
                    completeOperation(true);
                    return;
                }
                idx = result;
                output.verbose(CALL_INFO, 7, 0, "stepDecrease case 1: idx=%d\n", idx);
                if (idx == 0) { // Already at root
                    output.verbose(CALL_INFO, 7, 0, "stepDecrease case 1: already at root\n");
                    completeOperation(true);
                    return;
                }
                parent_idx = parent(idx);
                output.verbose(CALL_INFO, 7, 0, "stepDecrease case 1: parent_idx=%d\n", parent_idx);
                read(heapAddr(parent_idx));
                op_state = 2;
                break;
                
            case 2:
                output.verbose(CALL_INFO, 7, 0, "stepDecrease case 2: comparing key %d with parent %d\n", key, result);
                if (lt(key, result)) { // Key has higher priority than parent
                    output.verbose(CALL_INFO, 7, 0, "stepDecrease case 2: swapping with parent\n");
                    write(heapAddr(idx), result);
                    op_state = 3;
                } else {
                    output.verbose(CALL_INFO, 7, 0, "stepDecrease case 2: no swap needed\n");
                    completeOperation(true);
                }
                break;
                
            case 3:
                output.verbose(CALL_INFO, 7, 0, "stepDecrease case 3: updating index for parent val=%d to idx=%d\n", result, idx);
                write(indexAddr(result), idx);
                idx = parent_idx;
                op_state = 4;
                break;
            
            case 4:
                if (idx == 0) { // Reached root
                    output.verbose(CALL_INFO, 7, 0, "stepDecrease case 4: reached root, writing key=%d to root\n", key);
                    write(heapAddr(0), key);
                    op_state = 5;
                } else {
                    parent_idx = parent(idx);
                    output.verbose(CALL_INFO, 7, 0, "stepDecrease case 4: continuing upward, new parent_idx=%d\n", parent_idx);
                    read(heapAddr(parent_idx));
                    op_state = 2; // Back to comparison
                }
                break;
                
            case 5:
                output.verbose(CALL_INFO, 7, 0, "stepDecrease case 5: updating index for key=%d to root position\n", key);
                write(indexAddr(key), 0);
                op_state = 6;
                break;
            
            case 6:
                completeOperation(true);
                break;
        }
    }

    void completeOperation(bool success, int res = 0) {
        output.verbose(CALL_INFO, 7, 0, "complete request %d, success %d, res %d\n", current_op, success, res);
        
        // Assert that all memory operations are complete before finishing
        assert(outstanding_mem_requests == 0);
        
        HeapRespEvent* ev = new HeapRespEvent(success, res);
        response_port->send(ev);
        state = IDLE;
        op_state = 0;
        parent_idx = 0;
        child_idx = 0;
    }
};


#endif
