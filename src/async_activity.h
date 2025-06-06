#ifndef ASYNC_ACTIVITY_H
#define ASYNC_ACTIVITY_H

#include "async_base.h"

class Activity : public AsyncBase {
public:
    // Proxy class for activity value access
    class ActivityProxy {
    private:
        Activity* parent;
        size_t idx;
    public:
        ActivityProxy(Activity* p, size_t i) : parent(p), idx(i) {}
        
        // Implicit conversion to double for reading
        operator double() {
            parent->read(parent->calcAddr(idx), sizeof(double));
            
            memcpy(&parent->last_read, parent->read_buffer.data(), sizeof(double));
            return parent->last_read;
        }
        
        // Assignment operator for writing
        ActivityProxy& operator=(double value) {
            std::vector<uint8_t> buffer(sizeof(double));
            memcpy(buffer.data(), &value, sizeof(double));
            parent->write(parent->calcAddr(idx), sizeof(double), buffer);
            return *this;
        }
    };

    Activity(const std::string& prefix = "ACT->", int verbose = 0,
             SST::Interfaces::StandardMem* mem = nullptr,
             uint64_t base_addr = 0, coro_t::push_type** yield_ptr = nullptr)
        : AsyncBase(prefix.c_str(), verbose, mem, yield_ptr), base_addr(base_addr) {
        output.verbose(CALL_INFO, 1, 0, "base address: 0x%lx\n", base_addr);
    }
            
    // Array-style access - returns proxy for both read and write
    ActivityProxy operator[](size_t idx) { return ActivityProxy(this, idx); }
    
    std::vector<double> readBurstAct(size_t start, size_t count) {
        if (start + count > size_) {
            output.fatal(CALL_INFO, -1, "Activity read out of range: %zu + %zu > %zu\n", 
                         start, count, size_);
        }
        std::vector<double> result(count);
        readBurst(calcAddr(start), sizeof(double), count);
        
        memcpy(result.data(), burst_buffer.data(), count * sizeof(double));
        return result;
    }

    void push(double value);
    void rescaleAll(double factor);
    void reduceDB(const std::vector<double>& activities, const std::vector<bool>& to_remove);

protected:
    uint64_t base_addr;
    double last_read;
    
    // Helper function used by derived classes
    uint64_t calcAddr(size_t idx) const { return base_addr + idx * sizeof(double); }
};

#endif // ASYNC_ACTIVITY_H
