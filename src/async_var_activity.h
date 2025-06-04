#ifndef ASYNC_VAR_ACTIVITY_H
#define ASYNC_VAR_ACTIVITY_H

#include "async_activity.h"

// Forward declaration of Heap to avoid circular dependency
class Heap;

class VarActivity : public Activity {
public:
    VarActivity(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr,
              uint64_t base_addr = 0, Heap* heap_parent = nullptr);

    // Override core operations to use Heap's coroutine
    double read(size_t idx) override;
    void write(size_t idx, double value) override;
    std::vector<double> readBulk(size_t start, size_t count) override;
    void writeBulk(size_t start, const std::vector<double>& values) override;

    void setHeapSinkPtr(coro_t::push_type** sink_ptr) { heap_sink_ptr = sink_ptr; }
    
private:
    Heap* heap;                        // Reference to parent Heap
    coro_t::push_type** heap_sink_ptr; // Pointer to heap_sink_ptr in Heap
};

#endif // ASYNC_VAR_ACTIVITY_H
