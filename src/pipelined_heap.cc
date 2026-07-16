#include <sst/core/sst_config.h> // Required for all SST implementation files
#include "pipelined_heap.h"
#include <algorithm>
#include <random>
#include <cmath>

PipelinedHeap::PipelinedHeap(
    SST::ComponentId_t id, SST::Params& params
) : SST::SubComponent(id),
    line_size(64),
    num_vars(0),
    heap_size(0),
    var_inc_ptr(nullptr),
    assigned_ref_(nullptr),
    inheap_count_(0),
    active_inserts(0),
    rescale(false),
    rescale_sweep_started_(false),
    rescale_offchip_done_(false),
    rescale_onchip_cycles_(0),
    rescale_pending_reads(0),
    purge_suppress_(false),
    debug_heap_pending(false),
    debug_heap_errors(0) {

    output.init("PHEAP-> ", params.find<int>("verbose", 0), 0, SST::Output::STDOUT);

    var_ptr_base_addr = std::stoull(params.find<std::string>("var_act_base_addr", "0x1C0000000"), nullptr, 0);

    registerClock(params.find<std::string>("clock", "1GHz"),
                 new SST::Clock::Handler2<PipelinedHeap, &PipelinedHeap::tick>(this));
    maybe_active_ = false;

    response_port = configureLink("response");
    sst_assert(response_port != nullptr, CALL_INFO, -1,
              "Error: 'response_port' is not connected to a link\n");

    // Dedicated memory interface: activity traffic never touches the
    // solver's cache hierarchy.
    memory = loadUserSubComponent<SST::Interfaces::StandardMem>(
        "memory",
        SST::ComponentInfo::SHARE_NONE,
        getTimeConverter(params.find<std::string>("clock", "1GHz")),
        new SST::Interfaces::StandardMem::Handler2<PipelinedHeap, &PipelinedHeap::handleMem>(this));
    sst_assert(memory != nullptr, CALL_INFO, -1,
              "Unable to load StandardMem subcomponent on the 'memory' slot\n");

    output.verbose(CALL_INFO, 1, 0, "var act address: 0x%lx\n", var_ptr_base_addr);

    stat_insert_skips    = registerStatistic<uint64_t>("heap_insert_skips");
    stat_stale_created   = registerStatistic<uint64_t>("heap_stale_created");
    stat_stale_pops      = registerStatistic<uint64_t>("heap_stale_pops");
    stat_tail_trims      = registerStatistic<uint64_t>("heap_tail_trims");
    stat_purge_pops      = registerStatistic<uint64_t>("heap_purge_pops");
    stat_bump_unassigned = registerStatistic<uint64_t>("heap_bump_unassigned");
    stat_rebuilds        = registerStatistic<uint64_t>("heap_rebuilds");
    stat_size_sample     = registerStatistic<uint64_t>("heap_size_sample");
    stat_stale_sample    = registerStatistic<uint64_t>("heap_stale_sample");

    // Initialize heap memories for each level
    for (int level = 0; level < MAX_HEAP_LEVELS; level++) {
        int level_size = 1 << level;  // 2^level nodes at this level
        heap_vars[level].resize(level_size, var_Undef);
        heap_activities[level].resize(level_size, -1);
    }

    // Initialize pipeline stages
    for (int level = 0; level < MAX_HEAP_LEVELS; level++) {
        for (int stage = 0; stage < PIPELINE_DEPTH; stage++) {
            stages[level][stage].reset();
        }
    }
}

void PipelinedHeap::init(unsigned int phase) {
    memory->init(phase);
}

void PipelinedHeap::setup() {
    memory->setup();
    line_size = std::max(memory->getLineSize(), (uint64_t)64);
    output.verbose(CALL_INFO, 1, 0, "Activity cache line size: %zu bytes\n", line_size);
}

void PipelinedHeap::complete(unsigned int phase) {
    memory->complete(phase);
}

void PipelinedHeap::finish() {
    memory->finish();
}

void PipelinedHeap::sampleOccupancy() {
    stat_size_sample->addData(heap_size);
    stat_stale_sample->addData(staleCount());
}

uint32_t bit_reverse(uint32_t x) {
    x = ((x & 0x55555555) << 1) | ((x >> 1) & 0x55555555);
    x = ((x & 0x33333333) << 2) | ((x >> 2) & 0x33333333);
    x = ((x & 0x0F0F0F0F) << 4) | ((x >> 4) & 0x0F0F0F0F);
    x = ((x & 0x00FF00FF) << 8) | ((x >> 8) & 0x00FF00FF);
    x = (x << 16) | (x >> 16);
    return x;
}

// returns the index of the highest set bit (0-based), or 0 if x is 0
uint32_t priority_encoder(uint32_t x) {
    if (x == 0) return 0;
    x = bit_reverse(x);
    uint32_t one_hot_m1 = ~x & (x - 1);
    // count trailing ones in one_hot_m1
    int cnt = 31;
    for (int i = 0; i < 32; i++) {
        cnt -= (one_hot_m1 >> i) & 1;
    }
    return cnt;
}

void PipelinedHeap::lastSlot(uint32_t& level, uint32_t& idx) const {
    uint32_t heap_r = bit_reverse(heap_size);
    uint32_t heap_one_hot = bit_reverse(heap_r & ~(heap_r - 1));
    idx = heap_size & (~heap_one_hot);
    level = priority_encoder(heap_size);
}

bool PipelinedHeap::tailTrimOne() {
    if (heap_size == 0) return false;
    uint32_t last_level, last_idx;
    lastSlot(last_level, last_idx);
    Var w = heap_vars[last_level][last_idx];
    if (w == var_Undef || inheap_[w]) return false;
    setVar(last_level, last_idx, var_Undef);
    setActivity(last_level, last_idx, -1.0);
    heap_size--;
    stat_tail_trims->addData(1);
    sampleOccupancy();
    output.verbose(CALL_INFO, 6, 0, "TAIL-TRIM: dropped stale var %d at L%d idx %d\n",
                   w, last_level, last_idx);
    return true;
}

