#include "trace_writer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// CRC32 (IEEE 802.3, reflected). Used only for the FINISH sentinel.
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_ready = true;
    }
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

} // namespace

TraceWriter::TraceWriter() = default;

TraceWriter::~TraceWriter() {
    if (fp_) close(0);
    if (buf_) { std::free(buf_); buf_ = nullptr; }
}

bool TraceWriter::open(const std::string& path, size_t ring_bytes) {
    if (ring_bytes < 4096) ring_bytes = 4096;
    size_t alloc = ring_bytes + 64;
    buf_ = static_cast<uint8_t*>(std::malloc(alloc));
    if (!buf_) return false;
    cap_ = ring_bytes;
    pos_ = 0;

    fp_ = std::fopen(path.c_str(), "wb");
    if (!fp_) {
        std::free(buf_);
        buf_ = nullptr;
        return false;
    }
    // Hint sequential access to the page cache.
    std::setvbuf(fp_, nullptr, _IONBF, 0);
    return true;
}

void TraceWriter::writeHeader(const std::string& cnf_path, uint64_t seed,
                              uint32_t num_vars, uint32_t num_clauses) {
    if (!fp_ || failed_) return;
    // 8-byte magic
    const char magic[8] = {'S','S','T','S','A','T','\0','\0'};
    if (std::fwrite(magic, 1, 8, fp_) != 8) { failed_ = true; return; }
    bytes_ += 8;

    char line[1024];
    int n;
    n = std::snprintf(line, sizeof(line), "version=1.0\n");          std::fwrite(line, 1, (size_t)n, fp_); bytes_ += n;
    n = std::snprintf(line, sizeof(line), "cnf=%s\n", cnf_path.c_str()); std::fwrite(line, 1, (size_t)n, fp_); bytes_ += n;
    n = std::snprintf(line, sizeof(line), "seed=%llu\n",
                      (unsigned long long)seed);                     std::fwrite(line, 1, (size_t)n, fp_); bytes_ += n;
    n = std::snprintf(line, sizeof(line), "num_vars=%u\n", num_vars);   std::fwrite(line, 1, (size_t)n, fp_); bytes_ += n;
    n = std::snprintf(line, sizeof(line), "num_clauses=%u\n", num_clauses); std::fwrite(line, 1, (size_t)n, fp_); bytes_ += n;
    static const char marker[] = "---\n";
    std::fwrite(marker, 1, 4, fp_); bytes_ += 4;
}

void TraceWriter::ensureSpace(size_t n) {
    if (pos_ + n >= cap_) flushLocked();
}

void TraceWriter::flushLocked() {
    if (!fp_ || pos_ == 0) { pos_ = 0; return; }
    if (failed_) { pos_ = 0; return; }
    size_t wrote = std::fwrite(buf_, 1, pos_, fp_);
    if (wrote != pos_) { failed_ = true; pos_ = 0; return; }
    crc_ = crc32_update(crc_, buf_, pos_);
    bytes_ += pos_;
    pos_ = 0;
}

void TraceWriter::writeRaw(const void* data, size_t n) {
    if (!fp_ || failed_) return;
    if (pos_ + n >= cap_) flushLocked();
    if (n >= cap_) {
        // oversized: flush buffer and write direct
        if (std::fwrite(data, 1, n, fp_) != n) { failed_ = true; return; }
        crc_ = crc32_update(crc_, static_cast<const uint8_t*>(data), n);
        bytes_ += n;
        return;
    }
    std::memcpy(buf_ + pos_, data, n);
    pos_ += n;
}

void TraceWriter::emitPhase(uint8_t phase) {
    if (!fp_ || failed_) return;
    ensureSpace(2);
    buf_[pos_++] = TAG_PHASE;
    buf_[pos_++] = phase;
    ++events_;
}

void TraceWriter::emitLevel(int32_t level) {
    if (!fp_ || failed_) return;
    ensureSpace(11);
    buf_[pos_++] = TAG_LEVEL;
    uint8_t* p = writeUVarint(buf_ + pos_, (uint64_t)(level < 0 ? 0 : level));
    pos_ = (size_t)(p - buf_);
    ++events_;
}

void TraceWriter::emitTick(uint64_t cycle) {
    if (!fp_ || failed_) return;
    // Delta from the previous TICK. Use last_addr_[DS_UNKNOWN] as a
    // convenient scratch slot so the fast path's 9-slot array can double
    // as cycle state — the DS_UNKNOWN bucket is never hit by real mem
    // events with a configured DsMap.
    static thread_local uint64_t last_cycle = 0;
    // Per-instance state would be cleaner; keep simple for v1.
    uint64_t delta = cycle - last_cycle;
    last_cycle = cycle;

    ensureSpace(11);
    buf_[pos_++] = TAG_TICK;
    uint8_t* p = writeUVarint(buf_ + pos_, delta);
    pos_ = (size_t)(p - buf_);
    ++events_;
}

