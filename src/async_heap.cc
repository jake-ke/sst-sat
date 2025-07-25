#include <sst/core/sst_config.h> // This include is REQUIRED for all implementation files
#include "async_heap.h"

Heap::Heap(SST::ComponentId_t id, SST::Params& params,
           SST::Interfaces::StandardMem* mem, uint64_t heap_base_addr, uint64_t indices_base_addr) 
    : SST::SubComponent(id), memory(mem), state(IDLE), heap_size(0),
      outstanding_mem_requests(0), heap_addr(heap_base_addr), indices_addr(indices_base_addr),
      heap_sink_ptr(nullptr), need_rescale(false),
      var_activity(params.find<int>("verbose", 0), mem, 
                   params.find<uint64_t>("var_act_base_addr", 0x70000000), this) {
    
    output.init("HEAP-> ", params.find<int>("verbose", 0), 0, SST::Output::STDOUT);
    output.verbose(CALL_INFO, 1, 0, "base addresses: heap=0x%lx, indices=0x%lx\n", 
                   heap_base_addr, indices_base_addr);

    registerClock(params.find<std::string>("clock", "1GHz"),
        new SST::Clock::Handler2<Heap, &Heap::tick>(this));

    response_port = configureLink("response");
    sst_assert( response_port != nullptr, CALL_INFO, -1, "Error: 'response_port' is not connected to a link\n");

    var_act_base_addr = params.find<uint64_t>("var_act_base_addr", 0x70000000);
    
    // Set up VarActivity to use our heap_sink_ptr
    var_activity.setHeapSinkPtr(&heap_sink_ptr);
    var_activity.setReorderBuffer(&reorder_buffer);
}

bool Heap::tick(SST::Cycle_t cycle) {
    switch (state) {
        case IDLE: break;
        case WAIT: break;
        case STEP: {
            output.verbose(CALL_INFO, 8, 0, "=== Tick %lu === \n", cycle);
            assert(heap_active_workers.size() <= HEAPLANES);
            bool done = true;
            for (size_t j = 0; j < heap_active_workers.size(); j++) {
                // output.verbose(CALL_INFO, 8, 0, "Worker %zu: active=%d\n", j, (bool)heap_active_workers[j]);
                if (heap_active_workers[j]) {
                    heap_sink_ptr = heap_sink_ptrs[j];
                    (*heap_sources[j])();
                    heap_active_workers[j] = false;
                    if ((*heap_sources[j])) {
                        done = false;
                    } else {
                        delete heap_sources[j];
                        heap_sources[j] = nullptr;
                        heap_sink_ptrs[j] = nullptr;
                    }
                } else if (heap_sources[j]) done = false;
                else if (!pending_requests.empty() && !need_rescale) {
                    // Start a new worker immediately if we have empty slots
                    startNewWorker(j);
                    done = false;
                }
            }

            // since the polling workers never get triggered,
            // we need to check them after completing the active workers
            for (size_t j = 0; j < heap_polling.size(); j++) {
                if (heap_polling[j]) {
                    heap_polling[j] = false;
                    heap_sink_ptr = heap_sink_ptrs[j];
                    (*heap_sources[j])();
                }
            }

            if (!done) state = WAIT;
            else {
                state = IDLE;
                heap_sink_ptrs.clear();
                heap_sources.clear();
                heap_active_workers.clear();
                heap_polling.clear();

                // if (heap_active_workers.size() > 1) state = DEBUG;
            }
            break;
        }
        case DEBUG:
            heap_active_workers.push_back(false);
            heap_polling.push_back(false);
            heap_sources.push_back(nullptr);
            heap_sink_ptrs.push_back(nullptr);
            heap_sources[0] = new coro_t::pull_type(
                [this](coro_t::push_type &heap_sink) { 
                    heap_sink_ptr = &heap_sink;
                    heap_sink_ptrs[0] = &heap_sink;
                    debug_heap(0);
                });
            state = WAIT;
            break;
        default:
            output.fatal(CALL_INFO, -1, "Invalid state: %d\n", state);
    }

    // handle pending requests - start new workers if we have available slots
    if (!pending_requests.empty() && heap_active_workers.size() < HEAPLANES 
        && !need_rescale && state != DEBUG) {
        size_t idx = heap_active_workers.size();
        startNewWorker(idx);
    }

    return false;
}

