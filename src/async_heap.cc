#include "async_heap.h"

Heap::Heap(SST::ComponentId_t id, SST::Params& params, const VarOrderLt& c, 
           SST::Interfaces::StandardMem* mem, uint64_t heap_base_addr, uint64_t indices_base_addr) 
    : SST::SubComponent(id), lt(c), memory(mem), state(IDLE), heap_size(0),
      outstanding_mem_requests(0), heap_addr(heap_base_addr), indices_addr(indices_base_addr) {
    
    output.init("HEAP-> ", params.find<int>("verbose", 0), 0, SST::Output::STDOUT);

    registerClock(params.find<std::string>("clock", "1GHz"),
        new SST::Clock::Handler<Heap>(this, &Heap::tick));
    
    response_port = configureLink("response");
    sst_assert( response_port != nullptr, CALL_INFO, -1, "Error: 'response_port' is not connected to a link\n");
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
                        [this](coro_t::push_type &heap_sink) { insert(heap_sink); });
                    break;
                case HeapReqEvent::REMOVE_MIN:
                    heap_source = new coro_t::pull_type(
                        [this](coro_t::push_type &heap_sink) { removeMin(heap_sink); });
                    break;
                case HeapReqEvent::DECREASE:
                    heap_source = new coro_t::pull_type(
                        [this](coro_t::push_type &heap_sink) { decrease(heap_sink); });
                    break;
                case HeapReqEvent::IN_HEAP:
                    heap_source = new coro_t::pull_type(
                        [this](coro_t::push_type &heap_sink) { inHeap(heap_sink); });
                    break;
                case HeapReqEvent::READ:
                    heap_source = new coro_t::pull_type(
                        [this](coro_t::push_type &heap_sink) { readHeap(heap_sink); });
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
                state = IDLE;  // Operation completed
            }
            break;
        default:
            output.fatal(CALL_INFO, -1, "Invalid state: %d\n", state);
    }
    return false;
}

void Heap::handleMem(SST::Interfaces::StandardMem::Request* req) {
    if (auto resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req))
        memcpy(&read_data, resp->data.data(), sizeof(Var));
    outstanding_mem_requests--;
    state = STEP;  // Continue heap_source
    // Note: req is deleted by the caller (SATSolver::handleHeapMemEvent)
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

void Heap::read(uint64_t addr) { 
    memory->send(new SST::Interfaces::StandardMem::Read(addr, sizeof(Var)));
    outstanding_mem_requests++;
    state = WAIT;  // Wait for response
}

void Heap::write(uint64_t addr, Var val) { 
    std::vector<uint8_t> data(sizeof(Var));
    memcpy(data.data(), &val, sizeof(Var));
    memory->send(new SST::Interfaces::StandardMem::Write(addr, sizeof(Var), data));
    outstanding_mem_requests++;
    state = WAIT;  // Wait for response 
}

void Heap::complete(int res) {
    output.verbose(CALL_INFO, 7, 0, "Completed Op %d, res %d\n", current_op, res);
    assert(outstanding_mem_requests == 0);
    HeapRespEvent* ev = new HeapRespEvent(res);
    response_port->send(ev);
    state = IDLE;
}

void Heap::percolateUp(int i, coro_t::push_type &heap_sink) {
    output.verbose(CALL_INFO, 7, 0, "PercolateUp: idx %d\n", i);
    read(heapAddr(i));
    heap_sink();
    Var x = read_data;

    int p = parent(i);
    read(heapAddr(p));
    heap_sink();
    Var heap_p = read_data;

    while (i > 0 && lt(x, heap_p)) {
        write(heapAddr(i), heap_p);
        heap_sink();

        write(indexAddr(heap_p), i);
        heap_sink();

        i = p;
        if (i == 0) break;  // reached root
        
        p = parent(p);
        read(heapAddr(p));
        heap_sink();
        heap_p = read_data;
        output.verbose(CALL_INFO, 7, 0, 
            "PercolateUp: new idx %d, parent idx %d, parent Var %d\n", i, p, heap_p);
    }

    output.verbose(CALL_INFO, 7, 0, "PercolateUp: final idx %d\n", i);
    write(heapAddr(i), x);
    heap_sink();

    write(indexAddr(x), i);
    heap_sink();
    output.verbose(CALL_INFO, 7, 0, "PercolateUp: completed for idx %d\n", i);
}

void Heap::percolateDown(int i, coro_t::push_type &heap_sink) {
    read(heapAddr(i));
    heap_sink();
    Var x = read_data;

    while (i < (int)(heap_size / 2)) {
        int child = left(i);
        read(heapAddr(child));
        heap_sink();
        Var heap_child = read_data;

        if (child + 1 < (int)heap_size) {
            read(heapAddr(child + 1));
            heap_sink();
            if (lt(read_data, heap_child)) {
                child++;
                heap_child = read_data;
            }
        }

        if (!lt(heap_child, x)) break;

        write(heapAddr(i), heap_child);
        heap_sink();

        write(indexAddr(heap_child), i);
        heap_sink();

        i = child;
    }

    write(heapAddr(i), x);
    heap_sink();

    write(indexAddr(x), i);
    heap_sink();
}

void Heap::inHeap(coro_t::push_type &heap_sink) {
    output.verbose(CALL_INFO, 7, 0, "InHeap: key %d\n", key);
    read(indexAddr(key));
    heap_sink();
    complete(read_data >= 0);
}

void Heap::readHeap(coro_t::push_type &heap_sink) {
    output.verbose(CALL_INFO, 7, 0, "Read: idx %d\n", idx);
    if (idx < 0 || idx >= heap_size) {
        complete(var_Undef);
        return;
    }

    read(heapAddr(idx));
    heap_sink();
    complete(read_data);
}

void Heap::decrease(coro_t::push_type &heap_sink) {
    output.verbose(CALL_INFO, 7, 0, "Decrease: key %d\n", key);
    read(indexAddr(key));
    heap_sink();
    idx = read_data;
    
    // key not in heap or already at root
    if (idx <= 0) {
        complete(1);
        return;
    }
    percolateUp(idx, heap_sink);
    complete(true);
}

void Heap::insert(coro_t::push_type &heap_sink) {
    output.verbose(CALL_INFO, 7, 0, "Insert: key %d, heap size %ld\n", key, heap_size);
    write(indexAddr(key), heap_size);
    heap_sink();
    
    write(heapAddr(heap_size), key);
    heap_sink();

    heap_size++;
    if (heap_size == 1) {
        complete(true);
        return;
    }
    percolateUp(heap_size - 1, heap_sink);
    complete(true);
}

void Heap::removeMin(coro_t::push_type &heap_sink) {
    output.verbose(CALL_INFO, 7, 0, "RemoveMin, heap size %ld\n", heap_size);
    sst_assert(heap_size > 0, CALL_INFO, -1, "Heap is empty, cannot remove min\n");
    
    read(heapAddr(0));
    heap_sink();
    Var min_var = read_data;

    write(indexAddr(min_var), -1);
    heap_sink();
    
    if (heap_size == 1) {
        heap_size--;
        complete(min_var);
        return;
    }

    read(heapAddr(heap_size - 1));
    heap_sink();
    Var last_var = read_data;

    write(indexAddr(last_var), 0);
    heap_sink();

    write(heapAddr(0), last_var);
    heap_sink();
    
    heap_size--;
    if (heap_size > 1)
        percolateDown(0, heap_sink);
    complete(min_var);
}

