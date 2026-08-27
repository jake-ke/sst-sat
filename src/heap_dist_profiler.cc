#include <sst/core/sst_config.h> // Required for all SST implementation files
#include "heap_dist_profiler.h"
#include <climits>
#include <cmath>
#include <cstring>

HeapDistProfiler::HeapDistProfiler(const std::string& file, int onchip_levels)
    : file_(file), onchip_levels_(onchip_levels) {
    hist_live_.fill(0);
    hist_onchip_.fill(0);
    hist_offchip_.fill(0);
    hist_pop_.fill(0);
    hist_insert_boundary_.fill(0);
    hist_sift_boundary_.fill(0);
    for (auto& h : hist_margin_) h.fill(0);
}

HeapDistProfiler::~HeapDistProfiler() {
    if (fp_) std::fclose(fp_);
}

void HeapDistProfiler::onInit(size_t num_vars, const double* var_inc_ptr) {
    num_vars_ = num_vars;
    var_inc_ptr_ = var_inc_ptr;
    live_act_.assign(num_vars + 1, 0.0);
    live_val_ok_.assign(num_vars + 1, false);
    live_seen_gen_.assign(num_vars + 1, 0);
}

void HeapDistProfiler::onNodeWrite(uint64_t slot_off, Var v, double act) {
    if (slot_off >= shadow_nodes_.size()) shadow_nodes_.resize(slot_off + 1);
    shadow_nodes_[slot_off].var = v;
    shadow_nodes_[slot_off].act = act;
}

void HeapDistProfiler::onRescale(double factor) {
    for (double& a : live_act_) a *= factor;
    // The memory sweep only covers occupied slots, but scaling the whole
    // shadow keeps a shrunk-then-regrown slot from surfacing a pre-rescale
    // value in a snapshot taken while the regrow write is still in flight.
    // The DEBUG_HEAP cross-check never sees the divergence: it requires
    // quiescence and only audits slots <= heap_size.
    for (ShadowNode& n : shadow_nodes_)
        if (n.act > 0) n.act *= factor;
    rescales_total_++;
}

uint32_t HeapDistProfiler::gapBinG(double act, int& g) const {
    if (act == 0.0) { g = 0; return 257; }
    g = INT_MAX;
    if (std::fpclassify(act) == FP_SUBNORMAL) return 256;
    double vi = var_inc_ptr_ ? *var_inc_ptr_ : 1.0;
    g = std::ilogb(vi) - std::ilogb(act);
    if (g >= 504) return 256;
    int b = (g + 8) >> 1;
    if (b < 0) b = 0;
    else if (b > 255) b = 255;
    return (uint32_t)b;
}

void HeapDistProfiler::marginAdd(int cls, double a, double b) {
    uint32_t bin;
    if (a == b) {
        bin = 0;  // ties (and both-zero)
    } else if (a == 0.0 || b == 0.0) {
        bin = 65;  // decided by zero-vs-nonzero
    } else {
        int m = std::ilogb(a > b ? a : b) - std::ilogb(std::fabs(a - b));
        if (m < 0) m = 0;
        else if (m > 63) m = 63;
        bin = 1 + (uint32_t)m;
    }
    hist_margin_[cls][bin]++;
}

void HeapDistProfiler::onCompare(int cls, double a, double b) {
    if (a < 0.0 || b < 0.0) { cmp_vs_empty_++; return; }
    marginAdd(cls, a, b);
}

void HeapDistProfiler::onBoundaryInsert(double act) {
    boundary_inserts_++;
    int g;
    hist_insert_boundary_[gapBinG(act, g)]++;
}

void HeapDistProfiler::onBoundarySift(double act) {
    boundary_sifts_++;
    int g;
    hist_sift_boundary_[gapBinG(act, g)]++;
}

void HeapDistProfiler::onLivePop(double root_act) {
    live_pops_++;
    int g;
    hist_pop_[gapBinG(root_act, g)]++;
    pop_stash_live_ = true;
    pop_stash_root_ = root_act;
}

void HeapDistProfiler::popRunnerUp(double max_act, bool valid) {
    if (pop_stash_live_ && valid && max_act >= 0.0)
        marginAdd(3, pop_stash_root_, max_act);
    pop_stash_live_ = false;
}

void HeapDistProfiler::beginSnapshot(uint64_t conflicts, uint64_t decisions,
                                     uint64_t cycle, uint64_t heap_size,
                                     uint64_t live_count, uint64_t stale_count) {
    conflicts_ = conflicts;
    decisions_ = decisions;
    snap_gen_++;
    cycle_ = cycle;
    heap_size_ = heap_size;
    live_count_ = live_count;
    stale_count_ = stale_count;
    var_inc_ = var_inc_ptr_ ? *var_inc_ptr_ : 0.0;
    hist_live_.fill(0);
    hist_onchip_.fill(0);
    hist_offchip_.fill(0);
    level_rows_.clear();
}

void HeapDistProfiler::snapLive(Var v) {
    if (!live_val_ok_[v]) return;  // fetch still in flight
    int g;
    hist_live_[gapBinG(live_act_[v], g)]++;
}

