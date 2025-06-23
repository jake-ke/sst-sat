#include <sst/core/sst_config.h>
#include "async_watches.h"

uint64_t Watches::allocateBlock() {
    // First check if we have recycled blocks available
    if (!free_blocks.empty()) {
        uint64_t addr = free_blocks.front();
        free_blocks.pop();
        return addr;
    }
    
    // Otherwise, allocate a new block
    uint64_t addr = next_free_block;
    next_free_block += line_size;  // Use line_size directly
    output.verbose(CALL_INFO, 7, 0, "Allocating new block at 0x%lx\n", addr);
    return addr;
}

uint64_t Watches::readHeadPointer(int lit_idx, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "Read head pointer for var %d\n", lit_idx/2);
    read(watchesAddr(lit_idx), sizeof(uint64_t), worker_id);
    
    uint64_t headptr;
    memcpy(&headptr, reorder_buffer->getResponse(worker_id).data(), sizeof(uint64_t));
    return headptr;
}

void Watches::writeHeadPointer(int start_idx, const uint64_t headptr) {
    std::vector<uint8_t> data(sizeof(uint64_t));
    memcpy(data.data(), &headptr, sizeof(uint64_t));
    write(watchesAddr(start_idx), sizeof(uint64_t), data);
    output.verbose(CALL_INFO, 7, 0, 
        "Write head pointers[%d], 0x%lx\n", start_idx, headptr);
}

WatcherBlock Watches::readBlock(uint64_t addr, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "Read watcher block at 0x%lx\n", addr);
    read(addr, line_size, worker_id);

    WatcherBlock block;
    memcpy(&block, reorder_buffer->getResponse(worker_id).data(), sizeof(WatcherBlock));
    return block;
}

void Watches::writeBlock(uint64_t addr, const WatcherBlock& block) {
    output.verbose(CALL_INFO, 7, 0, "Write watcher block at 0x%lx\n", addr);
    std::vector<uint8_t> data(line_size, 0);
    memcpy(data.data(), &block, sizeof(WatcherBlock));
    write(addr, line_size, data);
}

void Watches::updateBlock(int lit_idx, uint64_t prev_addr, uint64_t curr_addr, 
                          WatcherBlock& prev_block, WatcherBlock& curr_block) {
    // have removed some watchers, update the blocks accordingly
    if (curr_block.valid_mask == 0) {
        // Block became empty, remove it
        if (prev_addr == 0) {
            // Current block was the head
            writeHeadPointer(lit_idx, curr_block.next_block);
        } else {
            // Update previous block's next pointer
            prev_block.next_block = curr_block.next_block;
            writeBlock(prev_addr, prev_block);
        }
        freeBlock(curr_addr);
    } else {
        // Block still has valid nodes, update it
        writeBlock(curr_addr, curr_block);
    }
}

