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

void Watches::writeMetaData(int start_idx, const WatchMetaData& metadata) {
    output.verbose(CALL_INFO, 7, 0, "Write metadata: lit %d, head: %u, size: %u\n",
        start_idx, metadata.head_ptr, metadata.size);

    std::vector<uint8_t> bytes(sizeof(WatchMetaData));
    memcpy(bytes.data(), &metadata, sizeof(WatchMetaData));
    write(watchesAddr(start_idx), bytes.size(), bytes);
}

void Watches::writePreWatchers(int start_idx, const WatcherNode* pre_watchers) {
    output.verbose(CALL_INFO, 7, 0, "Write pre-watchers for lit %d\n", start_idx);
    std::vector<uint8_t> bytes(sizeof(WatcherNode) * PRE_WATCHERS);
    memcpy(bytes.data(), pre_watchers, sizeof(WatcherNode) * PRE_WATCHERS);
    write(watchesAddr(start_idx) + offsetof(WatchMetaData, pre_watchers), bytes.size(), bytes);
}

void Watches::writeHeadPointer(int start_idx, const uint32_t headptr) {
    std::vector<uint8_t> bytes(sizeof(uint32_t));
    memcpy(bytes.data(), &headptr, sizeof(uint32_t));
    write(watchesAddr(start_idx), bytes.size(), bytes);
}

void Watches::writeSize(int start_idx, const uint32_t size) {
    std::vector<uint8_t> bytes(sizeof(uint32_t));
    memcpy(bytes.data(), &size, sizeof(uint32_t));
    write(watchesAddr(start_idx) + sizeof(uint32_t), bytes.size(), bytes);
}

WatcherBlock Watches::readBlock(uint32_t addr, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "Read watcher block at 0x%x\n", addr);
    readBurst(addr, block_size, worker_id);

    WatcherBlock block;
    memcpy(&block, reorder_buffer->getResponse(worker_id).data(), sizeof(WatcherBlock));
    return block;
}

void Watches::writeBlock(uint32_t addr, const WatcherBlock& block) {
    output.verbose(CALL_INFO, 7, 0, "Write watcher block at 0x%x\n", addr);
    std::vector<uint8_t> data(block_size, 0);
    memcpy(data.data(), &block, sizeof(WatcherBlock));
    writeBurst(addr, data);
}

