#include <sst/core/sst_config.h>
#include "async_clauses.h"
#include <cstring>
#include <algorithm>


AsyncClauses::AsyncClauses(int verbose, SST::Interfaces::StandardMem* mem, 
                          uint64_t clauses_base_addr, 
                          coro_t::push_type** yield_ptr)
    : memory(mem), clauses_base_addr(clauses_base_addr), 
      num_orig_clauses(0), busy(false), yield_ptr(yield_ptr), next_free_offset(0) {
    output.init("CLAUSES-> ", verbose, 0, SST::Output::STDOUT);
}

void AsyncClauses::initialize(const std::vector<Clause>& clauses, size_t num_original) {
    output.verbose(CALL_INFO, 7, 0, "Initializing %zu clauses (%zu original)\n", 
                  clauses.size(), num_original);
    
    // Save the number of original clauses
    num_orig_clauses = num_original;
    
    // Calculate memory needed for all clauses
    size_t total_memory = 0;
    clause_offsets.resize(clauses.size());
    
    for (size_t i = 0; i < clauses.size(); i++) {
        clause_offsets[i] = total_memory;
        // Memory for each clause: sizeof(size_t) for size + sizeof(Lit) * size for literals
        total_memory += sizeof(size_t) + clauses[i].literals.size() * sizeof(Lit);
    }
    
    // Update next available offset
    next_free_offset = total_memory;
    
    // Allocate and initialize buffer for all clauses
    std::vector<uint8_t> buffer(total_memory);
    size_t offset = 0;
    
    for (const auto& clause : clauses) {
        // Write size
        size_t size = clause.literals.size();
        memcpy(buffer.data() + offset, &size, sizeof(size_t));
        offset += sizeof(size_t);
        
        // Write literals
        memcpy(buffer.data() + offset, clause.literals.data(), size * sizeof(Lit));
        offset += size * sizeof(Lit);
    }
    
    // Write to memory in one operation
    memory->send(new SST::Interfaces::StandardMem::Write(
        clauses_base_addr, buffer.size(), buffer));
    busy = true;
    (**yield_ptr)();
    
    output.verbose(CALL_INFO, 7, 0, "Clauses initialized, total memory: %zu bytes\n", total_memory);
}

void AsyncClauses::addClause(const Clause& clause) {
    // Use the next available offset
    size_t offset = next_free_offset;
    clause_offsets.push_back(offset);
    
    // Calculate memory for this clause
    size_t size = clause.literals.size();
    size_t clause_memory = sizeof(size_t) + size * sizeof(Lit);
    
    // Update next available offset
    next_free_offset += clause_memory;
    
    // Create buffer for the clause
    std::vector<uint8_t> buffer(clause_memory);
    memcpy(buffer.data(), &size, sizeof(size_t));
    memcpy(buffer.data() + sizeof(size_t), clause.literals.data(), size * sizeof(Lit));
    
    // Send to memory
    memory->send(new SST::Interfaces::StandardMem::Write(
        clauses_base_addr + offset, buffer.size(), buffer));
    busy = true;
    (**yield_ptr)();
    
    output.verbose(CALL_INFO, 7, 0, 
                  "Added clause with %zu literals at offset %zu, next offset: %zu\n", 
                  size, offset, next_free_offset);
}

