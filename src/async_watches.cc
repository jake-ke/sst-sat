#include <sst/core/sst_config.h>
#include "async_watches.h"

uint32_t Watches::allocateBlock() {
    uint32_t addr;
    // First check if we have recycled blocks available
    if (!free_blocks.empty()) {
        addr = free_blocks.front();
        free_blocks.pop();
    } else {
        // Otherwise, allocate a new block
        addr = next_free_block;
        next_free_block += block_size;  // Allocate based on computed block size
    }
    
    output.verbose(CALL_INFO, 7, 0, "Allocating new block at 0x%x\n", addr);
    return addr;
}

WatchMetaData Watches::readMetaData(int lit_idx, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "Read metadata for var %d\n", lit_idx/2);
    read(watchesAddr(lit_idx), sizeof(WatchMetaData), worker_id);
    
    WatchMetaData wmd;
    memcpy(&wmd, reorder_buffer->getResponse(worker_id).data(), sizeof(WatchMetaData));
    return wmd;
}

void Watches::writeMetaData(int lit_idx, const WatchMetaData& metadata) {
    output.verbose(CALL_INFO, 7, 0, "Write metadata: lit %d, head: %u, free_head: %u\n",
        lit_idx, metadata.head_ptr, metadata.free_head);

    std::vector<uint8_t> bytes(sizeof(WatchMetaData));
    memcpy(bytes.data(), &metadata, sizeof(WatchMetaData));
    write(watchesAddr(lit_idx), bytes.size(), bytes);
}

void Watches::writeHeadPointer(int lit_idx, const uint32_t headptr) {
    std::vector<uint8_t> bytes(sizeof(uint32_t));
    memcpy(bytes.data(), &headptr, sizeof(uint32_t));
    write(watchesAddr(lit_idx), bytes.size(), bytes);
}

void Watches::writeFreeHead(int lit_idx, const uint32_t freehead) {
    std::vector<uint8_t> bytes(sizeof(uint32_t));
    memcpy(bytes.data(), &freehead, sizeof(uint32_t));
    write(watchesAddr(lit_idx) + sizeof(uint32_t), bytes.size(), bytes);
}

void Watches::writePreWatcher(int lit_idx, const WatcherNode node, const int index) {
    std::vector<uint8_t> bytes(sizeof(WatcherNode));
    memcpy(bytes.data(), &node, sizeof(WatcherNode));
    write(watchesAddr(lit_idx) + offsetof(WatchMetaData, pre_watchers) + index * sizeof(WatcherNode), bytes.size(), bytes);
}

void Watches::writePreWatchers(int lit_idx, const WatcherNode pre_watchers[PRE_WATCHERS]) {
    std::vector<uint8_t> bytes(sizeof(WatcherNode) * PRE_WATCHERS);
    memcpy(bytes.data(), pre_watchers, sizeof(WatcherNode) * PRE_WATCHERS);
    write(watchesAddr(lit_idx) + offsetof(WatchMetaData, pre_watchers), bytes.size(), bytes);
}

WatcherBlock Watches::readBlock(uint32_t addr, int worker_id) {
    readBurst(addr, block_size, worker_id);

    WatcherBlock block;
    memcpy(&block, reorder_buffer->getResponse(worker_id).data(), sizeof(WatcherBlock));
    return block;
}

void Watches::writeBlock(uint32_t addr, const WatcherBlock& block) {
    std::vector<uint8_t> data(block_size, 0);
    memcpy(data.data(), &block, sizeof(WatcherBlock));
    writeBurst(addr, data);
}

void Watches::writePrevFree(uint32_t node_ptr, const uint32_t prev_ptr) {
    // Extract block address and node index from the combined pointer
    uint32_t block_addr = node_ptr & ~(FREE_IDX_BITS - 1);
    int node_idx = node_ptr & (FREE_IDX_BITS - 1);
    uint32_t node_addr = block_addr + offsetof(WatcherBlock, nodes) + node_idx * sizeof(WatcherNode);
    
    // Write the prev_ptr directly (assuming LSB is already 0 for valid=0)
    std::vector<uint8_t> bytes(sizeof(uint32_t));
    memcpy(bytes.data(), &prev_ptr, sizeof(uint32_t));
    write(node_addr, bytes.size(), bytes);
}

