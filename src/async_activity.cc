#include <sst/core/sst_config.h>
#include "async_activity.h"

double Activity::readAct(size_t idx, int worker_id) {
    output.verbose(CALL_INFO, 7, 0, "Read activity at index %zu\n", idx);
    read(calcAddr(idx), sizeof(double), worker_id);

    double value;
    memcpy(&value, reorder_buffer->getResponse(worker_id).data(), sizeof(double));
    return value;
}

std::vector<double> Activity::readBurstAct(size_t start, size_t count, int worker_id) {
    if (start + count > size_) {
        output.fatal(CALL_INFO, -1, "Activity read out of range: %zu + %zu > %zu\n", 
                        start, count, size_);
    }
    std::vector<double> result(count);
    readBurst(calcAddr(start), count * sizeof(double), worker_id);

    memcpy(result.data(), reorder_buffer->getResponse(worker_id).data(), count * sizeof(double));
    return result;
}

void Activity::push(double value) {
    output.verbose(CALL_INFO, 7, 0, "Push new value %f at index %zu\n", value, size_);
    std::vector<uint8_t> buffer(sizeof(double));
    memcpy(buffer.data(), &value, sizeof(double));
    write(calcAddr(size_), sizeof(double), buffer);
    
    size_++;
}

void Activity::rescaleAll(double factor, int worker_id) {
    std::vector<double> values = readBurstAct(0, size_, worker_id);

    for (size_t i = 0; i < size_; i++) {
        values[i] *= factor;
    }
    
    // Write back
    std::vector<uint8_t> buffer(size_ * sizeof(double));
    memcpy(buffer.data(), values.data(), buffer.size());
    writeBurst(calcAddr(0), buffer);
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
    writeBurst(calcAddr(0), buffer);

    size_ = compacted.size();
    output.verbose(CALL_INFO, 7, 0, "ACTIVITY: Reduced from %zu to %zu\n", activities.size(), size_);
}