bool PipelinedHeap::tick(SST::Cycle_t cycle) {
    // Fast path: nothing queued and pipeline empty (set at the end of the
    // previous tick; handleRequest/handleMem set maybe_active_ on new work).
    if (!maybe_active_) return false;

    if (!insert_queue.empty() && canStartOperation(HEAP_OP_INSERT) && !rescale) {
        InsReq& op = insert_queue.front();
        active_inserts++;
        startOperation(HEAP_OP_INSERT, op.arg, op.activity);
        insert_queue.pop_front();
    }

    if (!request_queue.empty()) {
        const PendingRequest& pending = request_queue.front();
        switch (pending.op) {
            case HeapReqEvent::READ: {
                // READ is expensive to support in hardware
                int lvl = priority_encoder(pending.arg);
                int idx = pending.arg & ~(1 << lvl);
                sendResp(getVar(lvl, idx));
                request_queue.pop_front();
                break;
            }
            case HeapReqEvent::BUMP: {
                // Off-chip RMW only; never touches the pipeline. Same-var
                // RMWs serialize via bumps_inflight_. Also hold off while a
                // DEBUG_HEAP snapshot is in flight: a bump would mutate the
                // bitmap/memory mid-snapshot and produce false errors.
                if (!rescale && !debug_heap_pending
                    && bumps_inflight_.find(pending.arg) == bumps_inflight_.end()) {
                    Var v = pending.arg;
                    bumps_inflight_.insert(v);
                    if (inheap_[v]) {
                        // Resident copy keeps its old activity: it is stale now.
                        inheap_[v] = false;
                        inheap_count_--;
                        stat_stale_created->addData(1);
                        sampleOccupancy();
                    }
                    getAct(v, true);
                    request_queue.pop_front();
                }
                break;
            }
            case HeapReqEvent::INSERT: {
                if (!rescale && !debug_heap_pending) {
                    Var v = pending.arg;
                    // Stall behind an outstanding bump of the same var so the
                    // activity read sees the bumped value (freshness lemma).
                    if (bumps_inflight_.find(v) != bumps_inflight_.end()) break;
                    if (inheap_[v]) {
                        // Fresh copy already resident: drop with zero traffic.
                        stat_insert_skips->addData(1);
                    } else {
                        inheap_[v] = true;
                        inheap_count_++;
                        getAct(v, false);
                    }
                    request_queue.pop_front();
                }
                break;
            }
            case HeapReqEvent::REMOVE_MAX: {
                // after previous inserts/bumps have fully drained
                if (!rescale && active_inserts == 0 && req_to_op.empty()
                    && insert_queue.empty() && canStartOperation(HEAP_OP_REPLACE)) {
                    startOperation(HEAP_OP_REPLACE, 0, 0);
                    request_queue.pop_front();
                }
                break;
            }
            case HeapReqEvent::REBUILD: {
                // Wipe the tree and the bitmap once fully quiescent (modeled
                // as a flash-clear of per-level valid bits + the bitmap). The
                // solver reinserts all unassigned decision vars right after;
                // those INSERTs queue behind this request, and the next
                // REMOVE_MAX queues behind them, so no partial-heap decision
                // is possible.
                if (!rescale && !debug_heap_pending && active_inserts == 0
                    && req_to_op.empty() && insert_queue.empty() && isPipelineIdle()) {
                    for (int level = 0; level < MAX_HEAP_LEVELS; level++) {
                        size_t level_base = ((size_t)1 << level) - 1;
                        if (heap_size <= level_base) break;
                        size_t occupied = std::min((size_t)1 << level, heap_size - level_base);
                        std::fill_n(heap_vars[level].begin(), occupied, var_Undef);
                        std::fill_n(heap_activities[level].begin(), occupied, -1.0);
                    }
                    output.verbose(CALL_INFO, 2, 0,
                        "REBUILD: wiped %zu entries (%zu live, %zu stale)\n",
                        heap_size, inheap_count_, staleCount());
                    heap_size = 0;
                    std::fill(inheap_.begin(), inheap_.end(), false);
                    inheap_count_ = 0;
                    stat_rebuilds->addData(1);
                    sampleOccupancy();
                    request_queue.pop_front();
                }
                break;
            }
            case HeapReqEvent::DEBUG_HEAP: {
                // Wait for all previous requests and pipeline to finish
                if (active_inserts == 0 && req_to_op.empty() && insert_queue.empty()
                    && isPipelineIdle() && !rescale) {
                    // Start debug heap check
                    debug_heap_pending = true;
                    debug_heap_errors = 0;
                    debug_heap_acts.clear();
                    debug_heap_acts.reserve(num_vars + 1);

                    if (heap_size == 0 && inheap_count_ == 0) {
                        sendResp(0);
                        debug_heap_pending = false;
                    } else {
                        output.verbose(CALL_INFO, 6, 0, "DEBUG_HEAP: Reading memory for heap verification\n");
                        readBurstAll(var_ptr_base_addr, (num_vars + 1) * sizeof(double));
                    }
                    request_queue.pop_front();
                }
                break;
            }
            default: {
                request_queue.pop_front();
                break;
            }
        }
    } else if (!rescale && !debug_heap_pending && insert_queue.empty()
               && req_to_op.empty() && active_inserts == 0 && isPipelineIdle()) {
        // Idle-time cleanup: requests always take priority (this branch only
        // runs with an empty request queue and a fully drained heap).
        if (!tailTrimOne() && heap_size > 0) {
            Var root = getVar(0, 0);
            if (root != var_Undef && !inheap_[root] && canStartOperation(HEAP_OP_REPLACE)) {
                // Stale copy at the root: discard it with a self-consumed pop.
                purge_suppress_ = true;
                startOperation(HEAP_OP_REPLACE, 0, 0);
            }
        }
    }

    // Rescale drain point: every pre-trigger read has returned AND the
    // pipeline has fully drained. In-flight percolations carry activities in
    // stage registers that the sweep cannot scale; letting one settle after
    // the sweep would write a pre-scale (1e100x) value into a scaled heap.
    // Deadlock-free: rescale blocks all new dispatch (including queued
    // insert_queue starts), so the pipeline empties in bounded time.
    if (rescale && !rescale_sweep_started_ && req_to_op.empty()
        && active_inserts == 0 && isPipelineIdle()) {
        startRescaleSweep();
    }

    // Charge wall time for the on-chip level-SRAM sweep (values were scaled
    // atomically at sweep start; nothing can observe them while rescale
    // blocks dispatch, so only the duration is modeled). Overlaps the
    // off-chip burst; rescale completes when both are done.
    if (rescale && rescale_sweep_started_ && rescale_onchip_cycles_ > 0) {
        rescale_onchip_cycles_--;
        if (rescale_onchip_cycles_ == 0) maybeFinishRescale();
    }

    advancePipeline();

    // Cache the idle state so the (dominant) idle ticks cost O(1) instead of
    // sweeping queues and the MAX_HEAP_LEVELS x PIPELINE_DEPTH pipeline array
    // every simulated cycle.
    // NOTE: the clock handler must stay registered -- unregistering and
    // re-registering would reorder handlers on SST's shared per-frequency
    // Clock and skew same-cycle solver->heap request handling.
    maybe_active_ = !allIdle();
    return false;
}

bool PipelinedHeap::allIdle() const {
    return request_queue.empty() && insert_queue.empty() && req_to_op.empty()
        && bumps_inflight_.empty() && !rescale && !debug_heap_pending
        && active_inserts == 0 && isPipelineIdle()
        && !idleWorkAvailable();
}

