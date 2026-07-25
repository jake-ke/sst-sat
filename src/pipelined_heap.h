#ifndef PIPELINED_HEAP_H
#define PIPELINED_HEAP_H

#include <sst/core/subcomponent.h>
#include <sst/core/output.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/link.h>
#include <sst/core/event.h>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include "structs.h"
#include "trace_writer.h"

// Levels 0..onchip_levels_-1 live in per-level SRAM with the 3-stage pipeline;
// levels >= onchip_levels_ live in the DRAM node region behind the off-chip
// level controller (OLC). Configured by the "onchip_levels" param (default 14;
// 0 means every level is on-chip, i.e. the OLC never engages).
// Total-depth bound: path bits are uint32 and lastSlot() uses 32-bit tricks.
constexpr int MAX_TOTAL_HEAP_LEVELS = 30;
constexpr int PIPELINE_DEPTH = 3;  // stages per level (read, compare, write)

// Operation types for pipeline stages
enum HeapOpType {
    HEAP_OP_NONE = 0,
    HEAP_OP_INSERT = 1,
    HEAP_OP_REPLACE = 2,
    // Targeted clean: an insert-descent aimed at a verified stale slot that
    // morphs into a replace-descent there (the overwrite is the deletion).
    HEAP_OP_CLEAN = 3
};

struct InsReq {
    Var arg;
    double activity;
    InsReq(Var a = 0, double act = 0.0) : arg(a), activity(act) {}
};

enum class PendingMemOpType {
    INSERT_FETCH = 0,
    BUMP_RMW = 1,
    RESCALE = 2,
    DEBUG = 3
};

struct PendingMemOp {
    PendingMemOpType type;
    Var var;
    size_t offset;
    size_t size;

    PendingMemOp() : type(PendingMemOpType::INSERT_FETCH), var(0), offset(0), size(0) {}
    PendingMemOp(PendingMemOpType t, Var v) : type(t), var(v), offset(0), size(0) {}
    PendingMemOp(PendingMemOpType t, size_t off, size_t sz)
        : type(t), var(0), offset(off), size(sz) {}
};

// Structure for pipeline stage operations
struct PipelineStageOp {
    HeapOpType op_type;           // Type of operation
    int node_idx;                 // Node index at this level
    Var var;                      // Variable value
    double act;                   // Activity value
    bool valid;                   // Whether this stage has valid data
    bool ready;                   // Whether this stage is ready to receive new data
    int depth;                    // Current depth in the heap (number of levels to go)
    uint32_t path;                // Normalized path bits with leading 1 at leftmost position
    uint64_t dest;                // INSERT only: global destination slot (for OLC handoff)
    bool from_clean;              // op originated as a targeted clean (stats)

    PipelineStageOp() : op_type(HEAP_OP_NONE), node_idx(0), var(0),
                        act(0.0), valid(false), ready(true), depth(0), path(0), dest(0),
                        from_clean(false) {}

    void reset() {
        op_type = HEAP_OP_NONE;
        node_idx = 0;
        var = var_Undef;
        act = -1.0;
        valid = false;
        ready = true;
        depth = 0;
        path = 0;
        dest = 0;
        from_clean = false;
    }
};

// ---------------- Off-chip level controller (OLC) types ----------------

// A heap node as stored off-chip: 16 B {int32 var, 4 B pad, double act}.
struct OlcNode {
    Var var;
    double act;
    OlcNode() : var(var_Undef), act(-1.0) {}
    OlcNode(Var v, double a) : var(v), act(a) {}
};

// One 64 B line of the tail buffer's sliding window (4 node slots).
struct TailLine {
    uint64_t line;                // node-region line index (0 = first off-chip line)
    bool dirty;
    bool slot_valid[4];
    OlcNode node[4];
    TailLine(uint64_t l = 0) : line(l), dirty(false) {
        for (int i = 0; i < 4; i++) slot_valid[i] = false;
    }
};