void Watches::writeNextFree(uint32_t node_ptr, const uint32_t next_ptr) {
    // Extract block address and node index from the combined pointer
    uint32_t block_addr = node_ptr & ~(FREE_IDX_BITS - 1);
    int node_idx = node_ptr & (FREE_IDX_BITS - 1);
    uint32_t node_addr = block_addr + offsetof(WatcherBlock, nodes) + node_idx * sizeof(WatcherNode);
    
    // Write to next_free field
    std::vector<uint8_t> bytes(sizeof(uint32_t));
    memcpy(bytes.data(), &next_ptr, sizeof(uint32_t));
    write(node_addr + offsetof(WatcherNode, next_free), bytes.size(), bytes);
}

// Add a node to the free list
int Watches::addToFreeList(int lit_idx, WatchMetaData& metadata, WatcherBlock& block, 
                            uint32_t block_addr, int node_idx) {
    // If block is already in the free list, don't add it again
    if (block.isInFreeList()) return 0;

    int block_visits = 0;
    // Calculate combined pointer value (block address | node index)
    uint32_t node_ptr = block_addr | node_idx;
    // Set up the new free list head
    block.nodes[node_idx] = WatcherNode(0, metadata.free_head);
    
    // If there was an existing free head, update its prev_free to point to this node
    if (metadata.free_head != 0) {
        writePrevFree(metadata.free_head, node_ptr);
        block_visits++;
    }

    // Update the free list head in metadata
    metadata.free_head = node_ptr;
    writeFreeHead(lit_idx, node_ptr);

    // Update the block's free_index to mark which node is used for the free list
    block.free_index = node_idx;
    writeBlock(block_addr, block);
    block_visits++;

    output.verbose(CALL_INFO, 4, 0, 
        "Add to free list: var %d, lit_idx %d, block 0x%x, node %d\n", 
        lit_idx/2, lit_idx, block_addr, node_idx);
    return block_visits;
}

// Remove a node from the free list
int Watches::removeFromFreeList(int lit_idx, WatchMetaData& metadata, WatcherBlock& block) {
    if (!block.isInFreeList()) return 0;

    int block_visits = 0;
    WatcherNode& free_node = block.nodes[block.free_index];
    uint32_t prev_ptr = free_node.getPrevFree();
    uint32_t next_ptr = free_node.next_free;
    output.verbose(CALL_INFO, 4, 0, 
        "Removing from free list: var %d, lit_idx %d, head_ptr 0x%x, free_head=0x%x, prev_free=0x%x, next_free=0x%x\n",
        lit_idx/2, lit_idx, metadata.head_ptr, metadata.free_head, prev_ptr, next_ptr);
    
    // Update previous node if exists
    if (prev_ptr != 0) {
        writeNextFree(prev_ptr, next_ptr);
        block_visits++;
    }
    else {
        // This was the head, update metadata
        metadata.free_head = next_ptr;
        writeFreeHead(lit_idx, next_ptr);
    }
    
    // Update next node if exists
    if (next_ptr != 0) {
        writePrevFree(next_ptr, prev_ptr);
        block_visits++;
    }

    // Mark this block as not in free list anymore
    block.free_index = PROPAGATORS;
    return block_visits;
}

