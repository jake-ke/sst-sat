#include <sst/core/sst_config.h>
#include "memory_allocator.h"
#include <algorithm>

MemoryAllocator::MemoryAllocator(int verbose, uint64_t mem_base_addr, uint64_t total_size)
    : mem_base_addr(mem_base_addr), heap_size(total_size), reserved_size(0),
      req_mem(0), alloc_mem(0), frag_ratio(0.0), peak_frag_ratio(0.0) {

    output.init("ALLOC->", verbose, 0, SST::Output::STDOUT);
    // Cref offsets and BlockHeader.block_size are signed-32/31-bit, so the
    // managed region must stay below 2GiB regardless of what the caller asks for.
    if (total_size > 0x7FFFFFF0ULL) {
        output.fatal(CALL_INFO, -1,
            "Clause region size %lu B exceeds the 2GiB Cref/block_size limit\n",
            total_size);
    }
    // Initialize free lists
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        free_lists[i] = ClauseRef_Undef;
    }
}

void MemoryAllocator::initialize(AsyncBase* async_base, Cref res_size) {
    this->async_base = async_base;
    // Before this guard existed, reserved > heap_size made the uint32
    // free_size below wrap, sending the footer write ~4GiB past the region
    // (the historical constant-0x16FFFFFFB init fatal).
    if (res_size < 0 || (uint64_t)res_size + MIN_BLOCK_SIZE > heap_size) {
        output.fatal(CALL_INFO, -1,
            "Clause region too small: %ld B reserved for original clauses + %u B "
            "minimum free block > %lu B region. Instance needs a larger clauses region.\n",
            (long)res_size, MIN_BLOCK_SIZE, heap_size);
    }
    reserved_size = res_size;

    // Reset fragmentation tracking
    req_mem = res_size;
    alloc_mem = res_size;

    // Create one large free block for the rest of memory
    Cref start_addr = reserved_size;
    uint32_t free_size = heap_size - reserved_size;

    // Initialize the block as free - create header and footer
    BlockHeader tag;
    tag.allocated = 0;  // 0 means free
    tag.block_size = free_size;
    
    std::vector<uint8_t> tag_buffer(TAG_SIZE);
    memcpy(tag_buffer.data(), &tag, TAG_SIZE);
    
    // Write header and footer using writeUntimed
    async_base->writeUntimed(mem_base_addr + start_addr, TAG_SIZE, tag_buffer);
    async_base->writeUntimed(mem_base_addr + start_addr + free_size - TAG_SIZE, TAG_SIZE, tag_buffer);
    
    // Find suitable size class for the block
    int size_class = getSizeClass(free_size);
    free_lists[size_class] = start_addr;
    
    // Initialize next and prev pointers (both ClauseRef_Undef) using writeUntimed
    std::vector<uint8_t> pointers_buffer(2 * sizeof(Cref));
    Cref undef_value = ClauseRef_Undef;
    memcpy(pointers_buffer.data(), &undef_value, sizeof(Cref));
    memcpy(pointers_buffer.data() + sizeof(Cref), &undef_value, sizeof(Cref));
    async_base->writeUntimed(mem_base_addr + start_addr + TAG_SIZE, 2 * sizeof(Cref), pointers_buffer);
    
    output.verbose(CALL_INFO, 1, 0, 
        "Memory allocator initialized: heap size=%lu bytes, reserved=%u bytes\n", 
        heap_size, reserved_size);
}

int MemoryAllocator::getSizeClass(uint32_t size) const {
    for (int i = 1; i < NUM_SIZE_CLASSES; i++) {
        // find the first size class where size is less than the threshold
        if (size < SIZE_CLASSES[i]) {
            assert(size >= SIZE_CLASSES[i - 1]);
            return i - 1;
        }
    }
    return NUM_SIZE_CLASSES - 1; // Return the largest size class
}

BlockHeader MemoryAllocator::readBlockTag(Cref addr, int worker_id) {
    async_base->read(mem_base_addr + addr, TAG_SIZE, worker_id);

    BlockHeader header;
    memcpy(&header, reorder_buffer->getResponse(worker_id).data(), TAG_SIZE);
    return header;
}

void MemoryAllocator::setTags(Cref addr, uint32_t size, bool allocated) {
    // Create tag with allocation information
    BlockHeader tag;
    tag.allocated = allocated ? 1 : 0;
    tag.block_size = size;
    
    std::vector<uint8_t> tag_buffer(TAG_SIZE);
    memcpy(tag_buffer.data(), &tag, TAG_SIZE);
    
    // Write header and footer
    async_base->write(mem_base_addr + addr, TAG_SIZE, tag_buffer);
    async_base->write(mem_base_addr + addr + size - TAG_SIZE, TAG_SIZE, tag_buffer);
}