// Per-level state of an OLC insert context's pre-issued path read.
struct OlcPathRead {
    uint64_t slot;
    bool ready;          // data available
    bool issued;         // read sent (or served instantly); false = waiting on
                         // the OLC read budget, retried each tick
    bool conflict;       // slot shared with an older context's path: discard the
                         // parallel read and re-issue once the older has passed
    bool reissued;
    uint32_t gen;        // read generation; stale responses (gen mismatch) dropped
    OlcNode data;
    OlcPathRead() : slot(0), ready(false), issued(false), conflict(false),
                    reissued(false), gen(0) {}
};

// INSERT context: path slots known a priori, all reads issued in parallel,
// compare chain consumes them in level order (globally ordered across contexts).
struct OlcInsertCtx {
    uint64_t id;                  // stable id for response routing
    uint64_t dest;                // global destination slot
    int dest_level;
    Var var;                      // carried (descending) value
    double act;
    int progress;                 // next level to consume (starts at onchip_levels_)
    bool retired;
    std::vector<OlcPathRead> path;  // index: level - onchip_levels_, for levels < dest_level
};

// SIFT context (one in flight, v1): current slot's children line + the single
// aligned grandchildren line fetched together -> two levels per round trip.
struct OlcSiftCtx {
    bool active = false;
    uint64_t slot = 0;            // current slot (starts at the K-1 boundary slot)
    Var var = var_Undef;          // carried value
    double act = -1.0;
    bool boundary_write_pending = false;  // K-1 slot write not yet performed
    bool children_ready = false;
    bool grand_ready = false;     // also true when no grandchildren line was needed
    bool grand_needed = false;
    uint64_t hs_bound = 0;        // settled-size bound latched at the pop's
                                  // dispatch (tightened once at the boundary
                                  // handoff): every slot <= hs_bound holds
                                  // written content; every slot above is an
                                  // unwritten insert reservation. The sift
                                  // never reads or writes beyond it.
    uint32_t gen = 0;             // step generation for response routing
    OlcNode child[2];             // slots 2s, 2s+1 (validity re-checked vs heap_size)
    OlcNode grand[4];             // slots 4s..4s+3
};

// OLC memory traffic bookkeeping (kept separate from req_to_op so the
// solver-facing gates on req_to_op keep meaning "act traffic only").
enum class OlcMemType { INSERT_PATH, SIFT_CHILDREN, SIFT_GRAND, TAIL_REFILL, READ_PEEK };
struct OlcPendingRead {
    OlcMemType type;
    uint64_t ctx_id;   // insert ctx id, or sift/refill generation, or peek slot
    int level;         // insert path level
    uint32_t gen;
    OlcPendingRead() : type(OlcMemType::INSERT_PATH), ctx_id(0), level(0), gen(0) {}
    OlcPendingRead(OlcMemType t, uint64_t c, int l, uint32_t g)
        : type(t), ctx_id(c), level(l), gen(g) {}
};

class PipelinedHeap : public SST::SubComponent {
public:
    // SubComponent API (no constructor args beyond id/params: the heap owns
    // its memory interface and reads var_act_base_addr from its own params)
    SST_ELI_REGISTER_SUBCOMPONENT_API(PipelinedHeap)

