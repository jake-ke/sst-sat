#include <sst/core/sst_config.h>
#include "async_base.h"

AsyncBase::AsyncBase(const std::string& prefix, int verbose, SST::Interfaces::StandardMem* mem, 
                     coro_t::push_type** yield_ptr)
    : memory(mem), yield_ptr(yield_ptr), pre_yield_callback(nullptr), line_size(64), size_(0) {
    output.init(prefix.c_str(), verbose, 0, SST::Output::STDOUT);
}

void AsyncBase::read(uint64_t addr, size_t size, uint64_t worker_id) {
    output.verbose(CALL_INFO, 8, 0, "Read at 0x%lx, size %zu, worker %lu\n", 
                  addr, size, worker_id);
    // Create request and assign ID through the reorder buffer
    auto req = new SST::Interfaces::StandardMem::Read(addr, size);
    reorder_buffer->registerRequest(req->getID(), worker_id);
    memory->send(req);
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

void AsyncBase::readBurst(uint64_t start_addr, size_t element_size, size_t count, uint64_t worker_id) {
    size_t total_size = count * element_size;
    auto chunks = calculateCacheChunks(start_addr, total_size);
    
    // Create/update burst state for this worker
    BurstReadState& worker_state = burst_states[worker_id];
    worker_state.start_addr = start_addr;
    worker_state.pending_read_count = chunks.size();
    worker_state.completed = false;
    
    // Initialize buffer for the complete burst read
    reorder_buffer->startBurst(worker_id, total_size);

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
            "ReadBurst chunk: addr=0x%lx, size=%zu, offset=%zu, worker=%lu\n", 
            chunk.addr, aligned_size, chunk.offset_in_data, worker_id);

        // Create request and register with reorder buffer
        auto req = new SST::Interfaces::StandardMem::Read(chunk.addr, aligned_size);
        uint64_t req_id = req->getID();
        reorder_buffer->registerRequest(req_id, worker_id);
        memory->send(req);
    }
    
    // Wait for this worker's burst read to complete
    while (!worker_state.completed) {
        doYield();
    }
    
    // Clean up burst state for this worker
    burst_states.erase(worker_id);
    
    output.verbose(CALL_INFO, 7, 0, "ReadBurst: All %zu read requests completed for worker %lu\n", 
                  chunks.size(), worker_id);
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
        uint64_t req_id = resp->getID();
        int worker_id = reorder_buffer->lookUpWorkerId(req_id);
        
        output.verbose(CALL_INFO, 8, 0, "handleMem response for 0x%lx, req_id %lu, worker %d\n", 
                      addr, req_id, worker_id);
        
        // Check if this is part of a burst read
        auto burst_it = burst_states.find(worker_id);
        if (burst_it != burst_states.end()) {
            // This response belongs to a burst read
            BurstReadState& state = burst_it->second;
            uint64_t offset_in_buffer = addr - state.start_addr;
            reorder_buffer->storeResponse(req_id, resp->data, true, offset_in_buffer);
            
            if (--state.pending_read_count == 0) state.completed = true;
        } else {
            // Standard single read response
            reorder_buffer->storeResponse(req_id, resp->data);
        }
    }
    // req will be deleted by the caller in SATSolver::handleGlobalMemEvent
}
