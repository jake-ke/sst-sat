#ifndef TRACE_WRITER_H
#define TRACE_WRITER_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Compact binary trace writer for the SAT solver simulator.
//
// Format spec: TRACE_FORMAT.md (repo root).
// Thread model: single-threaded. SST clockTick and coroutines run on one OS
// thread; no locking. Do not reuse this class from multiple threads.
class TraceWriter {
public:
    enum DsId : uint8_t {
        DS_HEAP        = 0,
        DS_INDICES     = 1,
        DS_VARIABLES   = 2,
        DS_WATCHES     = 3,
        DS_WATCH_NODES = 4,
        DS_CLAUSES_CMD = 5,
        DS_CLAUSES     = 6,
        DS_VAR_ACT     = 7,
        DS_UNKNOWN     = 8,
        DS_COUNT       = 9,
    };

    struct DsMap {
        DsId nibble[16];
        DsMap() { for (int i = 0; i < 16; ++i) nibble[i] = DS_UNKNOWN; }
    };

    // Event tag bytes (see TRACE_FORMAT.md).
    enum Tag : uint8_t {
        TAG_TICK      = 0x00,
        TAG_PHASE     = 0x01,
        TAG_LEVEL     = 0x02,
        TAG_MEM_READ  = 0x10,
        TAG_MEM_WRITE = 0x11,
        TAG_DECIDE    = 0x20,
        TAG_ENQUEUE   = 0x21,
        TAG_CONFLICT  = 0x22,
        TAG_LEARN     = 0x23,
        TAG_BACKTRACK = 0x24,
        TAG_RESTART   = 0x25,
        TAG_REDUCE    = 0x26,
        TAG_FINISH    = 0x7f,
    };

    TraceWriter();
    ~TraceWriter();

    bool open(const std::string& path, size_t ring_bytes);
    bool enabled() const { return fp_ != nullptr && !failed_; }
    uint64_t events() const { return events_; }

    void setDsMap(const DsMap& m) { ds_map_ = m; }
    void writeHeader(const std::string& cnf_path, uint64_t seed,
                     uint32_t num_vars, uint32_t num_clauses);

    // Fast path: inlined. Every MEM_READ/MEM_WRITE funnels through here.
    // Encoding (per TRACE_FORMAT.md):
    //   byte 0: tag = 0x10 (read) or 0x11 (write)
    //   byte 1: packed: bits 6..4 = size_class, bits 3..0 = ds_id
    //   bytes 2..: zigzag varint `addr - last_addr_per_ds[ds]`
    //   [if size_class == 7]: uvarint explicit size
    inline void emitMem(bool is_write, uint64_t addr, uint32_t size) {
        if (__builtin_expect(fp_ == nullptr || failed_, 0)) return;
        if (__builtin_expect(pos_ + 32 >= cap_, 0)) flushLocked();

        DsId ds = classify(addr);
        uint8_t size_class;
        switch (size) {
            case 1:  size_class = 0; break;
            case 2:  size_class = 1; break;
            case 4:  size_class = 2; break;
            case 8:  size_class = 3; break;
            case 16: size_class = 4; break;
            case 32: size_class = 5; break;
            case 64: size_class = 6; break;
            default: size_class = 7; break;
        }

        uint8_t* p = buf_ + pos_;
        *p++ = is_write ? TAG_MEM_WRITE : TAG_MEM_READ;
        *p++ = (uint8_t)((size_class & 0x7) << 4) | (uint8_t)(ds & 0xF);

        int64_t delta = (int64_t)addr - (int64_t)last_addr_[ds];
        last_addr_[ds] = addr;
        p = writeSVarint(p, delta);
        if (size_class == 7) p = writeUVarint(p, size);

        pos_ = (size_t)(p - buf_);
        ++events_;
    }

    // Static context — emit only when changed. Cold path, not inlined.
    void emitPhase(uint8_t phase);
    void emitLevel(int32_t level);
    void emitTick(uint64_t cycle);

    // Algorithm events — cold path.
    void emitDecision(int var, bool sign, int new_level);
    void emitEnqueue(int var, bool sign, int reason_cref);
    void emitConflict(int cref);
    void emitLearn(int lbd, int clause_size, int bt_level, int new_cref);
    void emitBacktrack(int from_level, int to_level);
    void emitRestart(int restart_idx);
    void emitReduce(int removed, int kept);

    void close(uint64_t total_cycles);

private:
    static inline uint8_t* writeUVarint(uint8_t* p, uint64_t v) {
        while (v >= 0x80) {
            *p++ = (uint8_t)(v | 0x80);
            v >>= 7;
        }
        *p++ = (uint8_t)v;
        return p;
    }
    static inline uint8_t* writeSVarint(uint8_t* p, int64_t v) {
        uint64_t zz = (uint64_t)((v << 1) ^ (v >> 63));
        return writeUVarint(p, zz);
    }

    inline DsId classify(uint64_t addr) const {
        uint8_t nib = (uint8_t)((addr >> 28) & 0xF);
        return ds_map_.nibble[nib];
    }

    void ensureSpace(size_t n);
    void flushLocked();
    void writeRaw(const void* data, size_t n);
    void updateCrc(const uint8_t* data, size_t n);

    FILE*    fp_    = nullptr;
    uint8_t* buf_   = nullptr;
    size_t   cap_   = 0;     // usable capacity (real allocation - 64 B slack)
    size_t   pos_   = 0;
    uint64_t events_ = 0;
    uint64_t bytes_  = 0;
    uint32_t crc_    = 0xFFFFFFFFu;
    bool     failed_ = false;

    DsMap    ds_map_;
    uint64_t last_addr_[DS_COUNT] = {0};
};

#endif // TRACE_WRITER_H
