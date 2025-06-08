#include <sst/core/sst_config.h>
#include "async_clauses.h"
#include <cstring>
#include <algorithm>

Clauses::Clauses(int verbose, SST::Interfaces::StandardMem* mem, 
                 uint64_t clauses_cmd_base_addr, uint64_t clauses_base_addr, 
                 coro_t::push_type** yield_ptr)
    : AsyncBase("CLAUSES-> ", verbose, mem, yield_ptr),
      clauses_cmd_base_addr(clauses_cmd_base_addr),
      clauses_base_addr(clauses_base_addr),
      num_orig_clauses(0), next_free_offset(0) {
    output.verbose(CALL_INFO, 1, 0, "base addresses: "
        "cmd=0x%lx, data=0x%lx\n", clauses_cmd_base_addr, clauses_base_addr);
}

void Clauses::getMetaData(int idx) {
    if (idx < 0 || idx >= size_) {
        output.fatal(CALL_INFO, -1, "Invalid clause index: %d\n", idx);
    }
    read(cmdAddr(idx), sizeof(ClauseMetaData));
    
    // Copy metadata from read buffer
    memcpy(&last_metadata, read_buffer.data(), sizeof(ClauseMetaData));
}

void Clauses::writeMetaData(int idx, const ClauseMetaData& metadata) {
    if (idx < 0 || idx >= size_ + 1) { // Allow writing at size_ for adding new clauses
        output.fatal(CALL_INFO, -1, "Invalid clause index for metadata write: %d\n", idx);
    }
    std::vector<uint8_t> buffer(sizeof(ClauseMetaData));
    memcpy(buffer.data(), &metadata, sizeof(ClauseMetaData));
    write(cmdAddr(idx), sizeof(ClauseMetaData), buffer);
}

void Clauses::readClause(int idx) {
    if (idx < 0 || idx >= size_) {
        output.fatal(CALL_INFO, -1, "Invalid clause index: %d\n", idx);
    }
    getMetaData(idx);  // First, read metadata
    
    // Then read all actual clause literals
    size_t totalBytes = last_metadata.size * sizeof(Lit);
    uint64_t literalsAddr = clauses_base_addr + last_metadata.offset;
    readBurst(literalsAddr, sizeof(Lit), last_metadata.size);
    
    // Copy data to the clause literals
    last_read.literals.clear();
    last_read.literals.resize(last_metadata.size);
    memcpy(last_read.literals.data(), burst_buffer.data(), totalBytes);
}

void Clauses::writeClause(uint64_t offset, const Clause& c) {
    size_t totalBytes = c.size() * sizeof(Lit);
    uint64_t literalsAddr = clauses_base_addr + offset;
    std::vector<uint8_t> buffer(totalBytes);
    memcpy(buffer.data(), c.literals.data(), buffer.size());
    writeBurst(literalsAddr, sizeof(Lit), buffer);
}

void Clauses::initialize(const std::vector<Clause>& clauses) {
    num_orig_clauses = clauses.size();
    size_ = clauses.size();
    output.verbose(CALL_INFO, 1, 0, "Size: %zu clause metadata, %ld bytes\n",
                   size_, size_ * sizeof(ClauseMetaData));
    
    // Calculate memory needed for all clauses
    size_t total_memory = 0;
    std::vector<ClauseMetaData> metadata_array(clauses.size());
    
    for (size_t i = 0; i < clauses.size(); i++) {
        metadata_array[i].offset = total_memory;
        metadata_array[i].size = clauses[i].size();
        total_memory += clauses[i].size() * sizeof(Lit);
    }
    
    next_free_offset = total_memory;  // Update next available offset
    output.verbose(CALL_INFO, 1, 0, "Size: %zu clause literals, %ld bytes\n",
                   total_memory / sizeof(Lit), total_memory);
    
    // Write all metadata in one large batch
    std::vector<uint8_t> metadata_buffer(clauses.size() * sizeof(ClauseMetaData));
    memcpy(metadata_buffer.data(), metadata_array.data(), metadata_buffer.size());
    writeUntimed(clauses_cmd_base_addr, metadata_buffer.size(), metadata_buffer);
    
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
    writeUntimed(clauses_base_addr, literals_buffer.size(), literals_buffer);
    
    output.verbose(CALL_INFO, 7, 0, "Clauses initialized, total memory: %zu bytes\n", total_memory);
}

