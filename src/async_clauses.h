#ifndef CLAUSES_H
#define CLAUSES_H

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <vector>
#include <string>
#include "structs.h"

class Clauses {
public:
    // Proxy class for clause access
    class ClauseProxy {
    private:
        Clauses* parent;
        int clause_idx;

    public:
        ClauseProxy(Clauses* p, int idx) : parent(p), clause_idx(idx) {}
        
        // Implicit conversion to Clause reference for reading
        operator Clause&() {
            parent->readClause(clause_idx);
            return parent->getLastRead();
        }
        
        // const version for read-only access
        operator const Clause&() const {
            parent->readClause(clause_idx);
            return parent->getLastRead();
        }
        
        // Direct size operation
        size_t size() const { 
            parent->getMetaData(clause_idx);
            return parent->last_metadata.size;
        }
    };

    Clauses(int verbose = 0, SST::Interfaces::StandardMem* mem = nullptr, 
            uint64_t clauses_cmd_base_addr = 0, uint64_t clauses_base_addr = 0, 
            coro_t::push_type** yield_ptr = nullptr);

    // Array-style access
    ClauseProxy operator[](int idx) { return ClauseProxy(this, idx); }

    // Basic accessors
    size_t size() const { return offsets_length; } // Return cached size directly
    bool empty() const { return offsets_length == 0; }
    bool isBusy() const { return busy; }
    Clause& getLastRead() { return last_read; }
    
    // Core operations
    void initialize(const std::vector<Clause>& clauses);
    void addClause(const Clause& clause);
    void swapLiterals(int idx, size_t pos1, size_t pos2);
    void reduceDB(const std::vector<bool>& rm);
    void handleMem(SST::Interfaces::StandardMem::Request* req);

private:
    SST::Output output;
    SST::Interfaces::StandardMem* memory;
    coro_t::push_type** yield_ptr;
    uint64_t clauses_cmd_base_addr;
    uint64_t clauses_base_addr;

    bool busy;
    size_t num_orig_clauses;
    size_t offsets_length;              // Number of all clauses
    size_t next_free_offset;            // Next offset for insertion
    Clause last_read;                   // Last clause read from memory
    ClauseMetaData last_metadata;       // Metadata for the last read clause
    std::vector<uint8_t> last_buffer;   // Buffer for bulk reads

    void readClause(int idx) {
        if (idx < 0 || idx >= offsets_length) {
            output.fatal(CALL_INFO, -1, "Invalid clause index: %d\n", idx); }
        // First, read metadata (size and offset)
        getMetaData(idx);

        // Then read the actual clause literals
        memory->send(new SST::Interfaces::StandardMem::Read(
            clauses_base_addr + last_metadata.offset,
            last_metadata.size * sizeof(Lit)));
        busy = true;
        (**yield_ptr)();
        
        // Set up last_read clause from the buffer
        last_read.literals.resize(last_metadata.size);
        memcpy(last_read.literals.data(), last_buffer.data(), last_metadata.size * sizeof(Lit));
    }

    // New function to write clause literals to memory
    void writeClause(uint64_t offset, const Clause& c) {
        std::vector<uint8_t> buffer(c.size() * sizeof(Lit));
        memcpy(buffer.data(), c.literals.data(), buffer.size());
        
        memory->send(new SST::Interfaces::StandardMem::Write(
            clauses_base_addr + offset, buffer.size(), buffer));
        busy = true;
        (**yield_ptr)();
    }

    uint64_t cmdAddr(int idx) const {
        return clauses_cmd_base_addr + idx * sizeof(ClauseMetaData);
    }
    
    void getMetaData(int idx) {
        memory->send(new SST::Interfaces::StandardMem::Read(
            cmdAddr(idx), sizeof(ClauseMetaData)));
        busy = true;
        (**yield_ptr)();
        
        memcpy(&last_metadata, last_buffer.data(), sizeof(ClauseMetaData));
    }

    void writeMetaData(int idx, const ClauseMetaData& metadata) {
        std::vector<uint8_t> buffer(sizeof(ClauseMetaData));
        memcpy(buffer.data(), &metadata, sizeof(ClauseMetaData));
        
        memory->send(new SST::Interfaces::StandardMem::Write(
            cmdAddr(idx), buffer.size(), buffer));
        busy = true;
        (**yield_ptr)();
    }
    
};

#endif // CLAUSES_H
