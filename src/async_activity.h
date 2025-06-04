#ifndef ASYNC_ACTIVITY_H
#define ASYNC_ACTIVITY_H

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <vector>
#include <cstring>
#include "structs.h"

using coro_t = boost::coroutines2::coroutine<void>;

class Activity {
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
            return parent->read(idx);
        }
        
        // Assignment operator for writing
        ActivityProxy& operator=(double value) {
            parent->write(idx, value);
            return *this;
        }
    };

    Activity(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr,
            uint64_t base_addr = 0, coro_t::push_type** yield_ptr = nullptr);
            
    // Array-style access - returns proxy for both read and write
    ActivityProxy operator[](size_t idx) { return ActivityProxy(this, idx); }
    
    // Accessors
    size_t size() const { return size_; }
    bool isBusy() const { return busy; }

    // Core operations
    virtual double read(size_t idx);
    virtual void write(size_t idx, double value);
    virtual std::vector<double> readBulk(size_t start, size_t count);
    virtual void writeBulk(size_t start, const std::vector<double>& values);
    virtual void handleMem(SST::Interfaces::StandardMem::Request* req);
    
    void initialize(size_t count, double init_value = 0.0);
    void push(double value);
    void rescaleAll(double factor);
    void reduceDB(const std::vector<bool>& to_remove, size_t num_orig);

protected:
    // Members accessible to derived classes
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    coro_t::push_type** yield_ptr;
    uint64_t base_addr;
    size_t size_;
    bool busy;
    double last_value;
    std::vector<uint8_t> last_buffer;
    
    // Helper function used by derived classes
    uint64_t calcAddr(size_t idx) const { return base_addr + idx * sizeof(double); }
    
private:
};

#endif // ASYNC_ACTIVITY_H
