#include <sst/core/sst_config.h>
#include "async_activity.h"

Activity::Activity(int verbose, SST::Interfaces::StandardMem* mem, 
                   uint64_t base_addr, coro_t::push_type** yield_ptr)
    : memory(mem), yield_ptr(yield_ptr), base_addr(base_addr), size_(0) {
    output.init("ACTIVITY-> ", verbose, 0, SST::Output::STDOUT);
}

double Activity::read(size_t idx) {
    if (idx >= size_) {
        output.fatal(CALL_INFO, -1, "Activity index out of range: %zu\n", idx);
    }
    
    memory->send(new SST::Interfaces::StandardMem::Read(calcAddr(idx), sizeof(double)));
    (**yield_ptr)();
    
    memcpy(&last_value, last_buffer.data(), sizeof(double));
    return last_value;
}

void Activity::write(size_t idx, double value) {
    std::vector<uint8_t> buffer(sizeof(double));
    memcpy(buffer.data(), &value, sizeof(double));
    
    memory->send(new SST::Interfaces::StandardMem::Write(
        calcAddr(idx), sizeof(double), buffer));
    (**yield_ptr)();
}

std::vector<double> Activity::readBulk(size_t start, size_t count) {
    if (start + count > size_) {
        output.fatal(CALL_INFO, -1, "Bulk read out of range: %zu + %zu > %zu\n", 
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
            "ReadBulk chunk: addr=0x%lx, elements=%zu, bytes=%zu, line_offset=%zu\n", 
            addr, chunk_size, chunk_size * sizeof(double), line_offset);
        
        // Read this chunk
        memory->send(new SST::Interfaces::StandardMem::Read(
            addr, chunk_size * sizeof(double)));
        (**yield_ptr)();
        
        // Copy to result
        memcpy(&result[result_offset], last_buffer.data(), chunk_size * sizeof(double));
        
        // Update counters
        remaining -= chunk_size;
        current_idx += chunk_size;
        result_offset += chunk_size;
    }
    
    return result;
}

void Activity::writeBulk(size_t start, const std::vector<double>& values) {
    size_t count = values.size();
    if (start + count > size_) {
        output.fatal(CALL_INFO, -1, "Bulk write out of range: %zu + %zu > %zu\n", 
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
            "WriteBulk chunk: addr=0x%lx, elements=%zu, bytes=%zu, line_offset=%zu\n", 
            addr, chunk_size, chunk_size * sizeof(double), line_offset);
        
        // Prepare buffer for this chunk
        std::vector<uint8_t> buffer(chunk_size * sizeof(double));
        memcpy(buffer.data(), &values[values_offset], chunk_size * sizeof(double));
        
        // Write this chunk
        memory->send(new SST::Interfaces::StandardMem::Write(
            addr, chunk_size * sizeof(double), buffer));
        (**yield_ptr)();
        
        // Update counters
        remaining -= chunk_size;
        current_idx += chunk_size;
        values_offset += chunk_size;
    }
}

void Activity::push(double value) {
    output.verbose(CALL_INFO, 7, 0, 
                   "Push new value %f at index %zu\n", value, size_);
    write(size_, value);

    size_++;
}

void Activity::rescaleAll(double factor) {
    std::vector<double> values = readBulk(0, size_);
    
    for (size_t i = 0; i < size_; i++) {
        values[i] *= factor;
    }
    writeBulk(0, values);
}

void Activity::reduceDB(const std::vector<bool>& to_remove, size_t num_orig) {
    // Bulk read all activity values at once
    std::vector<double> activities = readBulk(0, size_);
    
    // Compact activities by removing marked ones
    std::vector<double> compacted;
    for (size_t i = 0; i < size_; i++)
    if (!to_remove[i + num_orig])
    compacted.push_back(activities[i]);
    
    // Bulk write compacted activities back if not empty
    if (!compacted.empty()) writeBulk(0, compacted);
    
    size_ = compacted.size();
    output.verbose(CALL_INFO, 7, 0, "ACTIVITY: Reduced from %zu to %zu\n", activities.size(), size_);
}

void Activity::handleMem(SST::Interfaces::StandardMem::Request* req) {
    if (auto* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        last_buffer = resp->data;
    }
}
