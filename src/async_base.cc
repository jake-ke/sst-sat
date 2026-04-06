#include <sst/core/sst_config.h>
#include "async_base.h"

AsyncBase::AsyncBase(const std::string& prefix, int verbose, SST::Interfaces::StandardMem* mem, 
                     coro_t::push_type** yield_ptr)
    : memory(mem), yield_ptr(yield_ptr), pre_yield_callback(nullptr), line_size(64), size_(0) {
    output.init(prefix.c_str(), verbose, 0, SST::Output::STDOUT);
}

void AsyncBase::read(uint64_t addr, size_t size, uint64_t worker_id) {
    if (WRITE_BUFFER) {
        // First check store queue for forwarding
        std::vector<uint8_t> forwarded_data;
        if (findStoreQueueData(addr, size, forwarded_data)) {
            output.verbose(CALL_INFO, 7, 0, "Read at 0x%lx, size %zu, forwarded from SQ\n", addr, size);
            reorder_buffer->storeDataByWorkerId(worker_id, forwarded_data);
            return;
        }
    }

    // Not found in store queue, create memory request
    auto req = new SST::Interfaces::StandardMem::Read(addr, size);
    reorder_buffer->registerRequest(req->getID(), worker_id);
    memory->send(req);
    output.verbose(CALL_INFO, 8, 0, "Read at 0x%lx, size %zu, worker %lu, req %lu\n", 
                   addr, size, worker_id, req->getID());
    doYield();
}

void AsyncBase::write(uint64_t addr, size_t size, const std::vector<uint8_t>& data) {
    output.verbose(CALL_INFO, 8, 0, "Write at 0x%lx, size %zu\n", addr, size);
    
    if (WRITE_BUFFER) {
        // Always add a new entry to the store queue
        StoreQueueEntry entry(addr, size, data);
        store_queue.push_back(entry);
        output.verbose(CALL_INFO, 7, 0, 
            "SQ[%zu]: [0x%lx-0x%lx], size %zu\n",
            store_queue.size() - 1, addr, addr + size - 1, size);
    }

    // Send to memory
    memory->send(new SST::Interfaces::StandardMem::Write(addr, size, data));
    // doYield();
}

void AsyncBase::writeUntimed(uint64_t addr, size_t size, const std::vector<uint8_t>& data) {
    output.verbose(CALL_INFO, 8, 0, "Untimed write at 0x%lx, size %zu\n", addr, size);
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        addr, size, data, true, 0x1)); // posted, not cacheable
}

// Find store queue data for the given read range [addr, addr+size).
// Single-pass newest→oldest: newer entries write into out_data first,
// older entries only fill bytes not yet written. Newest data always wins.
bool AsyncBase::findStoreQueueData(uint64_t addr, size_t size, std::vector<uint8_t>& out_data) {
    uint64_t read_end = addr + size;  // exclusive
    out_data.resize(size);
    size_t bytes_filled = 0;
    std::vector<bool> written(size, false);

    for (int i = store_queue.size() - 1; i >= 0; i--) {
        uint64_t sq_start = store_queue[i].addr;
        uint64_t sq_end   = sq_start + store_queue[i].size;

        // Compute overlap with [addr, read_end)
        uint64_t ov_start = std::max(addr, sq_start);
        uint64_t ov_end   = std::min(read_end, sq_end);
        if (ov_start >= ov_end) continue;

        // Copy only bytes not yet written by a newer entry
        for (uint64_t b = ov_start; b < ov_end; b++) {
            size_t dst = b - addr;
            if (!written[dst]) {
                out_data[dst] = store_queue[i].data[b - sq_start];
                written[dst] = true;
                bytes_filled++;
            }
        }

        // If every byte in the read range is filled, we're done
        if (bytes_filled == size) {
            output.verbose(CALL_INFO, 7, 0,
                "SQ match: read [0x%lx-0x%lx] fully forwarded\n",
                addr, read_end - 1);
            return true;
        }
    }
    return false;
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

void AsyncBase::readBurst(uint64_t start_addr, size_t total_size, uint64_t worker_id) {
    auto chunks = calculateCacheChunks(start_addr, total_size);
    
    // Create/update burst state for this worker
    BurstReadState& worker_state = burst_states[worker_id];
    worker_state.start_addr = start_addr;
    worker_state.pending_read_count = chunks.size();
    worker_state.completed = false;
    
    // Initialize buffer for the complete burst read
    reorder_buffer->startBurst(worker_id, total_size);

    for (const auto& chunk : chunks) {
        output.verbose(CALL_INFO, 8, 0, 
            "ReadBurst chunk: addr=0x%lx, size=%zu, offset=%zu, worker=%lu\n", 
            chunk.addr, chunk.size, chunk.offset_in_data, worker_id);

        if (WRITE_BUFFER) {
            // Check store queue for forwarding
            std::vector<uint8_t> forwarded_data;
            if (findStoreQueueData(chunk.addr, chunk.size, forwarded_data)) {
                reorder_buffer->storeDataByWorkerId(worker_id, forwarded_data, true, chunk.offset_in_data);
                if (--worker_state.pending_read_count == 0) worker_state.completed = true;
                continue;
            }
        }

        // Not found in store queue, create memory request
        auto req = new SST::Interfaces::StandardMem::Read(chunk.addr, chunk.size);
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
    
    output.verbose(CALL_INFO, 8, 0, "ReadBurst: All %zu read requests completed for worker %lu\n", 
                   chunks.size(), worker_id);
}

void AsyncBase::writeBurst(uint64_t start_addr, const std::vector<uint8_t>& data) {
    auto chunks = calculateCacheChunks(start_addr, data.size());
    
    for (const auto& chunk : chunks) {
        output.verbose(CALL_INFO, 8, 0, 
            "WriteBurst chunk: addr=0x%lx, size=%zu, offset=%zu\n", 
            chunk.addr, chunk.size, chunk.offset_in_data);
        
        std::vector<uint8_t> chunk_data(chunk.size);
        memcpy(chunk_data.data(), data.data() + chunk.offset_in_data, chunk.size);
        
        write(chunk.addr, chunk.size, chunk_data);
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
            uint64_t offset_in_buffer = addr - state.start_addr - state.offset;
            reorder_buffer->storeResponse(req_id, resp->data, true, offset_in_buffer);
            
            if (--state.pending_read_count == 0) state.completed = true;
        } else {
            // Standard single read response
            reorder_buffer->storeResponse(req_id, resp->data);
        }
    } else if (auto* write_resp = dynamic_cast<SST::Interfaces::StandardMem::WriteResp*>(req)) {
        if (!WRITE_BUFFER) return;

        uint64_t addr = write_resp->pAddr;
        
        // Find and remove the oldest matching store queue entry by address (front of queue)
        for (auto it = store_queue.begin(); it != store_queue.end(); ++it) {
            if (it->addr == addr) {
                output.verbose(CALL_INFO, 7, 0, "SQ removing 0x%lx\n", it->addr);
                store_queue.erase(it);
                break;
            }
        }
    }
    // req will be deleted by the caller in SATSolver::handleGlobalMemEvent
}