bool PipelinedHeap::idleWorkAvailable() const {
    if (heap_size == 0) return false;
    uint32_t last_level, last_idx;
    lastSlot(last_level, last_idx);
    Var w = heap_vars[last_level][last_idx];
    if (w != var_Undef && !inheap_[w]) return true;
    Var root = heap_vars[0][0];
    return root != var_Undef && !inheap_[root];
}

void PipelinedHeap::advancePipeline() {
    // Process each level in reverse order (bottom-up)
    for (int level = MAX_HEAP_LEVELS - 1; level >= 0; level--) {
        // Process each stage within the level in reverse order (WRITE->READ)
        for (int stage = PIPELINE_DEPTH - 1; stage >= 0; stage--) {
            if (stages[level][stage].valid) {
                executeStageOp(level, stage);
            }
        }
    }
}

bool PipelinedHeap::canStartOperation(HeapOpType op) {
    // 1 insertion / 1 cycle, 1 removal / 2 cycles

    // Check if the first stage (READ) of the first level is ready to receive new data
    if (!stages[0][STAGE_READ].ready) return false;

    if (op == HEAP_OP_REPLACE) {
        // Check if the previous operation in the READ stage is also a REPLACE
        if ((stages[0][STAGE_COMPARE].valid && stages[0][STAGE_COMPARE].op_type == HEAP_OP_REPLACE))
            return false;
    }

    return true;
}

void PipelinedHeap::startOperation(HeapOpType op, Var arg, double activity) {
    // Initialize READ stage at level 0 with the new operation
    stages[0][STAGE_READ].op_type = op;

    if (op == HEAP_OP_INSERT) {
        // insertion at the last position
        heap_size++;
        sampleOccupancy();
        // Stale copies count toward occupancy, so heap_size may exceed
        // num_vars; only physical capacity bounds it.
        sst_assert(heap_size <= (size_t) MAX_HEAP_SIZE, CALL_INFO, -1,
            "Failed to insert var %d: heap size overflow\n", arg);
        uint32_t dest = heap_size;

        uint32_t target_level = priority_encoder(dest);
        // need to pass down depth for termination condition
        stages[0][STAGE_READ].depth = target_level;
        // Normalize path by shifting so the leading 1 is at the MSB position
        uint32_t path = dest << (31 - target_level);
        stages[0][STAGE_READ].path = path << 1;  // remove the leading 1 bit

        output.verbose(CALL_INFO, 6, 0,
            "Start INSERT: heap_size=%lu, var %d (%.2f), idx=%u, path=0x%x, depth=%d\n",
            heap_size, arg, activity, dest, path, target_level);
    } else if (op == HEAP_OP_REPLACE) {
        // Drop a stale copy sitting at the tail before using it as the
        // replacement: one bit-check per pop cycle, and only when no
        // in-flight stage could hold the tail. Deeper trailing garbage is
        // handled by the idle-time trim at the same 1-per-cycle rate.
        if (isPipelineIdle()) {
            tailTrimOne();
        }
        if (heap_size == 0) {
            if (purge_suppress_) purge_suppress_ = false;
            else sendResp(var_Undef);
            return;
        }

        // determine the last level and node idx
        uint32_t last_level, last_node_idx;
        lastSlot(last_level, last_node_idx);

        // determine the replacement var
        if (stages[last_level][STAGE_WRITE].valid && (int)last_node_idx == stages[last_level][STAGE_WRITE].node_idx) {
            // bypass the WRITE_STAGE node if match
            arg = stages[last_level][STAGE_WRITE].var;
            activity = stages[last_level][STAGE_WRITE].act;
            stages[last_level][STAGE_WRITE].reset();
        } else if (stages[last_level][STAGE_COMPARE].valid && (int)last_node_idx == stages[last_level][STAGE_COMPARE].node_idx) {
            // bypass the COMP_STAGE node if match
            arg = stages[last_level][STAGE_COMPARE].var;
            activity = stages[last_level][STAGE_COMPARE].act;
            // stop the compare stage and inferred read stage we stole
            if (last_level < MAX_HEAP_LEVELS - 1) {
                stages[last_level+1][STAGE_READ].reset();
            }
            stages[last_level][STAGE_COMPARE].reset();
        } else {
            // use last_var and last_act by default
            arg = getVar(last_level, last_node_idx);
            activity = getActivity(last_level, last_node_idx);
        }
        // removes the last node from the heap
        setVar(last_level, last_node_idx, var_Undef);
        output.verbose(CALL_INFO, 6, 0, "set last level %d, idx %d, addr %d, to var_Undef\n", last_level, last_node_idx, (1 << last_level) | last_node_idx);
        output.verbose(CALL_INFO, 6, 0, "Start REPLACE: heap_size=%lu, last var %d (%.2f)\n",
            heap_size, arg, activity);

        heap_size--;
        sampleOccupancy();
    }

    assert(arg != var_Undef);
    stages[0][STAGE_READ].var = arg;
    stages[0][STAGE_READ].act = activity;
    stages[0][STAGE_READ].node_idx = 0;  // Always start at root
    stages[0][STAGE_READ].valid = true;
    stages[0][STAGE_READ].ready = false;
}

void PipelinedHeap::executeStageOp(int level, int stage) {
    if (stages[level][stage].op_type == HEAP_OP_INSERT) {
        handleStageInsert(level, stage);
    } else if (stages[level][stage].op_type == HEAP_OP_REPLACE) {
        handleStageReplace(level, stage);
    }
}

