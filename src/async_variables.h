#ifndef ASYNC_VARIABLES_H
#define ASYNC_VARIABLES_H

#include "async_base.h"

class Variables : public AsyncBase {
public:
    // Proxy class for variable access
    class VariableProxy {
    private:
        Variables* parent;
        int var_idx;

    public:
        VariableProxy(Variables* p, int idx) : parent(p), var_idx(idx) {}

        // Assignment for writing a complete Variable
        VariableProxy& operator=(const Variable& var) {
            parent->writeVar(var_idx, {var});
            return *this;
        }
    };

    Variables(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr,
              uint64_t var_base_addr = 0, coro_t::push_type** yield_ptr = nullptr)
        : AsyncBase("VAR-> ", verbose, mem, yield_ptr), var_base_addr(var_base_addr) {
        output.verbose(CALL_INFO, 1, 0, "base address: 0x%lx\n", var_base_addr);
    }

    uint64_t varAddr(int var_idx) const { return var_base_addr + var_idx * sizeof(Variable); }
    
    // Array-style access for write
    VariableProxy operator[](int idx) { return VariableProxy(this, idx); }

    Variable readVar(int var_idx, int worker_id = 0) {
        output.verbose(CALL_INFO, 7, 0, "Read variable %d\n", var_idx);
        assert(var_idx >= 0 && var_idx < size_);
        read(varAddr(var_idx), sizeof(Variable), worker_id);

        Variable var;
        memcpy(&var, reorder_buffer->getResponse(worker_id).data(), sizeof(Variable));
        return var;
    }

    int getReason(int var_idx, int worker_id = 0) {
        return readVar(var_idx, worker_id).reason;
    }

    int getLevel(int var_idx, int worker_id = 0) {
        return readVar(var_idx, worker_id).level;
    }

    void writeVar(int start_idx, const std::vector<Variable>& var_data) {
        assert(start_idx >= 0 && start_idx + var_data.size() <= size_);
        int count = var_data.size();
        std::vector<uint8_t> data(count * sizeof(Variable));
        memcpy(data.data(), &var_data[0], count * sizeof(Variable));
        
        output.verbose(CALL_INFO, 7, 0, "Write variables[%d], count %d\n", start_idx, count);
        write(varAddr(start_idx), count * sizeof(Variable), data);
    }

    void init(int num_vars) {
        size_ = num_vars + 1;
        int total_bytes = (num_vars + 1) * sizeof(Variable);
        output.verbose(CALL_INFO, 1, 0, "Size: %d variables, %d bytes\n", num_vars, total_bytes);
        // unnecessary to initialize all variables to zero
    }
    
private:
    uint64_t var_base_addr;
};

#endif // ASYNC_VARIABLES_H
