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

// Define maximum number of heap levels and corresponding parameters
#define MAX_HEAP_LEVELS 24
#define MAX_HEAP_SIZE ((1u << MAX_HEAP_LEVELS) - 1)
#define PIPELINE_DEPTH 3  // Number of stages per level (read, compare, write)

// Operation types for pipeline stages
enum HeapOpType {
    HEAP_OP_NONE = 0,
    HEAP_OP_INSERT = 1,
    HEAP_OP_REPLACE = 2
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

    PipelineStageOp() : op_type(HEAP_OP_NONE), node_idx(0), var(0),
                        act(0.0), valid(false), ready(true), depth(0), path(0) {}

    void reset() {
        op_type = HEAP_OP_NONE;
        node_idx = 0;
        var = var_Undef;
        act = -1.0;
        valid = false;
        ready = true;
        depth = 0;
        path = 0;
    }
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
        {"var_act_base_addr", "Base address of the per-variable activity array", "0x1C0000000"}
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
        {"heap_stale_sample", "Stale-copy count sampled on every change (Max = peak)", "count", 1}
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

    // Heap memory - arrays for variables and activities at each level
    std::vector<Var> heap_vars[MAX_HEAP_LEVELS];
    std::vector<double> heap_activities[MAX_HEAP_LEVELS];

    // Pipeline state - 2D arrays with [level][stage]
    PipelineStageOp stages[MAX_HEAP_LEVELS][PIPELINE_DEPTH];

    // DEBUG_HEAP state
    bool debug_heap_pending = false;
    int debug_heap_errors = 0;
    std::unordered_map<Var, double> debug_heap_acts;  // Off-chip activities read for verification

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
    struct StashedBump { Var var; double act; };
    std::vector<StashedBump> rescale_stash_;
    void maybeFinishRescale();

    // Idle root-peek purge: REPLACE launched by the heap itself; suppress the
    // root response toward the solver.
    bool purge_suppress_;

    // Statistics
    SST::Statistics::Statistic<uint64_t>* stat_insert_skips;
    SST::Statistics::Statistic<uint64_t>* stat_stale_created;
    SST::Statistics::Statistic<uint64_t>* stat_stale_pops;
    SST::Statistics::Statistic<uint64_t>* stat_tail_trims;
    SST::Statistics::Statistic<uint64_t>* stat_purge_pops;
    SST::Statistics::Statistic<uint64_t>* stat_bump_unassigned;
    SST::Statistics::Statistic<uint64_t>* stat_rebuilds;
    SST::Statistics::Statistic<uint64_t>* stat_size_sample;
    SST::Statistics::Statistic<uint64_t>* stat_stale_sample;
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