void PipelinedHeap::handleStageInsert(int level, int stage) {
    PipelineStageOp& curr_stage = stages[level][stage];
    int node_idx = curr_stage.node_idx;

    switch (stage) {
        case STAGE_READ: {
            // needs to induce this level's COMPARE
            // and may need to induce next level's READ
            if (!stages[level][STAGE_COMPARE].ready
                && level < MAX_HEAP_LEVELS - 1
                && level < curr_stage.depth
                && !stages[level+1][STAGE_READ].ready) {
                stages[level][stage].ready = false;
                break;
            }
            output.verbose(CALL_INFO, 6, 0, "INSERT[L%d-READ]: var %d (%.2f), node %d, depth %d, path 0x%x\n",
                level, curr_stage.var, curr_stage.act, node_idx, curr_stage.depth, curr_stage.path);

            // always ready for onchip
            stages[level][stage].ready = true;

            // Determine if and where to send operation to next level
            if (level < MAX_HEAP_LEVELS - 1 && level < curr_stage.depth) {
                // Determine which direction to go (left or right) based on MSB of path
                bool go_left = (curr_stage.path & 0x80000000) == 0;
                int child_idx = getChildIdx(level, node_idx, go_left);
                // Update path for next level by shifting left by 1 bit
                uint32_t next_path = curr_stage.path << 1;

                stages[level+1][STAGE_READ].op_type = curr_stage.op_type;
                stages[level+1][STAGE_READ].node_idx = child_idx;
                // fill with dummy inserting var and act which will be updated in COMPARE
                stages[level+1][STAGE_READ].var = curr_stage.var;
                stages[level+1][STAGE_READ].act = curr_stage.act;
                stages[level+1][STAGE_READ].depth = curr_stage.depth;
                stages[level+1][STAGE_READ].path = next_path;
                stages[level+1][STAGE_READ].valid = true;
                stages[level+1][STAGE_READ].ready = false;
                output.verbose(CALL_INFO, 6, 0, "INSERT[L%d-READ]: inducing L%d node %d, %s child\n",
                               level, level+1, child_idx, go_left ? "left" : "right");
            } else output.verbose(CALL_INFO, 6, 0, "INSERT[L%d-READ]: insertion ends this level\n", level);

            stages[level][STAGE_COMPARE].op_type = curr_stage.op_type;
            stages[level][STAGE_COMPARE].node_idx = node_idx;
            stages[level][STAGE_COMPARE].var = curr_stage.var;
            stages[level][STAGE_COMPARE].act = curr_stage.act;
            stages[level][STAGE_COMPARE].depth = curr_stage.depth;
            stages[level][STAGE_COMPARE].path = curr_stage.path;
            if (level == 0) {
                // for root level, always valid
                stages[level][STAGE_COMPARE].valid = true;
                stages[level][STAGE_COMPARE].ready = true;
            } else {
                // false because waiting for previous level's COMPARE to update inserting var
                stages[level][STAGE_COMPARE].valid = false;
                stages[level][STAGE_COMPARE].ready = false;
            }

            stages[level][stage].reset();
            break;
        }

        case STAGE_COMPARE: {
            // this level's node either from memory
            // or implicit bypass from this level's WRITE stage (previous operation)
            int curr_var = getVar(level, node_idx);
            double curr_act = getActivity(level, node_idx);

            int new_var = curr_stage.var;
            double new_act = curr_stage.act;

            output.verbose(CALL_INFO, 6, 0, "INSERT[L%d-COMP]: new var %d (%.2f), cur_var %d (%.2f) node %d, depth %d\n",
                level, new_var, new_act, curr_var, curr_act, node_idx, curr_stage.depth);

            // Compare and determine which value stays at this level
            // assume we can always insert at the destination level and idx
            if (new_act > curr_act || curr_stage.depth == level) {
                // Current value has higher activity, it stays here
                // The new value will continue down the pipeline
                std::swap(new_var, curr_var);
                std::swap(new_act, curr_act);

                // write the updated curr_var
                stages[level][STAGE_WRITE].op_type = curr_stage.op_type;
                stages[level][STAGE_WRITE].node_idx = node_idx;
                stages[level][STAGE_WRITE].var = curr_var;
                stages[level][STAGE_WRITE].act = curr_act;
                stages[level][STAGE_WRITE].depth = curr_stage.depth;
                stages[level][STAGE_WRITE].path = curr_stage.path;
                stages[level][STAGE_WRITE].valid = true;
                stages[level][STAGE_WRITE].ready = true;
            }

            // update the inserting var and activity for next level
            if (level < MAX_HEAP_LEVELS - 1 && level < curr_stage.depth) {
                stages[level+1][STAGE_COMPARE].var = new_var;
                stages[level+1][STAGE_COMPARE].act = new_act;
                stages[level+1][STAGE_COMPARE].valid = true;
                stages[level+1][STAGE_COMPARE].ready = true;
            }

            stages[level][stage].reset();
            break;
        }

        case STAGE_WRITE: {
            // always ready for new operations
            // Update the on-chip level arrays (no off-chip index writes)
            setVar(level, node_idx, curr_stage.var);
            setActivity(level, node_idx, curr_stage.act);
            output.verbose(CALL_INFO, 6, 0, "INSERT[L%d-WRITE]: Write back node %d: var=%d (%.2f)\n",
                           level, node_idx, curr_stage.var, curr_stage.act);

            // If this is the destination level, we've completed the operation
            if (level == curr_stage.depth) {
                active_inserts--;
                sst_assert(active_inserts >= 0, CALL_INFO, -1, "active_inserts became negative\n");
            }

            stages[level][stage].reset();
            break;
        }
    }
}

