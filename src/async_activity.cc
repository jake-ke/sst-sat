#include <sst/core/sst_config.h>
#include "async_activity.h"

void Activity::push(double value) {
    output.verbose(CALL_INFO, 7, 0, "Push new value %f at index %zu\n", value, size_);
    std::vector<uint8_t> buffer(sizeof(double));
    memcpy(buffer.data(), &value, sizeof(double));
    write(calcAddr(size_), sizeof(double), buffer);
    
    size_++;
}

void Activity::rescaleAll(double factor) {
    std::vector<double> values = readBurstAct(calcAddr(0), size_);
    
    for (size_t i = 0; i < size_; i++) {
        values[i] *= factor;
    }
    
    // Write back
    std::vector<uint8_t> buffer(size_ * sizeof(double));
    memcpy(buffer.data(), values.data(), buffer.size());
    writeBurst(calcAddr(0), sizeof(double), buffer);
}

void Activity::reduceDB(const std::vector<double>& activities, const std::vector<bool>& to_remove) {
    // Compact activities by removing marked ones
    std::vector<double> compacted;
    for (size_t i = 0; i < size_; i++) {
        if (!to_remove[i]) compacted.push_back(activities[i]);
    }
    
    // Bulk write compacted activities back
    std::vector<uint8_t> buffer(compacted.size() * sizeof(double));
    memcpy(buffer.data(), compacted.data(), buffer.size());
    writeBurst(calcAddr(0), sizeof(double), buffer);
    
    size_ = compacted.size();
    output.verbose(CALL_INFO, 7, 0, "ACTIVITY: Reduced from %zu to %zu\n", activities.size(), size_);
}

