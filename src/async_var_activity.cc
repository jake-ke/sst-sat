#include "async_var_activity.h"
#include "async_heap.h"

VarActivity::VarActivity(int verbose, SST::Interfaces::StandardMem* mem, 
                         uint64_t base_addr, Heap* heap_parent)
    : Activity("VAR_ACT-> ", verbose, mem, base_addr), heap(heap_parent) {
    // Set the pre-yield callback to update heap state
    setPreYieldCallback([this]() {
        heap->state = Heap::WAIT;
    });
}

void VarActivity::initialize(size_t count, double init_value) {
    output.verbose(CALL_INFO, 1, 0, "Size: %zu var activities, %zu bytes\n",
                   count, count * sizeof(double));
    size_ = count;
    std::vector<double> values(count, init_value);
    std::vector<uint8_t> buffer(count * sizeof(double));
    memcpy(buffer.data(), values.data(), buffer.size());
    writeUntimed(calcAddr(0), buffer.size(), buffer);
}

