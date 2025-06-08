#include <sst/core/sst_config.h>
#include "async_base.h"

AsyncBase::AsyncBase(const std::string& prefix, int verbose, SST::Interfaces::StandardMem* mem, 
                     coro_t::push_type** yield_ptr)
    : memory(mem), yield_ptr(yield_ptr), pre_yield_callback(nullptr), line_size(64), size_(0),
      pending_read_count(0), all_reads_completed(false), in_burst_read(false) {
    output.init(prefix.c_str(), verbose, 0, SST::Output::STDOUT);
}

void AsyncBase::read(uint64_t addr, size_t size) {
    output.verbose(CALL_INFO, 8, 0, "Read at 0x%lx, size %zu\n", addr, size);
    memory->send(new SST::Interfaces::StandardMem::Read(addr, size));
    doYield();
}

void AsyncBase::write(uint64_t addr, size_t size, const std::vector<uint8_t>& data) {
    output.verbose(CALL_INFO, 8, 0, "Write at 0x%lx, size %zu\n", addr, size);
    memory->send(new SST::Interfaces::StandardMem::Write(addr, size, data));
    // doYield();
}

void AsyncBase::writeUntimed(uint64_t addr, size_t size, const std::vector<uint8_t>& data) {
    output.verbose(CALL_INFO, 8, 0, "Untimed write at 0x%lx, size %zu\n", addr, size);
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        addr, size, data, true, 0x1)); // posted, not cacheable
}

std::vector<AsyncBase::CacheChunk> AsyncBase::calculateCacheChunks(uint64_t start_addr, size_t total_size) {
    std::vector<CacheChunk> chunks;
    size_t bytes_processed = 0;
    
    while (bytes_processed < total_size) {
        uint64_t current_addr = start_addr + bytes_processed;
        uint64_t line_offset = current_addr % line_size;
        size_t bytes_remaining = total_size - bytes_processed;
        size_t bytes_in_line = line_size - line_offset;
        size_t chunk_size = std::min(bytes_remaining, bytes_in_line);
        
        chunks.push_back({current_addr, bytes_processed, chunk_size});
        bytes_processed += chunk_size;
    }
    
    return chunks;
}

void AsyncBase::readBurst(uint64_t start_addr, size_t element_size, size_t count) {
    size_t total_size = count * element_size;
    auto chunks = calculateCacheChunks(start_addr, total_size);
    
    // Setup for burst read tracking
    burst_start_addr = start_addr;
    pending_read_count = chunks.size();
    all_reads_completed = false;
    in_burst_read = true;
    burst_buffer.resize(total_size);
    
    for (const auto& chunk : chunks) {
        // Try to align to element boundaries when possible
        size_t aligned_size = chunk.size;
        if (chunk.size % element_size != 0 && chunk.offset_in_data + chunk.size < total_size) {
            size_t elements_in_chunk = chunk.size / element_size;
            if (elements_in_chunk > 0) {
                aligned_size = elements_in_chunk * element_size;
            }
        }
        
        output.verbose(CALL_INFO, 8, 0, 
            "ReadBurst chunk: addr=0x%lx, size=%zu, offset=%zu\n", 
            chunk.addr, aligned_size, chunk.offset_in_data);

        // Send all read requests
        memory->send(new SST::Interfaces::StandardMem::Read(chunk.addr, aligned_size));
    }
    
    // Wait for all responses to be received
    while (!all_reads_completed) doYield();
    
    in_burst_read = false;
    output.verbose(CALL_INFO, 7, 0, "ReadBurst: All %zu read requests completed\n", chunks.size());
}

void AsyncBase::writeBurst(uint64_t start_addr, size_t element_size, const std::vector<uint8_t>& data) {
    auto chunks = calculateCacheChunks(start_addr, data.size());
    
    for (const auto& chunk : chunks) {
        // Try to align to element boundaries when possible
        size_t aligned_size = chunk.size;
        if (chunk.size % element_size != 0 && chunk.offset_in_data + chunk.size < data.size()) {
            size_t elements_in_chunk = chunk.size / element_size;
            if (elements_in_chunk > 0) {
                aligned_size = elements_in_chunk * element_size;
            }
        }
        
        output.verbose(CALL_INFO, 8, 0, 
            "WriteBurst chunk: addr=0x%lx, size=%zu, offset=%zu\n", 
            chunk.addr, aligned_size, chunk.offset_in_data);
        
        std::vector<uint8_t> chunk_data(aligned_size);
        memcpy(chunk_data.data(), data.data() + chunk.offset_in_data, aligned_size);
        
        memory->send(new SST::Interfaces::StandardMem::Write(
            chunk.addr, aligned_size, chunk_data, false));
        // doYield();
    }
}

void AsyncBase::handleMem(SST::Interfaces::StandardMem::Request* req) {
    if (auto* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        uint64_t addr = resp->pAddr;
        
        if (in_burst_read) {  // This is a response to a burst read
            // Calculate offset in buffer based on address
            size_t offset_in_buffer = addr - burst_start_addr;
            
            // Copy data directly to the appropriate position in burst_buffer
            memcpy(burst_buffer.data() + offset_in_buffer, 
                   resp->data.data(), resp->data.size());

            // Decrement counter and check if all reads completed
            if (--pending_read_count == 0) all_reads_completed = true;
        } else {  // Standard single read response
            read_buffer = resp->data;  // Copy the read data
        }
    }
    // req will be deleted by the caller in SATSolver::handleGlobalMemEvent
}
