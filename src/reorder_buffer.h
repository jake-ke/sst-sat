#ifndef REORDER_BUFFER_H
#define REORDER_BUFFER_H

#include <unordered_map>
#include <vector>
#include <cstdint>

class ReorderBuffer {
public:
    // Register a request ID with its worker ID
    void registerRequest(uint64_t req_id, int worker_id) {
        req_to_worker[req_id] = worker_id;
    }

    int lookUpWorkerId(uint64_t req_id) {
        auto it = req_to_worker.find(req_id);
        // discarded requests may happen
        if (it == req_to_worker.end()) return -1;
        return it->second;
    }
    
    // Store response data for a specific request ID
    void storeResponse(uint64_t req_id, const std::vector<uint8_t> data, bool burst = false, uint64_t offset = 0) {
        int worker_id = lookUpWorkerId(req_id);
        if (worker_id < 0) return;  // invalid worker_id, skip storing
        if (burst) {
            std::memcpy(worker_to_data[worker_id].data() + offset, data.data(), data.size());
        }
        else worker_to_data[worker_id] = data;
        req_to_worker.erase(req_id);  // Clean up the mapping after use
    }
    
    // Store data directly by worker ID (for store queue forwarding)
    void storeDataByWorkerId(int worker_id, const std::vector<uint8_t>& data, bool burst = false, uint64_t offset = 0) {
        if (burst) std::memcpy(worker_to_data[worker_id].data() + offset, data.data(), data.size());
        else worker_to_data[worker_id] = data;
    }
    
    // Retrieve response data for a specific worker ID
    const std::vector<uint8_t>& getResponse(int worker_id) const {
        auto it = worker_to_data.find(worker_id);
        assert(it != worker_to_data.end());
        return it->second;
    }

    void reset() {
        req_to_worker.clear();
        worker_to_data.clear();
    }

    void startBurst(int worker_id, uint64_t bytes) { worker_to_data[worker_id].resize(bytes); }
    

private:
    std::unordered_map<uint64_t, int> req_to_worker;  // Maps request ID to worker ID
    std::unordered_map<int, std::vector<uint8_t>> worker_to_data;  // Maps worker ID to response data
};

#endif // REORDER_BUFFER_H