// Helper method to start a new worker with a request
void Heap::startNewWorker(size_t idx) {
    HeapReqEvent* req = pending_requests.front();
    pending_requests.pop();
    output.verbose(CALL_INFO, 7, 0, "Starting new worker %zu for op %d, arg %d\n", 
                   idx, req->op, req->arg);
    
    // If idx is beyond current size, we need to extend the vectors
    if (idx >= heap_active_workers.size()) {
        heap_active_workers.push_back(false);
        heap_polling.push_back(false);
        heap_sources.push_back(nullptr);
        heap_sink_ptrs.push_back(nullptr);
    } else {  // We're reusing a slot that was freed
        assert(heap_active_workers[idx] == false);
        assert(heap_polling[idx] == false);
        assert(heap_sources[idx] == nullptr);
        assert(heap_sink_ptrs[idx] == nullptr);
    }
    
    switch(req->op) {
        case HeapReqEvent::INSERT:
            heap_sources[idx] = new coro_t::pull_type(
                [this, req, idx](coro_t::push_type &heap_sink) { 
                    heap_sink_ptr = &heap_sink;
                    heap_sink_ptrs[idx] = &heap_sink;
                    insert(req->arg, idx);
                });
            break;
        case HeapReqEvent::REMOVE_MIN:
            heap_sources[idx] = new coro_t::pull_type(
                [this, req, idx](coro_t::push_type &heap_sink) { 
                    heap_sink_ptr = &heap_sink;
                    heap_sink_ptrs[idx] = &heap_sink;
                    removeMin(); 
                });
            break;
        case HeapReqEvent::READ:
            heap_sources[idx] = new coro_t::pull_type(
                [this, req, idx](coro_t::push_type &heap_sink) { 
                    heap_sink_ptr = &heap_sink;
                    heap_sink_ptrs[idx] = &heap_sink;
                    readHeap(req->arg); 
                });
            break;
        case HeapReqEvent::BUMP:
            heap_sources[idx] = new coro_t::pull_type(
                [this, req, idx](coro_t::push_type &heap_sink) { 
                    heap_sink_ptr = &heap_sink;
                    heap_sink_ptrs[idx] = &heap_sink;
                    varBump(req->arg, idx); 
                });
            break;
        default:
            output.fatal(CALL_INFO, -1, "Unknown operation: %d\n", req->op);
    }
    delete req;
}

void Heap::handleMem(SST::Interfaces::StandardMem::Request* req) {
    output.verbose(CALL_INFO, 8, 0, "handleMem for Heap\n");
    if (auto* read_resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        int worker_id = reorder_buffer.lookUpWorkerId(read_resp->getID());
        if (heap_active_workers.size() > 0) heap_active_workers[worker_id] = true;
        
        uint64_t addr = read_resp->pAddr;
        if (addr >= var_act_base_addr) {  // VarActivity response
            var_activity.handleMem(req);
        } else {  // Heap response
            reorder_buffer.storeResponse(read_resp->getID(), read_resp->data);
            outstanding_mem_requests--;
        }

        state = STEP;
    }
}

void Heap::handleRequest(HeapReqEvent* req) {
    output.verbose(CALL_INFO, 7, 0, "HandleReq: op %d, arg %d\n", req->op, req->arg);
    sst_assert(state == IDLE || req->op == HeapReqEvent::INSERT || req->op == HeapReqEvent::BUMP,
        CALL_INFO, -1, "Heap is in %d, cannot handle request %d\n", state, req->op);
    pending_requests.push(req);
}

Var Heap::read(uint64_t addr, int worker_id) {
    auto req = new SST::Interfaces::StandardMem::Read(addr, sizeof(Var));
    reorder_buffer.registerRequest(req->getID(), worker_id);
    memory->send(req);
    outstanding_mem_requests++;
    state = WAIT;
    (*heap_sink_ptr)();
    
    Var v;
    memcpy(&v, reorder_buffer.getResponse(worker_id).data(), sizeof(Var));
    return v;
}

void Heap::write(uint64_t addr, Var val) {
    std::vector<uint8_t> data(sizeof(Var));
    memcpy(data.data(), &val, sizeof(Var));
    memory->send(new SST::Interfaces::StandardMem::Write(addr, sizeof(Var), data));
    // outstanding_mem_requests++;
    // state = WAIT;  // Wait for response 
    // (*heap_sink_ptr)();
}