Cref MemoryAllocator::getNextFreeBlock(Cref addr, int worker_id) {
    async_base->read(mem_base_addr + addr + TAG_SIZE, sizeof(Cref), worker_id);

    Cref next;
    memcpy(&next, reorder_buffer->getResponse(worker_id).data(), sizeof(Cref));
    return next;
}

Cref MemoryAllocator::getPrevFreeBlock(Cref addr, int worker_id) {
    async_base->read(mem_base_addr + addr + TAG_SIZE + sizeof(Cref), sizeof(Cref), worker_id);
    
    Cref prev;
    memcpy(&prev, reorder_buffer->getResponse(worker_id).data(), sizeof(Cref));
    return prev;
}

void MemoryAllocator::setNextFreeBlock(Cref addr, Cref next) {
    std::vector<uint8_t> buffer(sizeof(Cref));
    memcpy(buffer.data(), &next, sizeof(Cref));
    async_base->write(mem_base_addr + addr + TAG_SIZE, sizeof(Cref), buffer);
}

void MemoryAllocator::setPrevFreeBlock(Cref addr, Cref prev) {
    std::vector<uint8_t> buffer(sizeof(Cref));
    memcpy(buffer.data(), &prev, sizeof(Cref));
    async_base->write(mem_base_addr + addr + TAG_SIZE + sizeof(Cref), sizeof(Cref), buffer);
}

void MemoryAllocator::insertFreeBlock(Cref addr, uint32_t size) {
    int size_class = getSizeClass(size);
    output.verbose(CALL_INFO, 8, 0, "Insert block 0x%x, %u bytes into class of %u bytes\n", 
           addr, size, SIZE_CLASSES[size_class]);
    
    // Write block header and footer
    setTags(addr, size, false);
    
    // Insert the block at the front of the free list
    setNextFreeBlock(addr, free_lists[size_class]);
    setPrevFreeBlock(addr, ClauseRef_Undef);
    
    // If there is a block in the free list, update its previous head
    if (free_lists[size_class] != ClauseRef_Undef) {
        setPrevFreeBlock(free_lists[size_class], addr);
    }
    
    free_lists[size_class] = addr;
}

void MemoryAllocator::removeFreeBlock(Cref addr, uint32_t block_size, int worker_id) {
    Cref next = getNextFreeBlock(addr, worker_id);
    Cref prev = getPrevFreeBlock(addr, worker_id);
    
    // Update previous block's next pointer
    if (prev != ClauseRef_Undef) {
        setNextFreeBlock(prev, next);
    } else {
        // This was the first block in the list
        int size_class = getSizeClass(block_size);
        free_lists[size_class] = next;
    }
    
    // Update next block's prev pointer
    if (next != ClauseRef_Undef) {
        setPrevFreeBlock(next, prev);
    }
}

Cref MemoryAllocator::allocateBlock(uint32_t size) {
    // Ensure minimum block size
    uint32_t required_size = std::max(size + 2 * TAG_SIZE, MIN_BLOCK_SIZE);
    output.verbose(CALL_INFO, 8, 0, "Need a block of size >= %u bytes\n", required_size);

    // Find suitable size class
    Cref block = ClauseRef_Undef;
    uint32_t block_size = 0;
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        // if the required size is not equal to the size class,
        // we always use the next size up to avoid scanning the smaller free list
        if (required_size <= SIZE_CLASSES[i] && free_lists[i] != ClauseRef_Undef) {
            block = free_lists[i];
            block_size = readBlockTag(block).block_size;
            removeFreeBlock(block, block_size);
            output.verbose(CALL_INFO, 8, 0, "Found a block at 0x%x, %u bytes from size class %u bytes\n",
                block, block_size, SIZE_CLASSES[i]);
            assert(block_size >= required_size);
            break;
        }
    }
    
    // If no suitable block found, scan the largest free list
    if (block == ClauseRef_Undef) {
        // Search through the last size class for a block that fits
        if (free_lists[NUM_SIZE_CLASSES - 1] != ClauseRef_Undef) {
            Cref current = free_lists[NUM_SIZE_CLASSES - 1];
            while (current != ClauseRef_Undef) {
                block_size = readBlockTag(current).block_size;
                if (block_size >= required_size) {
                    block = current;
                    removeFreeBlock(block, block_size);
                    output.verbose(CALL_INFO, 8, 0, "Found a block at 0x%x, %u bytes from largest size class\n",
                        block, block_size);
                    break;
                }
                current = getNextFreeBlock(current);
            }
        }

        if (block == ClauseRef_Undef) {
            output.fatal(CALL_INFO, -1, "Out of memory: failed to allocate %u bytes\n", size);
            return ClauseRef_Undef;
        }
    }

    // Check if we can split the block
    if (block_size >= required_size + MIN_BLOCK_SIZE) {
        Cref remainder = block + required_size;
        uint32_t remainder_size = block_size - required_size;
        
        // Insert remainder into appropriate free list
        insertFreeBlock(remainder, remainder_size);
        
        // Update block size
        block_size = required_size;
        output.verbose(CALL_INFO, 8, 0, 
            "Splitted, block now at 0x%x, %u bytes, remainder at 0x%x, %u bytes\n", 
            block, block_size, remainder, remainder_size);
    }
    
    // Mark block as allocated
    setTags(block, block_size, true);
    
    // Update internal fragmentation stats
    req_mem += size;
    alloc_mem += block_size;
    updateFragStats();
    
    output.verbose(CALL_INFO, 7, 0, "Fragmentation: req=%lu alloc=%lu ratio=%.2f%%\n",
                   req_mem, alloc_mem, frag_ratio * 100.0);
    
    return block;
}

