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

// update the pointer to clause literals at clause index
void Clauses::writeAddr(uint32_t idx, const Cref& addr) {
    if (idx >= size_ + 1) { // Allow writing at size_ for adding new clauses
        output.fatal(CALL_INFO, -1, "Invalid clause index for metadata write: %d\n", idx);
    }
    std::vector<uint8_t> buffer(sizeof(Cref));
    memcpy(buffer.data(), &addr, sizeof(Cref));
    write(cmdAddr(idx), sizeof(Cref), buffer);
}

// get the number of literals in a clause from clause address
uint32_t Clauses::getClauseSize(Cref addr, int worker_id) {
    read(clauseAddr(addr), sizeof(uint32_t), worker_id);

    uint32_t size;
    memcpy(&size, reorder_buffer->getResponse(worker_id).data(), sizeof(uint32_t));
    return size;
}

// read the clause literals from clause address
Clause Clauses::readClause(Cref addr, int worker_id) {
    uint32_t num_lits = getClauseSize(addr, worker_id);
    
    // Read the rest of clause data (activity + literals)
    readBurst(clauseAddr(addr + sizeof(uint32_t)), CLAUSE_MEMBER_SIZE * (num_lits + 1), worker_id);

    const uint8_t* data = reorder_buffer->getResponse(worker_id).data();
    
    Clause c(num_lits);
    memcpy(&c.activity, data, sizeof(float));  // Read activity first
    memcpy(c.literals.data(), data + sizeof(float), num_lits * sizeof(Lit));
    return c;
}

void Clauses::writeClause(Cref addr, const Clause& c) {
    std::vector<uint8_t> buffer(c.size());
    memcpy(buffer.data(), &c, CLAUSE_MEMBER_SIZE * 2); // num_lits and activity
    memcpy(buffer.data() + CLAUSE_MEMBER_SIZE * 2, c.literals.data(), 
           c.litSize() * sizeof(Lit)); // literals
    writeBurst(clauseAddr(addr), buffer);
}

void Clauses::initialize(const std::vector<Clause>& clauses) {
    num_orig_clauses = clauses.size();
    size_ = clauses.size();
    output.verbose(CALL_INFO, 1, 0, "Size: %zu clause pointers, %ld bytes\n",
                   size_, size_ * sizeof(Cref));

    // find the total memory required for clauses
    size_t total_memory = 0;
    std::vector<Cref> addr_array(clauses.size());
    
    for (size_t i = 0; i < clauses.size(); i++) {
        addr_array[i] = total_memory;
        total_memory += clauses[i].size();
    }

    // write all clause pointers in one operation
    std::vector<uint8_t> addr_buffer(clauses.size() * sizeof(Cref));
    memcpy(addr_buffer.data(), addr_array.data(), addr_buffer.size());
    writeUntimed(clauses_cmd_base_addr, addr_buffer.size(), addr_buffer);
    
    // Allocate and initialize buffer for all clause data
    std::vector<uint8_t> literals_buffer(total_memory);
    size_t offset = 0;
    
    for (const auto& clause : clauses) {
        // num_lits and activity
        memcpy(literals_buffer.data() + offset, &clause, CLAUSE_MEMBER_SIZE * 2);
        // literals
        memcpy(literals_buffer.data() + offset + CLAUSE_MEMBER_SIZE * 2,
               clause.literals.data(), clause.litSize() * sizeof(Lit));
        offset += clause.size();
    }

    // Write all clause data to memory in one operation
    writeUntimed(clauses_base_addr, literals_buffer.size(), literals_buffer);

    next_free_offset = total_memory;  // Update next available offset
    learnt_offset = next_free_offset;  // original clauses are never deleted
    
    output.verbose(CALL_INFO, 1, 0, "Size: %zu clause structs, %ld bytes\n",
                   size_, total_memory);
}

