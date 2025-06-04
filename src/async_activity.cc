#include <sst/core/sst_config.h>
#include "async_activity.h"

Activity::Activity(int verbose, SST::Interfaces::StandardMem* mem, 
                   uint64_t base_addr, coro_t::push_type** yield_ptr)
    : memory(mem), yield_ptr(yield_ptr), base_addr(base_addr),
      size_(0), busy(false) {
    output.init("ACTIVITY-> ", verbose, 0, SST::Output::STDOUT);
}

double Activity::read(size_t idx) {
    if (idx >= size_) {
        output.fatal(CALL_INFO, -1, "Activity index out of range: %zu\n", idx);
    }
    
    memory->send(new SST::Interfaces::StandardMem::Read(calcAddr(idx), sizeof(double)));
    busy = true;
    (**yield_ptr)();
    
    memcpy(&last_value, last_buffer.data(), sizeof(double));
    return last_value;
}

void Activity::write(size_t idx, double value) {
    std::vector<uint8_t> buffer(sizeof(double));
    memcpy(buffer.data(), &value, sizeof(double));
    
    memory->send(new SST::Interfaces::StandardMem::Write(
        calcAddr(idx), sizeof(double), buffer));
    busy = true;
    (**yield_ptr)();
}

std::vector<double> Activity::readBulk(size_t start, size_t count) {
    if (start + count > size_) {
        output.fatal(CALL_INFO, -1, "Bulk read out of range: %zu + %zu > %zu\n", 
                     start, count, size_);
    }
    
    memory->send(new SST::Interfaces::StandardMem::Read(
        calcAddr(start), count * sizeof(double)));
    busy = true;
    (**yield_ptr)();
    
    std::vector<double> result(count);
    memcpy(result.data(), last_buffer.data(), count * sizeof(double));
    return result;
}

void Activity::writeBulk(size_t start, const std::vector<double>& values) {
    size_t count = values.size();
    if (start + count > size_) {
        output.fatal(CALL_INFO, -1, "Bulk write out of range: %zu + %zu > %zu\n", 
                     start, count, size_);
    }
    
    std::vector<uint8_t> buffer(count * sizeof(double));
    memcpy(buffer.data(), values.data(), buffer.size());
    
    memory->send(new SST::Interfaces::StandardMem::Write(
        calcAddr(start), buffer.size(), buffer));
    busy = true;
    (**yield_ptr)();
}

void Activity::initialize(size_t count, double init_value) {
    size_ = count;
    if (count == 0) return;
    
    std::vector<double> values(count, init_value);
    writeBulk(0, values);
}

void Activity::push(double value) {
    output.verbose(CALL_INFO, 7, 0, 
                   "Push new value %f at index %zu\n", value, size_);
    write(size_, value);

    size_++;
}

void Activity::rescaleAll(double factor) {
    std::vector<double> values = readBulk(0, size_);
    
    for (size_t i = 0; i < size_; i++) {
        values[i] *= factor;
    }
    writeBulk(0, values);
}

void Activity::reduceDB(const std::vector<bool>& to_remove, size_t num_orig) {
    // Bulk read all activity values at once
    std::vector<double> activities = readBulk(0, size_);
    
    // Compact activities by removing marked ones
    std::vector<double> compacted;
    for (size_t i = 0; i < size_; i++)
    if (!to_remove[i + num_orig])
    compacted.push_back(activities[i]);
    
    // Bulk write compacted activities back if not empty
    if (!compacted.empty()) writeBulk(0, compacted);
    
    size_ = compacted.size();
    output.verbose(CALL_INFO, 7, 0, "ACTIVITY: Reduced from %zu to %zu\n", activities.size(), size_);
}

void Activity::handleMem(SST::Interfaces::StandardMem::Request* req) {
    busy = false;
    
    if (auto* resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        last_buffer = resp->data;
    }
}