void Clauses::addClause(const Clause& clause) {
    // Create and write new metadata
    size_t offset = next_free_offset;  // Use the next available offset
    ClauseMetaData metadata(offset, clause.size());
    writeMetaData(size_, metadata);
    
    // Update length and next free offset
    size_++;
    next_free_offset += clause.size() * sizeof(Lit);
    writeClause(offset, clause);  // Write literals to memory
    
    output.verbose(CALL_INFO, 7, 0, 
                   "Added clause %ld with %u literals at offset %zu\n", 
                   size_ - 1, clause.size(), offset);
}

void Clauses::swapLiterals(int idx, size_t pos1, size_t pos2) {
    // Validate positions
    if (pos1 >= last_read.size() || pos2 >= last_read.size()) {
        output.fatal(CALL_INFO, -1,
            "Invalid positions for swapping literals: %zu and %zu in clause of size %u\n", 
            pos1, pos2, last_read.size());
    } else if (pos1 == pos2) return;
    
    // Swap the literals and write back the clause
    std::swap(last_read.literals[pos1], last_read.literals[pos2]);
    writeClause(last_metadata.offset, last_read);
    
    output.verbose(CALL_INFO, 7, 0, 
        "Swapped literals at positions %zu and %zu in clause %d\n", 
        pos1, pos2, idx);
}

void Clauses::reduceDB(const std::vector<bool>& rm) {
    size_t nl = size_ - num_orig_clauses;
    output.verbose(CALL_INFO, 7, 0, "REDUCEDB: Starting with %zu learned clauses\n", nl);
    if (nl == 0) return;
    
    // Read all learned clause metadata
    std::vector<ClauseMetaData> meta(nl);
    readBurst(cmdAddr(num_orig_clauses), sizeof(ClauseMetaData), nl);
    memcpy(meta.data(), burst_buffer.data(), nl * sizeof(ClauseMetaData));

    // Find total literals size and starting offset
    uint64_t firstOffset = meta[0].offset;
    size_t totalLiteralBytes = 0;
    for (auto& m : meta) totalLiteralBytes += m.size * sizeof(Lit);
    
    // Read all literals
    readBurst(clauses_base_addr + firstOffset, sizeof(Lit), totalLiteralBytes / sizeof(Lit));
    std::vector<uint8_t> literalsBuffer = burst_buffer;
    
    // Compact clauses
    std::vector<ClauseMetaData> newMeta;
    std::vector<uint8_t> newLiteralsBuffer;
    size_t origPos = 0, newOffset = firstOffset;
    
    for (size_t i = 0; i < nl; i++) {
        size_t literalBytes = meta[i].size * sizeof(Lit);
        
        if (!rm[i + num_orig_clauses]) {
            newMeta.push_back({newOffset, meta[i].size});
            size_t oldSize = newLiteralsBuffer.size();
            newLiteralsBuffer.resize(oldSize + literalBytes);
            memcpy(newLiteralsBuffer.data() + oldSize, literalsBuffer.data() + origPos, literalBytes);
            newOffset += literalBytes;
        }
        origPos += literalBytes;
    }
    
    // Write compacted data if any clauses remain
    if (!newMeta.empty()) {
        // Prepare metadata buffer
        std::vector<uint8_t> metadataBuffer(newMeta.size() * sizeof(ClauseMetaData));
        memcpy(metadataBuffer.data(), newMeta.data(), metadataBuffer.size());
        
        // Write metadata using writeBurst
        writeBurst(cmdAddr(num_orig_clauses), sizeof(ClauseMetaData), metadataBuffer);
        
        // Write literals using writeBurst
        writeBurst(clauses_base_addr + firstOffset, sizeof(Lit), newLiteralsBuffer);
    }
    
    // Update state and report stats
    size_ = num_orig_clauses + newMeta.size();
    next_free_offset = firstOffset + newLiteralsBuffer.size();
    output.verbose(CALL_INFO, 7, 0, "DB reduction: %zu → %zu\n", 
                   num_orig_clauses + nl, size_);
}

