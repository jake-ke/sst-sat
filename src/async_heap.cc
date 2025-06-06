#include "async_heap.h"

Heap::Heap(SST::ComponentId_t id, SST::Params& params,
           SST::Interfaces::StandardMem* mem, uint64_t heap_base_addr, uint64_t indices_base_addr) 
    : SST::SubComponent(id), memory(mem), state(IDLE), heap_size(0),
      outstanding_mem_requests(0), heap_addr(heap_base_addr), indices_addr(indices_base_addr),
      heap_sink_ptr(nullptr),
      var_activity(params.find<int>("verbose", 0), mem, 
                   params.find<uint64_t>("var_act_base_addr", 0x70000000), this) {
    
    output.init("HEAP-> ", params.find<int>("verbose", 0), 0, SST::Output::STDOUT);
    output.verbose(CALL_INFO, 1, 0, "base addresses: heap=0x%lx, indices=0x%lx\n", 
                   heap_base_addr, indices_base_addr);

    registerClock(params.find<std::string>("clock", "1GHz"),
        new SST::Clock::Handler<Heap>(this, &Heap::tick));
    
    response_port = configureLink("response");
    sst_assert( response_port != nullptr, CALL_INFO, -1, "Error: 'response_port' is not connected to a link\n");

    var_act_base_addr = params.find<uint64_t>("var_act_base_addr", 0x70000000);
    
    // Set up VarActivity to use our heap_sink_ptr
    var_activity.setHeapSinkPtr(&heap_sink_ptr);
}

bool Heap::tick(SST::Cycle_t cycle) {
    switch (state) {
        case IDLE: break;
        case WAIT: break;
        output.verbose(CALL_INFO, 7, 0, "Tick %lu: state %d, OP: %d\n", cycle, state, current_op);
        case START:
            switch(current_op) {
                case HeapReqEvent::INSERT:
                    heap_source = new coro_t::pull_type(
                        [this](coro_t::push_type &heap_sink) { 
                            heap_sink_ptr = &heap_sink; 
                            insert(); 
                        });
                    break;
                case HeapReqEvent::REMOVE_MIN:
                    heap_source = new coro_t::pull_type(
                        [this](coro_t::push_type &heap_sink) { 
                            heap_sink_ptr = &heap_sink; 
                            removeMin(); 
                        });
                    break;
                case HeapReqEvent::IN_HEAP:
                    heap_source = new coro_t::pull_type(
                        [this](coro_t::push_type &heap_sink) { 
                            heap_sink_ptr = &heap_sink; 
                            inHeap(); 
                        });
                    break;
                case HeapReqEvent::READ:
                    heap_source = new coro_t::pull_type(
                        [this](coro_t::push_type &heap_sink) { 
                            heap_sink_ptr = &heap_sink; 
                            readHeap(); 
                        });
                    break;
                case HeapReqEvent::BUMP:
                    heap_source = new coro_t::pull_type(
                        [this](coro_t::push_type &heap_sink) { 
                            heap_sink_ptr = &heap_sink; 
                            varBump(); 
                        });
                    break;
                default:
                    output.fatal(CALL_INFO, -1, "Unknown operation: %d\n", current_op);
            }
            state = WAIT;
            break;
        case STEP:
            (*heap_source)();
            if (*heap_source) state = WAIT;
            else {
                delete heap_source;
                heap_source = nullptr;
                heap_sink_ptr = nullptr;  // Clear the sink pointer
                state = IDLE;             // Operation completed
            }
            break;
        default:
            output.fatal(CALL_INFO, -1, "Invalid state: %d\n", state);
    }
    return false;
}

void Heap::handleMem(SST::Interfaces::StandardMem::Request* req) {
    output.verbose(CALL_INFO, 8, 0, "handleMem for Heap\n");
    uint64_t addr = 0;
    if (auto* read_req = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        addr = read_req->pAddr;
        // Check if this response is for VarActivity
        if (addr >= var_act_base_addr) {
            var_activity.handleMem(req);
        } else {
            // Store data from read response
            memcpy(&read_data, read_req->data.data(), 
                   std::min(sizeof(Var), read_req->data.size()));
        }
    }
    
    outstanding_mem_requests--;
    state = STEP;
}

void Heap::handleRequest(HeapReqEvent* req) {
    assert(outstanding_mem_requests == 0);
    assert(state == IDLE);
    output.verbose(CALL_INFO, 7, 0, "HandleReq: op %d, arg %d\n", req->op, req->arg);
    
    current_op = req->op;
    // request arg can be key or idx depending on operation
    key = req->arg;
    idx = key;
    state = START;
}

Var Heap::read(uint64_t addr) {
    memory->send(new SST::Interfaces::StandardMem::Read(addr, sizeof(Var)));
    outstanding_mem_requests++;
    state = WAIT;
    (*heap_sink_ptr)();
    
    return read_data;
}

void Heap::write(uint64_t addr, Var val) {
    std::vector<uint8_t> data(sizeof(Var));
    memcpy(data.data(), &val, sizeof(Var));
    memory->send(new SST::Interfaces::StandardMem::Write(addr, sizeof(Var), data));
    outstanding_mem_requests++;
    state = WAIT;  // Wait for response 
    (*heap_sink_ptr)();
}

void Heap::complete(int res) {
    output.verbose(CALL_INFO, 7, 0, "Completed Op %d, res %d\n", current_op, res);
    assert(outstanding_mem_requests == 0);
    HeapRespEvent* ev = new HeapRespEvent(res);
    response_port->send(ev);
    state = IDLE;
}

