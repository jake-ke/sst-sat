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
        VariableProxy(Variables* p, int idx) 
            : parent(p), var_idx(idx) {}
        
        // Implicit conversion to Variable for reading
        operator Variable() {
            parent->readVar(var_idx);
            return parent->getLastRead();
        }
        
        // Assignment for writing a complete Variable
        VariableProxy& operator=(const Variable& var) {
            parent->writeVar(var_idx, {var});
            return *this;
        }
        
        // Direct access to common fields
        int reason() {
            parent->readVar(var_idx);
            return parent->getLastRead().reason;
        }
        
        int level() {
            parent->readVar(var_idx);
            return parent->getLastRead().level;
        }
    };

    Variables(int verbose, SST::Interfaces::StandardMem* mem, uint64_t var_base_addr, 
        coro_t::push_type** yield_ptr = nullptr)
        : AsyncBase("VAR-> ", verbose, mem, yield_ptr), var_base_addr(var_base_addr) {
        output.verbose(CALL_INFO, 1, 0, "Variables base address: 0x%lx\n", var_base_addr);
    }

    uint64_t varAddr(int var_idx) const { return var_base_addr + var_idx * sizeof(Variable); }
    Variable getLastRead() const { return last_read[0]; }
    
    // Array-style access
    VariableProxy operator[](int idx) {
        return VariableProxy(this, idx);
    }
    
    void readVar(int var_idx, int count = 1) {
        output.verbose(CALL_INFO, 7, 0, "Read variable %d\n", var_idx);
        read(varAddr(var_idx), count * sizeof(Variable));

        last_read.resize(count);
        memcpy(&last_read[0], read_buffer.data(), count * sizeof(Variable));
    }

    void writeVar(int start_idx, const std::vector<Variable>& var_data) {
        int count = var_data.size();
        std::vector<uint8_t> data(count * sizeof(Variable));
        memcpy(data.data(), &var_data[0], count * sizeof(Variable));
        
        output.verbose(CALL_INFO, 7, 0, "Write variables[%d], count %d\n", start_idx, count);
        write(varAddr(start_idx), count * sizeof(Variable), data);
    }

    void init(int num_vars) {
        output.verbose(CALL_INFO, 7, 0, "Initializing %d variables\n", num_vars);
        std::vector<uint8_t> init_data((num_vars + 1) * sizeof(Variable), 0);
        writeUntimed(var_base_addr, init_data.size(), init_data);
    }
    
private:
    uint64_t var_base_addr;
    std::vector<Variable> last_read;
};

#endif // ASYNC_VARIABLES_H