void PipelinedHeap::handleStageReplace(int level, int stage) {
    PipelineStageOp& curr_stage = stages[level][stage];
    int node_idx = curr_stage.node_idx;

    switch (stage) {
        case STAGE_READ: {
            if (level == 0) {
                // bypass from L0 WRITE stage but skipped for simplicity
                Var root = getVar(0, 0);
                // could be replaced if it is the only node left
                if (heap_size == 0) root = curr_stage.var;
                assert(root != var_Undef);
                if (purge_suppress_) {
                    purge_suppress_ = false;
                    stat_purge_pops->addData(1);
                    sampleOccupancy();
                    output.verbose(CALL_INFO, 6, 0, "PURGE[L0-READ]: discarding stale root %d\n", root);
                } else {
                    sendResp(root);
                    // Popping any copy of a var clears its bit: if this was
                    // the fresh copy the var is no longer (freshly) resident;
                    // if the bit was already clear, a stale copy got cleaned.
                    if (inheap_[root]) {
                        inheap_[root] = false;
                        inheap_count_--;
                    } else {
                        stat_stale_pops->addData(1);
                    }
                    sampleOccupancy();
                }
                output.verbose(CALL_INFO, 6, 0, "REPLACE[L%d-READ]: removing Min %d\n", level, root);
                if (heap_size == 0) {
                    // If heap will be empty after removal, skip compare stage
                    // assume write stage is always ready
                    stages[level][STAGE_WRITE].op_type = curr_stage.op_type;
                    stages[level][STAGE_WRITE].node_idx = node_idx;
                    stages[level][STAGE_WRITE].var = var_Undef;
                    stages[level][STAGE_WRITE].act = -1.0;
                    stages[level][STAGE_WRITE].valid = true;
                    stages[level][STAGE_WRITE].ready = false;
                    curr_stage.reset();
                    break;
                }
            }

            // If both paths are blocked, stall this stage
            if (!stages[level][STAGE_COMPARE].ready
                && level < MAX_HEAP_LEVELS - 1
                && !stages[level+1][STAGE_READ].ready) {
                stages[level][stage].ready = false;
                break;
            }

            stages[level][stage].ready = true;  // always ready for onchip

            // start fetching children speculatively
            if (level < MAX_HEAP_LEVELS - 1) {
                int child_idx = getChildIdx(level, node_idx, 1);  // left child as start
                // node_idx, var and activity will be updated in COMPARE stage
                stages[level+1][STAGE_READ].op_type = curr_stage.op_type;
                stages[level+1][STAGE_READ].node_idx = child_idx;
                stages[level+1][STAGE_READ].valid = true;
                stages[level+1][STAGE_READ].ready = false;
                output.verbose(CALL_INFO, 6, 0, "REPLACE[L%d-READ]: inducing L%d children of node %d\n",
                               level, level+1, node_idx);
            }

            // Pass operation to COMPARE stage
            stages[level][STAGE_COMPARE].op_type = curr_stage.op_type;
            stages[level][STAGE_COMPARE].node_idx = node_idx;
            if (level == 0) {
                // root level use the input as replacement var, either last var or previous insertion
                stages[level][STAGE_COMPARE].var = curr_stage.var;
                stages[level][STAGE_COMPARE].act = curr_stage.act;
                stages[level][STAGE_COMPARE].valid = true;
                stages[level][STAGE_COMPARE].ready = true;
            } else {
                // lower levels get replacement var from upper levels
                // false because waiting for previous level's COMPARE to update replacement var
                stages[level][STAGE_COMPARE].valid = false;
                stages[level][STAGE_COMPARE].ready = false;
            }

            // Mark READ stage as ready for new operations
            stages[level][stage].reset();
            break;
        }

        case STAGE_COMPARE: {
            Var repl_var = curr_stage.var;
            double repl_act = curr_stage.act;

            Var left_child = var_Undef, right_child = var_Undef;
            double left_act = 0.0, right_act = 0.0;
            // bypass from next level's WRITE is omitted for simplicity
            // because next level always executes before this stage and has updated the memory
            // node_idx is updated by previous level's COMPARE
            // The deepest level has no children: without this guard a compare
            // executing at level MAX_HEAP_LEVELS-1 indexes heap_vars[MAX_HEAP_LEVELS]
            // out of bounds (reachable once heap_size >= 2^(MAX_HEAP_LEVELS-1)).
            bool has_children = level < MAX_HEAP_LEVELS - 1;
            uint32_t lchild_idx = has_children ? getChildIdx(level, node_idx, true) : 0;
            if (has_children) {
                left_child = getVar(level+1, lchild_idx);
                left_act = getActivity(level+1, lchild_idx);
            }
            bool has_right = has_children && heap_size >= ((lchild_idx + 1) | (1 << (level + 1)));
            if (has_right) {
                right_child = getVar(level+1, lchild_idx + 1);
                right_act = getActivity(level+1, lchild_idx + 1);
            }

            // comparison
            // Find the maximum child
            bool use_right = has_right && (right_act > left_act);
            Var max_child = use_right ? right_child : left_child;
            double max_act = use_right ? right_act : left_act;
            int max_child_idx = use_right ? (lchild_idx + 1) : lchild_idx;

            // Check if we need to swap
            if (max_act > repl_act && max_child != var_Undef) {
                // Pass maximum child to WRITE stage
                stages[level][STAGE_WRITE].op_type = curr_stage.op_type;
                stages[level][STAGE_WRITE].node_idx = node_idx;
                stages[level][STAGE_WRITE].var = max_child;
                stages[level][STAGE_WRITE].act = max_act;
                stages[level][STAGE_WRITE].valid = true;
                stages[level][STAGE_WRITE].ready = true;

                // Continue replacement to next level with the child that was chosen
                if (level < MAX_HEAP_LEVELS - 1) {
                    stages[level+1][STAGE_COMPARE].node_idx = max_child_idx;
                    stages[level+1][STAGE_COMPARE].var = repl_var;
                    stages[level+1][STAGE_COMPARE].act = repl_act;
                    stages[level+1][STAGE_COMPARE].valid = true;
                    stages[level+1][STAGE_COMPARE].ready = true;
                }

                output.verbose(CALL_INFO, 6, 0, "REPLACE[L%d-COMP]: %s child %d (%.2f) > repl_var %d (%.2f), swapping\n",
                    level, use_right ? "right" : "left", max_child, max_act, repl_var, repl_act);
            } else {
                // Current value is already the max, no swap needed
                stages[level][STAGE_WRITE].op_type = curr_stage.op_type;
                stages[level][STAGE_WRITE].node_idx = node_idx;
                stages[level][STAGE_WRITE].var = repl_var;
                stages[level][STAGE_WRITE].act = repl_act;
                stages[level][STAGE_WRITE].valid = true;
                stages[level][STAGE_WRITE].ready = true;

                // Replacement ends here, invalidate speculative operations
                if (level < MAX_HEAP_LEVELS - 1) {
                    stages[level+1][STAGE_READ].ready = true;
                    stages[level+1][STAGE_COMPARE].ready = true;
                }
                // cancel the READ stage
                if (level < MAX_HEAP_LEVELS - 2) {
                    stages[level+2][STAGE_READ].valid = false;
                    stages[level+2][STAGE_READ].ready = true;
                }

                output.verbose(CALL_INFO, 6, 0, "REPLACE[L%d-COMP]: repl_var %d (%.2f) >= both children, ends here\n",
                    level, repl_var, repl_act);
            }

            // Mark COMPARE stage as ready for new operations
            stages[level][stage].reset();
            break;
        }

        case STAGE_WRITE: {
            // Update the on-chip level arrays (no off-chip index writes)
            setVar(level, node_idx, curr_stage.var);
            setActivity(level, node_idx, curr_stage.act);

            output.verbose(CALL_INFO, 6, 0, "REPLACE[L%d-WRITE]: Write back node %d: var=%d (%.2f)\n",
                         level, node_idx, curr_stage.var, curr_stage.act);
            if (level != 0) assert(curr_stage.var != 0);
            // Mark this stage as ready for new operations
            stages[level][stage].reset();
            break;
        }
    }
}

void PipelinedHeap::handleRequest(HeapReqEvent* req) {
    output.verbose(CALL_INFO, 6, 0, "Received request: op=%d, arg=%d\n",
                   req->op, req->arg);

    // Assert var is valid. READ args are heap positions, which can exceed
    // num_vars once stale copies inflate heap_size, so only bound var ops.
    if (req->op == HeapReqEvent::INSERT || req->op == HeapReqEvent::BUMP) {
        sst_assert(req->arg != var_Undef && req->arg >= 1 && req->arg <= (int)num_vars,
            CALL_INFO, -1, "Invalid var %d for insert/bump (num_vars %zu)", req->arg, num_vars);
    }
    // Freshness-lemma guard, sampled at enqueue time: the solver must only
    // bump assigned vars. (Dispatch-time sampling would false-positive on
    // benign FIFO interleavings where a stalled bump dispatches after the
    // solver already backtracked.)
    if (req->op == HeapReqEvent::BUMP && assigned_ref_
        && req->arg < (int)assigned_ref_->size() && !(*assigned_ref_)[req->arg])
        stat_bump_unassigned->addData(1);
    request_queue.emplace_back(req->op, req->arg);
    delete req;
    maybe_active_ = true;
}

