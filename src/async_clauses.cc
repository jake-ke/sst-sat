#include <sst/core/sst_config.h>
#include "async_clauses.h"
#include <cstring>
#include <algorithm>

Clauses::Clauses(int verbose, SST::Interfaces::StandardMem* mem, 
                 uint64_t clauses_cmd_base_addr, uint64_t clauses_base_addr, 
                 coro_t::push_type** yield_ptr)
    : memory(mem), yield_ptr(yield_ptr), clauses_cmd_base_addr(clauses_cmd_base_addr),
      clauses_base_addr(clauses_base_addr), offsets_length(0),
      num_orig_clauses(0), next_free_offset(0) {
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
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        clauses_cmd_base_addr, metadata_buffer.size(), metadata_buffer,
        false, 0x1));  // not posted, and not cacheable
    
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
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        clauses_base_addr, literals_buffer.size(), literals_buffer,
        false, 0x1));  // not posted, and not cacheable
    
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
    
    // Read all learned clause metadata - need to respect cache line boundaries
    size_t meta_size = sizeof(ClauseMetaData);
    std::vector<ClauseMetaData> meta(nl);
    
    // Read metadata in chunks that respect cache line boundaries
    for (size_t i = 0; i < nl; i++) {
        uint64_t addr = cmdAddr(num_orig_clauses + i);
        uint64_t line_offset = addr % line_size;
        
        // Check if this metadata would cross a cache line
        if (line_offset + meta_size > line_size) {
            // This would cross a cache line boundary - read in two parts
            size_t first_part = line_size - line_offset;
            size_t second_part = meta_size - first_part;
            
            // Read first part
            memory->send(new SST::Interfaces::StandardMem::Read(addr, first_part));
            (**yield_ptr)();
            memcpy(reinterpret_cast<uint8_t*>(&meta[i]), last_buffer.data(), first_part);
            
            // Read second part
            memory->send(new SST::Interfaces::StandardMem::Read(addr + first_part, second_part));
            (**yield_ptr)();
            memcpy(reinterpret_cast<uint8_t*>(&meta[i]) + first_part, last_buffer.data(), second_part);
        } else {
            // Won't cross cache line - read normally
            memory->send(new SST::Interfaces::StandardMem::Read(addr, meta_size));
            (**yield_ptr)();
            memcpy(&meta[i], last_buffer.data(), meta_size);
        }
    }
    
    // Find total literals size
    uint64_t foff = meta[0].offset;
    size_t lsize = 0;
    for (auto& m : meta) lsize += m.size * sizeof(Lit);
    
    // Read all literals in chunks that respect cache lines
    size_t lit_size = sizeof(Lit);
    std::vector<uint8_t> literals_buffer(lsize);
    size_t bytes_read = 0;
    
    while (bytes_read < lsize) {
        uint64_t curr_addr = clauses_base_addr + foff + bytes_read;
        uint64_t line_offset = curr_addr % line_size;
        size_t bytes_remaining = lsize - bytes_read;
        size_t bytes_in_line = line_size - line_offset;
        size_t bytes_to_read = std::min(bytes_remaining, bytes_in_line);
        
        // Make sure we read complete literals
        bytes_to_read = (bytes_to_read / lit_size) * lit_size;
        if (bytes_to_read == 0 && bytes_remaining > 0) {
            // A literal spans a cache line boundary
            bytes_to_read = lit_size;
        }
        
        memory->send(new SST::Interfaces::StandardMem::Read(curr_addr, bytes_to_read));
        (**yield_ptr)();
        
        memcpy(literals_buffer.data() + bytes_read, last_buffer.data(), bytes_to_read);
        bytes_read += bytes_to_read;
    }
    
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
            memcpy(nbuf.data() + old, literals_buffer.data() + pos, sz);
            noff += sz;
        }
        pos += sz;
    }
    
    // Write compacted data if any clauses remain
    if (!nmeta.empty()) {
        // Write metadata with respect to cache lines
        for (size_t i = 0; i < nmeta.size(); i++) {
            uint64_t addr = cmdAddr(num_orig_clauses + i);
            uint64_t line_offset = addr % line_size;
            
            // Check if this write would cross a cache line
            if (line_offset + meta_size > line_size) {
                // This would cross a cache line boundary - write in two parts
                size_t first_part = line_size - line_offset;
                size_t second_part = meta_size - first_part;
                
                // Write first part
                std::vector<uint8_t> buffer1(first_part);
                memcpy(buffer1.data(), &nmeta[i], first_part);
                memory->send(new SST::Interfaces::StandardMem::Write(addr, first_part, buffer1));
                (**yield_ptr)();
                
                // Write second part
                std::vector<uint8_t> buffer2(second_part);
                memcpy(buffer2.data(), reinterpret_cast<const uint8_t*>(&nmeta[i]) + first_part, second_part);
                memory->send(new SST::Interfaces::StandardMem::Write(addr + first_part, second_part, buffer2));
                (**yield_ptr)();
            } else {
                // Won't cross cache line - write normally
                std::vector<uint8_t> buffer(meta_size);
                memcpy(buffer.data(), &nmeta[i], meta_size);
                memory->send(new SST::Interfaces::StandardMem::Write(addr, meta_size, buffer));
                (**yield_ptr)();
            }
        }
        
        // Write literals with respect to cache lines
        size_t bytes_written = 0;
        
        while (bytes_written < nbuf.size()) {
            uint64_t curr_addr = clauses_base_addr + foff + bytes_written;
            uint64_t line_offset = curr_addr % line_size;
            size_t bytes_remaining = nbuf.size() - bytes_written;
            size_t bytes_in_line = line_size - line_offset;
            size_t bytes_to_write = std::min(bytes_remaining, bytes_in_line);
            
            // Make sure we write complete literals
            bytes_to_write = (bytes_to_write / lit_size) * lit_size;
            if (bytes_to_write == 0 && bytes_remaining > 0) {
                // A literal spans a cache line boundary
                bytes_to_write = lit_size;
            }
            
            std::vector<uint8_t> buffer(bytes_to_write);
            memcpy(buffer.data(), nbuf.data() + bytes_written, bytes_to_write);
            
            memory->send(new SST::Interfaces::StandardMem::Write(curr_addr, bytes_to_write, buffer));
            (**yield_ptr)();
            
            bytes_written += bytes_to_write;
        }
    }
    
    // Update state and report stats
    offsets_length = num_orig_clauses + nmeta.size();
    next_free_offset = foff + nbuf.size();
    output.verbose(CALL_INFO, 7, 0, "DB reduction: %zu → %zu\n", 
                  num_orig_clauses + nl, offsets_length);
}

void Clauses::handleMem(SST::Interfaces::StandardMem::Request* req) {
    output.verbose(CALL_INFO, 8, 0, "handleMem for Clauses\n");
    
    if (auto* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        uint64_t addr = resp->pAddr;
        last_buffer = resp->data;
    }
}