void Watches::initWatches(size_t watch_count, std::vector<Clause>& clauses) {
    // Calculate how many nodes can fit in a cache line
    size_t header_size = sizeof(uint8_t) + sizeof(uint32_t); // valid_mask + next_block
    nodes_per_block = (line_size - header_size) / sizeof(WatcherNode);
    
    // First, allocate and initialize head pointers to null
    std::vector<uint64_t> head_ptrs(watch_count, 0);
    size_ = watch_count;
    
    // Create temporary data structure to build the linked lists
    std::vector<std::vector<std::pair<int, Lit>>> tmp_watches(watch_count);
    
    // Add watchers for all clauses
    for (int ci = 0; ci < clauses.size(); ci++) {
        Clause& c = clauses[ci];
        if (c.size() >= 2) {
            // needs to be distinct for parallel propagation
            // cannot have two watchers for the same literal
            assert(c.literals[0] != c.literals[1]);

            // Watch the first two literals
            int idx0 = toWatchIndex(~c.literals[0]);
            int idx1 = toWatchIndex(~c.literals[1]);
            
            // Add watchers to the temporary structure
            tmp_watches[idx0].push_back(std::make_pair(ci, c.literals[1]));
            tmp_watches[idx1].push_back(std::make_pair(ci, c.literals[0]));
        }
    }
    
    // Allocate memory for batched write operations
    std::vector<uint8_t> all_blocks_data;
    
    // Track which blocks we've used
    size_t block_idx_counter = 0;
    
    for (size_t lit_idx = 0; lit_idx < watch_count; lit_idx++) {
        auto& watch_list = tmp_watches[lit_idx];
        if (watch_list.empty()) continue;
        
        // Calculate blocks needed for this watch list
        size_t blocks_needed = (watch_list.size() + nodes_per_block - 1) / nodes_per_block;  // Ceiling division
        
        // Set head pointer for this watch list
        uint64_t first_block_addr = nodes_base_addr + (block_idx_counter * line_size);
        head_ptrs[lit_idx] = first_block_addr;
        
        // Fill the blocks
        size_t node_in_list = 0;
        
        for (size_t block_idx = 0; block_idx < blocks_needed; block_idx++) {
            // Prepare block
            WatcherBlock block;
            size_t nodes_in_this_block = 0;
            
            // Add nodes to this block
            while (node_in_list < watch_list.size() && nodes_in_this_block < nodes_per_block) {
                auto& watcher = watch_list[node_in_list];
                block.nodes[nodes_in_this_block] = WatcherNode(watcher.first, watcher.second);
                block.valid_mask |= (1 << nodes_in_this_block);
                nodes_in_this_block++;
                node_in_list++;
            }
            
            // Set next block pointer if there are more blocks
            if (block_idx < blocks_needed - 1) {
                block.next_block = first_block_addr + ((block_idx + 1) * line_size);
            }
            
            // Copy block data to the batch buffer
            std::vector<uint8_t> block_data(line_size, 0);
            uint64_t offset = (block_idx_counter + block_idx) * line_size;
            memcpy(block_data.data(), &block, sizeof(WatcherBlock));
            all_blocks_data.insert(all_blocks_data.end(), block_data.begin(), block_data.end());
        }
        
        block_idx_counter += blocks_needed;
    }
    
    // Write all blocks in one operation
    writeUntimed(nodes_base_addr, all_blocks_data.size(), all_blocks_data);
    
    // Update next_free_block to point after our allocated blocks
    next_free_block = nodes_base_addr + (block_idx_counter * line_size);
    
    // Write all head pointers in one operation
    std::vector<uint8_t> ptr_data(watch_count * sizeof(uint64_t));
    memcpy(ptr_data.data(), head_ptrs.data(), ptr_data.size());
    writeUntimed(watches_base_addr, ptr_data.size(), ptr_data);
    
    output.verbose(CALL_INFO, 1, 0, "Size: %zu watches, %ld bytes\n", 
                   watch_count, watch_count * sizeof(uint64_t));
    output.verbose(CALL_INFO, 1, 0, "Size: %zu watch node blocks, %ld bytes\n", 
                   block_idx_counter, block_idx_counter * line_size);
    
}