void PipelinedHeap::sendResp(int result) {
    response_port->send(new HeapRespEvent(result));
}

void PipelinedHeap::completeInsertFetch(Var v, double act) {
    insert_queue.emplace_back(v, act);
}

void PipelinedHeap::completeBump(Var v, double act) {
    if (act + *var_inc_ptr > 1e100) {
        // Trigger rescale. Stash the bump unwritten: it completes after the
        // sweep so the sweep cannot re-scale its result (the old code wrote
        // first and the sweep double-scaled the triggering var).
        output.verbose(CALL_INFO, 2, 0, "Rescaling variable activities (trigger var %d)\n", v);
        rescale = true;
        rescale_sweep_started_ = false;
        rescale_stash_.push_back({v, act});
        maybe_active_ = true;
        return;  // v stays in bumps_inflight_ until the post-sweep completion
    }
    setAct(v, act + *var_inc_ptr);
    bumps_inflight_.erase(v);
}

void PipelinedHeap::startRescaleSweep() {
    // All pre-trigger reads have drained; writes from bumps that completed
    // during the drain are already in flight ahead of the sweep reads, so the
    // sweep scales them too.
    *var_inc_ptr *= 1e-100;
    for (int level = 0; level < MAX_HEAP_LEVELS; level++) {
        for (int i = 0; i < (1 << level); i++) {
            if (heap_activities[level][i] > 0)
                heap_activities[level][i] *= 1e-100;
        }
    }
    // Queued-but-unstarted inserts carry pre-scale activities.
    for (auto& e : insert_queue) e.activity *= 1e-100;

    // On-chip sweep wall time: per-level SRAMs scale their occupied entries
    // in parallel at 1 entry/cycle, so the duration is the largest occupied
    // level (the partial last level or the full level above it).
    rescale_onchip_cycles_ = 0;
    for (int level = 0; level < MAX_HEAP_LEVELS; level++) {
        size_t level_base = ((size_t)1 << level) - 1;   // slots before this level
        if (heap_size <= level_base) break;
        size_t occupied = std::min((size_t)1 << level, heap_size - level_base);
        rescale_onchip_cycles_ = std::max(rescale_onchip_cycles_, occupied);
    }

    rescale_sweep_started_ = true;
    rescale_offchip_done_ = false;
    readBurstAll(var_ptr_base_addr, (num_vars + 1) * sizeof(double));
    output.verbose(CALL_INFO, 2, 0, "Rescale sweep started, var_inc now %g (on-chip %zu cycles)\n",
                   *var_inc_ptr, rescale_onchip_cycles_);
}

void PipelinedHeap::maybeFinishRescale() {
    if (rescale_sweep_started_ && rescale_offchip_done_ && rescale_onchip_cycles_ == 0)
        finishRescale();
}

void PipelinedHeap::finishRescale() {
    // All sweep writes are in flight; complete the stashed trigger bump(s)
    // with pre-scale reads scaled manually (their memory entries were swept,
    // and these writes are issued after the sweep writes, so they land last).
    for (auto& s : rescale_stash_) {
        double act = s.act * 1e-100 + *var_inc_ptr;
        setAct(s.var, act);
        bumps_inflight_.erase(s.var);
    }
    rescale_stash_.clear();
    rescale = false;
    rescale_sweep_started_ = false;
    rescale_offchip_done_ = false;
    output.verbose(CALL_INFO, 2, 0, "Rescale complete\n");
}

void PipelinedHeap::handleMem(SST::Interfaces::StandardMem::Request* req) {
    if (auto* read_resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        maybe_active_ = true;  // read responses feed the pipeline/queues; WriteResp below does not need ticks
        auto it = req_to_op.find(read_resp->getID());
        sst_assert(it != req_to_op.end(), CALL_INFO, -1, "Unexpected memory response ID %lu", read_resp->getID());

        PendingMemOp pending = it->second;
        req_to_op.erase(it);

        if (pending.type == PendingMemOpType::INSERT_FETCH
            || pending.type == PendingMemOpType::BUMP_RMW) {
            double act;
            sst_assert(read_resp->data.size() >= sizeof(double), CALL_INFO, -1,
                "Memory response data size too small: %zu\n", read_resp->data.size());
            memcpy(&act, read_resp->data.data(), sizeof(double));

            if (pending.type == PendingMemOpType::BUMP_RMW) completeBump(pending.var, act);
            else completeInsertFetch(pending.var, act);
        } else if (pending.type == PendingMemOpType::RESCALE) {
            const size_t chunk_size = pending.size;
            const size_t entry_size = sizeof(double);
            sst_assert(chunk_size % entry_size == 0, CALL_INFO, -1,
                       "Rescale chunk size %zu is not aligned to activity size %zu",
                       chunk_size, entry_size);

            const double* resp_entries = reinterpret_cast<const double*>(read_resp->data.data());
            std::vector<double> scaled_entries(resp_entries, resp_entries + (chunk_size / entry_size));
            for (auto& entry : scaled_entries) {
                entry *= 1e-100;
            }

            std::vector<uint8_t> write_data(chunk_size);
            memcpy(write_data.data(), scaled_entries.data(), chunk_size);

            uint64_t write_addr = read_resp->pAddr;
            if (WRITE_BUFFER) {
                // Sweep writes enter the store queue too: a later read must
                // forward the scaled value, never an older pre-sweep entry.
                store_queue.push_back(StoreQueueEntry(write_addr, chunk_size, write_data));
            }
            if (tracer_) tracer_->emitMem(true, write_addr, (uint32_t)chunk_size);
            memory->send(new SST::Interfaces::StandardMem::Write(write_addr, chunk_size, write_data));

            if (rescale_pending_reads > 0) {
                rescale_pending_reads--;
                if (rescale_pending_reads == 0) {
                    rescale_offchip_done_ = true;
                    maybeFinishRescale();
                }
            }
        } else if (pending.type == PendingMemOpType::DEBUG) {
            // Handle debug heap verification - collect all activities first
            const size_t chunk_size = pending.size;
            const size_t entry_size = sizeof(double);
            sst_assert(chunk_size % entry_size == 0, CALL_INFO, -1,
                       "Chunk size %zu is not aligned to activity size %zu",
                       chunk_size, entry_size);

            // Get base address to calculate var indices
            uint64_t base_offset = read_resp->pAddr - var_ptr_base_addr;
            size_t start_idx = base_offset / entry_size;

            const double* resp_entries = reinterpret_cast<const double*>(read_resp->data.data());
            for (size_t i = 0; i < (chunk_size / entry_size); i++) {
                Var curr_var = start_idx + i;
                debug_heap_acts[curr_var] = resp_entries[i];
            }

            // Check if we're done with all reads for debug
            if (rescale_pending_reads > 0) {
                rescale_pending_reads--;
                if (rescale_pending_reads == 0) {
                    verifyDebugHeap();  // All data collected, now perform the verification
                }
            }
        }
    } else if (auto* write_resp = dynamic_cast<SST::Interfaces::StandardMem::WriteResp*>(req)) {
        assert(!write_resp->getFail() && "Write response should not fail");
        if (WRITE_BUFFER) {
            uint64_t addr = write_resp->pAddr;
            // Find and remove the oldest matching store queue entry by address (front of queue)
            for (auto it = store_queue.begin(); it != store_queue.end(); ++it) {
                if (it->addr == addr) {
                    store_queue.erase(it);
                    break;
                }
            }
        }
    }
    delete req;
}