void Heap::complete(int res, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "Complete[%d]: res %d\n", worker_id, res);

    if (heap_active_workers.size() == 1) {
        sst_assert(outstanding_mem_requests == 0, CALL_INFO, -1,
                   "outstanding_mem_requests: %d\n", outstanding_mem_requests);
    }

    HeapRespEvent* ev = new HeapRespEvent(res);
    response_port->send(ev);
    state = IDLE;
}

// debugging only
void Heap::debug_heap(int worker_id) {
    printf("start debugging\n");
    sst_assert(outstanding_mem_requests == 0, CALL_INFO, -1,
               "outstanding_mem_requests: %d\n", outstanding_mem_requests);

    for (auto lock : locks) {
        sst_assert(!lock, CALL_INFO, -1, "Heap lock still held\n");
    }

    for (int i = 0; i < heap_size; i++) {
        Var v = read(heapAddr(i), worker_id);
        int idx = read(indexAddr(v), worker_id);
        sst_assert(idx == i, CALL_INFO, -1, "Heap index mismatch: expected %d, got %d for key %d\n", i, idx, v);
    }

    // for (Var v = 1; v <= 41; v++) {  // change the range as needed
    //     int idx = read(indexAddr(v), worker_id);
    //     if (idx >= 0) {
    //         Var heap_v = read(heapAddr(idx), worker_id);
    //         sst_assert(heap_v == v, CALL_INFO, -1, "Heap value mismatch: expected %d at index %d, got %d\n", v, idx, heap_v);
    //     }
    // }

    state = IDLE;
    heap_sink_ptrs.clear();
    heap_sources.clear();
    heap_active_workers.clear();
    heap_polling.clear();
}

void Heap::percolateUp(int i, Var x, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "PercolateUp[%d]: idx %d, key %d\n", worker_id, i, x);
    // printf("lock[%d]: %d\n", worker_id, i);
    lock(i);

    int p = parent(i);
    while (isLocked(p)) {
        heap_polling[worker_id] = true;
        (*heap_sink_ptr)();
    }
    // printf("lock[%d]: %d\n", worker_id, p);
    lock(p);
    Var heap_p = read(heapAddr(p), worker_id);
    // printf("percolateUp[%d]: parent %d, var %d\n", worker_id, parent(i), heap_p);

    while (i > 0 && lt(x, heap_p, worker_id)) {
        write(heapAddr(i), heap_p);
        write(indexAddr(heap_p), i);
        // printf("unlock[%d]: %d\n", worker_id, i);
        unlock(i);

        i = p;
        // printf("percolateUp[%d]: swapped, now at idx %d\n", worker_id, i);
        if (i == 0) break;  // reached root
        
        p = parent(p);
        while (isLocked(p)) {
            heap_polling[worker_id] = true;
            (*heap_sink_ptr)();
        }
        lock(p);
        // printf("lock[%d]: %d\n", worker_id, p);
        heap_p = read(heapAddr(p), worker_id);
        // printf("percolateUp[%d]: parent %d, var %d\n", worker_id, p, heap_p);
    }
    // printf("exit loop[%d]: i %d, p %d\n", worker_id, i, p);

    write(heapAddr(i), x);
    write(indexAddr(x), i);
    unlock(i);
    unlock(p);
    // printf("unlock[%d]: %d\n", worker_id, i);
    // printf("unlock[%d]: %d\n", worker_id, p);
    output.verbose(CALL_INFO, 7, 0, "PercolateUp[%d]: key %d, final idx %d\n", worker_id, x, i);
}

void Heap::percolateDown(int i, Var key) {
    Var x;
    if (key != var_Undef) x = key;
    else x = read(heapAddr(i));

    // printf("percolateDown: idx %d, key %d\n", i, x);
    while (i < (int)(heap_size / 2)) {
        int child = left(i);
        Var heap_child = read(heapAddr(child));
        // printf("percolateDown: left child %d, value %d\n", child, heap_child);

        if (child + 1 < (int)heap_size) {
            Var right_child = read(heapAddr(child + 1));
            // printf("percolateDown: right child %d, value %d\n", child + 1, right_child);
            if (lt(right_child, heap_child)) {
                child++;
                heap_child = right_child;
            }
        }

        if (!lt(heap_child, x)) break;

        write(heapAddr(i), heap_child);
        write(indexAddr(heap_child), i);
        i = child;
        // printf("percolateDown: swapped, now at idx %d\n", i);
    }

    // printf("percolateDown: final idx %d, value %d\n", i, x);
    write(heapAddr(i), x);
    write(indexAddr(x), i);
}

