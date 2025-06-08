#ifndef ASYNC_BASE_H
#define ASYNC_BASE_H

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <vector>
#include <cstring>
#include <functional>
#include "structs.h"

using coro_t = boost::coroutines2::coroutine<void>;

class AsyncBase {
public:
    // Function type for pre-yield callbacks
    using PreYieldCallback = std::function<void()>;
    
    AsyncBase(const std::string& prefix, int verbose, SST::Interfaces::StandardMem* mem, 
              coro_t::push_type** yield_ptr = nullptr);
    virtual ~AsyncBase() = default;

    // Core memory operations
    void read(uint64_t addr, size_t size);
    void write(uint64_t addr, size_t size, const std::vector<uint8_t>& data);
    void writeUntimed(uint64_t addr, size_t size, const std::vector<uint8_t>& data);
    
    // Cache-aware burst operations
    void readBurst(uint64_t start_addr, size_t element_size, size_t count);
    void writeBurst(uint64_t start_addr, size_t element_size, const std::vector<uint8_t>& data);
    
    // Memory response handling
    virtual void handleMem(SST::Interfaces::StandardMem::Request* req);
    
    // Configuration
    void setLineSize(size_t size) { line_size = size; }
    void setPreYieldCallback(PreYieldCallback cb) { pre_yield_callback = cb; }
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
    
    // Member variables
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    coro_t::push_type** yield_ptr;
    PreYieldCallback pre_yield_callback;
    size_t line_size;
    std::vector<uint8_t> read_buffer;  // Stores the last read data
    std::vector<uint8_t> burst_buffer;  // Stores accumulated data for burst operations
    size_t size_;
    
    // For bulk read operations
    uint64_t burst_start_addr;     // Starting address of current burst read
    size_t pending_read_count;     // Counter for pending read responses
    bool all_reads_completed;      // Flag to indicate completion
    bool in_burst_read;            // Flag to indicate if currently in burst read mode
};

#endif // ASYNC_BASE_H
