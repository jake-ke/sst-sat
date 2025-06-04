#include <sst/core/sst_config.h>
#include "async_clauses.h"
#include <cstring>
#include <algorithm>

Clauses::Clauses(int verbose, SST::Interfaces::StandardMem* mem, 
                 uint64_t clauses_cmd_base_addr, uint64_t clauses_base_addr, 
                 coro_t::push_type** yield_ptr)
    : memory(mem), yield_ptr(yield_ptr), clauses_cmd_base_addr(clauses_cmd_base_addr),
      clauses_base_addr(clauses_base_addr), offsets_length(0),
      num_orig_clauses(0), busy(false), next_free_offset(0) {
    output.init("CLAUSES-> ", verbose, 0, SST::Output::STDOUT);
}

void Clauses::initialize(const std::vector<Clause>& clauses) {
    output.verbose(CALL_INFO, 7, 0, "Initializing %zu clauses\n", clauses.size());
    num_orig_clauses = clauses.size();
    offsets_length = clauses.size();
    
    // Calculate memory needed for all clauses
    size_t total_memory = 0;
    std::vector<ClauseMetaData> metadata_array(clauses.size());
    
    for (size_t i = 0; i < clauses.size(); i++) {
        metadata_array[i].offset = total_memory;
        metadata_array[i].size = clauses[i].size();
        total_memory += clauses[i].size() * sizeof(Lit);
    }
    
    next_free_offset = total_memory;  // Update next available offset
    
    // Write all metadata in one large batch
    std::vector<uint8_t> metadata_buffer(clauses.size() * sizeof(ClauseMetaData));
    memcpy(metadata_buffer.data(), metadata_array.data(), metadata_buffer.size());
    memory->send(new SST::Interfaces::StandardMem::Write(
        clauses_cmd_base_addr, metadata_buffer.size(), metadata_buffer));
    busy = true;
    (**yield_ptr)();
    
    // Allocate and initialize buffer for all clause literals
    std::vector<uint8_t> literals_buffer(total_memory);
    size_t offset = 0;
    
    for (const auto& clause : clauses) {
        // Write literals directly (no size field needed now)
        memcpy(literals_buffer.data() + offset, 
               clause.literals.data(), clause.size() * sizeof(Lit));
        offset += clause.size() * sizeof(Lit);
    }
    
    // Write to memory in one operation
    memory->send(new SST::Interfaces::StandardMem::Write(
        clauses_base_addr, literals_buffer.size(), literals_buffer));
    busy = true;
    (**yield_ptr)();
    
    output.verbose(CALL_INFO, 7, 0, "Clauses initialized, total memory: %zu bytes\n", total_memory);
}

void Clauses::addClause(const Clause& clause) {
    size_t offset = next_free_offset;  // Use the next available offset
    
    // Create and write new metadata
    ClauseMetaData metadata(offset, clause.size());
    writeMetaData(offsets_length, metadata);
    
    // Update length and next free offset
    offsets_length++;
    next_free_offset += clause.size() * sizeof(Lit);
    
    // Write literals to memory
    writeClause(offset, clause);
    
    output.verbose(CALL_INFO, 7, 0, 
                  "Added clause %ld with %u literals at offset %zu\n", 
                  offsets_length - 1, clause.size(), offset);
}

void Clauses::swapLiterals(int idx, size_t pos1, size_t pos2) {
    // Validate positions
    if (pos1 >= last_read.size() || pos2 >= last_read.size()) {
        output.fatal(CALL_INFO, -1, "Invalid positions for swapping literals: %zu and %zu in clause of size %u\n", 
                 pos1, pos2, last_read.size());
    } else if (pos1 == pos2) return;
    
    // Swap the literals
    std::swap(last_read.literals[pos1], last_read.literals[pos2]);
    
    // Create buffer for the literals
    std::vector<uint8_t> buffer(last_read.size() * sizeof(Lit));
    memcpy(buffer.data(), last_read.literals.data(), buffer.size());
    
    // Write the literals using the common function
    writeClause(last_metadata.offset, last_read);
    
    output.verbose(CALL_INFO, 7, 0, 
        "Swapped literals at positions %zu and %zu in clause %d\n", 
        pos1, pos2, idx);
}

void Clauses::reduceDB(const std::vector<bool>& rm) {
    size_t nl = offsets_length - num_orig_clauses;
    if (nl == 0) return;
    
    // Read all learned clause metadata
    memory->send(new SST::Interfaces::StandardMem::Read(
        cmdAddr(num_orig_clauses), nl * sizeof(ClauseMetaData)));
    busy = true; (**yield_ptr)();
    
    // Parse metadata and read all literals
    std::vector<ClauseMetaData> meta(nl);
    memcpy(meta.data(), last_buffer.data(), nl * sizeof(ClauseMetaData));
    
    uint64_t foff = meta[0].offset;
    size_t lsize = 0;
    for (auto& m : meta) lsize += m.size * sizeof(Lit);
    
    memory->send(new SST::Interfaces::StandardMem::Read(
        clauses_base_addr + foff, lsize));
    busy = true; (**yield_ptr)();
    
    // Compact clauses
    std::vector<ClauseMetaData> nmeta;
    std::vector<uint8_t> nbuf;
    size_t pos = 0, noff = foff;
    
    for (size_t i = 0; i < nl; i++) {
        size_t sz = meta[i].size * sizeof(Lit);
        
        if (!rm[i + num_orig_clauses]) {
            nmeta.push_back({noff, meta[i].size});
            size_t old = nbuf.size();
            nbuf.resize(old + sz);
            memcpy(nbuf.data() + old, last_buffer.data() + pos, sz);
            noff += sz;
        }
        pos += sz;
    }
    
    // Write compacted data if any clauses remain
    if (!nmeta.empty()) {
        // Write metadata
        std::vector<uint8_t> mbuf(nmeta.size() * sizeof(ClauseMetaData));
        memcpy(mbuf.data(), nmeta.data(), mbuf.size());
        memory->send(new SST::Interfaces::StandardMem::Write(
            cmdAddr(num_orig_clauses), mbuf.size(), mbuf));
        busy = true; (**yield_ptr)();
        
        // Write literals
        memory->send(new SST::Interfaces::StandardMem::Write(
            clauses_base_addr + foff, nbuf.size(), nbuf));
        busy = true; (**yield_ptr)();
    }
    
    // Update state and report stats
    offsets_length = num_orig_clauses + nmeta.size();
    next_free_offset = foff + nbuf.size();
    output.verbose(CALL_INFO, 7, 0, "DB reduction: %zu → %zu\n", 
                  num_orig_clauses + nl, offsets_length);
}

void Clauses::handleMem(SST::Interfaces::StandardMem::Request* req) {
    output.verbose(CALL_INFO, 8, 0, "handleMem for Clauses\n");
    busy = false;
    
    if (auto* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        uint64_t addr = resp->pAddr;
        last_buffer = resp->data;
    }
}

