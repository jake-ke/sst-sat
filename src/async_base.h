#ifndef ASYNC_BASE_H
#define ASYNC_BASE_H

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <vector>
#include <cstring>
#include <functional>
#include <unordered_map>
#include "structs.h"
#include "reorder_buffer.h"


class AsyncBase {
public:
    // Function type for pre-yield callbacks
    using PreYieldCallback = std::function<void()>;
    
    AsyncBase(const std::string& prefix, int verbose, SST::Interfaces::StandardMem* mem, 
              coro_t::push_type** yield_ptr = nullptr);
    virtual ~AsyncBase() = default;

    // Core memory operations
    void read(uint64_t addr, size_t size, uint64_t worker_id = 0);
    void write(uint64_t addr, size_t size, const std::vector<uint8_t>& data);
    void writeUntimed(uint64_t addr, size_t size, const std::vector<uint8_t>& data);
    
    // Cache-aware burst operations
    void readBurst(uint64_t start_addr, size_t total_size, uint64_t worker_id = 0);
    void writeBurst(uint64_t start_addr, const std::vector<uint8_t>& data);
    void readBurst2D(uint64_t start_addr, uint64_t offset, size_t element_size, size_t count, uint64_t worker_id = 0);
    
    // Memory response handling
    virtual void handleMem(SST::Interfaces::StandardMem::Request* req);
    
    // Configuration
    void setLineSize(size_t size) { line_size = size; }
    void setPreYieldCallback(PreYieldCallback cb) { pre_yield_callback = cb; }
    void setReorderBuffer(ReorderBuffer* rb) { reorder_buffer = rb; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

protected:
    // Helper method to perform the yield operation
    void doYield() {
        if (pre_yield_callback) pre_yield_callback();
        (**yield_ptr)();
    }

    // Cache line alignment helpers
    struct CacheChunk {
        uint64_t addr;
        size_t offset_in_data;
        size_t size;
    };
    std::vector<CacheChunk> calculateCacheChunks(uint64_t start_addr, size_t total_size);
    
    // Per-worker burst read state tracking
    struct BurstReadState {
        uint64_t start_addr;        // Starting address of burst read
        uint64_t offset;            // Offset within the burst read
        size_t pending_read_count;  // Number of pending reads
        bool completed;             // Flag to indicate completion

        BurstReadState() : start_addr(0), offset(0), pending_read_count(0), completed(false) {}
    };
    
    // Member variables
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    coro_t::push_type** yield_ptr;
    PreYieldCallback pre_yield_callback;
    size_t line_size;
    size_t size_;
    
    // Worker-specific burst read state map
    std::unordered_map<uint64_t, BurstReadState> burst_states;
    
    // Reorder buffer for managing parallel memory requests
    ReorderBuffer* reorder_buffer;
};

#endif // ASYNC_BASE_H
