#ifndef ASYNC_VAR_ACTIVITY_H
#define ASYNC_VAR_ACTIVITY_H

#include "async_activity.h"

// Forward declaration of Heap to avoid circular dependency
class Heap;

class VarActivity : public Activity {
public:
    VarActivity(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr,
              uint64_t base_addr = 0, Heap* heap_parent = nullptr);
    
    void setHeapSinkPtr(coro_t::push_type** sink_ptr) { 
        yield_ptr = sink_ptr; // Just pass directly to base class's yield_ptr
    }

    void initialize(size_t count, double init_value = 0.0);
    
private:
    Heap* heap;  // Reference to parent Heap
};

#endif // ASYNC_VAR_ACTIVITY_H