void Watches::initWatches(size_t watch_count, std::vector<Clause>& clauses) {
    // First, allocate and initialize metadata to null
    std::vector<WatchMetaData> metadata(watch_count);
    size_ = watch_count;
    
    // Create temporary data structure to build the linked lists
    std::vector<std::vector<std::pair<Cref, Lit>>> tmp_watches(watch_count);
    
    // Add watchers for all clauses
    Cref addr = line_size;  // addr 0 is ClauseRef_Undef
    for (int ci = 0; ci < clauses.size(); ci++) {
        Clause& c = clauses[ci];
        if (c.litSize() >= 2) {
            // needs to be distinct for parallel propagation
            // cannot have two watchers for the same literal
            assert(c[0] != c[1]);

            // Watch the first two literals
            int idx0 = toWatchIndex(~c[0]);
            int idx1 = toWatchIndex(~c[1]);
            
            // Add watchers to the temporary structure
            tmp_watches[idx0].push_back(std::make_pair(addr, c[1]));
            tmp_watches[idx1].push_back(std::make_pair(addr, c[0]));
        }
        addr += c.size();
    }
    
    // Allocate memory for batched write operations
    std::vector<uint8_t> all_blocks_data;
    
    // Track which blocks we've used
    size_t block_idx_counter = 0;
    
    for (size_t lit_idx = 0; lit_idx < watch_count; lit_idx++) {
        auto& watch_list = tmp_watches[lit_idx];
        if (watch_list.empty()) continue;
        
        // First fill pre-watchers array
        size_t node_in_list = 0;
        while (node_in_list < watch_list.size() && node_in_list < PRE_WATCHERS) {
            auto& watcher = watch_list[node_in_list];
            metadata[lit_idx].pre_watchers[node_in_list] = WatcherNode(watcher.first, watcher.second);
            node_in_list++;
        }
        int pre_count = std::min(static_cast<size_t>(PRE_WATCHERS), watch_list.size());
        
        // Calculate blocks needed for the remaining watchers
        size_t remaining_watchers = watch_list.size() - pre_count;
        if (remaining_watchers == 0) {
            // All watchers fit in pre-watchers, no blocks needed
            continue;
        }
        
        size_t blocks_needed = (remaining_watchers + PROPAGATORS - 1) / PROPAGATORS;  // Ceiling division
        
        // Set head_ptr in metadata
        uint32_t first_block_addr = next_free_block + (block_idx_counter * block_size);
        metadata[lit_idx].head_ptr = first_block_addr;
        
        // Fill the blocks with remaining watchers
        for (size_t block_idx = 0; block_idx < blocks_needed; block_idx++) {
            // Prepare block
            WatcherBlock block;
            size_t nodes_in_this_block = 0;
            
            // Add nodes to this block
            while (node_in_list < watch_list.size() && nodes_in_this_block < PROPAGATORS) {
                auto& watcher = watch_list[node_in_list];
                block.nodes[nodes_in_this_block] = WatcherNode(watcher.first, watcher.second);
                nodes_in_this_block++;
                node_in_list++;
            }
            
            // Set next block pointer if there are more blocks
            if (block_idx < blocks_needed - 1) {
                block.setNextBlock(first_block_addr + ((block_idx + 1) * block_size));
            }
            
            // If this is the last block and it isn't full, add it to the free list
            // but only if free list is enabled
            if (USE_FREE_LIST && block_idx == blocks_needed - 1 && nodes_in_this_block < PROPAGATORS) {
                // Calculate the node address for the first free slot
                uint32_t curr_block_addr = first_block_addr + (block_idx * block_size);
                uint32_t free_node_idx = nodes_in_this_block;  // First empty slot
                
                // Set up the free node, no prev or next free
                block.nodes[free_node_idx] = WatcherNode(0, 0);
                
                // Update free_index in the block
                block.free_index = free_node_idx;
                
                // Update metadata to point to this free node
                metadata[lit_idx].free_head = curr_block_addr | free_node_idx;
            }
            
            // Copy block data to the batch buffer
            std::vector<uint8_t> block_data(block_size, 0);
            memcpy(block_data.data(), &block, sizeof(WatcherBlock));
            all_blocks_data.insert(all_blocks_data.end(), block_data.begin(), block_data.end());
        }
        
        block_idx_counter += blocks_needed;
    }
    
    // Write all blocks in one operation if any
    if (all_blocks_data.size() > 0) {
        writeUntimed(nodes_base_addr, all_blocks_data.size(), all_blocks_data);
    }
    
    // Update next_free_block to point after our allocated blocks
    next_free_block = next_free_block + (block_idx_counter * block_size);
    
    // Write all metadata in one operation
    std::vector<uint8_t> wmd_bytes(watch_count * sizeof(WatchMetaData));
    memcpy(wmd_bytes.data(), metadata.data(), wmd_bytes.size());
    writeUntimed(watches_base_addr, wmd_bytes.size(), wmd_bytes);
    
    output.verbose(CALL_INFO, 1, 0, "Size: %zu watches, %ld bytes\n", 
                   watch_count, watch_count * sizeof(WatchMetaData));
    output.verbose(CALL_INFO, 1, 0, "Size: %zu watch node blocks, %ld bytes\n", 
                   block_idx_counter, block_idx_counter * block_size);
}