void MemoryAllocator::freeBlock(Cref addr, size_t req_size, int worker_id) {
    // Read the current block's information
    BlockHeader curr_header = readBlockTag(addr, worker_id);
    uint32_t curr_size = curr_header.block_size;
    output.verbose(CALL_INFO, 8, 0, "Freeing block at 0x%x, %u bytes\n", addr, curr_size);

    // fragmentation stats
    req_mem -= req_size;
    alloc_mem -= curr_size;
    updateFragStats();

    Cref final_addr = addr;
    uint32_t final_size = curr_size;

    // Check if the previous physical block is free
    if (addr - TAG_SIZE >= reserved_size) {
        BlockFooter prev_footer = readBlockTag(addr - TAG_SIZE, worker_id);
        Cref prev_physical = addr - prev_footer.block_size;
        if (!prev_footer.allocated) {
            // Coalesce with previous block
            removeFreeBlock(prev_physical, prev_footer.block_size, worker_id);
            final_addr = prev_physical;
            final_size += prev_footer.block_size;
        }
    }

    // Check if the next physical block is free. Crefs are region-relative, so
    // the bound is heap_size — comparing against getMemoryEnd() (absolute) was
    // always true and read a phantom header past the region for the last block.
    Cref next_physical = addr + curr_size;
    if ((uint64_t)next_physical + TAG_SIZE <= heap_size) {
        BlockHeader next_header = readBlockTag(next_physical, worker_id);
        if (!next_header.allocated) {
            // Coalesce with next block
            removeFreeBlock(next_physical, next_header.block_size, worker_id);
            final_size += next_header.block_size;
        }
    }

    output.verbose(CALL_INFO, 8, 0, "Final coalesced block at 0x%x, size %u bytes\n",
        final_addr, final_size);
    // Insert into free list with the potentially coalesced block
    insertFreeBlock(final_addr, final_size);
}

bool MemoryAllocator::shrinkTop(uint32_t chunk) {
    // The topmost block's footer sits at heap_size - TAG_SIZE.
    BlockFooter top_footer = readBlockTag(heap_size - TAG_SIZE);
    if (top_footer.allocated) {
        output.verbose(CALL_INFO, 1, 0,
            "shrinkTop: topmost block is allocated, cannot reclaim %u B\n", chunk);
        return false;
    }
    if (top_footer.block_size < chunk + MIN_BLOCK_SIZE) {
        output.verbose(CALL_INFO, 1, 0,
            "shrinkTop: top free block (%u B) too small to give up %u B\n",
            top_footer.block_size, chunk);
        return false;
    }
    Cref top_addr = (Cref)(heap_size - top_footer.block_size);
    removeFreeBlock(top_addr, top_footer.block_size);
    insertFreeBlock(top_addr, top_footer.block_size - chunk);
    heap_size -= chunk;
    output.verbose(CALL_INFO, 3, 0,
        "shrinkTop: ceiling lowered by %u B to %lu B\n", chunk, heap_size);
    return true;
}

void MemoryAllocator::updateFragStats() {
    assert(alloc_mem > 0);
    frag_ratio = static_cast<double>(alloc_mem - req_mem) / alloc_mem;
    peak_frag_ratio = std::max(peak_frag_ratio, frag_ratio);
}

void MemoryAllocator::printFragStats() const {
    output.output("  Heap: %lu bytes, Reserved: %u bytes\n", heap_size, reserved_size);
    output.output("  Requested: %lu bytes\n", req_mem);
    output.output("  Allocated: %lu bytes\n", alloc_mem);
    output.output("  Wasted: %lu bytes\n", alloc_mem - req_mem);
    output.output("  Current frag: %.2f%%\n", frag_ratio * 100.0);
    output.output("  Peak frag: %.2f%%\n", peak_frag_ratio * 100.0);
}