// Insert a new watcher for a literal - simplified to only check the first block
void Watches::insertWatcher(int lit_idx, int clause_idx, Lit blocker, int worker_id) {
    if (busy.find(lit_idx) != busy.end()) {
        output.fatal(CALL_INFO, -1, "Watches: Already busy with var %d\n", lit_idx/2);
    }
    busy.insert(lit_idx);

    // Read current head pointer
    uint64_t head = readHeadPointer(lit_idx, worker_id);

    // Case 1: Empty watch list - create first block
    if (head == 0) {
        uint64_t new_block_addr = allocateBlock();
        WatcherBlock new_block;
        new_block.nodes[0] = WatcherNode(clause_idx, blocker);
        new_block.valid_mask = 0x01;  // First node is valid
        writeBlock(new_block_addr, new_block);
        writeHeadPointer(lit_idx, new_block_addr);

        busy.erase(lit_idx);
        output.verbose(CALL_INFO, 7, 0, 
            "Worker[%d] Inserted watcher in empty head, clause %d, var %d\n", 
            worker_id, clause_idx, lit_idx/2);
        return;
    }
    
    // Case 2: Try to insert into the first block
    WatcherBlock first_block = readBlock(head, worker_id);
    
    // Check if this block has a free slot
    uint8_t full_mask = (1 << nodes_per_block) - 1;  // All bits set for nodes_per_block
    if (first_block.valid_mask != full_mask) {
        // Find the first free slot
        for (size_t i = 0; i < nodes_per_block; i++) {
            if ((first_block.valid_mask & (1 << i)) == 0) {
                // Found a free slot
                first_block.nodes[i] = WatcherNode(clause_idx, blocker);
                first_block.valid_mask |= (1 << i);
                writeBlock(head, first_block);
                
                busy.erase(lit_idx);
                output.verbose(CALL_INFO, 7, 0, 
                    "Worker[%d] Inserted watcher in slot %zu, clause %d, var %d\n", 
                    worker_id, i, clause_idx, lit_idx/2);
                return;
            }
        }
    }

    // Case 3: First block is full - add a new block at front
    uint64_t new_block_addr = allocateBlock();
    WatcherBlock new_block;
    new_block.nodes[0] = WatcherNode(clause_idx, blocker);
    new_block.valid_mask = 0x01;  // First node is valid
    new_block.next_block = head;  // Link to current head
    writeBlock(new_block_addr, new_block);
    writeHeadPointer(lit_idx, new_block_addr);
    
    busy.erase(lit_idx);
    output.verbose(CALL_INFO, 7, 0, 
        "Worker[%d] Inserted watcher in new block, clause %d, var %d\n", 
        worker_id, clause_idx, lit_idx/2);
}

// Remove a watcher with given clause index
void Watches::removeWatcher(int lit_idx, int clause_idx, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, 
        "Removing watcher for clause %d at var %d\n", clause_idx, lit_idx/2);
    // Read head pointer
    uint64_t head = readHeadPointer(lit_idx, worker_id);
    if (head == 0) output.fatal(CALL_INFO, -1, "Empty watch list for var %d\n", lit_idx/2);
    
    uint64_t curr_addr = head;
    uint64_t prev_addr = 0;
    WatcherBlock prev_block;
    
    while (curr_addr != 0) {
        WatcherBlock curr_block = readBlock(curr_addr, worker_id);
        
        // Search for the clause in this block
        for (size_t i = 0; i < nodes_per_block; i++) {
            if ((curr_block.valid_mask & (1 << i)) && curr_block.nodes[i].clause_idx == clause_idx) {
                // Found the clause, invalidate this node
                curr_block.valid_mask &= ~(1 << i);
                
                // Check if the entire block is now empty
                if (curr_block.valid_mask == 0) {
                    // Block is empty, remove it from the list
                    if (prev_addr == 0) {
                        // This was the head block
                        writeHeadPointer(lit_idx, curr_block.next_block);
                    } else {
                        // Update previous block's next pointer
                        prev_block.next_block = curr_block.next_block;
                        writeBlock(prev_addr, prev_block);
                    }
                    
                    // Add the empty block to the free list
                    freeBlock(curr_addr);
                    
                    output.verbose(CALL_INFO, 7, 0, 
                        "Removed watcher for clause %d at var %d, freed empty block 0x%lx\n", 
                        clause_idx, lit_idx/2, curr_addr);
                } else {
                    // Block still has valid nodes, just update it
                    writeBlock(curr_addr, curr_block);
                    
                    output.verbose(CALL_INFO, 7, 0, 
                        "Removed watcher for clause %d at var %d, updated block 0x%lx\n", 
                        clause_idx, lit_idx/2, curr_addr);
                }
                
                return;
            }
        }
        
        prev_addr = curr_addr;
        prev_block = curr_block;
        curr_addr = curr_block.next_block;
    }
    
    output.fatal(CALL_INFO, -1, "Remove failed clause %d, var %d\n", clause_idx, lit_idx/2);
}