bool Heap::inHeap(Var key, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "InHeap: key %d\n", key);
    int i = read(indexAddr(key), worker_id);

    return i >= 0;
}

void Heap::readHeap(int idx) {
    output.verbose(CALL_INFO, 7, 0, "Read: idx %d\n", idx);
    if (idx < 0 || idx >= heap_size) {
        complete(var_Undef);
        return;
    }
    Var v = read(heapAddr(idx));

    complete(v);
}

void Heap::decrease(Var key, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "Decrease[%d]: key %d\n", worker_id, key);
    int idx;
    while (true) {
        idx = read(indexAddr(key), worker_id);

        // key not in heap or already at root
        if (idx <= 0) {
            return;
        }
        
        // check if the key has been swapped while reading its index
        while (isLocked(idx)) {
            heap_polling[worker_id] = true;
            (*heap_sink_ptr)();
        }
        lock(idx);
        Var heap_key = read(heapAddr(idx), worker_id);

        if (heap_key == key) break;  // run percolateUp directly
        else unlock(idx);  // has been swapped; read the new index
    }

    percolateUp(idx, key, worker_id);
}

void Heap::insert(Var key, int worker_id) {
    if (inHeap(key, worker_id)) {
        output.verbose(CALL_INFO, 7, 0, "Insert[%d]: already in heap\n", worker_id);
        complete(true, worker_id);
        return;
    }

    // insert at the end of the heap
    write(indexAddr(key), heap_size);
    write(heapAddr(heap_size), key);
    heap_size++;
    output.verbose(CALL_INFO, 7, 0, "Insert[%d]: key %d, heap size %ld\n",
        worker_id, key, heap_size);
    if (heap_size == 1) {
        complete(true, worker_id);
        return;
    }

    percolateUp(heap_size - 1, key, worker_id);
    complete(true, worker_id);
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
    percolateDown(0, last_var);
    complete(min_var);
}

void Heap::initHeap() {
    output.verbose(CALL_INFO, 1, 0, "Size: %ld decision variables, %lu bytes\n",
                   (heap_size + 1), (heap_size + 1) * sizeof(Var));
    output.verbose(CALL_INFO, 1, 0, "Size: %ld indices, %lu bytes\n",
                   (heap_size + 1), (heap_size + 1) * sizeof(int));
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
        true, 0x1));  // posted, and not cacheable
    
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        indices_addr, indices_data.size(), indices_data,
        true, 0x1));  // posted, and not cacheable

    locks.resize(heap_size + 1, false);  // Initialize locks for all variables

    // Initialize var_activity
    output.verbose(CALL_INFO, 7, 0, "Intializing var_activity\n");
    var_activity.initialize(heap_size + 1, 0.0);
}

bool Heap::lt(Var x, Var y, int worker_id) {
    double act_x = var_activity.readAct(x, worker_id);
    double act_y = var_activity.readAct(y, worker_id);
    output.verbose(CALL_INFO, 7, 0, "Comparing var %d (act %.2f) with var %d (act %.2f)\n", 
                   x, act_x, y, act_y);
    return act_x > act_y;
}

void Heap::varBump(Var key, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "BUMP[%d] activity for var %d\n", worker_id, key);
    double act = var_activity.readAct(key, worker_id);

    if (need_rescale) {  // wait for rescale and then retry
        pending_requests.push(new HeapReqEvent(HeapReqEvent::BUMP, key));
        return;
    }

    double new_act = act + *(var_inc_ptr);
    var_activity[key] = new_act;

    if (new_act > 1e100) {
        output.verbose(CALL_INFO, 4, 0, "BUMP[%d] need to rescale variable activity\n", worker_id);
        need_rescale = true;
        // wait for all workers to finish before rescaling
        while (true) {
            int active_workers = 0;
            int active_idx = 0;
            for (size_t i = 0; i < heap_sources.size(); i++) {
                if (heap_sources[i]) {
                    active_workers++;
                    active_idx = i;
                }
            }

            if (active_workers == 1 && active_idx == worker_id) break;
            else {
                heap_polling[worker_id] = true;
                (*heap_sink_ptr)();
            }
        }

        var_activity.rescaleAll(1e-100, worker_id);
        *var_inc_ptr *= 1e-100;
        need_rescale = false;
    }

    decrease(key, worker_id);
    
    complete(true, worker_id);
}