    SST_ELI_REGISTER_SUBCOMPONENT(
        PipelinedHeap, "satsolver", "PipelinedHeap", SST_ELI_ELEMENT_VERSION(1,0,0),
        "Pipelined heap implementation for variable ordering", PipelinedHeap
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"clock", "Clock frequency", "1GHz"},
        {"verbose", "Verbosity level", "0"},
        {"var_act_base_addr", "Base address of the per-variable activity array", "0x1C0000000"},
        {"heap_region_end", "End of the heap-owned memory region ([acts | nodes] partition)", "0x200000000"},
        {"onchip_levels", "Heap levels held in on-chip SRAM (K); 0 = all levels on-chip (no OLC)", "14"}
    )

    SST_ELI_DOCUMENT_PORTS(
        {"response", "Response port to parent", {"sst.Event"}}
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"memory", "Dedicated memory interface for the activity array", "SST::Interfaces::StandardMem"}
    )

    SST_ELI_DOCUMENT_STATISTICS(
        {"heap_insert_skips", "INSERT requests dropped because the inheap bit was set", "count", 1},
        {"heap_stale_created", "Bumps that cleared a set inheap bit (resident copy became stale)", "count", 1},
        {"heap_stale_pops", "Solver pops that returned a stale copy (bit already clear)", "count", 1},
        {"heap_tail_trims", "Stale copies dropped from the last heap slot", "count", 1},
        {"heap_purge_pops", "Stale copies removed by idle root-peek purges", "count", 1},
        {"heap_bump_unassigned", "Bumps of unassigned variables (freshness-lemma guard)", "count", 1},
        {"heap_rebuilds", "Heap wipes triggered by stale-copy buildup (solver reinserts after)", "count", 1},
        {"heap_size_sample", "Heap occupancy sampled on every change (Max = peak)", "count", 1},
        {"heap_stale_sample", "Stale-copy count sampled on every change (Max = peak)", "count", 1},
        {"olc_node_reads", "Off-chip node region reads issued by the OLC", "count", 1},
        {"olc_node_writes", "Off-chip node region writes issued by the OLC", "count", 1},
        {"olc_boundary_crossings", "Ops handed off from the on-chip pipeline to the OLC", "count", 1},
        {"olc_insert_ctx_sample", "OLC insert contexts in use, sampled per handoff (Max = peak)", "count", 1},
        {"olc_sift_parked_sample", "Active + pipeline-parked sifts, sampled per park event (Max = peak)", "count", 1},
        {"olc_tail_refills", "Tail buffer slide-down line refills", "count", 1},
        {"olc_tail_stalls", "Cycles a pop/trim waited on the tail buffer or sift subtree gate", "count", 1},
        {"heap_rescales", "Activity rescale sweeps performed", "count", 1},
        {"heap_cleans", "Targeted cleans launched (stale copies removed pre-pop)", "count", 1},
        {"heap_clean_crossings", "Targeted cleans whose descent crossed to the OLC", "count", 1},
        {"heap_clean_search_misses", "Mint-buffer entries dropped after a full search sweep found no copy", "count", 1},
        {"heap_mint_drops", "Mint-buffer entries dropped on overflow", "count", 1}
    )

    PipelinedHeap(SST::ComponentId_t id, SST::Params& params);

    // Lifecycle (driven by the parent component)
    void init(unsigned int phase) override;
    void setup() override;
    void complete(unsigned int phase) override;
    void finish() override;

    // Main interface functions
    bool tick(SST::Cycle_t cycle);
    void handleRequest(HeapReqEvent* req);
    void handleMem(SST::Interfaces::StandardMem::Request* req);

    // Heap management functions
    size_t size() const { return heap_size; }
    bool empty() const { return heap_size == 0; }
    size_t liveCount() const { return inheap_count_; }
    // Clamped: between an INSERT's dispatch (bit set) and its pipeline start
    // (heap_size++), inheap_count_ transiently exceeds heap_size.
    size_t staleCount() const { return heap_size > inheap_count_ ? heap_size - inheap_count_ : 0; }
    // Any slot >= 2^K lives off-chip. A rebuild only earns its trajectory
    // perturbation when there are crossings/deep discard-pops to remove, i.e.
    // when the heap is actually off-chip; on-chip stale cost is <= +1 level.
    bool isOffChip() const { return heap_size >= firstOffchipSlot(); }
    // Anti-churn floor for mid-search rebuilds: a pile below ~capacity/8
    // adds at most a level or two to percolations and cannot approach the
    // boundary -- not worth a wipe's tie-break reshuffle. Clamped at the
    // K=14 reference budget so oversized K (onchip_levels=0 maps to 30)
    // keeps a finite floor instead of disabling rebuilds outright.
    size_t rebuildStaleFloor() const {
        int k = onchip_levels_ < 14 ? onchip_levels_ : 14;
        return ((size_t)1 << k) >> 3;
    }
    // True from a REBUILD's enqueue until its wipe dispatches. Solver-side
    // staleCount() reads the pre-wipe value inside that window, so rebuild
    // triggers must hold off (one 1-bit wire in hardware).
    bool rebuildQueued() const { return rebuild_queued_; }

    // Setters for SAT solver integration
    void setDecisionFlags(const std::vector<bool>& dec) { decision = dec; }
    void setHeapSize(size_t size) { heap_size = size; num_vars = size; }
    void setVarIncPtr(double* ptr) { var_inc_ptr = ptr; }
    void setTracer(TraceWriter* t, uint8_t /*ds_id*/) { tracer_ = t; }
    // Debug/guard-stat visibility into assignment state (not used for heap logic)
    void setAssignedFlagsRef(const std::vector<bool>* assigned) { assigned_ref_ = assigned; }

    // Initialize heap with given size
    void initHeap(uint64_t random_seed = 0);