void Watches::updateBlock(int lit_idx, uint32_t prev_addr, uint32_t curr_addr, 
                          WatcherBlock& prev_block, WatcherBlock& curr_block) {
    // have removed some watchers, update the blocks accordingly
    if (curr_block.countValidNodes() == 0) {
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
        
        // Set size in metadata
        metadata[lit_idx].size = watch_list.size();
        
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
                block.next_block = first_block_addr + ((block_idx + 1) * block_size);
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

// Insert a new watcher for a literal
int Watches::insertWatcher(int lit_idx, Cref clause_addr, Lit blocker, int worker_id) {
    if (busy.find(lit_idx) != busy.end()) {
        output.fatal(CALL_INFO, -1, "Watches: Already busy with var %d\n", lit_idx/2);
    }
    busy.insert(lit_idx);
    
    int block_visits = 1; // Start with 1 for metadata read

    // Read current metadata
    WatchMetaData metadata = readMetaData(lit_idx, worker_id);

    // Case 1: Check if there's room in pre-watchers
    for (int i = 0; i < PRE_WATCHERS; i++) {
        if (!metadata.pre_watchers[i].valid) {
            metadata.pre_watchers[i] = WatcherNode(clause_addr, blocker);
            metadata.size++;
            writeMetaData(lit_idx, metadata);
            
            busy.erase(lit_idx);
            output.verbose(CALL_INFO, 7, 0, 
                "Worker[%d] Inserted watcher in pre_watcher[%d], clause 0x%x, var %d\n", 
                worker_id, i, clause_addr, lit_idx/2);
            return block_visits;
        }
    }

    uint32_t curr_addr = metadata.head_ptr;

    // Case 2: Empty block list - create first block
    if (curr_addr == 0) {
        uint32_t new_block_addr = allocateBlock();
        WatcherBlock new_block;
        new_block.nodes[0] = WatcherNode(clause_addr, blocker);
        writeBlock(new_block_addr, new_block);
        block_visits++; // Count new block write
        metadata.head_ptr = new_block_addr;
        metadata.size++;
        writeMetaData(lit_idx, metadata);

        busy.erase(lit_idx);
        output.verbose(CALL_INFO, 7, 0, 
            "Worker[%d] Inserted watcher in empty block list, clause 0x%x, var %d\n", 
            worker_id, clause_addr, lit_idx/2);
        return block_visits;
    }
    
    // Case 3: Search all blocks for a free slot
    while (curr_addr != 0) {
        WatcherBlock curr_block = readBlock(curr_addr, worker_id);
        block_visits++; // Count each block read

        // Check if this block has a free slot
        for (size_t i = 0; i < PROPAGATORS; i++) {
            if (!curr_block.nodes[i].valid) {
                // Found a free slot
                curr_block.nodes[i] = WatcherNode(clause_addr, blocker);
                writeBlock(curr_addr, curr_block);
                writeSize(lit_idx, metadata.size + 1);
                busy.erase(lit_idx);
                output.verbose(CALL_INFO, 7, 0, 
                    "Worker[%d] Inserted watcher in slot %zu, clause 0x%x, var %d\n", 
                    worker_id, i, clause_addr, lit_idx/2);
                return block_visits;
            }
        }
        curr_addr = curr_block.next_block;
    }

    // Case 4: all blocks are full - add a new block at front
    uint32_t new_block_addr = allocateBlock();
    
    WatcherBlock new_block;
    new_block.nodes[0] = WatcherNode(clause_addr, blocker);
    new_block.next_block = metadata.head_ptr;  // Link to current head
    writeBlock(new_block_addr, new_block);
    block_visits++; // Count new block write
    
    metadata.head_ptr = new_block_addr;
    metadata.size++;
    writeMetaData(lit_idx, metadata);

    busy.erase(lit_idx);
    output.verbose(CALL_INFO, 7, 0, 
        "Worker[%d] Inserted watcher in new block, clause 0x%x, var %d\n", 
        worker_id, clause_addr, lit_idx/2);
    return block_visits;
}

// Remove a watcher with given clause address
void Watches::removeWatcher(int lit_idx, Cref clause_addr, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, 
        "Removing watcher for clause 0x%x at var %d\n", clause_addr, lit_idx/2);

    // Read metadata
    WatchMetaData metadata = readMetaData(lit_idx, worker_id);
    if (metadata.size == 0) output.fatal(CALL_INFO, -1, "Empty watch list for var %d\n", lit_idx/2);

    // First check pre-watchers
    for (int i = 0; i < PRE_WATCHERS; i++) {
        if (metadata.pre_watchers[i].valid && 
            metadata.pre_watchers[i].getClauseAddr() == clause_addr) {
            // Found in pre-watchers, invalidate it
            metadata.pre_watchers[i].valid = 0;
            metadata.size--;
            writeMetaData(lit_idx, metadata);
            
            output.verbose(CALL_INFO, 7, 0, 
                "Removed watcher for clause 0x%x at var %d from pre_watcher[%d]\n", 
                clause_addr, lit_idx/2, i);
            return;
        }
    }
    
    uint32_t curr_addr = metadata.head_ptr;
    uint32_t prev_addr = 0;
    WatcherBlock prev_block;

    while (curr_addr != 0) {
        WatcherBlock curr_block = readBlock(curr_addr, worker_id);
        
        // Search for the clause in this block
        for (size_t i = 0; i < PROPAGATORS; i++) {
            if (curr_block.nodes[i].valid && curr_block.nodes[i].getClauseAddr() == clause_addr) {
                // Found the clause, invalidate this node
                curr_block.nodes[i].valid = 0;
                writeSize(lit_idx, metadata.size - 1);
                
                // Check if the entire block is now empty
                if (curr_block.countValidNodes() == 0) {
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
                        "Removed watcher for clause 0x%x at var %d, freed empty block 0x%x\n", 
                        clause_addr, lit_idx/2, curr_addr);
                } else {
                    // Block still has valid nodes, just update it
                    writeBlock(curr_addr, curr_block);
                    
                    output.verbose(CALL_INFO, 7, 0, 
                        "Removed watcher for clause 0x%x at var %d, updated block 0x%x\n", 
                        clause_addr, lit_idx/2, curr_addr);
                }
                
                return;
            }
        }
        
        prev_addr = curr_addr;
        prev_block = curr_block;
        curr_addr = curr_block.next_block;
    }

    output.fatal(CALL_INFO, -1, "Remove failed clause 0x%x, var %d\n", clause_addr, lit_idx/2);
}
