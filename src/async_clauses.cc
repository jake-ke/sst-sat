#include <sst/core/sst_config.h>
#include "async_clauses.h"

Clauses::Clauses(int verbose, SST::Interfaces::StandardMem* mem, 
                 uint64_t clauses_cmd_base_addr, uint64_t clauses_base_addr, 
                 coro_t::push_type** yield_ptr)
    : AsyncBase("CLAUSES-> ", verbose, mem, yield_ptr),
      clauses_cmd_base_addr(clauses_cmd_base_addr),
      clauses_base_addr(clauses_base_addr),
      num_orig_clauses(0), learnt_offset(0),
      allocator(verbose, clauses_base_addr, 0x0FFFFFFF) {
    
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
    read(clauseAddr(addr) + offsetof(Clause, num_lits), sizeof(uint32_t), worker_id);

    uint32_t size;
    memcpy(&size, reorder_buffer->getResponse(worker_id).data(), sizeof(uint32_t));
    return size;
}

// read the clause literals from clause address
Clause Clauses::readClause(Cref addr, int worker_id) {
    uint32_t num_lits = getClauseSize(addr, worker_id);
    
    // Read the rest of clause data (activity + literals)
    readBurst(clauseAddr(addr + offsetof(Clause, activity)), CLAUSE_MEMBER_SIZE * (num_lits + 1), worker_id);

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

void Clauses::writeLiteral(Cref addr, const Lit& lit, int idx) {
    std::vector<uint8_t> buffer(sizeof(Lit));
    memcpy(buffer.data(), &lit, sizeof(Lit));
    write(clauseAddr(addr + offsetof(Clause, literals) + idx * sizeof(Lit)), sizeof(Lit), buffer);
}

void Clauses::initialize(const std::vector<Clause>& clauses) {
    num_orig_clauses = clauses.size();
    size_ = clauses.size();
    output.verbose(CALL_INFO, 1, 0, "Size: %zu clause pointers, %ld bytes\n",
                   size_, size_ * sizeof(Cref));
    
    // Calculate total size needed for original clauses
    size_t total_memory = line_size;  // addr 0 is ClauseRef_Undef
    std::vector<Cref> addr_array(clauses.size());
    
    for (size_t i = 0; i < clauses.size(); i++) {
        addr_array[i] = total_memory;
        total_memory += clauses[i].size();
    }

    // Initialize allocator with the reserved area for original clauses
    allocator.initialize(this, total_memory);
    
    // Set learnt offset to start after original clauses
    learnt_offset = total_memory;
    
    // Write all clause pointers in one operation
    std::vector<uint8_t> addr_buffer(clauses.size() * sizeof(Cref));
    memcpy(addr_buffer.data(), addr_array.data(), addr_buffer.size());
    writeUntimed(clauses_cmd_base_addr, addr_buffer.size(), addr_buffer);
    
    // Prepare buffer for all clause data - no headers/footers needed for original clauses
    std::vector<uint8_t> literals_buffer(total_memory);
    size_t offset = line_size;  // Start after ClauseRef_Undef
    
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
    
    output.verbose(CALL_INFO, 1, 0, "Size: %zu clause structs, %ld bytes\n",
                   size_, total_memory);
}

Cref Clauses::addClause(const Clause& clause) {
    Cref block_addr = allocator.allocateBlock(clause.size());
    writeAddr(size_, block_addr);  // Write new ptr at index size_
    
    size_++;
    writeClause(block_addr, clause);  // Write clause data to memory
    
    output.verbose(CALL_INFO, 7, 0, 
                  "Added clause %ld with %u literals at offset %u\n", 
                  size_ - 1, clause.litSize(), block_addr);
    return block_addr;
}

void Clauses::freeClause(Cref addr, uint32_t cls_size) {
    assert(addr >= learnt_offset);
    size_t req_size = CLAUSE_MEMBER_SIZE * 2 + cls_size * sizeof(Lit); // size + activity + literals
    allocator.freeBlock(addr, req_size);
}

void Clauses::writeAct(Cref addr, float act) {
    std::vector<uint8_t> buffer(sizeof(float));
    memcpy(buffer.data(), &act, sizeof(float));
    write(clauseAddr(addr + offsetof(Clause, activity)), sizeof(float), buffer);
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
    // We'll read each activity individually since clauses are now scattered in memory
    std::vector<float> result(addr.size());
    
    // TODO: parallelize by using non blocking reads with different worker IDs
    for (size_t i = 0; i < addr.size(); i++) {
        read(clauseAddr(addr[i] + offsetof(Clause, activity)), sizeof(float), worker_id);
        memcpy(&result[i], reorder_buffer->getResponse(worker_id).data(), sizeof(float));
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

void Clauses::reduceDB(const std::vector<Cref>& to_keep) {
    std::vector<uint8_t> addr_buffer(to_keep.size() * sizeof(Cref));
    memcpy(addr_buffer.data(), to_keep.data(), addr_buffer.size());
    writeBurst(cmdAddr(num_orig_clauses), addr_buffer);

    size_ = to_keep.size() + num_orig_clauses;  // Update size
}