bool PipelinedHeap::isPipelineIdle() const {
    for (int level = 0; level < MAX_HEAP_LEVELS; ++level) {
        for (int stage = 0; stage < PIPELINE_DEPTH; ++stage) {
            if (stages[level][stage].valid) {
                return false;
            }
        }
    }

    // Verify active_inserts is consistent
    sst_assert(active_inserts == 0, CALL_INFO, -1, "active_inserts not 0 when pipeline is idle\n");

    return true;
}

void PipelinedHeap::initHeap(uint64_t random_seed) {
    bumps_inflight_.clear();
    inheap_.assign(num_vars + 1, false);
    inheap_count_ = 0;
    // Capacity is 2^MAX_HEAP_LEVELS - 1 (levels 0..MAX_HEAP_LEVELS-1, 1-indexed
    // tree): must match the INSERT-path bound in startOperation, and must stay
    // an sst_assert so it cannot be compiled out.
    sst_assert(heap_size <= (size_t)MAX_HEAP_SIZE, CALL_INFO, -1,
        "Instance has %lu vars but pipelined heap capacity is %u (MAX_HEAP_LEVELS=%d)\n",
        heap_size, MAX_HEAP_SIZE, MAX_HEAP_LEVELS);
    // Collect all decision variables first
    std::vector<Var> decision_vars;
    for (Var v = 1; v <= (Var)num_vars; v++) {
        if (decision[v]) {
            decision_vars.push_back(v);
        }
    }
    heap_size = decision_vars.size();

    // Randomize if a seed is provided
    if (random_seed != 0) {
        output.verbose(CALL_INFO, 1, 0, "Randomizing heap with seed %lu\n", random_seed);
        std::mt19937 rng(random_seed);
        std::shuffle(decision_vars.begin(), decision_vars.end(), rng);
    }

    // initialize var activity memory
    std::vector<double> values((num_vars + 1), 0.0);

    // Build heap level by level
    size_t added = 0;
    for (int level = 0; level < MAX_HEAP_LEVELS && added < heap_size; level++) {
        int level_size = 1 << level;
        for (int i = 0; i < level_size; i++) {
            heap_vars[level][i] = decision_vars[added];
            heap_activities[level][i] = 0.0;
            inheap_[decision_vars[added]] = true;
            added++;
            if (added >= heap_size) break;
        }
    }
    inheap_count_ = added;

    std::vector<uint8_t> buffer((num_vars + 1) * sizeof(double));
    memcpy(buffer.data(), values.data(), buffer.size());
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        actAddr(0), buffer.size(), buffer, true,
        static_cast<uint32_t>(SST::Interfaces::StandardMem::Request::Flag::F_NONCACHEABLE)));

    output.verbose(CALL_INFO, 1, 0, "Heap Size: %lu entries\n", heap_size);
    output.verbose(CALL_INFO, 1, 0, "Var Act Size: %zu entries, %zu bytes\n",
                   (num_vars + 1), (num_vars + 1) * sizeof(double));
    output.verbose(CALL_INFO, 1, 0, "Inheap bitmap: %zu bits\n", num_vars + 1);
}

// Helper methods
int PipelinedHeap::getChildIdx(int level, int node_idx, bool left) {
    // Calculate child index based on current level and node index
    return left ? (node_idx * 2) : (node_idx * 2 + 1);
}

double PipelinedHeap::getActivity(int level, int idx) {
    assert(idx >= 0 && idx < heap_activities[level].size());
    return heap_activities[level][idx];
}

Var PipelinedHeap::getVar(int level, int idx) {
    assert(idx >= 0 && idx < heap_vars[level].size());
    return heap_vars[level][idx];
}

void PipelinedHeap::setActivity(int level, int idx, double value) {
    assert(idx >= 0 && idx < heap_activities[level].size());
    sst_assert(value <= 1e100, CALL_INFO, -1, "activity out of bound\n");
    heap_activities[level][idx] = value;
}

void PipelinedHeap::setVar(int level, int idx, Var value) {
    assert(idx >= 0 && idx < heap_vars[level].size());
    heap_vars[level][idx] = value;
}

void PipelinedHeap::setAct(Var v, double act) {
    // update the authoritative off-chip activity
    sst_assert(act <= 1e100, CALL_INFO, -1, "activity out of bound\n");
    size_t size = sizeof(double);
    uint64_t addr = actAddr(v);
    std::vector<uint8_t> data(size);
    memcpy(data.data(), &act, size);
    if (WRITE_BUFFER) {
        // Always add a new entry to the store queue
        StoreQueueEntry entry(addr, size, data);
        store_queue.push_back(entry);
    }

    if (tracer_) tracer_->emitMem(true, addr, (uint32_t)size);
    memory->send(new SST::Interfaces::StandardMem::Write(addr, size, data));
}

void PipelinedHeap::getAct(Var v, bool bump) {
    output.verbose(CALL_INFO, 6, 0, "Get act: var %d, bump=%d\n", v, bump);
    size_t size = sizeof(double);
    uint64_t addr = actAddr(v);
    if (WRITE_BUFFER) {
        // forward from store queue if possible (entry may be a full line
        // from the rescale sweep, so honor the offset within it)
        int idx = findStoreQueueEntry(addr, size);
        if (idx >= 0) {
            double act;
            memcpy(&act, store_queue[idx].data.data() + (addr - store_queue[idx].addr), size);
            if (bump) completeBump(v, act);
            else completeInsertFetch(v, act);
            return;
        }
    }

    // read from memory; handleMem completes the bump/insert
    auto req = new SST::Interfaces::StandardMem::Read(addr, size);
    req_to_op[req->getID()] = PendingMemOp(
        bump ? PendingMemOpType::BUMP_RMW : PendingMemOpType::INSERT_FETCH, v);
    if (tracer_) tracer_->emitMem(false, addr, (uint32_t)size);
    memory->send(req);
}