// Update Watcher Blocks after potential removes
void Watches::updateBlock(int lit_idx, uint32_t prev_addr, uint32_t curr_addr, 
                          WatcherBlock& prev_block, WatcherBlock& curr_block, WatchMetaData& metadata) {
    // Check if block is empty and should be removed from watch list
    if (curr_block.countValidNodes() == 0) {
        // Block became empty, remove it
        if (prev_addr == 0) {
            // Current block was the head
            writeHeadPointer(lit_idx, curr_block.getNextBlock());
        } else {
            // Update previous block's next pointer
            prev_block.setNextBlock(curr_block.getNextBlock());
            writeBlock(prev_addr, prev_block);
        }
        if (USE_FREE_LIST) removeFromFreeList(lit_idx, metadata, curr_block);
        freeBlock(curr_addr);
    } else {
        // Block still has valid nodes, update it
        writeBlock(curr_addr, curr_block);
        
        // If the block has free slots and free list is enabled, add it to the free list
        if (USE_FREE_LIST) {
            int free_slot = curr_block.findNextFreeNode();
            if (free_slot != -1) addToFreeList(lit_idx, metadata, curr_block, curr_addr, free_slot);
        }
    }
}

// Insert a new watcher for a literal
int Watches::insertWatcher(int lit_idx, Cref clause_addr, Lit blocker, int worker_id) {
    output.verbose(CALL_INFO, 4, 0, "Inserting watcher for var %d, clause 0x%x, blocker %d\n", lit_idx/2, clause_addr, toInt(blocker));
    if (busy.find(lit_idx) != busy.end()) {
        output.fatal(CALL_INFO, -1, "Watches: Already busy with var %d\n", lit_idx/2);
    }
    busy.insert(lit_idx);
    
    int block_visits = 0; // Start with 1 for metadata read

    // Read current metadata
    WatchMetaData metadata = readMetaData(lit_idx, worker_id);

    // Case 1: Check if there's room in pre-watchers
    for (int i = 0; i < PRE_WATCHERS; i++) {
        if (!metadata.pre_watchers[i].valid) {
            metadata.pre_watchers[i] = WatcherNode(clause_addr, blocker);
            writePreWatcher(lit_idx, metadata.pre_watchers[i], i);

            busy.erase(lit_idx);
            output.verbose(CALL_INFO, 4, 0, 
                "Worker[%d] Inserted watcher in pre_watcher[%d], clause 0x%x, var %d\n", 
                worker_id, i, clause_addr, lit_idx/2);
            return block_visits;
        }
    }

    // Case 2: Check free list if available and enabled
    if (USE_FREE_LIST && metadata.free_head != 0) {
        uint32_t free_node_ptr = metadata.free_head;
        uint32_t free_block_addr = free_node_ptr & ~(FREE_IDX_BITS - 1);
        int node_idx = free_node_ptr & (FREE_IDX_BITS - 1);

        // Read the block containing the free node
        WatcherBlock block = readBlock(free_block_addr, worker_id);
        block_visits++;
        
        // Look for any free slot in the block
        int free_slot = block.findNextFreeNode();
        // If no more free slot left, remove from the free list
        if (free_slot == node_idx) block_visits += removeFromFreeList(lit_idx, metadata, block);

        // Insert the new watcher into the selected free slot
        block.nodes[free_slot] = WatcherNode(clause_addr, blocker);
        writeBlock(free_block_addr, block);
        
        busy.erase(lit_idx);
        output.verbose(CALL_INFO, 4, 0, 
            "Worker[%d] Inserted watcher using free list at block 0x%x index %d, clause 0x%x, var %d\n", 
            worker_id, free_block_addr, free_slot, clause_addr, lit_idx/2);
        
        return block_visits;
    }
    
    // Case 3 (when free list is disabled): Search all blocks for an empty slot
    if (!USE_FREE_LIST && metadata.head_ptr != 0) {
        uint32_t curr_addr = metadata.head_ptr;
        
        while (curr_addr != 0) {
            // Read the current block
            WatcherBlock block = readBlock(curr_addr, worker_id);
            block_visits++;
            
            // Look for any free slot in the block
            for (int i = 0; i < PROPAGATORS; i++) {
                if (!block.nodes[i].valid) {
                    // Found a free slot, insert the watcher here
                    block.nodes[i] = WatcherNode(clause_addr, blocker);
                    writeBlock(curr_addr, block);
                    
                    busy.erase(lit_idx);
                    output.verbose(CALL_INFO, 4, 0, 
                        "Worker[%d] Inserted watcher in existing block 0x%x index %d, clause 0x%x, var %d\n", 
                        worker_id, curr_addr, i, clause_addr, lit_idx/2);
                    
                    return block_visits;
                }
            }
            
            // Move to the next block
            curr_addr = block.getNextBlock();
        }
    }
    
    // Case 4: all blocks full or no blocks - add a new block at front
    uint32_t new_block_addr = allocateBlock();
    WatcherBlock new_block;
    new_block.nodes[0] = WatcherNode(clause_addr, blocker);
    // Link to current head
    if (metadata.head_ptr != 0) new_block.setNextBlock(metadata.head_ptr);
    
    // Since we now have a free slot, add this block to the free list if enabled
    if (USE_FREE_LIST && PROPAGATORS > 1)
        block_visits += addToFreeList(lit_idx, metadata, new_block, new_block_addr, 1);
    else {
        writeBlock(new_block_addr, new_block);
        block_visits++; // Count new block write
    }

    writeHeadPointer(lit_idx, new_block_addr);

    busy.erase(lit_idx);
    output.verbose(CALL_INFO, 4, 0, 
        "Worker[%d] Inserted watcher in new block 0x%x, clause 0x%x, var %d\n", 
        worker_id, new_block_addr, clause_addr, lit_idx/2);
    return block_visits;
}