Cref Clauses::addClause(const Clause& clause) {
    Cref offset = next_free_offset;  // Use the next available offset
    writeAddr(size_, offset);  // Write new metadata at index size_
    
    // Update length and next free offset
    size_++;
    next_free_offset += clause.size();
    writeClause(offset, clause);  // Write literals to memory
    
    output.verbose(CALL_INFO, 7, 0, 
                   "Added clause %ld with %u literals at offset %u\n", 
                   size_ - 1, clause.size(), offset);
    return offset;
}

void Clauses::writeAct(Cref addr, float act) {
    std::vector<uint8_t> buffer(sizeof(float));
    memcpy(buffer.data(), &act, sizeof(float));
    write(clauseAddr(addr + sizeof(uint32_t)), sizeof(float), buffer);
}

std::vector<Cref> Clauses::readAllAddr(int worker_id) {
    size_t nl = size_ - num_orig_clauses;  // Number of learnt clauses
    readBurst(cmdAddr(num_orig_clauses), sizeof(Cref) * nl, worker_id);
    
    const Cref* addr_ptr = reinterpret_cast<const Cref*>(reorder_buffer->getResponse(worker_id).data());
    std::vector<Cref> result(nl);
    memcpy(result.data(), addr_ptr, nl * sizeof(Cref));
    return result;
}

std::vector<float> Clauses::readAllAct(const std::vector<Cref>& addr, int worker_id) {
    size_t totalBytes = next_free_offset - learnt_offset;
    readBurst(clauseAddr(learnt_offset), totalBytes, worker_id);
    const uint8_t* data_ptr = reorder_buffer->getResponse(worker_id).data();

    std::vector<float> result(addr.size());
    for (size_t i = 0; i < result.size(); i++) {
        const float* act_ptr = reinterpret_cast<const float*>(
            data_ptr + (addr[i] - learnt_offset) + sizeof(uint32_t));
        result[i] = *act_ptr;
    }
    return result;
}

void Clauses::rescaleAllAct(float factor) {
    std::vector<Cref> addr = readAllAddr();
    std::vector<float> activities = readAllAct(addr);

    for (size_t i = 0; i < activities.size(); i++) {
        float act = activities[i] * factor;
        writeAct(addr[i], act);  // Write back the rescaled activity
    }
}

std::unordered_map<Cref, Cref> Clauses::reduceDB(const std::vector<Cref>& to_keep) {
    std::unordered_map<Cref, Cref> clause_map;

    // Compact clauses
    Cref newOffset = learnt_offset;
    std::vector<Cref> newAddr;
    std::vector<uint8_t> newClauseData;

    const uint8_t* data_ptr = reorder_buffer->getResponse(0).data();
    for (const Cref& addr : to_keep) {
        assert(addr >= learnt_offset);
        clause_map[addr] = newOffset;  // Map old address to new offset

        newAddr.push_back(newOffset);

        // read and copy the clause size
        uint32_t num_lits = getClauseSize(addr);
        newClauseData.insert(newClauseData.end(), data_ptr, data_ptr + sizeof(uint32_t));

        // read and copy activity and literals
        uint32_t read_size = sizeof(float) + num_lits * sizeof(Lit);
        readBurst(clauseAddr(addr + sizeof(uint32_t)), CLAUSE_MEMBER_SIZE * (num_lits + 1));
        newClauseData.insert(newClauseData.end(), data_ptr, data_ptr + read_size);

        newOffset += read_size + sizeof(uint32_t);
    }

    std::vector<uint8_t> addr_buffer(to_keep.size() * sizeof(Cref));
    memcpy(addr_buffer.data(), newAddr.data(), addr_buffer.size());
    writeBurst(cmdAddr(num_orig_clauses), addr_buffer);
    writeBurst(clauseAddr(learnt_offset), newClauseData);

    size_ = to_keep.size() + num_orig_clauses;  // Update size
    next_free_offset = newOffset;  // Update next free offset

    return clause_map;
}