void TraceWriter::emitDecision(int var, bool sign, int new_level) {
    if (!fp_ || failed_) return;
    ensureSpace(1 + 10 + 1 + 10);
    buf_[pos_++] = TAG_DECIDE;
    uint8_t* p = writeUVarint(buf_ + pos_, (uint64_t)(var < 0 ? 0 : var));
    pos_ = (size_t)(p - buf_);
    buf_[pos_++] = sign ? 1 : 0;
    p = writeUVarint(buf_ + pos_, (uint64_t)(new_level < 0 ? 0 : new_level));
    pos_ = (size_t)(p - buf_);
    ++events_;
}

void TraceWriter::emitEnqueue(int var, bool sign, int reason_cref) {
    if (!fp_ || failed_) return;
    ensureSpace(1 + 10 + 1 + 10);
    buf_[pos_++] = TAG_ENQUEUE;
    uint8_t* p = writeUVarint(buf_ + pos_, (uint64_t)(var < 0 ? 0 : var));
    pos_ = (size_t)(p - buf_);
    buf_[pos_++] = sign ? 1 : 0;
    p = writeSVarint(buf_ + pos_, (int64_t)reason_cref);
    pos_ = (size_t)(p - buf_);
    ++events_;
}

void TraceWriter::emitConflict(int cref) {
    if (!fp_ || failed_) return;
    ensureSpace(1 + 10);
    buf_[pos_++] = TAG_CONFLICT;
    uint8_t* p = writeUVarint(buf_ + pos_, (uint64_t)(cref < 0 ? 0 : cref));
    pos_ = (size_t)(p - buf_);
    ++events_;
}

void TraceWriter::emitLearn(int lbd, int clause_size, int bt_level, int new_cref) {
    if (!fp_ || failed_) return;
    ensureSpace(1 + 4*10);
    buf_[pos_++] = TAG_LEARN;
    uint8_t* p = buf_ + pos_;
    p = writeUVarint(p, (uint64_t)(lbd < 0 ? 0 : lbd));
    p = writeUVarint(p, (uint64_t)(clause_size < 0 ? 0 : clause_size));
    p = writeUVarint(p, (uint64_t)(bt_level < 0 ? 0 : bt_level));
    p = writeUVarint(p, (uint64_t)(new_cref < 0 ? 0 : new_cref));
    pos_ = (size_t)(p - buf_);
    ++events_;
}

void TraceWriter::emitBacktrack(int from_level, int to_level) {
    if (!fp_ || failed_) return;
    ensureSpace(1 + 2*10);
    buf_[pos_++] = TAG_BACKTRACK;
    uint8_t* p = buf_ + pos_;
    p = writeUVarint(p, (uint64_t)(from_level < 0 ? 0 : from_level));
    p = writeUVarint(p, (uint64_t)(to_level   < 0 ? 0 : to_level));
    pos_ = (size_t)(p - buf_);
    ++events_;
}

void TraceWriter::emitRestart(int restart_idx) {
    if (!fp_ || failed_) return;
    ensureSpace(1 + 10);
    buf_[pos_++] = TAG_RESTART;
    uint8_t* p = writeUVarint(buf_ + pos_, (uint64_t)(restart_idx < 0 ? 0 : restart_idx));
    pos_ = (size_t)(p - buf_);
    ++events_;
}

void TraceWriter::emitReduce(int removed, int kept) {
    if (!fp_ || failed_) return;
    ensureSpace(1 + 2*10);
    buf_[pos_++] = TAG_REDUCE;
    uint8_t* p = buf_ + pos_;
    p = writeUVarint(p, (uint64_t)(removed < 0 ? 0 : removed));
    p = writeUVarint(p, (uint64_t)(kept    < 0 ? 0 : kept));
    pos_ = (size_t)(p - buf_);
    ++events_;
}

void TraceWriter::close(uint64_t total_cycles) {
    if (!fp_) return;
    if (!failed_) {
        // Drain ring buffer, then append the FINISH record and flush once more.
        flushLocked();

        uint8_t finish[1 + 8 + 8 + 4];
        size_t p = 0;
        finish[p++] = TAG_FINISH;
        auto wu64 = [&](uint64_t v) {
            for (int i = 0; i < 8; ++i) finish[p++] = (uint8_t)((v >> (8*i)) & 0xFF);
        };
        auto wu32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i) finish[p++] = (uint8_t)((v >> (8*i)) & 0xFF);
        };
        wu64(total_cycles);
        wu64(events_);
        uint32_t final_crc = crc_ ^ 0xFFFFFFFFu;
        wu32(final_crc);

        std::fwrite(finish, 1, sizeof(finish), fp_);
        bytes_ += sizeof(finish);
    }
    std::fclose(fp_);
    fp_ = nullptr;
}