// Remove a watcher with given clause address
void Watches::removeWatcher(int lit_idx, Cref clause_addr) {
    output.verbose(CALL_INFO, 4, 0, 
        "Removing watcher for clause 0x%x at var %d\n", clause_addr, lit_idx/2);

    // Read metadata
    WatchMetaData metadata = readMetaData(lit_idx);

    // First check pre-watchers
    for (int i = 0; i < PRE_WATCHERS; i++) {
        if (metadata.pre_watchers[i].valid && 
            metadata.pre_watchers[i].getClauseAddr() == clause_addr) {
            // Found in pre-watchers, invalidate it
            metadata.pre_watchers[i].valid = 0;
            writeMetaData(lit_idx, metadata);
            
            output.verbose(CALL_INFO, 4, 0, 
                "Removed watcher for clause 0x%x at var %d from pre_watcher[%d]\n", 
                clause_addr, lit_idx/2, i);
            return;
        }
    }
    
    uint32_t curr_addr = metadata.head_ptr;
    uint32_t prev_addr = 0;
    WatcherBlock prev_block;

    while (curr_addr != 0) {
        WatcherBlock curr_block = readBlock(curr_addr);
        
        // Search for the clause in this block
        for (int i = 0; i < PROPAGATORS; i++) {
            if (curr_block.nodes[i].valid && curr_block.nodes[i].getClauseAddr() == clause_addr) {
                // Found the clause, invalidate this node
                curr_block.nodes[i].valid = 0;
                
                // Update the block
                updateBlock(lit_idx, prev_addr, curr_addr, prev_block, curr_block, metadata);

                output.verbose(CALL_INFO, 4, 0, 
                    "Removed watcher for clause 0x%x at var %d\n", 
                    clause_addr, lit_idx/2);
                
                return;
            }
        }
        
        prev_addr = curr_addr;
        prev_block = curr_block;
        curr_addr = curr_block.getNextBlock();
    }

    output.fatal(CALL_INFO, -1, "Remove failed clause 0x%x, var %d\n", clause_addr, lit_idx/2);
}