void AsyncClauses::reduceDB(const std::vector<bool>& to_remove) {
    output.verbose(CALL_INFO, 7, 0, "Starting DB reduction\n");
    
    // Read all learnt clauses in one operation
    uint64_t learnt_start_offset = clause_offsets[num_orig_clauses];
    uint64_t learnt_size = next_free_offset - learnt_start_offset;
    memory->send(new SST::Interfaces::StandardMem::Read(
        clauses_base_addr + learnt_start_offset, learnt_size));
    busy = true;
    (**yield_ptr)();
    
    // Count kept clauses and prepare new buffer
    size_t kept_clauses = num_orig_clauses;
    for (size_t i = num_orig_clauses; i < clause_offsets.size(); i++) {
        if (!to_remove[i]) kept_clauses++;
    }
    
    output.verbose(CALL_INFO, 7, 0, "DB reduction: %zu -> %zu clauses\n", 
                  clause_offsets.size(), kept_clauses);
    
    // Keep original clause offsets and prepare buffer for kept learnt clauses
    std::vector<uint8_t> new_buffer;
    std::vector<size_t> new_offsets(clause_offsets.begin(), clause_offsets.begin() + num_orig_clauses);
    
    // Process and compact learnt clauses from buffer
    uint8_t* buffer_ptr = last_buffer.data();
    for (size_t i = num_orig_clauses; i < clause_offsets.size(); i++) {
        if (i >= to_remove.size() || to_remove[i]) continue; // Skip removed clauses
        
        // Read clause data
        size_t rel_pos = clause_offsets[i] - learnt_start_offset;
        size_t size;
        memcpy(&size, buffer_ptr + rel_pos, sizeof(size_t));
        size_t clause_memory = sizeof(size_t) + size * sizeof(Lit);
        
        // Store new offset and copy clause data
        new_offsets.push_back(learnt_start_offset + new_buffer.size());
        size_t old_size = new_buffer.size();
        new_buffer.resize(old_size + clause_memory);
        memcpy(new_buffer.data() + old_size, buffer_ptr + rel_pos, clause_memory);
    }
    
    // Write back compacted clauses if any
    if (!new_buffer.empty()) {
        memory->send(new SST::Interfaces::StandardMem::Write(
            clauses_base_addr + learnt_start_offset, new_buffer.size(), new_buffer));
        busy = true;
        (**yield_ptr)();
    }
    
    // Update state
    clause_offsets = new_offsets;
    next_free_offset = learnt_start_offset + new_buffer.size();
    
    output.verbose(CALL_INFO, 7, 0, "DB reduction complete\n");
}

void AsyncClauses::handleMem(SST::Interfaces::StandardMem::Request* req) {
    output.verbose(CALL_INFO, 8, 0, "handleMem for Clauses\n");
    busy = false;
    
    if (auto* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        uint64_t addr = resp->pAddr;
        
        // Store the data for bulk reads
        last_buffer = resp->data;
        
        size_t size;
        memcpy(&size, resp->data.data(), sizeof(size_t));
        last_read.literals.resize(size);
        memcpy(last_read.literals.data(), resp->data.data() + sizeof(size_t), size * sizeof(Lit));
    }
}

void AsyncClauses::readClause(int idx) const {
    if (idx >= clause_offsets.size()) {
        output.fatal(CALL_INFO, -1, "Invalid clause index: %d\n", idx);
    }
    
    // Calculate address of clause
    uint64_t addr = clauses_base_addr + clause_offsets[idx];
    
    // First, read the size
    memory->send(new SST::Interfaces::StandardMem::Read(addr, sizeof(size_t)));
    busy = true;
    (**yield_ptr)();
    
    // Now read the full clause - we need to know the size first
    size_t size = last_read.size();
    memory->send(new SST::Interfaces::StandardMem::Read(
        addr, sizeof(size_t) + size * sizeof(Lit)));
    busy = true;
    (**yield_ptr)();
}

void AsyncClauses::swapLiterals(int idx, size_t pos1, size_t pos2) {
    // Validate positions
    if (pos1 >= last_read.literals.size() || pos2 >= last_read.literals.size()) {
        output.fatal(CALL_INFO, -1, "Invalid positions for swapping literals: %zu and %zu in clause of size %zu\n", 
                 pos1, pos2, last_read.literals.size());
    }
    
    // Skip if the positions are the same
    if (pos1 == pos2) return;
    
    // Swap the literals
    std::swap(last_read.literals[pos1], last_read.literals[pos2]);
    
    // Calculate address of clause
    uint64_t addr = clauses_base_addr + clause_offsets[idx];
    
    // Create buffer for the literals part
    std::vector<uint8_t> buffer(last_read.literals.size() * sizeof(Lit));
    memcpy(buffer.data(), last_read.literals.data(), buffer.size());
    
    // Only write the literals part (not size)
    memory->send(new SST::Interfaces::StandardMem::Write(
        addr + sizeof(size_t), // Skip the size field
        buffer.size(),
        buffer
    ));
    busy = true;
    (**yield_ptr)();
    
    output.verbose(CALL_INFO, 7, 0, 
        "Swapped literals at positions %zu and %zu in clause %d\n", 
        pos1, pos2, idx);
}