void HeapDistProfiler::snapStored(int level, Var v, double act, bool bit_set) {
    int g;
    uint32_t b = gapBinG(act, g);
    (level >= onchip_levels_ ? hist_offchip_ : hist_onchip_)[b]++;
    if ((size_t)level >= level_rows_.size()) level_rows_.resize(level + 1);
    LevelRow& r = level_rows_[level];
    r.count++;
    // A corpse-pop can clear the bit while the fresh copy stays resident; a
    // re-insert then creates a bit-identical twin. Credit at most one live
    // copy per var per snapshot (first in walk order wins).
    if (bit_set && isFreshCopy(v, act) && live_seen_gen_[v] != snap_gen_) {
        live_seen_gen_[v] = snap_gen_;
        r.live++;
    }
    if (b == 257) { r.zero++; r.b16++; r.b32++; r.b64++; r.b128++; return; }
    if (g >= 16) r.b16++;
    if (g >= 32) r.b32++;
    if (g >= 64) r.b64++;
    if (g >= 128) r.b128++;
}

void HeapDistProfiler::endSnapshot() {
    writeFrame();
    hist_pop_.fill(0);
    hist_insert_boundary_.fill(0);
    hist_sift_boundary_.fill(0);
    for (auto& h : hist_margin_) h.fill(0);
    live_pops_ = 0;
    stale_pops_ = 0;
    purge_pops_ = 0;
    inserts_accepted_ = 0;
    insert_skips_ = 0;
    boundary_inserts_ = 0;
    boundary_sifts_ = 0;
    cmp_vs_empty_ = 0;
}

bool HeapDistProfiler::shadowAt(uint64_t slot_off, Var& v, double& act) const {
    if (slot_off >= shadow_nodes_.size()) return false;
    v = shadow_nodes_[slot_off].var;
    act = shadow_nodes_[slot_off].act;
    return true;
}

// ---------------- binary frame writer ----------------
// Little-endian; format spec in plans/pheap-act-dist.md (version 1).

void HeapDistProfiler::putU32(uint32_t v) {
    if (std::fwrite(&v, 4, 1, fp_) != 1) write_failed_ = true;
}

void HeapDistProfiler::putU64(uint64_t v) {
    if (std::fwrite(&v, 8, 1, fp_) != 1) write_failed_ = true;
}

void HeapDistProfiler::writeFileHeader() {
    const uint8_t magic[4] = {'H', 'D', 'S', 'T'};
    if (std::fwrite(magic, 1, 4, fp_) != 4) write_failed_ = true;
    uint16_t version = 1, flags = 0;
    if (std::fwrite(&version, 2, 1, fp_) != 1) write_failed_ = true;
    if (std::fwrite(&flags, 2, 1, fp_) != 1) write_failed_ = true;
    putU32((uint32_t)num_vars_);
    putU32((uint32_t)onchip_levels_);
    putU32(GAP_BINS);
    putU32(MARGIN_BINS);
    putU32(2);            // gap_step: exponent steps per bin
    putU32((uint32_t)-8); // gap_min (i32)
}

void HeapDistProfiler::writeFrame() {
    if (file_.empty() || write_failed_) return;
    if (!fp_) {
        fp_ = std::fopen(file_.c_str(), "wb");
        if (!fp_) {
            std::fprintf(stderr, "HeapDistProfiler: cannot open %s, disabling frame output\n",
                         file_.c_str());
            write_failed_ = true;
            return;
        }
        writeFileHeader();
    }
    uint64_t var_inc_bits;
    std::memcpy(&var_inc_bits, &var_inc_, 8);
    const uint64_t hdr[16] = {
        conflicts_, decisions_, cycle_, heap_size_, live_count_, stale_count_,
        rescales_total_, var_inc_bits, live_pops_, stale_pops_, purge_pops_,
        inserts_accepted_, insert_skips_, boundary_inserts_, boundary_sifts_,
        cmp_vs_empty_
    };
    for (uint64_t v : hdr) putU64(v);
    putU32((uint32_t)level_rows_.size());
    putU32(0);  // pad
    const std::array<uint32_t, GAP_BINS>* gap_hists[6] = {
        &hist_live_, &hist_onchip_, &hist_offchip_, &hist_pop_,
        &hist_insert_boundary_, &hist_sift_boundary_
    };
    for (auto* h : gap_hists)
        if (std::fwrite(h->data(), 4, GAP_BINS, fp_) != GAP_BINS) write_failed_ = true;
    for (auto& h : hist_margin_)
        if (std::fwrite(h.data(), 4, MARGIN_BINS, fp_) != MARGIN_BINS) write_failed_ = true;
    for (size_t lvl = 0; lvl < level_rows_.size(); lvl++) {
        const LevelRow& r = level_rows_[lvl];
        const uint32_t row[8] = {(uint32_t)lvl, r.count, r.live, r.zero,
                                 r.b16, r.b32, r.b64, r.b128};
        if (std::fwrite(row, 4, 8, fp_) != 8) write_failed_ = true;
    }
    if (std::fflush(fp_) != 0) write_failed_ = true;  // frames survive timeouts/kills
    if (write_failed_)
        std::fprintf(stderr, "HeapDistProfiler: write to %s failed, disabling frame output\n",
                     file_.c_str());
}
