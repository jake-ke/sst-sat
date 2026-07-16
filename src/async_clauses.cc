#include <sst/core/sst_config.h>
#include "async_clauses.h"

Clauses::Clauses(int verbose, SST::Interfaces::StandardMem* mem,
                 uint64_t clauses_cmd_base_addr, uint64_t clauses_base_addr,
                 coro_t::push_type** yield_ptr,
                 uint64_t clauses_region_size)
    : AsyncBase("CLAUSES-> ", verbose, mem, yield_ptr),
      clauses_cmd_base_addr(clauses_cmd_base_addr),
      clauses_base_addr(clauses_base_addr),
      num_orig_clauses(0), learnt_offset(0),
      binary_next_((Cref)clauses_region_size),
      region_size_((Cref)clauses_region_size),
      num_binary_(0),
      // The top BINARY_CHUNK of the region seeds the binary-clause bump
      // region; the allocator ceiling drops further chunks on demand.
      allocator(verbose, clauses_base_addr, clauses_region_size - BINARY_CHUNK) {

    output.verbose(CALL_INFO, 1, 0, "base addresses: "
        "cmd=0x%lx, data=0x%lx, region=%lu B (binary seed %u B)\n",
        clauses_cmd_base_addr, clauses_base_addr, clauses_region_size,
        BINARY_CHUNK);
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
    assert(num_lits >= 2);

    // Read the rest of clause data (activity + literals)
    readBurst(clauseAddr(addr + offsetof(Clause, activity)), CLAUSE_MEMBER_SIZE * (num_lits + 1), worker_id);

    const uint8_t* data = reorder_buffer->getResponse(worker_id).data();

    Clause c(num_lits);
    memcpy(&c.activity, data, sizeof(float));  // Read activity first
    memcpy(c.literals.data(), data + sizeof(float), num_lits * sizeof(Lit));
    return c;
}

// Read the first 16 B of a clause in one pipelined burst: num_lits, activity
// and the two watched literals — everything reduceDB needs per clause.
ClauseHead Clauses::readClauseHead(Cref addr, int worker_id) {
    readBurst(clauseAddr(addr), sizeof(ClauseHead), worker_id);

    ClauseHead h;
    memcpy(&h, reorder_buffer->getResponse(worker_id).data(), sizeof(ClauseHead));
    assert(h.num_lits >= 2);
    return h;
}

float Clauses::readAct(Cref addr, int worker_id) {
    read(clauseAddr(addr + offsetof(Clause, activity)), sizeof(float), worker_id);

    float act;
    memcpy(&act, reorder_buffer->getResponse(worker_id).data(), sizeof(float));
    return act;
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

    // Guard here while the byte count is still size_t: allocator.initialize
    // takes Cref (int), so an oversized instance would otherwise wrap negative
    // before the allocator's own check can see it.
    if (total_memory > (size_t)0x7FFFFFF0 || total_memory + MIN_BLOCK_SIZE > allocator.capacity()) {
        output.fatal(CALL_INFO, -1,
            "Original clauses need %zu B but the clauses region holds %lu B "
            "(learnt headroom excluded). Instance does not fit the clause DB.\n",
            total_memory, allocator.capacity());
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
    Cref block_addr;
    if (clause.litSize() == 2) {
        // Binary learnt clauses are never removed: bump-allocate them from
        // the dedicated top-of-region area (no tags, no free list). Grow the
        // area by lowering the allocator ceiling when it fills up.
        if (binary_next_ - (Cref)clause.size() < (Cref)allocator.capacity()) {
            if (!allocator.shrinkTop(BINARY_CHUNK)) {
                output.fatal(CALL_INFO, -1,
                    "Binary clause region cannot grow: %zu binaries "
                    "(%ld B) and the heap top is not free. Clause region "
                    "exhausted.\n", num_binary_,
                    (long)(region_size_ - binary_next_));
            }
        }
        binary_next_ -= (Cref)clause.size();
        block_addr = binary_next_;
        num_binary_++;
    } else {
        block_addr = allocator.allocateBlock(clause.size());
    }
    writeAddr(size_, block_addr);  // Write new ptr at index size_

    size_++;
    writeClause(block_addr, clause);  // Write clause data to memory

    output.verbose(CALL_INFO, 7, 0,
                  "Added clause %ld with %u literals at offset %u\n",
                  size_ - 1, clause.litSize(), block_addr);
    return block_addr;
}

void Clauses::freeClause(Cref addr, uint32_t cls_size, int worker_id) {
    assert(addr >= learnt_offset && !isBinaryLearnt(addr));
    size_t req_size = CLAUSE_MEMBER_SIZE * 2 + cls_size * sizeof(Lit); // size + activity + literals
    allocator.freeBlock(addr, req_size, worker_id);
}

void Clauses::writeAct(Cref addr, float act) {
    std::vector<uint8_t> buffer(sizeof(float));
    memcpy(buffer.data(), &act, sizeof(float));
    write(clauseAddr(addr + offsetof(Clause, activity)), sizeof(float), buffer);
}

std::vector<Cref> Clauses::readAddrChunk(size_t learnt_start, size_t count, int worker_id) {
    assert(learnt_start + count <= numLearnts());
    readBurst(cmdAddr(num_orig_clauses + learnt_start), sizeof(Cref) * count, worker_id);

    std::vector<Cref> result(count);
    memcpy(result.data(), reorder_buffer->getResponse(worker_id).data(),
           count * sizeof(Cref));
    return result;
}

// In-place compaction: surviving pointer keep_idx (in commit order) is written
// back into the learnt slice of the pointer array. The write index can never
// overrun the streamer's read position (survivors are a subset of scanned).
void Clauses::compactKeep(size_t keep_idx, Cref addr) {
    writeAddr(num_orig_clauses + keep_idx, addr);
}

void Clauses::finishReduce(size_t kept) {
    size_ = num_orig_clauses + kept;
}

void Clauses::printBinaryStats() const {
    output.output("  Binary region: %zu clauses, %ld B used, ceiling %lu B\n",
                  num_binary_, (long)(region_size_ - binary_next_),
                  allocator.capacity());
}
