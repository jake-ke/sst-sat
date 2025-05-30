#ifndef ASYNC_VARIABLES_H
#define ASYNC_VARIABLES_H

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <cstring>
#include <functional>
#include "structs.h"

class Variables {
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
            parent->read(var_idx);
            return parent->getLastRead();
        }
        
        // Assignment for writing a complete Variable
        VariableProxy& operator=(const Variable& var) {
            parent->write(var_idx, {var});
            return *this;
        }
        
        // Direct access to common fields
        int reason() {
            parent->read(var_idx);
            return parent->getLastRead().reason;
        }
        
        int level() {
            parent->read(var_idx);
            return parent->getLastRead().level;
        }
    };

    Variables(SST::Interfaces::StandardMem* mem, uint64_t var_base_addr, coro_t::push_type** yield_ptr = nullptr)
        : memory(mem), var_base_addr(var_base_addr), busy(false), yield_ptr(yield_ptr) {
        output.init("VAR-> ", 0, 7, SST::Output::STDOUT);
    }

    uint64_t varAddr(int var_idx) const { return var_base_addr + var_idx * sizeof(Variable); }
    Variable getLastRead() const { return last_read.empty() ? Variable() : last_read[0]; }
    std::vector<Variable> getLastReadVec() const { return last_read; }
    bool isBusy() const { return busy; }
    
    // Array-style access with yield - no longer needs yield parameter
    VariableProxy operator()(int idx) {
        return VariableProxy(this, idx);
    }
    
    void read(int var_idx, int count = 1) {
        output.verbose(CALL_INFO, 7, 0, "Read variable %d\n", var_idx);
        memory->send(new SST::Interfaces::StandardMem::Read(
            varAddr(var_idx), count * sizeof(Variable)));
        busy = true;
        (**yield_ptr)();
    }

    void write(int start_idx, const std::vector<Variable>& var_data) {
        int count = var_data.size();
        std::vector<uint8_t> data(count * sizeof(Variable));
        memcpy(data.data(), &var_data[0], count * sizeof(Variable));
        
        output.verbose(CALL_INFO, 7, 0, "Write variables[%d], count %d\n", start_idx, count);
        memory->send(new SST::Interfaces::StandardMem::Write(
            varAddr(start_idx), count * sizeof(Variable), data));
        busy = true;
        (**yield_ptr)();
    }

    void handleMem(SST::Interfaces::StandardMem::Request* req) {
        output.verbose(CALL_INFO, 7, 0, "handleMem\n");
        busy = false;
        if (auto* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
            size_t var_count = resp->data.size() / sizeof(Variable);
            last_read.resize(var_count);
            memcpy(&last_read[0], resp->data.data(), resp->data.size());
        }
    }
    
private:
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    uint64_t var_base_addr;
    std::vector<Variable> last_read;
    bool busy;
    coro_t::push_type** yield_ptr; // Store pointer to the yield_ptr in SATSolver
};

#endif // ASYNC_VARIABLES_H
