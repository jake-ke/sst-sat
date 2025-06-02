#include <sst/core/sst_config.h>
#include "async_watches.h"

uint64_t Watches::allocateNode() {
    // First check if we have recycled nodes available
    if (!free_nodes.empty()) {
        uint64_t addr = free_nodes.front();
        free_nodes.pop();
        return addr;
    }
    
    // Otherwise, allocate a new node
    uint64_t addr = next_free_node;
    next_free_node += sizeof(WatcherNode);
    output.verbose(CALL_INFO, 7, 0, "Allocating new node at 0x%lx\n", addr);
    return addr;
}

// Read head pointer for a literal
void Watches::readHeadPointer(int lit_idx) {
    output.verbose(CALL_INFO, 7, 0, "Read head pointer for var %d\n", lit_idx/2);
    memory->send(new SST::Interfaces::StandardMem::Read(
        watchesAddr(lit_idx), sizeof(uint64_t)));
    busy = true;
    (**yield_ptr)();
}

// Write head pointers (for initialization)
void Watches::writeHeadPointers(int start_idx, const std::vector<uint64_t>& ptrs) {
    int count = ptrs.size();
    std::vector<uint8_t> data(count * sizeof(uint64_t));
    memcpy(data.data(), ptrs.data(), count * sizeof(uint64_t));
    
    output.verbose(CALL_INFO, 7, 0, 
        "Write head pointers[%d], count %d, 0x%lx\n", start_idx, count, ptrs[0]);
    memory->send(new SST::Interfaces::StandardMem::Write(
        watchesAddr(start_idx), count * sizeof(uint64_t), data));
    busy = true;
    (**yield_ptr)();
}

// Read a watcher node from memory
void Watches::readNode(uint64_t addr) {
    output.verbose(CALL_INFO, 7, 0, "Read watcher node at 0x%lx\n", addr);
    memory->send(new SST::Interfaces::StandardMem::Read(
        addr, sizeof(WatcherNode)));
    busy = true;
    (**yield_ptr)();
}

// Write a watcher node to memory
void Watches::writeNode(uint64_t addr, const WatcherNode& node) {
    std::vector<uint8_t> data(sizeof(WatcherNode));
    memcpy(data.data(), &node, sizeof(WatcherNode));
    
    output.verbose(CALL_INFO, 7, 0, "Write watcher node at 0x%lx\n", addr);
    memory->send(new SST::Interfaces::StandardMem::Write(
        addr, sizeof(WatcherNode), data));
    busy = true;
    (**yield_ptr)();
}

void Watches::initWatches(size_t watch_count, std::vector<Clause>& clauses) {
    output.verbose(CALL_INFO, 3, 0, "Initializing %zu watch lists\n", watch_count);
    
    // First, allocate and initialize head pointers to null
    std::vector<uint64_t> head_ptrs(watch_count, 0);
    num_watches = watch_count;
    
    // Create temporary data structure to build the linked lists
    std::vector<std::vector<std::pair<int, Lit>>> tmp_watches(watch_count);
    
    // Add watchers for all clauses
    for (int ci = 0; ci < clauses.size(); ci++) {
        Clause& c = clauses[ci];
        if (c.size() >= 2) {
            // Watch the first two literals
            int idx0 = toWatchIndex(~c.literals[0]);
            int idx1 = toWatchIndex(~c.literals[1]);
            
            // Add watchers to the temporary structure
            tmp_watches[idx0].push_back(std::make_pair(ci, c.literals[1]));
            tmp_watches[idx1].push_back(std::make_pair(ci, c.literals[0]));
        }
    }
    
    // Calculate total number of watcher nodes needed
    size_t total_nodes = 0;
    for (auto& watch_list : tmp_watches) {
        total_nodes += watch_list.size();
    }
    output.verbose(CALL_INFO, 3, 0, "Creating %zu watcher nodes\n", total_nodes);
    
    if (total_nodes == 0) {
        // If no nodes, just write empty head pointers
        memory->send(new SST::Interfaces::StandardMem::Write(
            watches_base_addr, watch_count * sizeof(uint64_t), 
            std::vector<uint8_t>(watch_count * sizeof(uint64_t), 0)));
        busy = true;
        (**yield_ptr)();
        return;
    }
    
    // Allocate memory for all nodes at once
    std::vector<WatcherNode> all_nodes(total_nodes);
    std::vector<uint64_t> node_addrs(total_nodes);
    
    // Build linked lists and node addresses
    size_t node_idx = 0;
    for (size_t lit_idx = 0; lit_idx < watch_count; lit_idx++) {
        auto& watch_list = tmp_watches[lit_idx];
        if (watch_list.empty()) continue;
        
        // Set head pointer for this watch list
        head_ptrs[lit_idx] = nodes_base_addr + node_idx * sizeof(WatcherNode);
        
        // Build linked list for this literal
        for (size_t i = 0; i < watch_list.size(); i++) {
            auto& watcher = watch_list[i];
            node_addrs[node_idx] = nodes_base_addr + node_idx * sizeof(WatcherNode);
            
            uint64_t next_addr = 0;
            if (i < watch_list.size() - 1) {
                next_addr = nodes_base_addr + (node_idx + 1) * sizeof(WatcherNode);
            }
            
            all_nodes[node_idx] = WatcherNode(watcher.first, watcher.second, next_addr);
            node_idx++;
        }
    }
    
    // Write all nodes to memory in one operation
    std::vector<uint8_t> node_data(total_nodes * sizeof(WatcherNode));
    memcpy(node_data.data(), all_nodes.data(), node_data.size());
    
    memory->send(new SST::Interfaces::StandardMem::Write(
        nodes_base_addr, node_data.size(), node_data));
    busy = true;
    (**yield_ptr)();
    
    // Update next_free_node to point after our allocated nodes
    next_free_node = nodes_base_addr + total_nodes * sizeof(WatcherNode);
    
    // Write all head pointers in one operation
    std::vector<uint8_t> ptr_data(watch_count * sizeof(uint64_t));
    memcpy(ptr_data.data(), head_ptrs.data(), ptr_data.size());
    
    memory->send(new SST::Interfaces::StandardMem::Write(
        watches_base_addr, ptr_data.size(), ptr_data));
    busy = true;
    (**yield_ptr)();
    
    output.verbose(CALL_INFO, 3, 0, 
        "Watch initialization complete: wrote %zu nodes, next free at 0x%lx\n", 
        total_nodes, next_free_node);
}

