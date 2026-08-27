#ifndef HEAP_DIST_PROFILER_H
#define HEAP_DIST_PROFILER_H

#include <sst/core/event.h>  // structs.h needs SST::Event declared first
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "structs.h"

// Host-side heap activity distribution profiler (plans/pheap-act-dist.md).
// Pure measurement: no SST statistics, no events, no RNG, no modeled latency —
// the simulation is bit-identical with the profiler on or off. Owned by
// PipelinedHeap as a unique_ptr (null when disabled); every hook site is
// gated on that pointer.
//
// Activities are binned by exponent gap g = ilogb(var_inc) - ilogb(act),
// which is rescale-invariant; one exponent step ~ one half-life (~13.5
// conflicts at var_decay 0.95). Compare margins are binned by the number of
// relative bits below the leading bit that decided the compare.
class HeapDistProfiler {
public:
    static const int GAP_BINS = 258;      // 128 2-step gap bins x2 + ancient + zero
    static const int MARGIN_BINS = 68;    // equal, 1+m (m in 0..63), one-zero, 2 reserved
    static const int MARGIN_CLASSES = 4;  // top4, onchip_rest, olc, pop_margin

    // Shadow of one stored off-chip node (mirrors OlcNode without depending
    // on pipelined_heap.h).
    struct ShadowNode {
        Var var = var_Undef;
        double act = -1.0;
    };

    HeapDistProfiler(const std::string& file, int onchip_levels);
    ~HeapDistProfiler();

    // ---------------- shadow-state hooks ----------------
    void onInit(size_t num_vars, const double* var_inc_ptr);
    // INSERT dispatch sets inheap_ before the activity fetch returns: the
    // live value is unknown until completeInsertFetch records it.
    void onInsertDispatch(Var v) { live_val_ok_[v] = false; inserts_accepted_++; }
    void onInsertSkip() { insert_skips_++; }
    void onLiveValue(Var v, double act) { live_act_[v] = act; live_val_ok_[v] = true; }
    void onNodeWrite(uint64_t slot_off, Var v, double act);
    // Mirror the rescale sweep exactly: live acts scale unconditionally (like
    // the activity-array sweep); shadow acts scale when > 0 (like the node
    // sweep), and only the offchip_slots the memory sweep actually covers.
    // `factor` is the heap's sweep factor (1e-100, or 2^-332 under act_mant).
    void onRescale(double factor);

    // ---------------- interval series hooks ----------------
    void onBoundaryInsert(double act);
    void onBoundarySift(double act);
    // Live pop at the root: series 5 + class-3 margin vs the runner-up
    // (occupied level-1 entries only; empty slots carry act < 0).
    void onLivePop(double root_act);
    // Completes the pop margin at the same replace op's L0 compare, where the
    // child values are settled (reading them at L0-READ races the previous
    // pop's level-1 write). Stale/purge pops invalidate the stash so every
    // consume pairs with its own op.
    void popRunnerUp(double max_act, bool valid);
    void onStalePop() { stale_pops_++; pop_stash_live_ = false; }
    void onPurgePop() { purge_pops_++; pop_stash_live_ = false; }
    // One call per activity compare. A side with act < 0 is an empty slot:
    // counted in cmp_vs_empty, not binned.
    void onCompare(int cls, double a, double b);

    // ---------------- snapshot (driven by PipelinedHeap::distSnapshot) ----------------
    void beginSnapshot(uint64_t conflicts, uint64_t decisions, uint64_t cycle,
                       uint64_t heap_size, uint64_t live_count, uint64_t stale_count);
    void snapLive(Var v);
    void snapStored(int level, Var v, double act, bool bit_set);
    void endSnapshot();

    // A bit-set var can still own stale corpses from older inserts; only the
    // copy whose stored activity matches the recorded live value is the fresh
    // one (corpses froze a strictly smaller pre-bump value).
    bool isFreshCopy(Var v, double act) const {
        return liveValOk(v) && act == live_act_[v];
    }

    // ---------------- verifyDebugHeap accessors ----------------
    bool liveValOk(Var v) const {
        return (size_t)v < live_val_ok_.size() && live_val_ok_[v];
    }
    double liveAct(Var v) const { return live_act_[v]; }
    bool shadowAt(uint64_t slot_off, Var& v, double& act) const;

private:
    // Gap bin per the frame format; g is set to the exponent gap (INT_MAX for
    // the ancient bin, unused for the zero bin).
    uint32_t gapBinG(double act, int& g) const;
    void marginAdd(int cls, double a, double b);
    void writeFileHeader();
    void writeFrame();
    void putU32(uint32_t v);
    void putU64(uint64_t v);

    std::string file_;
    std::FILE* fp_ = nullptr;
    bool write_failed_ = false;
    int onchip_levels_;
    size_t num_vars_ = 0;
    const double* var_inc_ptr_ = nullptr;

    // Shadow state
    std::vector<double> live_act_;
    std::vector<bool> live_val_ok_;
    std::vector<ShadowNode> shadow_nodes_;  // index: slot - 2^K, lazily grown

    // Snapshot-walk histograms (rebuilt every frame)
    std::array<uint32_t, GAP_BINS> hist_live_;
    std::array<uint32_t, GAP_BINS> hist_onchip_;
    std::array<uint32_t, GAP_BINS> hist_offchip_;
    struct LevelRow {
        uint32_t count = 0, live = 0, zero = 0, b16 = 0, b32 = 0, b64 = 0, b128 = 0;
    };
    std::vector<LevelRow> level_rows_;

    // Interval accumulators (reset after every frame)
    std::array<uint32_t, GAP_BINS> hist_pop_;
    std::array<uint32_t, GAP_BINS> hist_insert_boundary_;
    std::array<uint32_t, GAP_BINS> hist_sift_boundary_;
    std::array<std::array<uint32_t, MARGIN_BINS>, MARGIN_CLASSES> hist_margin_;
    uint64_t live_pops_ = 0;
    uint64_t stale_pops_ = 0;
    uint64_t purge_pops_ = 0;
    uint64_t inserts_accepted_ = 0;
    uint64_t insert_skips_ = 0;
    uint64_t boundary_inserts_ = 0;
    uint64_t boundary_sifts_ = 0;
    uint64_t cmp_vs_empty_ = 0;
    bool pop_stash_live_ = false;   // a live pop's margin awaits its L0 compare
    double pop_stash_root_ = -1.0;
    std::vector<uint32_t> live_seen_gen_;  // per-var stamp: counted live this snapshot
    uint32_t snap_gen_ = 0;

    // Frame header fields latched at beginSnapshot
    uint64_t conflicts_ = 0;
    uint64_t decisions_ = 0;
    uint64_t cycle_ = 0;
    uint64_t heap_size_ = 0;
    uint64_t live_count_ = 0;
    uint64_t stale_count_ = 0;
    uint64_t rescales_total_ = 0;
    double var_inc_ = 0.0;
};

#endif // HEAP_DIST_PROFILER_H