private:
    // Output and configuration
    SST::Output output;
    SST::Link* response_port;
    SST::Interfaces::StandardMem* memory;
    size_t line_size;
    uint64_t var_ptr_base_addr;
    uint64_t heap_region_end_;
    int onchip_levels_;     // K: levels in SRAM ("onchip_levels" param; 0 maps
                            // to MAX_TOTAL_HEAP_LEVELS, i.e. all on-chip)
    size_t num_vars;

    // Heap state
    size_t heap_size;                       // Current number of entries (live + stale copies)
    std::vector<bool> decision;             // Whether each variable is eligible for decisions
    double* var_inc_ptr;                    // Pointer to variable increment value
    const std::vector<bool>* assigned_ref_; // Solver's var_assigned (debug/guard stats only)

    // On-chip membership bit per variable: inheap_[v]=1 means the copy from
    // v's most recent insert is resident with its activity current (freshness
    // lemma). Cleared by any bump of v and by any pop of a v-copy.
    std::vector<bool> inheap_;
    size_t inheap_count_;

    // Heap memory - arrays for variables and activities at each ON-CHIP level
    std::vector<Var> heap_vars[MAX_TOTAL_HEAP_LEVELS];
    std::vector<double> heap_activities[MAX_TOTAL_HEAP_LEVELS];

    // Pipeline state - 2D arrays with [level][stage], on-chip levels only
    PipelineStageOp stages[MAX_TOTAL_HEAP_LEVELS][PIPELINE_DEPTH];

    // ---------------- OLC state ----------------
    static const int OLC_INSERT_CTXS = 8;     // insert context table size
    static const int TAIL_WINDOW_LINES = 16;  // sliding window: 16 lines = 64 slots
    static const int TAIL_REFILL_MARGIN = 8;  // refill when runway (lines) < margin
    static const int TAIL_REFILLS_MAX = 2;    // refill line reads in flight
    // Outstanding node reads+writes cap: keeps the OLC from flooding the act
    // cache's MSHRs (a real controller has finite request slots too; in sim a
    // chronically full MSHR also re-processes stalled events every cycle).
    // Sift reads bypass the cap (<=2, top priority); everything else queues.
    static const int OLC_MEM_BUDGET = 12;
    // Outstanding act reads (bump RMWs + insert fetches) cap: bump bursts
    // dispatch 1/cycle and, when node traffic thrashes the act cache, their
    // misses alone can fill the MSHR. Combined with OLC_MEM_BUDGET this stays
    // below the 32-entry MSHR. Dispatch stalls at the queue head when full
    // (finite request queue), draining as responses return.
    static const int ACT_READS_MAX = 16;
    // olcMemBudgetOk reserve tiers: headroom left under OLC_MEM_BUDGET for
    // higher-priority traffic. Standard reads (insert paths, peeks, the
    // re-anchor) leave room for the sift's exempt line pair; refills are the
    // lowest-priority prefetches and leave half the budget free.
    static const int OLC_RESERVE_SIFT = 2;
    static const int OLC_RESERVE_PREFETCH = 6;
    // Targeted clean: corpses recorded at their bump-mint, searched in the
    // top on-chip levels (VSIDS locality puts hot corpses there; deeper ones
    // never obstruct pops and fall to the rebuild backstop).
    static const int MINT_BUFFER_CAP = 32;    // (var, pre-bump act) entries
    static const int MINT_CMP_WIDTH = 8;      // buffer entries matched per sweep
    static const int CLEAN_SEARCH_LEVELS = 8; // levels 0..7: 511 slots, <=32 cycles

    uint64_t nodes_base_;                     // 64-aligned start of the node region
    uint64_t heap_capacity_;                  // total slots: 2^K-1 + node region slots

    std::deque<OlcInsertCtx> insert_ctxs_;    // arrival order (front = oldest)
    uint64_t next_ctx_id_;
    OlcSiftCtx sift_;
    // Settled-size watermark: re-latched to heap_size at EVERY replace/clean
    // dispatch (deletes pipeline ~1 per 2 cycles, so a descent reaching the
    // boundary deliberately reads the YOUNGEST dispatch's value, not its own —
    // younger grabs have vacated slots the older descent must not read, and
    // nothing can land off-chip in between: later inserts are stuck behind it
    // in the in-order pipe and park at the boundary while a sift is active).
    // Slots above the watermark are vacated or unwritten insert reservations
    // and must be treated as nonexistent. One frontend register in hardware.
    uint64_t replace_hs_bound_ = 0;
    std::unordered_map<uint64_t, OlcPendingRead> olc_pending;  // mem req id -> OLC read
    int node_writes_inflight_;                // node-region writes awaiting WriteResp
    int tail_refills_inflight_;
    uint32_t refill_gen_;
    // Budget check for non-sift node reads; `reserve` keeps headroom for
    // higher-priority traffic (sift, reanchor).
    bool olcMemBudgetOk(int reserve) const {
        return (int)olc_pending.size() + node_writes_inflight_ < OLC_MEM_BUDGET - reserve;
    }
    void olcIssuePendingReads();
    // Refill responses may return out of order; installed only when adjacent
    // to the window bottom (contiguity invariant).
    std::unordered_map<uint64_t, std::vector<uint8_t>> refill_done_;

    // ---------------- targeted clean ----------------
    std::deque<std::pair<Var, double>> mint_buffer_;  // (var, pre-bump act)
    std::unordered_set<Var> minted_pending_;  // bump minted; act arrives at completeBump
    bool clean_enabled_;        // set by CLEAN_HINT dispatch, cleared by tree arrivals
    bool hit_pending_;          // search hit awaiting launch (same idle window)
    uint64_t hit_slot_;
    Var clean_var_;             // corpse identity for the launch verify and
    double clean_act_;          // the at-depth assert
    // Launch->morph window: a pop flowing behind the clean must not grab the
    // clean's target slot as its filler (the one frontend action that can
    // touch a deep slot ahead of the level-ordered pipeline). One comparator
    // on the pop gate; cleared the moment the corpse is overwritten.
    bool clean_target_armed_;
    uint32_t search_row_;       // per-level parallel sweep cursor (4 slots/row)
    void mintPush(Var v, double act);
    bool treeIdle() const;
    bool cleanWorkPending() const {
        return clean_enabled_ && (hit_pending_ || !mint_buffer_.empty());
    }
    void cleanTick();
    void launchClean();
    // Parked refill snapshots go stale if a node write lands between the
    // refill response and its install; every node-region writer must patch
    // them (<= TAIL_REFILLS_MAX+1 entries: a fixed comparator bank in HW).
    void patchParkedRefill(uint64_t line, int slot_off, const uint8_t* src16);

    // Tail buffer: contiguous window of node-region lines [front.line, back.line]
    // locked to heap_size (back covers the tail). Empty while heap_size < 2^K.
    std::deque<TailLine> tail_win_;

    // Slot/address helpers for the node region
    inline uint64_t firstOffchipSlot() const { return 1ull << onchip_levels_; }
    inline uint64_t nodeAddr(uint64_t slot) const {
        return nodes_base_ + (slot - firstOffchipSlot()) * 16;
    }
    inline uint64_t lineOf(uint64_t slot) const { return (slot - firstOffchipSlot()) >> 2; }
    inline uint64_t lineBaseSlot(uint64_t line) const { return firstOffchipSlot() + (line << 2); }
    inline uint64_t lineAddr(uint64_t line) const { return nodes_base_ + (line << 6); }

    // OLC core
    void olcTick();
    bool olcIdle() const;
    bool olcCanAcceptInsert() const;
    bool olcCanAcceptSift() const;
    void olcStartInsert(Var v, double act, uint64_t dest);
    void olcStartSift(uint64_t boundary_slot, Var v, double act);
    void olcProcessInserts();
    void olcIssueInsertRead(OlcInsertCtx& ctx, int level);
    void olcSiftIssueReads();
    void olcCompleteSiftStep();
    void olcRetireSift();
    void olcHandleMem(SST::Interfaces::StandardMem::ReadResp* resp, const OlcPendingRead& p);
    void sampleSiftParked();
    void siftWriteSlot(uint64_t slot, Var v, double act);
    bool siftBlocks(uint64_t slot) const;   // slot within the active sift's subtree

    // Node access through the single ordering point: tail buffer -> store
    // queue -> memory. Instant-read results: 1 = data in out now, 0 = issue a
    // memory read, 2 = buffered but not yet available (retry later).
    int nodeReadInstant(uint64_t slot, OlcNode& out);
    int lineReadInstant(uint64_t line, OlcNode* out4);
    void nodeWrite(uint64_t slot, Var v, double act);
    bool lineInWindow(uint64_t line) const;
    TailLine* tailLineFor(uint64_t slot);
    void tailComposeTo(uint64_t slot);                   // grow window to cover slot
    void tailDropAbove(uint64_t new_heap_size);          // shrink: drop dead top lines
    void tailEvictBottomIfOver();
    bool tailSlotReady(uint64_t slot);
    OlcNode tailGrab(uint64_t slot);                     // read+clear the tail slot
    void tailRefillTick();
    void installRefills();
    void tailScaleActs();                                // rescale: scale resident lines
    void tailWipe();                                     // rebuild: drop everything
    bool popGateOk();                                    // off-chip tail: ready + subtree gate

    // 16 B node <-> memory bytes
    static void packNode(const OlcNode& n, std::vector<uint8_t>& out);
    static OlcNode unpackNode(const uint8_t* p);
    void overlayStoreQueue(uint64_t addr, std::vector<uint8_t>& data) const;

    // DEBUG_HEAP state
    bool debug_heap_pending = false;
    int debug_heap_errors = 0;
    std::unordered_map<Var, double> debug_heap_acts;      // Off-chip activities read for verification
    std::unordered_map<uint64_t, OlcNode> debug_nodes_;   // Off-chip nodes read for verification
    bool debugNodeAt(uint64_t slot, OlcNode& out) const;  // buffer/queue/snapshot accessor

    struct PendingRequest {
        HeapReqEvent::OpType op;
        int arg;
        PendingRequest(HeapReqEvent::OpType o = HeapReqEvent::INSERT, int a = 0)
            : op(o), arg(a) {}
    };

    // Store queue for Write->Read ordering
    std::vector<StoreQueueEntry> store_queue;
    int findStoreQueueEntry(uint64_t addr, size_t size);

    // Trace writer (shared, not owned).
    TraceWriter* tracer_ = nullptr;

    // Request queues
    std::deque<PendingRequest> request_queue;
    std::deque<InsReq> insert_queue;

    // In-flight memory operations (insert fetches, bump RMWs, sweeps)
    std::unordered_map<uint64_t, PendingMemOp> req_to_op;
    int32_t active_inserts;
    // Vars with an outstanding bump read-modify-write. A second BUMP or an
    // INSERT of the same var stalls at the queue head until the bump's write
    // is store-queue-visible (otherwise the insert reads a pre-bump activity
    // and lands a copy that is stale while its bit is set).
    std::unordered_set<Var> bumps_inflight_;

    // Rescale: on trigger the pending bump is stashed unwritten, dispatch
    // stops, in-flight reads drain and the pipeline empties, then on-chip
    // state + the off-chip sweep scale, and finally the stashed bump(s)
    // complete with the scaled inc. The on-chip sweep is charged wall time
    // (per-level SRAMs scale in parallel at 1 entry/cycle, so its duration is
    // the largest occupied level) overlapped with the off-chip burst; rescale
    // completes when BOTH are done.
    bool rescale;
    bool rescale_sweep_started_;
    bool rescale_offchip_done_;
    size_t rescale_onchip_cycles_;
    size_t rescale_pending_reads;
    // Sweep bursts (rescale/debug) stream through a fixed window instead of
    // issuing every chunk at once: an unbounded burst (up to millions of line
    // reads with a deep node region) floods the act cache's MSHR.
    static const int BURST_WINDOW = 16;
    std::deque<std::pair<uint64_t, size_t>> burst_queue_;  // (addr, size) chunks
    int burst_inflight_;
    void issueBurstReads();
    struct StashedBump { Var var; double act; };
    std::vector<StashedBump> rescale_stash_;
    void maybeFinishRescale();

    // Idle root-peek purge: REPLACE launched by the heap itself; suppress the
    // root response toward the solver.
    bool purge_suppress_;
    bool rebuild_queued_;               // REBUILD enqueued, wipe not yet dispatched

    // Statistics
    SST::Statistics::Statistic<uint64_t>* stat_insert_skips;
    SST::Statistics::Statistic<uint64_t>* stat_stale_created;
    SST::Statistics::Statistic<uint64_t>* stat_stale_pops;
    SST::Statistics::Statistic<uint64_t>* stat_tail_trims;
    SST::Statistics::Statistic<uint64_t>* stat_rescales;
    SST::Statistics::Statistic<uint64_t>* stat_cleans;
    SST::Statistics::Statistic<uint64_t>* stat_clean_crossings;
    SST::Statistics::Statistic<uint64_t>* stat_clean_search_misses;
    SST::Statistics::Statistic<uint64_t>* stat_mint_drops;
    SST::Statistics::Statistic<uint64_t>* stat_purge_pops;
    SST::Statistics::Statistic<uint64_t>* stat_bump_unassigned;
    SST::Statistics::Statistic<uint64_t>* stat_rebuilds;
    SST::Statistics::Statistic<uint64_t>* stat_size_sample;
    SST::Statistics::Statistic<uint64_t>* stat_stale_sample;
    SST::Statistics::Statistic<uint64_t>* stat_olc_node_reads;
    SST::Statistics::Statistic<uint64_t>* stat_olc_node_writes;
    SST::Statistics::Statistic<uint64_t>* stat_olc_boundary_crossings;
    SST::Statistics::Statistic<uint64_t>* stat_olc_insert_ctx_sample;
    SST::Statistics::Statistic<uint64_t>* stat_olc_sift_parked_sample;
    SST::Statistics::Statistic<uint64_t>* stat_olc_tail_refills;
    SST::Statistics::Statistic<uint64_t>* stat_olc_tail_stalls;
    void sampleOccupancy();

    // Idle fast path: true when queues/pipeline may hold work. Recomputed at
    // the end of each active tick; set by handleRequest/handleMem on new work.
    bool maybe_active_;
    bool allIdle() const;
    bool idleWorkAvailable() const;

    // Pipeline control functions
    void advancePipeline();
    bool canStartOperation(HeapOpType op);
    void startOperation(HeapOpType op, Var arg, double activity);
    bool isPipelineIdle() const;

    // Stage operations
    void executeStageOp(int level, int stage);
    void handleStageInsert(int level, int stage);
    void handleStageReplace(int level, int stage);

    // Idle-time cleanup
    void lastSlot(uint32_t& level, uint32_t& idx) const;
    bool tailTrimOne();

    // Helper functions
    inline uint64_t actAddr(Var v) { return var_ptr_base_addr + v*sizeof(double); }
    void sendResp(int result);
    int getChildIdx(int level, int node_idx, bool left);
    double getActivity(int level, int idx);
    Var getVar(int level, int idx);
    void setActivity(int level, int idx, double value);
    void setVar(int level, int idx, Var value);
    void setAct(Var v, double act);
    void getAct(Var v, bool bump);
    void completeBump(Var v, double act);
    void completeInsertFetch(Var v, double act);
    void startRescaleSweep();
    void finishRescale();
    void readBurstAll(uint64_t start_addr, size_t total_size);
    void verifyDebugHeap();

    // Stage indices for clarity
    enum StageIndex {
        STAGE_READ = 0,
        STAGE_COMPARE = 1,
        STAGE_WRITE = 2
    };
};

#endif // PIPELINED_HEAP_H