// Insert a new watcher for a literal
void Watches::insertWatcher(int lit_idx, int clause_idx, Lit blocker) {
    // Read current head pointer
    readHeadPointer(lit_idx);
    uint64_t head = getLastHeadPointer();
    
    // Allocate new node
    uint64_t new_node_addr = allocateNode();
    WatcherNode new_node(clause_idx, blocker, head);
    
    // Write the new node
    writeNode(new_node_addr, new_node);
    
    // Update head pointer
    std::vector<uint64_t> new_head(1, new_node_addr);
    writeHeadPointers(lit_idx, new_head);
    
    output.verbose(CALL_INFO, 7, 0, 
        "Inserted watcher for clause %d at var %d, addr 0x%lx\n", 
        clause_idx, lit_idx/2, new_node_addr);
}

// Remove a watcher with given clause index
void Watches::removeWatcher(int lit_idx, int clause_idx) {
    // Read head pointer
    readHeadPointer(lit_idx);
    uint64_t head = getLastHeadPointer();
    if (head == 0) output.fatal(CALL_INFO, -1, "Empty watch list for var %d\n", lit_idx/2);
    
    // Read the first node
    readNode(head);
    WatcherNode current = getLastReadNode();
    
    // If first node matches, update head pointer
    if (current.clause_idx == clause_idx) {
        writeHeadPointers(lit_idx, {current.next});
        
        freeNode(head);  // Add node to free list for recycling
        return;
    }
    
    // Otherwise, traverse the list
    uint64_t prev_addr = head;
    while (current.next != 0) {
        readNode(current.next);
        WatcherNode next = getLastReadNode();
        if (next.clause_idx == clause_idx) {
            freeNode(current.next);  // Add removed node to free list
            current.next = next.next;  // Update previous node's next pointer
            writeNode(prev_addr, current);
            return;
        }
        
        prev_addr = current.next;
        current = next;
    }
    
    output.fatal(CALL_INFO, -1, "Remove failed clause %d, var %d\n", clause_idx, lit_idx/2);
}

// Handle memory response
void Watches::handleMem(SST::Interfaces::StandardMem::Request* req) {
    output.verbose(CALL_INFO, 8, 0, "handleMem\n");
    busy = false;
    
    if (auto* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        uint64_t addr = resp->pAddr;
        
        // Determine if this is a head pointer read or a node read based on address range
        if (addr >= watches_base_addr && addr < watches_base_addr + num_watches * sizeof(uint64_t)) {
            // Head pointer read
            memcpy(&last_head_ptr, resp->data.data(), sizeof(uint64_t));
            output.verbose(CALL_INFO, 7, 0, 
                "Head pointer read response: 0x%lx for address 0x%lx\n", last_head_ptr, addr);
        } else if (addr >= nodes_base_addr) {
            // Node read
            memcpy(&last_node, resp->data.data(), sizeof(WatcherNode));
            output.verbose(CALL_INFO, 7, 0, 
                "Node read response: clause=%d, blocker=%d, next=0x%lx for address 0x%lx\n", 
                last_node.clause_idx, toInt(last_node.blocker), last_node.next, addr);
        } else {
            output.fatal(CALL_INFO, -1, 
                "Memory response for unknown address range: 0x%lx\n", addr);
        }
    }
}