void Heap::percolateUp(int i) {
    output.verbose(CALL_INFO, 7, 0, "PercolateUp: idx %d\n", i);
    Var x = read(heapAddr(i));

    int p = parent(i);
    Var heap_p = read(heapAddr(p));

    while (i > 0 && lt(x, heap_p)) {
        write(heapAddr(i), heap_p);

        write(indexAddr(heap_p), i);

        i = p;
        if (i == 0) break;  // reached root
        
        p = parent(p);
        heap_p = read(heapAddr(p));

        output.verbose(CALL_INFO, 7, 0, 
            "PercolateUp: new idx %d, parent idx %d, parent Var %d\n", i, p, heap_p);
    }

    output.verbose(CALL_INFO, 7, 0, "PercolateUp: final idx %d\n", i);
    write(heapAddr(i), x);

    write(indexAddr(x), i);
    output.verbose(CALL_INFO, 7, 0, "PercolateUp: completed for idx %d\n", i);
}

void Heap::percolateDown(int i) {
    Var x = read(heapAddr(i));

    while (i < (int)(heap_size / 2)) {
        int child = left(i);
        Var heap_child = read(heapAddr(child));

        if (child + 1 < (int)heap_size) {
            read(heapAddr(child + 1));
            if (lt(read_data, heap_child)) {
                child++;
                heap_child = read_data;
            }
        }

        if (!lt(heap_child, x)) break;

        write(heapAddr(i), heap_child);

        write(indexAddr(heap_child), i);

        i = child;
    }

    write(heapAddr(i), x);

    write(indexAddr(x), i);
}

void Heap::inHeap() {
    output.verbose(CALL_INFO, 7, 0, "InHeap: key %d\n", key);
    read(indexAddr(key));
    complete(read_data >= 0);
}

void Heap::readHeap() {
    output.verbose(CALL_INFO, 7, 0, "Read: idx %d\n", idx);
    if (idx < 0 || idx >= heap_size) {
        complete(var_Undef);
        return;
    }
    read(heapAddr(idx));

    complete(read_data);
}

void Heap::decrease() {
    output.verbose(CALL_INFO, 7, 0, "Decrease: key %d\n", key);
    idx = read(indexAddr(key));
    
    // key not in heap or already at root
    if (idx <= 0) {
        complete(1);
        return;
    }
    percolateUp(idx);
}

void Heap::insert() {
    output.verbose(CALL_INFO, 7, 0, "Insert: key %d, heap size %ld\n", key, heap_size);
    write(indexAddr(key), heap_size);
    
    write(heapAddr(heap_size), key);

    heap_size++;
    if (heap_size == 1) {
        complete(true);
        return;
    }
    percolateUp(heap_size - 1);
    complete(true);
}

void Heap::removeMin() {
    output.verbose(CALL_INFO, 7, 0, "RemoveMin, heap size %ld\n", heap_size);
    sst_assert(heap_size > 0, CALL_INFO, -1, "Heap is empty, cannot remove min\n");
    
    Var min_var = read(heapAddr(0));

    write(indexAddr(min_var), -1);
    
    if (heap_size == 1) {
        heap_size--;
        complete(min_var);
        return;
    }

    Var last_var = read(heapAddr(heap_size - 1));

    write(indexAddr(last_var), 0);

    write(heapAddr(0), last_var);
    
    heap_size--;
    if (heap_size > 1)
        percolateDown(0);
    complete(min_var);
}

void Heap::initHeap() {
    output.verbose(CALL_INFO, 7, 0, "Initializing heap with %ld decision variables\n", heap_size);
    // Count decision variables and prepare data in one pass
    std::vector<uint8_t> heap_data;
    std::vector<int> pos_map(heap_size + 1, -1);  // All indices start as -1 (not in heap)
    
    int heap_idx = 0;
    for (Var v = 1; v <= (Var)heap_size; v++) {
        if (!decision[v]) continue;
        
        // Append to heap array
        heap_data.resize((heap_idx + 1) * sizeof(Var));
        memcpy(heap_data.data() + heap_idx * sizeof(Var), &v, sizeof(Var));
       
        pos_map[v] = heap_idx++;   // Mark position in indices map
    }

    // Convert positions map to byte array
    std::vector<uint8_t> indices_data((heap_size + 1) * sizeof(Var));
    memcpy(indices_data.data(), pos_map.data(), indices_data.size());
    
    // Send bulk writes
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        heap_addr, heap_data.size(), heap_data,
        false, 0x1));  // not posted, and not cacheable
    
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        indices_addr, indices_data.size(), indices_data,
        false, 0x1));  // not posted, and not cacheable

    // Initialize var_activity
    output.verbose(CALL_INFO, 7, 0, "Intializing var_activity\n");
    var_activity.initialize(heap_size + 1, 0.0);
}

bool Heap::lt(Var x, Var y) {
    return var_activity[x] > var_activity[y];
}

void Heap::varBump() {
    output.verbose(CALL_INFO, 7, 0, "bumped activity for var %d\n", key);

    double act = var_activity[key];
    double new_act = act + *(var_inc_ptr);

    var_activity[key] = new_act;

    if (new_act > 1e100) {
        output.verbose(CALL_INFO, 7, 0, "Rescaling variable activity\n");
        var_activity.rescaleAll(1e-100);
        *var_inc_ptr = 1e-100;
    }

    decrease();
    
    complete(true);
}
