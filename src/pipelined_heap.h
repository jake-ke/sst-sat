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
#include "structs.h"

// Define maximum number of heap levels and corresponding parameters
#define MAX_HEAP_LEVELS 22
#define MAX_HEAP_SIZE (1 << MAX_HEAP_LEVELS) - 1
#define PIPELINE_DEPTH 3  // Number of stages per level (read, compare, write)

// Operation types for pipeline stages
enum HeapOpType {
    HEAP_OP_NONE = 0,
    HEAP_OP_INSERT = 1,
    HEAP_OP_REPLACE = 2,
    HEAP_OP_BUMP = 3,
    HEAP_OP_RESCALE = 4
};

struct VarMem {
    uint32_t addr;
    double act;

    VarMem () : addr(0), act(0.0) {}
    VarMem (uint32_t ad, double ac) : addr(ad), act(ac) {}
};

struct InsReq {
    int arg;
    double activity;    // Used for INSERT and BUMP operations
    bool bump;          // true if this is a bump operation
    uint32_t dest;
    InsReq(int a = 0, double act = 0.0, bool b = false, uint32_t d = 0)
        : arg(a), activity(act), bump(b), dest(d) {}
};

enum class PendingMemOpType {
    INSERT_FETCH = 0,
    RESCALE = 1,
    DEBUG = 2
};

struct PendingMemOp {
    PendingMemOpType type;
    InsReq insert_req;
    size_t offset;
    size_t size;

    PendingMemOp()
        : type(PendingMemOpType::INSERT_FETCH), insert_req(), offset(0), size(0) {}

    explicit PendingMemOp(const InsReq& req)
        : type(PendingMemOpType::INSERT_FETCH), insert_req(req), offset(0), size(0) {}

    PendingMemOp(PendingMemOpType op_type, size_t off, size_t sz)
        : type(op_type), insert_req(), offset(off), size(sz) {}
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

// Structure for bypassing data between stages
struct BypassData {
    bool valid;
    int node_idx;
    Var var;
    double act;

    BypassData() : valid(false), node_idx(0), var(0), act(0.0) {}

    void reset() {
        valid = false;
        node_idx = 0;
        var = var_Undef;
        act = -1.0;
    }
};

class PipelinedHeap : public SST::SubComponent {
public:
    // SubComponent API
    SST_ELI_REGISTER_SUBCOMPONENT_API(PipelinedHeap, SST::Interfaces::StandardMem*, uint64_t)
    
    SST_ELI_REGISTER_SUBCOMPONENT(
        PipelinedHeap, "satsolver-lits-16", "PipelinedHeap", SST_ELI_ELEMENT_VERSION(1,0,0),
        "Pipelined heap implementation for variable ordering", PipelinedHeap
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"clock", "Clock frequency", "1GHz"},
        {"verbose", "Verbosity level", "0"}
    )

    SST_ELI_DOCUMENT_PORTS(
        {"response", "Response port to parent", {"sst.Event"}}
    )

    PipelinedHeap(SST::ComponentId_t id, SST::Params& params,
                  SST::Interfaces::StandardMem* mem, uint64_t var_ptr_base_addr);

    // Main interface functions
    bool tick(SST::Cycle_t cycle);
    void handleRequest(HeapReqEvent* req);
    void handleMem(SST::Interfaces::StandardMem::Request* req);

    // Heap management functions
    size_t size() const { return heap_size; }
    bool empty() const { return heap_size == 0; }

    // Setters for SAT solver integration
    void setDecisionFlags(const std::vector<bool>& dec) { decision = dec; }
    void setHeapSize(size_t size) { heap_size = size; num_vars = size; }
    void setVarIncPtr(double* ptr) { var_inc_ptr = ptr; }
    void setLineSize(size_t size) { line_size = size; }

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
    size_t heap_size;                       // Current number of elements in heap
    std::vector<bool> decision;             // Whether each variable is eligible for decisions
    double* var_inc_ptr;                    // Pointer to variable increment value

    // Heap memory - arrays for variables and activities at each level
    std::vector<Var> heap_vars[MAX_HEAP_LEVELS];
    std::vector<double> heap_activities[MAX_HEAP_LEVELS];

    // Pipeline state - 2D arrays with [level][stage]
    PipelineStageOp stages[MAX_HEAP_LEVELS][PIPELINE_DEPTH];
    BypassData bypass_data[MAX_HEAP_LEVELS];

    // DEBUG_HEAP state
    bool debug_heap_pending = false;
    int debug_heap_errors = 0;
    std::unordered_map<Var, VarMem> debug_heap_varmem;  // Store all read VarMem data

    struct PendingRequest {
        HeapReqEvent::OpType op;
        int arg;
        PendingRequest(HeapReqEvent::OpType o = HeapReqEvent::INSERT, int a = 0)
            : op(o), arg(a) {}
    };

    // Store queue for Write->Read ordering
    std::vector<StoreQueueEntry> store_queue;
    int findStoreQueueEntry(uint64_t addr, size_t size);

    // Request queues
    std::deque<PendingRequest> request_queue;
    std::deque<InsReq> insert_queue;

    // Parallel Var Activity Memory Reads
    // maps memory request ID to Heap ins/bump request
    std::unordered_map<uint64_t, PendingMemOp> req_to_op;
    bool bump_active;
    bool bump_mem_inflight;
    uint32_t active_inserts;
    std::unordered_set<Var> in_progress_vars;
    bool rescale;
    size_t rescale_pending_reads;

    // Pipeline control functions
    void advancePipeline();
    bool canStartOperation(HeapOpType op);
    void startOperation(HeapOpType op, int arg, double activity, bool bump, int dest);
    bool isPipelineIdle() const;

    // Stage operations
    void executeStageOp(int level, int stage);
    void handleStageInsert(int level, int stage);
    void handleStageReplace(int level, int stage);

    // Helper functions
    inline uint64_t varMemAddr(Var v) { return var_ptr_base_addr + v*sizeof(VarMem); }
    void sendResp(int result);
    int getChildIdx(int level, int node_idx, bool left);
    double getActivity(int level, int idx);
    Var getVar(int level, int idx);
    void setActivity(int level, int idx, double value);
    void setVar(int level, int idx, Var value);
    void setVarMem(Var v, VarMem p);
    void getVarMem(Var v, bool bump);
    void rescaleAct(Var v, VarMem& vmem);
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
