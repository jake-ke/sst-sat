#include "async_var_activity.h"
#include "async_heap.h"

VarActivity::VarActivity(int verbose, SST::Interfaces::StandardMem* mem, 
                       uint64_t base_addr, Heap* heap_parent)
    : Activity(verbose, mem, base_addr, nullptr), heap(heap_parent), heap_sink_ptr(nullptr) {
    output.init("VARACT-> ", verbose, 0, SST::Output::STDOUT);
}

double VarActivity::read(size_t idx) {
    if (idx >= size_) {
        output.fatal(CALL_INFO, -1, "VarActivity index out of range: %zu\n", idx);
    }
    
    output.verbose(CALL_INFO, 7, 0, "Reading variable activity for idx %zu\n", idx);
    memory->send(new SST::Interfaces::StandardMem::Read(calcAddr(idx), sizeof(double)));
    heap->outstanding_mem_requests++;
    heap->state = Heap::WAIT;
    (**heap_sink_ptr)();  // Use the heap's coroutine
    
    memcpy(&last_value, last_buffer.data(), sizeof(double));
    return last_value;
}

void VarActivity::write(size_t idx, double value) {
    if (idx >= size_) {
        output.fatal(CALL_INFO, -1, "VarActivity index out of range: %zu\n", idx);
    }

    output.verbose(CALL_INFO, 7, 0, "Writing variable activity %f for idx %zu\n", value, idx);
    std::vector<uint8_t> buffer(sizeof(double));
    memcpy(buffer.data(), &value, sizeof(double));
    
    memory->send(new SST::Interfaces::StandardMem::Write(
        calcAddr(idx), sizeof(double), buffer));
    heap->outstanding_mem_requests++;
    heap->state = Heap::WAIT;
    (**heap_sink_ptr)();  // Use the heap's coroutine
}

std::vector<double> VarActivity::readBulk(size_t start, size_t count) {
    if (start + count > size_) {
        output.fatal(CALL_INFO, -1, "VarActivity bulk read out of range: %zu + %zu > %zu\n", 
                    start, count, size_);
    }
    
    memory->send(new SST::Interfaces::StandardMem::Read(
        calcAddr(start), count * sizeof(double)));
    heap->outstanding_mem_requests++;
    heap->state = Heap::WAIT;
    (**heap_sink_ptr)();  // Use the heap's coroutine
    
    std::vector<double> result(count);
    memcpy(result.data(), last_buffer.data(), count * sizeof(double));
    return result;
}

void VarActivity::writeBulk(size_t start, const std::vector<double>& values) {
    size_t count = values.size();
    if (start + count > size_) {
        output.fatal(CALL_INFO, -1, "VarActivity bulk write out of range: %zu + %zu > %zu\n", 
                    start, count, size_);
    }
    
    std::vector<uint8_t> buffer(count * sizeof(double));
    memcpy(buffer.data(), values.data(), buffer.size());
    
    memory->send(new SST::Interfaces::StandardMem::Write(
        calcAddr(start), buffer.size(), buffer));
    heap->outstanding_mem_requests++;
    heap->state = Heap::WAIT;
    (**heap_sink_ptr)();  // Use the heap's coroutine
}

