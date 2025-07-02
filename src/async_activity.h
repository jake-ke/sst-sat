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
            
    // Array-style access - returns proxy for write
    ActivityProxy operator[](size_t idx) { return ActivityProxy(this, idx); }

    double readAct(uint64_t addr, int worker_id = 0);
    std::vector<double> readBurstAct(size_t start, size_t count, int worker_id = 0);
    void push(double value);
    void rescaleAll(double factor, int worker_id = 0);
    void reduceDB(const std::vector<double>& activities, const std::vector<bool>& to_remove);

protected:
    uint64_t base_addr;
    
    // Helper function used by derived classes
    uint64_t calcAddr(size_t idx) const { return base_addr + idx * sizeof(double); }
};

#endif // ASYNC_ACTIVITY_H
