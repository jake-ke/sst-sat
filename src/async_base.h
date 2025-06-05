#ifndef ASYNC_BASE_H
#define ASYNC_BASE_H

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <vector>
#include <cstring>
#include "structs.h"

using coro_t = boost::coroutines2::coroutine<void>;

class AsyncBase {
public:
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
    
    // Memory response handling (should delete request in implementation)
    virtual void handleMem(SST::Interfaces::StandardMem::Request* req);
    
    // Configuration
    void setLineSize(size_t size) { line_size = size; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

protected:
    // Cache line alignment helpers
    struct CacheChunk {
        uint64_t addr;
        size_t offset_in_data;
        size_t size;
    };
    std::vector<CacheChunk> calculateCacheChunks(uint64_t start_addr, size_t total_size);
    
    // Common functionality to calculate element address
    uint64_t calcAddr(size_t idx, size_t elem_size) const { 
        return base_addr + idx * elem_size; 
    }
    
    // Member variables
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    coro_t::push_type** yield_ptr;
    size_t line_size;
    std::vector<uint8_t> read_buffer;  // Stores the last read data
    std::vector<uint8_t> burst_buffer;  // Stores accumulated data for burst operations
    uint64_t base_addr;
    size_t size_;
};

#endif // ASYNC_BASE_H
