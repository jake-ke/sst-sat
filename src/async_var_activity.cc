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
    
    std::vector<double> result(count);
    
    // Process the read in chunks that don't cross cache lines
    size_t remaining = count;
    size_t current_idx = start;
    size_t result_offset = 0;
    
    while (remaining > 0) {
        // Calculate address and alignment
        uint64_t addr = calcAddr(current_idx);
        uint64_t line_offset = addr % line_size;
        
        // Calculate max elements we can read without crossing a cache line
        size_t elem_per_line = line_size / sizeof(double);
        size_t line_remaining = (line_size - line_offset) / sizeof(double);
        size_t chunk_size = std::min(remaining, line_remaining);
        
        output.verbose(CALL_INFO, 8, 0, 
            "VarActivity ReadBulk chunk: addr=0x%lx, elements=%zu, bytes=%zu, line_offset=%zu\n", 
            addr, chunk_size, chunk_size * sizeof(double), line_offset);
        
        // Read this chunk
        memory->send(new SST::Interfaces::StandardMem::Read(
            addr, chunk_size * sizeof(double)));
        heap->outstanding_mem_requests++;
        heap->state = Heap::WAIT;
        (**heap_sink_ptr)();
        
        // Copy to result
        memcpy(&result[result_offset], last_buffer.data(), chunk_size * sizeof(double));
        
        // Update counters
        remaining -= chunk_size;
        current_idx += chunk_size;
        result_offset += chunk_size;
    }
    
    return result;
}

void VarActivity::writeBulk(size_t start, const std::vector<double>& values) {
    size_t count = values.size();
    if (start + count > size_) {
        output.fatal(CALL_INFO, -1, "VarActivity bulk write out of range: %zu + %zu > %zu\n", 
                    start, count, size_);
    }
    
    // Process the write in chunks that don't cross cache lines
    size_t remaining = count;
    size_t current_idx = start;
    size_t values_offset = 0;
    
    while (remaining > 0) {
        // Calculate address and alignment
        uint64_t addr = calcAddr(current_idx);
        uint64_t line_offset = addr % line_size;
        
        // Calculate max elements we can write without crossing a cache line
        size_t elem_per_line = line_size / sizeof(double);
        size_t line_remaining = (line_size - line_offset) / sizeof(double);
        size_t chunk_size = std::min(remaining, line_remaining);
        
        output.verbose(CALL_INFO, 8, 0, 
            "VarActivity WriteBulk chunk: addr=0x%lx, elements=%zu, bytes=%zu, line_offset=%zu\n", 
            addr, chunk_size, chunk_size * sizeof(double), line_offset);
        
        // Prepare buffer for this chunk
        std::vector<uint8_t> buffer(chunk_size * sizeof(double));
        memcpy(buffer.data(), &values[values_offset], chunk_size * sizeof(double));
        
        // Write this chunk
        memory->send(new SST::Interfaces::StandardMem::Write(
            addr, chunk_size * sizeof(double), buffer));
        heap->outstanding_mem_requests++;
        heap->state = Heap::WAIT;
        (**heap_sink_ptr)();
        
        // Update counters
        remaining -= chunk_size;
        current_idx += chunk_size;
        values_offset += chunk_size;
    }
}

void VarActivity::initialize(size_t count, double init_value) {
    size_ = count;
    if (count == 0) return;
    
    std::vector<double> values(count, init_value);
    std::vector<uint8_t> buffer(count * sizeof(double));
    memcpy(buffer.data(), values.data(), buffer.size());
    
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        calcAddr(0), buffer.size(), buffer,
        false, 0x1));  // not posted, and not cacheable
}

