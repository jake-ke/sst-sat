#ifndef CLAUSES_H
#define CLAUSES_H

#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <boost/coroutine2/all.hpp>
#include <vector>
#include <string>
#include <algorithm>
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
    Clause& getLastRead() { return last_read; }
    void setLineSize(size_t size) { line_size = size; }
    
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
    size_t line_size;

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

        // Then read the actual clause literals, respecting cache line boundaries
        size_t litSize = sizeof(Lit);
        size_t totalBytes = last_metadata.size * litSize;
        uint64_t baseAddr = clauses_base_addr + last_metadata.offset;
        
        // Clear and resize the last_read clause
        last_read.literals.clear();
        last_read.literals.resize(last_metadata.size);
        
        // Read literals in chunks that don't cross cache lines
        size_t bytesRead = 0;
        size_t literalsRead = 0;
        
        while (bytesRead < totalBytes) {
            uint64_t currentAddr = baseAddr + bytesRead;
            uint64_t lineOffset = currentAddr % line_size;
            size_t bytesRemaining = totalBytes - bytesRead;
            size_t bytesInLine = line_size - lineOffset;
            size_t bytesToRead = std::min(bytesRemaining, bytesInLine);
            
            // Make sure we read complete literals
            bytesToRead = (bytesToRead / litSize) * litSize;
            if (bytesToRead == 0 && bytesRemaining > 0) {
                // Literal spans a cache line boundary
                bytesToRead = litSize;
            }
            
            if (bytesToRead == 0) break;
            
            // Perform the read
            memory->send(new SST::Interfaces::StandardMem::Read(currentAddr, bytesToRead));
            (**yield_ptr)();
            
            // Copy data to the clause
            size_t literalsInChunk = bytesToRead / litSize;
            memcpy(&last_read.literals[literalsRead], 
                   last_buffer.data(), 
                   literalsInChunk * litSize);
            
            bytesRead += bytesToRead;
            literalsRead += literalsInChunk;
        }
    }

    // New function to write clause literals to memory
    void writeClause(uint64_t offset, const Clause& c) {
        size_t litSize = sizeof(Lit);
        size_t totalBytes = c.size() * litSize;
        uint64_t baseAddr = clauses_base_addr + offset;
        
        // Write literals in chunks that don't cross cache lines
        size_t bytesWritten = 0;
        size_t literalsWritten = 0;
        
        while (bytesWritten < totalBytes) {
            uint64_t currentAddr = baseAddr + bytesWritten;
            uint64_t lineOffset = currentAddr % line_size;
            size_t bytesRemaining = totalBytes - bytesWritten;
            size_t bytesInLine = line_size - lineOffset;
            size_t bytesToWrite = std::min(bytesRemaining, bytesInLine);
            
            // Make sure we write complete literals
            bytesToWrite = (bytesToWrite / litSize) * litSize;
            if (bytesToWrite == 0 && bytesRemaining > 0) {
                // Literal spans a cache line boundary
                bytesToWrite = litSize;
            }
            
            if (bytesToWrite == 0) break;
            
            // Create buffer for this chunk
            std::vector<uint8_t> buffer(bytesToWrite);
            size_t literalsInChunk = bytesToWrite / litSize;
            memcpy(buffer.data(), 
                   &c.literals[literalsWritten], 
                   literalsInChunk * litSize);
            
            // Perform the write
            memory->send(new SST::Interfaces::StandardMem::Write(
                currentAddr, bytesToWrite, buffer));
            (**yield_ptr)();
            
            bytesWritten += bytesToWrite;
            literalsWritten += literalsInChunk;
        }
    }

    uint64_t cmdAddr(int idx) const {
        return clauses_cmd_base_addr + idx * sizeof(ClauseMetaData);
    }
    
    void getMetaData(int idx) {
        memory->send(new SST::Interfaces::StandardMem::Read(
            cmdAddr(idx), sizeof(ClauseMetaData)));
        (**yield_ptr)();
        
        memcpy(&last_metadata, last_buffer.data(), sizeof(ClauseMetaData));
    }

    void writeMetaData(int idx, const ClauseMetaData& metadata) {
        std::vector<uint8_t> buffer(sizeof(ClauseMetaData));
        memcpy(buffer.data(), &metadata, sizeof(ClauseMetaData));
        
        memory->send(new SST::Interfaces::StandardMem::Write(
            cmdAddr(idx), buffer.size(), buffer));
        (**yield_ptr)();
    }
    
};

#endif // CLAUSES_H