int PipelinedHeap::findStoreQueueEntry(uint64_t addr, size_t size) {
    for (int i = store_queue.size() - 1; i >= 0; i--) {
        // Check if read address range falls completely within the store address range
        uint64_t store_start = store_queue[i].addr;
        uint64_t store_end = store_start + store_queue[i].size - 1;
        uint64_t read_end = addr + size - 1;

        if (addr >= store_start && read_end <= store_end) return i;
    }
    return -1; // Not found
}

void PipelinedHeap::readBurstAll(uint64_t start_addr, size_t total_size) {
    rescale_pending_reads = 0;

    size_t offset = 0;
    while (offset < total_size) {
        uint64_t current_addr = start_addr + offset;
        uint64_t line_offset = current_addr % line_size;
        size_t remaining = total_size - offset;
        size_t bytes_in_line = line_size - line_offset;
        size_t chunk_size = std::min(bytes_in_line, remaining);

        auto* req = new SST::Interfaces::StandardMem::Read(current_addr, chunk_size);
        // Use appropriate type based on debug_heap_pending flag
        PendingMemOpType type = debug_heap_pending ? PendingMemOpType::DEBUG : PendingMemOpType::RESCALE;
        req_to_op.emplace(req->getID(), PendingMemOp(type, offset, chunk_size));
        rescale_pending_reads++;
        if (tracer_) tracer_->emitMem(false, current_addr, (uint32_t)chunk_size);
        memory->send(req);

        offset += chunk_size;
    }
}

void PipelinedHeap::verifyDebugHeap() {
    output.verbose(CALL_INFO, 6, 0, "DEBUG_HEAP: Verifying heap consistency...\n");

    // Collect per-var copy counts and per-var max stored activity, checking
    // structural invariants along the way.
    std::unordered_map<Var, int> copies;
    std::unordered_map<Var, double> max_stored;
    size_t occupied = 0;

    for (int level = 0; level < MAX_HEAP_LEVELS; ++level) {
        for (size_t idx = 0; idx < heap_vars[level].size(); ++idx) {
            Var var = heap_vars[level][idx];
            uint32_t slot = (1u << level) | idx;
            bool in_range = slot <= heap_size;
            if (var != var_Undef) {
                if (!in_range) {
                    output.verbose(CALL_INFO, 0, 0,
                        "DEBUG_HEAP ERROR: occupied slot %u (var %d) beyond heap_size %zu\n",
                        slot, var, heap_size);
                    debug_heap_errors++;
                    continue;
                }
                occupied++;
                double act = heap_activities[level][idx];
                copies[var]++;
                auto it = max_stored.find(var);
                if (it == max_stored.end() || act > it->second) max_stored[var] = act;

                // Heap property: parent stored act >= child stored act
                if (level > 0) {
                    double parent_act = heap_activities[level-1][idx >> 1];
                    if (parent_act < act) {
                        output.verbose(CALL_INFO, 0, 0,
                            "DEBUG_HEAP ERROR: heap property violated at (L%d,i%zu): "
                            "parent %.12g < child %.12g (var %d)\n",
                            level, idx, parent_act, act, var);
                        debug_heap_errors++;
                    }
                }
            } else if (in_range) {
                output.verbose(CALL_INFO, 0, 0,
                    "DEBUG_HEAP ERROR: empty slot %u within heap_size %zu\n", slot, heap_size);
                debug_heap_errors++;
            }
        }
    }

    if (occupied != heap_size) {
        output.verbose(CALL_INFO, 0, 0,
            "DEBUG_HEAP ERROR: %zu occupied slots but heap_size is %zu\n", occupied, heap_size);
        debug_heap_errors++;
    }

    // Bitmap consistency
    size_t bit_count = 0;
    for (Var v = 1; v <= (Var)num_vars; v++) {
        if (!inheap_[v]) continue;
        bit_count++;
        // Freshness lemma: bit set => a copy is resident and the max stored
        // activity equals the authoritative memory activity.
        auto it = copies.find(v);
        if (it == copies.end()) {
            output.verbose(CALL_INFO, 0, 0,
                "DEBUG_HEAP ERROR: inheap[%d]=1 but no copy resident\n", v);
            debug_heap_errors++;
            continue;
        }
        double mem_act = debug_heap_acts.count(v) ? debug_heap_acts[v] : -1.0;
        double stored = max_stored[v];
        double tol = std::max(1e-8, std::abs(mem_act) * 1e-12);
        if (std::abs(stored - mem_act) > tol) {
            output.verbose(CALL_INFO, 0, 0,
                "DEBUG_HEAP ERROR: var %d fresh copy act %.12g != mem act %.12g\n",
                v, stored, mem_act);
            debug_heap_errors++;
        }
    }
    if (bit_count != inheap_count_) {
        output.verbose(CALL_INFO, 0, 0,
            "DEBUG_HEAP ERROR: inheap bitmap popcount %zu != inheap_count %zu\n",
            bit_count, inheap_count_);
        debug_heap_errors++;
    }

    // Stale copies never exceed the authoritative activity
    for (const auto& [var, stored] : max_stored) {
        double mem_act = debug_heap_acts.count(var) ? debug_heap_acts[var] : -1.0;
        double tol = std::max(1e-8, std::abs(mem_act) * 1e-12);
        if (stored > mem_act + tol) {
            output.verbose(CALL_INFO, 0, 0,
                "DEBUG_HEAP ERROR: var %d stored act %.12g exceeds mem act %.12g\n",
                var, stored, mem_act);
            debug_heap_errors++;
        }
    }

    // Soundness: every unassigned decision var has at least one copy resident
    if (assigned_ref_) {
        for (Var v = 1; v <= (Var)num_vars; v++) {
            if (v < (Var)decision.size() && decision[v]
                && v < (Var)assigned_ref_->size() && !(*assigned_ref_)[v]
                && copies.find(v) == copies.end()) {
                output.verbose(CALL_INFO, 0, 0,
                    "DEBUG_HEAP ERROR: unassigned decision var %d has no copy in heap\n", v);
                debug_heap_errors++;
            }
        }
    }

    output.verbose(CALL_INFO, 6, 0,
        "DEBUG_HEAP: Verification complete, %d errors (occupancy %zu, live %zu, stale %zu)\n",
        debug_heap_errors, heap_size, inheap_count_, staleCount());

    // Send the response and clear the debug state
    sendResp(debug_heap_errors);
    debug_heap_pending = false;
    debug_heap_acts.clear();
}
