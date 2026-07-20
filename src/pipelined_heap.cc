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
    nodes_base_(0),
    heap_capacity_(0),
    next_ctx_id_(1),
    node_writes_inflight_(0),
    tail_refills_inflight_(0),
    refill_gen_(0),
    active_inserts(0),
    rescale(false),
    rescale_sweep_started_(false),
    rescale_offchip_done_(false),
    rescale_onchip_cycles_(0),
    rescale_pending_reads(0),
    burst_inflight_(0),
    purge_suppress_(false),
    rebuild_queued_(false),
    debug_heap_pending(false),
    debug_heap_errors(0) {

    output.init("PHEAP-> ", params.find<int>("verbose", 0), 0, SST::Output::STDOUT);

    var_ptr_base_addr = std::stoull(params.find<std::string>("var_act_base_addr", "0x1C0000000"), nullptr, 0);
    heap_region_end_ = std::stoull(params.find<std::string>("heap_region_end", "0x200000000"), nullptr, 0);
    onchip_levels_ = params.find<int>("onchip_levels", 14);
    if (onchip_levels_ == 0) onchip_levels_ = MAX_TOTAL_HEAP_LEVELS;
    sst_assert(onchip_levels_ >= 1 && onchip_levels_ <= MAX_TOTAL_HEAP_LEVELS,
        CALL_INFO, -1, "onchip_levels %d out of range [0..%d]\n",
        onchip_levels_, MAX_TOTAL_HEAP_LEVELS);

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
    stat_olc_node_reads          = registerStatistic<uint64_t>("olc_node_reads");
    stat_olc_node_writes         = registerStatistic<uint64_t>("olc_node_writes");
    stat_olc_boundary_crossings  = registerStatistic<uint64_t>("olc_boundary_crossings");
    stat_olc_insert_ctx_sample   = registerStatistic<uint64_t>("olc_insert_ctx_sample");
    stat_olc_sift_parked_sample  = registerStatistic<uint64_t>("olc_sift_parked_sample");
    stat_olc_tail_refills        = registerStatistic<uint64_t>("olc_tail_refills");
    stat_olc_tail_stalls         = registerStatistic<uint64_t>("olc_tail_stalls");

    // Initialize heap memories for each on-chip level
    for (int level = 0; level < onchip_levels_; level++) {
        int level_size = 1 << level;  // 2^level nodes at this level
        heap_vars[level].resize(level_size, var_Undef);
        heap_activities[level].resize(level_size, -1);
    }

    // Initialize pipeline stages
    for (int level = 0; level < onchip_levels_; level++) {
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
    if (last_level >= onchip_levels_) {
        // Off-chip tail: read via the tail buffer. Skip when the line has not
        // been refilled yet, and while the active sift's dynamic path could
        // still reach this slot (its write would land on a trimmed-dead slot
        // and the carried value would be lost).
        uint64_t slot = heap_size;
        if (!tailSlotReady(slot)) return false;
        if (sift_.active) {
            int lvl_gap = (63 - __builtin_clzll(slot)) - (63 - __builtin_clzll(sift_.slot));
            if (lvl_gap >= 0 && (slot >> lvl_gap) == sift_.slot) return false;
        }
        TailLine* tl = tailLineFor(slot);
        OlcNode& n = tl->node[(slot - lineBaseSlot(tl->line)) & 3];
        if (n.var == var_Undef || inheap_[n.var]) return false;
        Var w = n.var;
        n = OlcNode();
        tl->slot_valid[(slot - lineBaseSlot(tl->line)) & 3] = true;  // cleared, still known
        tl->dirty = true;
        heap_size--;
        tailDropAbove(heap_size);
        stat_tail_trims->addData(1);
        sampleOccupancy();
        output.verbose(CALL_INFO, 6, 0, "TAIL-TRIM: dropped stale var %d at off-chip slot %lu\n",
                       w, slot);
        return true;
    }
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

    // Off-chip level controller first: it sits "below" the deepest on-chip
    // level, so its completions (boundary SRAM writes, context retires) land
    // before this tick's dispatch gates and stage compares observe them.
    olcTick();

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
                if (lvl >= onchip_levels_) {
                    // Off-chip peek (random-decision heuristic): resolve via
                    // the OLC ordering point; respond when data arrives. A
                    // transiently stale value under an active sift is
                    // acceptable -- this is a heuristic sample, not a pop.
                    uint64_t slot = (uint64_t)(uint32_t)pending.arg;
                    OlcNode n;
                    int r = nodeReadInstant(slot, n);
                    if (r == 1) {
                        sendResp(n.var);
                        request_queue.pop_front();
                    } else if (r == 0 && olcMemBudgetOk(OLC_RESERVE_SIFT)) {
                        uint64_t addr = nodeAddr(slot);
                        auto* req = new SST::Interfaces::StandardMem::Read(addr, 16);
                        olc_pending[req->getID()] =
                            OlcPendingRead(OlcMemType::READ_PEEK, slot, 0, 0);
                        stat_olc_node_reads->addData(1);
                        if (tracer_) tracer_->emitMem(false, addr, 16);
                        memory->send(req);
                        request_queue.pop_front();
                    }
                    // r == 2: buffered but unfilled -- retry next tick
                    break;
                }
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
                    && req_to_op.size() < (size_t)ACT_READS_MAX
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
                if (!rescale && !debug_heap_pending
                    && req_to_op.size() < (size_t)ACT_READS_MAX) {
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
                // (active_inserts covers OLC insert contexts too: they retire
                // only when the off-chip dest write is issued). popGateOk()
                // adds the off-chip tail conditions: tail line resident in
                // the buffer, and the active sift's subtree cannot reach the
                // slot the grab will take.
                if (!rescale && active_inserts == 0 && req_to_op.empty()
                    && insert_queue.empty() && canStartOperation(HEAP_OP_REPLACE)
                    && popGateOk()) {
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
                    && req_to_op.empty() && insert_queue.empty() && isPipelineIdle()
                    && olcIdle()) {
                    for (int level = 0; level < onchip_levels_; level++) {
                        size_t level_base = ((size_t)1 << level) - 1;
                        if (heap_size <= level_base) break;
                        size_t occupied = std::min((size_t)1 << level, heap_size - level_base);
                        std::fill_n(heap_vars[level].begin(), occupied, var_Undef);
                        std::fill_n(heap_activities[level].begin(), occupied, -1.0);
                    }
                    // Off-chip: bounds guards make the node region unreachable
                    // once heap_size resets, so the wipe is O(on-chip): drop
                    // the tail window (dirty lines are dead) and any pending
                    // node writes still in the store queue (already-issued
                    // ones land on dead slots behind the guards).
                    tailWipe();
                    output.verbose(CALL_INFO, 2, 0,
                        "REBUILD: wiped %zu entries (%zu live, %zu stale)\n",
                        heap_size, inheap_count_, staleCount());
                    heap_size = 0;
                    std::fill(inheap_.begin(), inheap_.end(), false);
                    inheap_count_ = 0;
                    rebuild_queued_ = false;
                    stat_rebuilds->addData(1);
                    sampleOccupancy();
                    request_queue.pop_front();
                }
                break;
            }
            case HeapReqEvent::DEBUG_HEAP: {
                // Wait for all previous requests and pipeline to finish
                if (active_inserts == 0 && req_to_op.empty() && insert_queue.empty()
                    && isPipelineIdle() && !rescale && olcIdle()) {
                    // Start debug heap check
                    debug_heap_pending = true;
                    debug_heap_errors = 0;
                    debug_heap_acts.clear();
                    debug_heap_acts.reserve(num_vars + 1);
                    debug_nodes_.clear();

                    if (heap_size == 0 && inheap_count_ == 0) {
                        sendResp(0);
                        debug_heap_pending = false;
                    } else {
                        output.verbose(CALL_INFO, 6, 0, "DEBUG_HEAP: Reading memory for heap verification\n");
                        rescale_pending_reads = 0;
                        readBurstAll(var_ptr_base_addr, (num_vars + 1) * sizeof(double));
                        if (heap_size >= firstOffchipSlot()) {
                            // Snapshot the occupied node region too. Dirty
                            // tail lines / pending node writes are overlaid
                            // at verification time (debugNodeAt), so no flush
                            // is needed to read truth.
                            readBurstAll(nodes_base_,
                                         (heap_size - firstOffchipSlot() + 1) * 16);
                        }
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
        // tailTrimOne self-gates on the off-chip tail (buffer readiness +
        // sift subtree); the purge's replacement grab needs the same gate.
        if (!tailTrimOne() && heap_size > 0) {
            Var root = getVar(0, 0);
            if (root != var_Undef && !inheap_[root] && canStartOperation(HEAP_OP_REPLACE)
                && popGateOk()) {
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
    // olcIdle() extends the drain to sift/insert contexts and tail refills:
    // a refill response landing after the sweep would install pre-scale acts.
    if (rescale && !rescale_sweep_started_ && req_to_op.empty()
        && active_inserts == 0 && isPipelineIdle() && olcIdle()) {
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
        && active_inserts == 0 && isPipelineIdle() && olcIdle()
        && !idleWorkAvailable();
}

bool PipelinedHeap::idleWorkAvailable() const {
    if (heap_size == 0) return false;
    uint32_t last_level, last_idx;
    lastSlot(last_level, last_idx);
    if (last_level >= onchip_levels_) {
        // Off-chip tail: only claim work if the slot is resident (a stale
        // var there would be trimmable). Root check below still applies.
        uint64_t slot = heap_size;
        for (const TailLine& tl : tail_win_) {
            if (tl.line == lineOf(slot)) {
                const OlcNode& n = tl.node[(slot - lineBaseSlot(tl.line)) & 3];
                if (tl.slot_valid[(slot - lineBaseSlot(tl.line)) & 3]
                    && n.var != var_Undef && !inheap_[n.var]) return true;
                break;
            }
        }
    } else {
        Var w = heap_vars[last_level][last_idx];
        if (w != var_Undef && !inheap_[w]) return true;
    }
    Var root = heap_vars[0][0];
    return root != var_Undef && !inheap_[root];
}

void PipelinedHeap::advancePipeline() {
    // Process each level in reverse order (bottom-up)
    for (int level = onchip_levels_ - 1; level >= 0; level--) {
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
        // num_vars; only the region-derived capacity bounds it.
        sst_assert(heap_size <= heap_capacity_, CALL_INFO, -1,
            "Failed to insert var %d: heap size overflow (capacity %lu)\n",
            arg, heap_capacity_);
        uint32_t dest = heap_size;

        uint32_t target_level = priority_encoder(dest);
        // need to pass down depth for termination condition
        stages[0][STAGE_READ].depth = target_level;
        stages[0][STAGE_READ].dest = dest;
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

        if (last_level >= onchip_levels_) {
            // Off-chip tail: grab from the tail buffer (dispatch gate ensured
            // residency and that no in-flight op can still write this slot:
            // active_inserts==0 covers OLC inserts, popGateOk covers the
            // active sift's subtree).
            OlcNode n = tailGrab(heap_size);
            arg = n.var;
            activity = n.act;
            output.verbose(CALL_INFO, 6, 0,
                "Start REPLACE: heap_size=%lu, off-chip last var %d (%.2f)\n",
                heap_size, arg, activity);
            heap_size--;
            tailDropAbove(heap_size);
            sampleOccupancy();
        } else {
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
                if (last_level < onchip_levels_ - 1) {
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
            // Valid/ready backpressure: this READ feeds this level's COMPARE
            // and (when the descent continues on-chip) the induced next-level
            // READ. Either target still occupied by a parked op (OLC
            // backpressure) => hold; the stall propagates up one level/cycle.
            // A pre-staged (valid=false) compare left by an abandoned op is
            // dead and safe to overwrite: a LIVE reservation's owner always
            // has a valid compare one level up, which holds this op's chain
            // there. So only .valid gates here.
            bool will_induce = level < onchip_levels_ - 1 && level < curr_stage.depth;
            bool comp_free = !stages[level][STAGE_COMPARE].valid;
            bool next_read_free = !will_induce || !stages[level+1][STAGE_READ].valid;
            if (!comp_free || !next_read_free) {
                output.verbose(CALL_INFO, 5, 0,
                    "INSERT[L%d-READ]: HOLD var %d (comp v%d, nextread v%d)\n",
                    level, curr_stage.var, stages[level][STAGE_COMPARE].valid,
                    level < onchip_levels_-1 ? stages[level+1][STAGE_READ].valid : -1);
                stages[level][stage].ready = false;
                break;
            }
            output.verbose(CALL_INFO, 6, 0, "INSERT[L%d-READ]: var %d (%.2f), node %d, depth %d, path 0x%x\n",
                level, curr_stage.var, curr_stage.act, node_idx, curr_stage.depth, curr_stage.path);

            stages[level][stage].ready = true;

            // Determine if and where to send operation to next level
            if (will_induce) {
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
                stages[level+1][STAGE_READ].dest = curr_stage.dest;
                stages[level+1][STAGE_READ].valid = true;
                stages[level+1][STAGE_READ].ready = false;
                output.verbose(CALL_INFO, 6, 0, "INSERT[L%d-READ]: inducing L%d node %d, %s child\n",
                               level, level+1, child_idx, go_left ? "left" : "right");
            } else output.verbose(CALL_INFO, 6, 0, "INSERT[L%d-READ]: insertion ends this level or goes off-chip\n", level);

            stages[level][STAGE_COMPARE].op_type = curr_stage.op_type;
            stages[level][STAGE_COMPARE].node_idx = node_idx;
            stages[level][STAGE_COMPARE].var = curr_stage.var;
            stages[level][STAGE_COMPARE].act = curr_stage.act;
            stages[level][STAGE_COMPARE].depth = curr_stage.depth;
            stages[level][STAGE_COMPARE].path = curr_stage.path;
            stages[level][STAGE_COMPARE].dest = curr_stage.dest;
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
            bool descends = level < curr_stage.depth;
            bool offchip_next = descends && level == onchip_levels_ - 1;

            // Hold BEFORE any reads/side effects: the descent target compare
            // may hold a parked op, and at the boundary the OLC may reject.
            // Also hold while this op's own read chain is parked at the next
            // level: the descend fill relies on READ[level+1] having
            // pre-staged COMPARE[level+1]'s node_idx/path/depth.
            if (descends && !offchip_next
                && (stages[level+1][STAGE_COMPARE].valid
                    || stages[level+1][STAGE_READ].valid)) {
                output.verbose(CALL_INFO, 5, 0,
                    "INSERT[L%d-COMP]: HOLD var %d (nextcomp v%d, nextread v%d)\n",
                    level, curr_stage.var, stages[level+1][STAGE_COMPARE].valid,
                    stages[level+1][STAGE_READ].valid);
                curr_stage.ready = false;
                break;
            }
            if (offchip_next) {
                // The active sift's unfinished boundary write targets a K-1
                // slot; this compare reads its own K-1 path slot which could
                // be that slot (conservative one-bit hold, ~1 RTT window).
                if (sift_.active && sift_.boundary_write_pending) {
                    output.verbose(CALL_INFO, 5, 0,
                        "INSERT[L%d-COMP]: HOLD var %d (sift boundary pending)\n",
                        level, curr_stage.var);
                    curr_stage.ready = false;
                    break;
                }
                if (!olcCanAcceptInsert()) {
                    output.verbose(CALL_INFO, 5, 0,
                        "INSERT[L%d-COMP]: HOLD var %d (OLC reject: sift %d ctxs %zu)\n",
                        level, curr_stage.var, sift_.active, insert_ctxs_.size());
                    curr_stage.ready = false;
                    break;
                }
            }

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
            if (descends) {
                if (offchip_next) {
                    // Cross the boundary: the descending value continues in an
                    // OLC insert context; the on-chip stage frees immediately.
                    olcStartInsert(new_var, new_act, curr_stage.dest);
                } else {
                    stages[level+1][STAGE_COMPARE].var = new_var;
                    stages[level+1][STAGE_COMPARE].act = new_act;
                    stages[level+1][STAGE_COMPARE].valid = true;
                    stages[level+1][STAGE_COMPARE].ready = true;
                }
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
            // Backpressure check FIRST (before the level-0 response side
            // effects, which must not repeat on a held retry): hold while the
            // compare this READ feeds, or the next-level READ it would
            // induce, still holds a parked op.
            {
                // .valid-only gating: see the INSERT READ note (dead
                // reservations are overwritable; live ones are protected by
                // the owner's valid compare upstream).
                bool will_induce = level < onchip_levels_ - 1;
                bool comp_free = !stages[level][STAGE_COMPARE].valid;
                bool next_read_free = !will_induce || !stages[level+1][STAGE_READ].valid;
                if (!comp_free || !next_read_free) {
                    stages[level][stage].ready = false;
                    break;
                }
            }

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

            stages[level][stage].ready = true;

            // start fetching children speculatively (on-chip levels only; the
            // OLC issues its own reads at the boundary handoff)
            if (level < onchip_levels_ - 1) {
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
            // Guards BEFORE any child reads: a parked op below may own slots
            // this compare would read.
            // (b) descent target compare still occupied (parked sift/insert),
            // or the next level has a pending WRITE: a hold anywhere below
            // delays writes by a tick, and this compare would read the
            // pre-write child value (the "next level always executes before
            // this stage" invariant only holds when nothing ever stalls).
            if (level < onchip_levels_ - 1
                && (stages[level+1][STAGE_COMPARE].valid
                    || stages[level+1][STAGE_WRITE].valid)) {
                curr_stage.ready = false;
                break;
            }
            // (a) this compare reads K-1 children while the active sift's
            // boundary write to its K-1 slot is pending: one of those slots
            // may be it (holds a stale duplicate until the write lands).
            if (level == onchip_levels_ - 2 && sift_.active && sift_.boundary_write_pending) {
                curr_stage.ready = false;
                break;
            }

            // This op's speculative child fetch may still be parked below
            // (held READ that never executed). It is dead weight now -- the
            // compare reads the arrays directly, and leaving it would let it
            // fire AFTER the op passes, stranding a reservation nobody fills.
            if (level < onchip_levels_ - 1 && stages[level+1][STAGE_READ].valid
                && stages[level+1][STAGE_READ].op_type == HEAP_OP_REPLACE) {
                stages[level+1][STAGE_READ].reset();
            }

            Var repl_var = curr_stage.var;
            double repl_act = curr_stage.act;

            if (level == onchip_levels_ - 1) {
                // Boundary: children (if any) live off-chip.
                uint64_t s = ((uint64_t)1 << level) | (uint64_t)node_idx;
                if (2 * s <= heap_size) {
                    // Hand the boundary compare to the OLC (it reads the
                    // K-children + grandchildren lines, writes this slot back
                    // into the boundary SRAM, and percolates below).
                    if (!olcCanAcceptSift()) {
                        curr_stage.ready = false;
                        sampleSiftParked();
                        break;
                    }
                    olcStartSift(s, repl_var, repl_act);
                    output.verbose(CALL_INFO, 6, 0,
                        "REPLACE[L%d-COMP]: handing off slot %lu var %d (%.2f) to OLC\n",
                        level, s, repl_var, repl_act);
                    curr_stage.reset();
                    break;
                }
                // No off-chip children: the replacement settles here.
                stages[level][STAGE_WRITE].op_type = curr_stage.op_type;
                stages[level][STAGE_WRITE].node_idx = node_idx;
                stages[level][STAGE_WRITE].var = repl_var;
                stages[level][STAGE_WRITE].act = repl_act;
                stages[level][STAGE_WRITE].valid = true;
                stages[level][STAGE_WRITE].ready = true;
                output.verbose(CALL_INFO, 6, 0, "REPLACE[L%d-COMP]: repl_var %d (%.2f) settles at boundary leaf\n",
                    level, repl_var, repl_act);
                stages[level][stage].reset();
                break;
            }

            Var left_child = var_Undef, right_child = var_Undef;
            double left_act = 0.0, right_act = 0.0;
            // bypass from next level's WRITE is omitted for simplicity
            // because next level always executes before this stage and has updated the memory
            // node_idx is updated by previous level's COMPARE
            // (level == onchip_levels_-1 was fully handled above, so child
            // array reads here always stay within the on-chip arrays.)
            bool has_children = level < onchip_levels_ - 1;
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
                stages[level+1][STAGE_COMPARE].node_idx = max_child_idx;
                stages[level+1][STAGE_COMPARE].var = repl_var;
                stages[level+1][STAGE_COMPARE].act = repl_act;
                stages[level+1][STAGE_COMPARE].valid = true;
                stages[level+1][STAGE_COMPARE].ready = true;

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

                // Replacement ends here, invalidate speculative operations.
                // The induced READ below may not have executed yet if it was
                // held by backpressure -- kill it outright (valid=false).
                stages[level+1][STAGE_READ].valid = false;
                stages[level+1][STAGE_READ].ready = true;
                stages[level+1][STAGE_COMPARE].ready = true;
                // cancel the READ stage
                if (level < onchip_levels_ - 2) {
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
    if (req->op == HeapReqEvent::REBUILD) rebuild_queued_ = true;
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
    for (int level = 0; level < onchip_levels_; level++) {
        for (int i = 0; i < (1 << level); i++) {
            if (heap_activities[level][i] > 0)
                heap_activities[level][i] *= 1e-100;
        }
    }
    // Tail-buffer resident lines scale in place (dirty lines are the only
    // copy; clean lines stay consistent because their memory copy gets the
    // same scaling from the sweep below).
    tailScaleActs();
    // Parked refill snapshots hold pre-scale acts the DRAM sweep cannot
    // reach; installing one post-sweep would resurrect unscaled values.
    refill_done_.clear();
    // Queued-but-unstarted inserts carry pre-scale activities.
    for (auto& e : insert_queue) e.activity *= 1e-100;

    // On-chip sweep wall time: per-level SRAMs scale their occupied entries
    // in parallel at 1 entry/cycle, so the duration is the largest occupied
    // level (the partial last level or the full level above it).
    rescale_onchip_cycles_ = 0;
    for (int level = 0; level < onchip_levels_; level++) {
        size_t level_base = ((size_t)1 << level) - 1;   // slots before this level
        if (heap_size <= level_base) break;
        size_t occupied = std::min((size_t)1 << level, heap_size - level_base);
        rescale_onchip_cycles_ = std::max(rescale_onchip_cycles_, occupied);
    }

    rescale_sweep_started_ = true;
    rescale_offchip_done_ = false;
    rescale_pending_reads = 0;
    readBurstAll(var_ptr_base_addr, (num_vars + 1) * sizeof(double));
    if (heap_size >= firstOffchipSlot()) {
        // RMW the occupied node region too (act fields only, handled by the
        // node-region branch in handleMem).
        readBurstAll(nodes_base_, (heap_size - firstOffchipSlot() + 1) * 16);
    }
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
        // OLC node traffic is tracked separately from req_to_op so the
        // solver-facing gates keep meaning "act traffic only".
        auto oit = olc_pending.find(read_resp->getID());
        if (oit != olc_pending.end()) {
            OlcPendingRead p = oit->second;
            olc_pending.erase(oit);
            olcHandleMem(read_resp, p);
            delete req;
            return;
        }
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
            std::vector<uint8_t> write_data(read_resp->data.begin(),
                                            read_resp->data.begin() + chunk_size);
            if (nodes_base_ != 0 && read_resp->pAddr >= nodes_base_) {
                // Node-region chunk ({var, pad, act} x N): scale only the act
                // fields at offset 8 of each 16 B node.
                sst_assert(chunk_size % 16 == 0 && read_resp->pAddr % 16 == 0,
                           CALL_INFO, -1, "Rescale node chunk misaligned (%zu @ 0x%lx)",
                           chunk_size, read_resp->pAddr);
                for (size_t off = 8; off + 8 <= chunk_size; off += 16) {
                    double a;
                    memcpy(&a, write_data.data() + off, 8);
                    if (a > 0) a *= 1e-100;
                    memcpy(write_data.data() + off, &a, 8);
                }
            } else {
                const size_t entry_size = sizeof(double);
                sst_assert(chunk_size % entry_size == 0, CALL_INFO, -1,
                           "Rescale chunk size %zu is not aligned to activity size %zu",
                           chunk_size, entry_size);
                double* entries = reinterpret_cast<double*>(write_data.data());
                for (size_t i = 0; i < chunk_size / entry_size; i++) {
                    entries[i] *= 1e-100;
                }
            }

            uint64_t write_addr = read_resp->pAddr;
            if (WRITE_BUFFER) {
                // Sweep writes enter the store queue too: a later read must
                // forward the scaled value, never an older pre-sweep entry.
                store_queue.push_back(StoreQueueEntry(write_addr, chunk_size, write_data));
            }
            if (tracer_) tracer_->emitMem(true, write_addr, (uint32_t)chunk_size);
            // Node-region sweep write-backs must enter the same in-flight
            // count their WriteResps decrement, or sweep responses eat the
            // counts of real tail/percolation writes and the OLC budget
            // transiently over-admits.
            if (nodes_base_ != 0 && write_addr >= nodes_base_) node_writes_inflight_++;
            memory->send(new SST::Interfaces::StandardMem::Write(write_addr, chunk_size, write_data));

            burst_inflight_--;
            issueBurstReads();
            if (rescale_pending_reads > 0) {
                rescale_pending_reads--;
                if (rescale_pending_reads == 0) {
                    rescale_offchip_done_ = true;
                    maybeFinishRescale();
                }
            }
        } else if (pending.type == PendingMemOpType::DEBUG) {
            // Handle debug heap verification - collect all data first
            const size_t chunk_size = pending.size;
            if (nodes_base_ != 0 && read_resp->pAddr >= nodes_base_) {
                // Node-region snapshot chunk
                sst_assert(chunk_size % 16 == 0 && read_resp->pAddr % 16 == 0,
                           CALL_INFO, -1, "Debug node chunk misaligned (%zu @ 0x%lx)",
                           chunk_size, read_resp->pAddr);
                uint64_t slot0 = firstOffchipSlot() + (read_resp->pAddr - nodes_base_) / 16;
                for (size_t i = 0; i < chunk_size / 16; i++) {
                    debug_nodes_[slot0 + i] = unpackNode(read_resp->data.data() + i * 16);
                }
            } else {
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
            }

            // Check if we're done with all reads for debug
            burst_inflight_--;
            issueBurstReads();
            if (rescale_pending_reads > 0) {
                rescale_pending_reads--;
                if (rescale_pending_reads == 0) {
                    verifyDebugHeap();  // All data collected, now perform the verification
                }
            }
        }
    } else if (auto* write_resp = dynamic_cast<SST::Interfaces::StandardMem::WriteResp*>(req)) {
        assert(!write_resp->getFail() && "Write response should not fail");
        if (nodes_base_ != 0 && write_resp->pAddr >= nodes_base_
            && node_writes_inflight_ > 0) {
            node_writes_inflight_--;
            maybe_active_ = true;  // budget freed: deferred reads can issue
        }
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
    for (int level = 0; level < onchip_levels_; ++level) {
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

    // Partition the heap-owned region [var_act_base, heap_region_end):
    // [acts | nodes]. Total capacity = on-chip slots + node-region slots,
    // clamped so path bits / lastSlot 32-bit math stay valid. Must stay an
    // sst_assert so it cannot be compiled out.
    uint64_t acts_end = var_ptr_base_addr + (num_vars + 1) * sizeof(double);
    nodes_base_ = (acts_end + 63) & ~63ull;
    sst_assert(heap_region_end_ > nodes_base_, CALL_INFO, -1,
        "Heap region [0x%lx, 0x%lx) leaves no node space (acts end 0x%lx)\n",
        var_ptr_base_addr, heap_region_end_, acts_end);
    uint64_t node_slots = (heap_region_end_ - nodes_base_) / 16;
    heap_capacity_ = ((1ull << onchip_levels_) - 1) + node_slots;
    uint64_t max_slots = (1ull << MAX_TOTAL_HEAP_LEVELS) - 1;
    if (heap_capacity_ > max_slots) heap_capacity_ = max_slots;
    sst_assert(heap_size <= heap_capacity_, CALL_INFO, -1,
        "Instance has %lu vars but heap capacity is %lu (on-chip 2^%d-1 + %lu node slots)\n",
        heap_size, heap_capacity_, onchip_levels_, node_slots);

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

    // Build the on-chip levels (slot s <-> decision_vars[s-1], BFS order)
    size_t added = 0;
    for (int level = 0; level < onchip_levels_ && added < heap_size; level++) {
        int level_size = 1 << level;
        for (int i = 0; i < level_size; i++) {
            heap_vars[level][i] = decision_vars[added];
            heap_activities[level][i] = 0.0;
            inheap_[decision_vars[added]] = true;
            added++;
            if (added >= heap_size) break;
        }
    }

    // Off-chip slots: untimed node-region init + tail window prefill
    tail_win_.clear();
    refill_done_.clear();
    if (heap_size >= firstOffchipSlot()) {
        size_t nslots = heap_size - firstOffchipSlot() + 1;
        std::vector<uint8_t> nbuf(nslots * 16, 0);
        for (size_t i = 0; i < nslots; i++) {
            uint64_t slot = firstOffchipSlot() + i;
            OlcNode n(decision_vars[slot - 1], 0.0);
            std::vector<uint8_t> nb;
            packNode(n, nb);
            memcpy(nbuf.data() + i * 16, nb.data(), 16);
            inheap_[n.var] = true;
            added++;
        }
        memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
            nodes_base_, nbuf.size(), nbuf, true,
            static_cast<uint32_t>(SST::Interfaces::StandardMem::Request::Flag::F_NONCACHEABLE)));

        // Prefill the sliding window over the top W lines (clean: memory has
        // identical data from the untimed write above).
        uint64_t tail_line = lineOf(heap_size);
        uint64_t lo_line = tail_line >= (uint64_t)(TAIL_WINDOW_LINES - 1)
                         ? tail_line - (TAIL_WINDOW_LINES - 1) : 0;
        for (uint64_t line = lo_line; line <= tail_line; line++) {
            TailLine tl(line);
            for (int i = 0; i < 4; i++) {
                uint64_t slot = lineBaseSlot(line) + i;
                if (slot <= heap_size) {
                    tl.node[i] = OlcNode(decision_vars[slot - 1], 0.0);
                    tl.slot_valid[i] = true;
                }
            }
            tail_win_.push_back(tl);
        }
    }
    inheap_count_ = added;

    std::vector<uint8_t> buffer((num_vars + 1) * sizeof(double));
    memcpy(buffer.data(), values.data(), buffer.size());
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        actAddr(0), buffer.size(), buffer, true,
        static_cast<uint32_t>(SST::Interfaces::StandardMem::Request::Flag::F_NONCACHEABLE)));

    output.verbose(CALL_INFO, 1, 0, "Heap Size: %lu entries (capacity %lu, nodes @ 0x%lx)\n",
                   heap_size, heap_capacity_, nodes_base_);
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
    // Additive: callers reset rescale_pending_reads, then may burst several
    // regions (acts + nodes) under one completion count. Chunks stream
    // through a fixed window (issueBurstReads) rather than all at once: with
    // the node region a sweep can span millions of lines, and an unbounded
    // burst floods the act cache's MSHR.
    size_t offset = 0;
    while (offset < total_size) {
        uint64_t current_addr = start_addr + offset;
        uint64_t line_offset = current_addr % line_size;
        size_t remaining = total_size - offset;
        size_t bytes_in_line = line_size - line_offset;
        size_t chunk_size = std::min(bytes_in_line, remaining);

        burst_queue_.emplace_back(current_addr, chunk_size);
        rescale_pending_reads++;
        offset += chunk_size;
    }
    issueBurstReads();
}

void PipelinedHeap::issueBurstReads() {
    while (burst_inflight_ < BURST_WINDOW && !burst_queue_.empty()) {
        auto [addr, chunk_size] = burst_queue_.front();
        burst_queue_.pop_front();
        auto* req = new SST::Interfaces::StandardMem::Read(addr, chunk_size);
        // Use appropriate type based on debug_heap_pending flag
        PendingMemOpType type = debug_heap_pending ? PendingMemOpType::DEBUG : PendingMemOpType::RESCALE;
        req_to_op.emplace(req->getID(), PendingMemOp(type, 0, chunk_size));
        burst_inflight_++;
        if (tracer_) tracer_->emitMem(false, addr, (uint32_t)chunk_size);
        memory->send(req);
    }
}

void PipelinedHeap::verifyDebugHeap() {
    output.verbose(CALL_INFO, 6, 0, "DEBUG_HEAP: Verifying heap consistency...\n");

    // Collect per-var copy counts and per-var max stored activity, checking
    // structural invariants along the way.
    std::unordered_map<Var, int> copies;
    std::unordered_map<Var, double> max_stored;
    size_t occupied = 0;

    for (int level = 0; level < onchip_levels_; ++level) {
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

    // Off-chip slots via the coherent accessor (tail buffer -> store queue ->
    // memory snapshot). Slots beyond heap_size are unreachable garbage behind
    // the bounds guards: no check applies to them.
    for (uint64_t slot = firstOffchipSlot(); slot <= heap_size; slot++) {
        OlcNode n;
        if (!debugNodeAt(slot, n)) {
            output.verbose(CALL_INFO, 0, 0,
                "DEBUG_HEAP ERROR: off-chip slot %lu unreadable\n", slot);
            debug_heap_errors++;
            continue;
        }
        if (n.var == var_Undef) {
            output.verbose(CALL_INFO, 0, 0,
                "DEBUG_HEAP ERROR: empty off-chip slot %lu within heap_size %zu\n",
                slot, heap_size);
            debug_heap_errors++;
            continue;
        }
        occupied++;
        copies[n.var]++;
        auto it = max_stored.find(n.var);
        if (it == max_stored.end() || n.act > it->second) max_stored[n.var] = n.act;

        // Heap property across (and below) the boundary
        uint64_t pslot = slot >> 1;
        double parent_act;
        if (pslot < firstOffchipSlot()) {
            int plevel = onchip_levels_ - 1;
            parent_act = heap_activities[plevel][pslot & ~(1ull << plevel)];
        } else {
            OlcNode pn;
            if (!debugNodeAt(pslot, pn)) continue;  // already reported above
            parent_act = pn.act;
        }
        if (parent_act < n.act) {
            output.verbose(CALL_INFO, 0, 0,
                "DEBUG_HEAP ERROR: heap property violated at off-chip slot %lu: "
                "parent %.12g < child %.12g (var %d)\n",
                slot, parent_act, n.act, n.var);
            debug_heap_errors++;
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
    debug_nodes_.clear();
}

bool PipelinedHeap::debugNodeAt(uint64_t slot, OlcNode& out) const {
    // Coherence order: tail buffer (only copy of dirty/composed lines) ->
    // newest store-queue entry -> memory snapshot from the debug burst.
    for (const TailLine& tl : tail_win_) {
        if (tl.line != (slot - firstOffchipSlot()) >> 2) continue;
        int off = (slot - lineBaseSlot(tl.line)) & 3;
        if (!tl.slot_valid[off]) return false;
        out = tl.node[off];
        return true;
    }
    uint64_t addr = nodes_base_ + (slot - firstOffchipSlot()) * 16;
    for (int i = (int)store_queue.size() - 1; i >= 0; i--) {
        uint64_t s = store_queue[i].addr, e = s + store_queue[i].size;
        if (addr >= s && addr + 16 <= e) {
            out = unpackNode(store_queue[i].data.data() + (addr - s));
            return true;
        }
    }
    auto it = debug_nodes_.find(slot);
    if (it == debug_nodes_.end()) return false;
    out = it->second;
    return true;
}

// ==================== Off-chip level controller (OLC) ====================
// Levels >= onchip_levels_ live in the DRAM node region [nodes_base_,
// heap_region_end_). All node accesses resolve through a single ordering
// point: tail buffer -> store queue -> memory (actL1 -> DRAM). v1 concurrency:
// insert contexts pipeline among themselves with globally ordered per-level
// consumption; ONE sift at a time (blocked sifts park in the on-chip compare
// stages, propagating backpressure up); sift and insert phases are mutually
// exclusive inside the OLC.

static inline int slotLevel(uint64_t s) { return 63 - __builtin_clzll(s); }

void PipelinedHeap::packNode(const OlcNode& n, std::vector<uint8_t>& out) {
    out.assign(16, 0);
    memcpy(out.data(), &n.var, sizeof(Var));
    memcpy(out.data() + 8, &n.act, sizeof(double));
}

OlcNode PipelinedHeap::unpackNode(const uint8_t* p) {
    OlcNode n;
    memcpy(&n.var, p, sizeof(Var));
    memcpy(&n.act, p + 8, sizeof(double));
    return n;
}

void PipelinedHeap::overlayStoreQueue(uint64_t addr, std::vector<uint8_t>& data) const {
    // Overlay every intersecting store-queue entry, oldest to newest, so the
    // freshest write wins. Needed because node line reads (64 B) can overlap
    // 16 B node writes and vice versa; the containment-only forwarding of
    // findStoreQueueEntry cannot express partial overlap.
    uint64_t rd_end = addr + data.size();
    for (const auto& e : store_queue) {
        uint64_t lo = std::max(addr, e.addr);
        uint64_t hi = std::min(rd_end, e.addr + e.size);
        if (lo < hi)
            memcpy(data.data() + (lo - addr), e.data.data() + (lo - e.addr), hi - lo);
    }
}

bool PipelinedHeap::olcIdle() const {
    return !sift_.active && insert_ctxs_.empty() && olc_pending.empty();
}

bool PipelinedHeap::olcCanAcceptInsert() const {
    // Sift <-> insert are mutually exclusive OLC phases: removes every
    // cross-type slot conflict without associative tracking.
    return !sift_.active && (int)insert_ctxs_.size() < OLC_INSERT_CTXS;
}

bool PipelinedHeap::olcCanAcceptSift() const {
    return !sift_.active && insert_ctxs_.empty();
}

bool PipelinedHeap::siftBlocks(uint64_t slot) const {
    if (!sift_.active) return false;
    int gap = slotLevel(slot) - slotLevel(sift_.slot);
    return gap >= 0 && (slot >> gap) == sift_.slot;
}

void PipelinedHeap::sampleSiftParked() {
    uint64_t parked = sift_.active ? 1 : 0;
    for (int l = 0; l < onchip_levels_; l++)
        if (stages[l][STAGE_COMPARE].valid && !stages[l][STAGE_COMPARE].ready
            && stages[l][STAGE_COMPARE].op_type == HEAP_OP_REPLACE) parked++;
    stat_olc_sift_parked_sample->addData(parked);
}

// ---------------- tail buffer ----------------

bool PipelinedHeap::lineInWindow(uint64_t line) const {
    return !tail_win_.empty()
        && line >= tail_win_.front().line && line <= tail_win_.back().line;
}

TailLine* PipelinedHeap::tailLineFor(uint64_t slot) {
    if (slot < firstOffchipSlot()) return nullptr;
    uint64_t line = lineOf(slot);
    if (!lineInWindow(line)) return nullptr;
    return &tail_win_[line - tail_win_.front().line];
}

void PipelinedHeap::tailComposeTo(uint64_t slot) {
    // Growing up never needs fill reads: new lines become valid by
    // composition as insert dest writes land.
    if (slot < firstOffchipSlot()) return;
    uint64_t line = lineOf(slot);
    if (tail_win_.empty()) {
        // Re-anchor by composition only on a clean line start: creating a
        // line over pre-existing live slots would shadow their DRAM truth
        // with invalid buffer slots (a pop of those slots would stall on a
        // hole nothing fills). Unaligned dests write through the
        // below-window path; the reanchor refill restores residency.
        if (lineBaseSlot(line) != slot) return;
        tail_win_.emplace_back(line);
        return;
    }
    while (tail_win_.back().line < line) {
        tail_win_.emplace_back(tail_win_.back().line + 1);
        tailEvictBottomIfOver();
    }
}

void PipelinedHeap::tailEvictBottomIfOver() {
    while ((int)tail_win_.size() > TAIL_WINDOW_LINES) {
        TailLine& bot = tail_win_.front();
        if (bot.dirty) {
            // Bottom lines are >= W lines below the tail: fully composed.
            std::vector<uint8_t> data(64, 0);
            for (int i = 0; i < 4; i++) {
                sst_assert(bot.slot_valid[i], CALL_INFO, -1,
                    "Tail buffer evicting a partially valid line %lu\n", bot.line);
                std::vector<uint8_t> nb;
                packNode(bot.node[i], nb);
                memcpy(data.data() + i * 16, nb.data(), 16);
            }
            uint64_t addr = lineAddr(bot.line);
            for (int i = 0; i < 4; i++)
                patchParkedRefill(bot.line, i, data.data() + i * 16);
            if (WRITE_BUFFER) store_queue.push_back(StoreQueueEntry(addr, 64, data));
            node_writes_inflight_++;
            stat_olc_node_writes->addData(1);
            if (tracer_) tracer_->emitMem(true, addr, 64);
            memory->send(new SST::Interfaces::StandardMem::Write(addr, 64, data));
        }
        tail_win_.pop_front();
    }
}

void PipelinedHeap::tailDropAbove(uint64_t new_heap_size) {
    // Shrinking: lines above the tail were cleared slot-by-slot by the
    // pops/trims that shrank past them -- dropped without writeback.
    if (tail_win_.empty()) return;
    if (new_heap_size < firstOffchipSlot()) {
        tail_win_.clear();
        refill_done_.clear();
        return;
    }
    uint64_t tail_line = lineOf(new_heap_size);
    while (!tail_win_.empty() && tail_win_.back().line > tail_line)
        tail_win_.pop_back();
}

bool PipelinedHeap::tailSlotReady(uint64_t slot) {
    TailLine* tl = tailLineFor(slot);
    return tl && tl->slot_valid[(slot - lineBaseSlot(tl->line)) & 3];
}

OlcNode PipelinedHeap::tailGrab(uint64_t slot) {
    TailLine* tl = tailLineFor(slot);
    sst_assert(tl != nullptr, CALL_INFO, -1,
        "Tail grab of slot %lu missed the buffer window\n", slot);
    int off = (slot - lineBaseSlot(tl->line)) & 3;
    sst_assert(tl->slot_valid[off], CALL_INFO, -1,
        "Tail grab of slot %lu hit an unfilled buffer slot\n", slot);
    OlcNode n = tl->node[off];
    tl->node[off] = OlcNode();
    tl->dirty = true;
    return n;
}

void PipelinedHeap::tailRefillTick() {
    if (rescale || debug_heap_pending) return;
    if (heap_size < firstOffchipSlot()) return;
    uint64_t tail_line = lineOf(heap_size);

    if (tail_win_.empty()) {
        // Window ran dry (pops consumed every resident line): re-anchor by
        // fetching the tail line itself, otherwise the pop path stalls until
        // some unrelated insert wave recomposes the window.
        if (tail_refills_inflight_ == 0 && olcMemBudgetOk(OLC_RESERVE_SIFT)) {
            if (!refill_done_.empty()) { refill_done_.clear(); }
            uint64_t addr = lineAddr(tail_line);
            auto* req = new SST::Interfaces::StandardMem::Read(addr, 64);
            olc_pending[req->getID()] =
                OlcPendingRead(OlcMemType::TAIL_REFILL, tail_line, 0, refill_gen_);
            tail_refills_inflight_++;
            stat_olc_node_reads->addData(1);
            if (tracer_) tracer_->emitMem(false, addr, 64);
            memory->send(req);
        }
        return;
    }

    // Purge undeliverable out-of-order refills once nothing is in flight
    // (window moved past them via eviction or re-anchor).
    if (tail_refills_inflight_ == 0 && !refill_done_.empty()
        && !refill_done_.count(tail_win_.front().line - 1))
        refill_done_.clear();
    if (!lineInWindow(tail_line)) return;
    uint64_t runway = tail_line - tail_win_.front().line + 1;
    if ((int)runway + tail_refills_inflight_ >= TAIL_REFILL_MARGIN) return;
    if (tail_refills_inflight_ >= TAIL_REFILLS_MAX) return;
    if (!olcMemBudgetOk(OLC_RESERVE_PREFETCH)) return;  // refills are lowest-priority prefetches
    if (tail_win_.front().line == 0) return;  // already at the first off-chip line
    uint64_t target = tail_win_.front().line - 1 - tail_refills_inflight_;
    if (target + 1 == 0) return;
    uint64_t addr = lineAddr(target);
    auto* req = new SST::Interfaces::StandardMem::Read(addr, 64);
    olc_pending[req->getID()] = OlcPendingRead(OlcMemType::TAIL_REFILL, target, 0, refill_gen_);
    tail_refills_inflight_++;
    stat_olc_node_reads->addData(1);
    if (tracer_) tracer_->emitMem(false, addr, 64);
    memory->send(req);
}

void PipelinedHeap::installRefills() {
    // Re-anchor case: empty window and a completed read of the CURRENT tail
    // line. If the tail moved (or composition re-anchored first), the entry
    // is stale and gets purged by tailRefillTick instead.
    if (tail_win_.empty() && heap_size >= firstOffchipSlot()) {
        auto it = refill_done_.find(lineOf(heap_size));
        if (it != refill_done_.end()) {
            TailLine tl(it->first);
            for (int i = 0; i < 4; i++) {
                tl.node[i] = unpackNode(it->second.data() + i * 16);
                tl.slot_valid[i] = true;
            }
            tl.dirty = false;
            tail_win_.push_back(tl);
            refill_done_.erase(it);
            stat_olc_tail_refills->addData(1);
        }
    }
    while (!tail_win_.empty()) {
        auto it = refill_done_.find(tail_win_.front().line - 1);
        if (it == refill_done_.end()) break;
        TailLine tl(it->first);
        for (int i = 0; i < 4; i++) {
            tl.node[i] = unpackNode(it->second.data() + i * 16);
            tl.slot_valid[i] = true;
        }
        tl.dirty = false;
        tail_win_.push_front(tl);
        refill_done_.erase(it);
        stat_olc_tail_refills->addData(1);
    }
}

void PipelinedHeap::tailScaleActs() {
    for (TailLine& tl : tail_win_)
        for (int i = 0; i < 4; i++)
            if (tl.slot_valid[i] && tl.node[i].act > 0)
                tl.node[i].act *= 1e-100;
}

void PipelinedHeap::tailWipe() {
    tail_win_.clear();
    refill_done_.clear();
    // Pending node writes target dead slots after the wipe; drop their queue
    // entries so no future read can forward pre-rebuild data. (Their
    // WriteResp handler tolerates a missing entry.)
    for (auto it = store_queue.begin(); it != store_queue.end();) {
        if (it->addr >= nodes_base_ && nodes_base_ != 0) it = store_queue.erase(it);
        else ++it;
    }
}

bool PipelinedHeap::popGateOk() {
    if (heap_size == 0) return true;
    uint32_t last_level, last_idx;
    lastSlot(last_level, last_idx);
    if (last_level < onchip_levels_) return true;  // on-chip: stage bypasses handle it

    uint64_t grab = heap_size;
    // Mirror the single pre-grab trim startOperation will perform, so the
    // gate covers the slot that will actually be grabbed.
    if (isPipelineIdle()) {
        TailLine* tl = tailLineFor(grab);
        if (!tl || !tl->slot_valid[(grab - lineBaseSlot(tl->line)) & 3]) {
            stat_olc_tail_stalls->addData(1);
            return false;
        }
        const OlcNode& n = tl->node[(grab - lineBaseSlot(tl->line)) & 3];
        if (n.var != var_Undef && !inheap_[n.var]) {
            if (siftBlocks(grab)) { stat_olc_tail_stalls->addData(1); return false; }
            grab = heap_size - 1;
            if (grab == 0 || grab < firstOffchipSlot()) return true;
        }
    }
    if (!tailSlotReady(grab)) { stat_olc_tail_stalls->addData(1); return false; }
    if (siftBlocks(grab)) { stat_olc_tail_stalls->addData(1); return false; }
    return true;
}

// ---------------- node access ----------------

int PipelinedHeap::nodeReadInstant(uint64_t slot, OlcNode& out) {
    TailLine* tl = tailLineFor(slot);
    if (tl) {
        int off = (slot - lineBaseSlot(tl->line)) & 3;
        if (!tl->slot_valid[off]) return 2;  // buffered line, slot not filled yet
        out = tl->node[off];
        return 1;
    }
    int idx = findStoreQueueEntry(nodeAddr(slot), 16);
    if (idx >= 0) {
        out = unpackNode(store_queue[idx].data.data()
                         + (nodeAddr(slot) - store_queue[idx].addr));
        return 1;
    }
    return 0;
}

int PipelinedHeap::lineReadInstant(uint64_t line, OlcNode* out4) {
    if (lineInWindow(line)) {
        TailLine& tl = tail_win_[line - tail_win_.front().line];
        for (int i = 0; i < 4; i++)
            out4[i] = tl.slot_valid[i] ? tl.node[i] : OlcNode();
        return 1;
    }
    return 0;
}

void PipelinedHeap::patchParkedRefill(uint64_t line, int slot_off, const uint8_t* src16) {
    auto it = refill_done_.find(line);
    if (it == refill_done_.end()) return;
    memcpy(it->second.data() + (size_t)slot_off * 16, src16, 16);
}

void PipelinedHeap::nodeWrite(uint64_t slot, Var v, double act) {
    sst_assert(act <= 1e100, CALL_INFO, -1, "node activity out of bound\n");
    TailLine* tl = tailLineFor(slot);
    if (tl) {
        int off = (slot - lineBaseSlot(tl->line)) & 3;
        tl->node[off] = OlcNode(v, act);
        tl->slot_valid[off] = true;
        tl->dirty = true;
        return;  // absorbed by the tail buffer, no memory traffic
    }
    uint64_t addr = nodeAddr(slot);
    std::vector<uint8_t> data;
    packNode(OlcNode(v, act), data);
    patchParkedRefill(lineOf(slot), (int)((slot - lineBaseSlot(lineOf(slot))) & 3),
                      data.data());
    if (WRITE_BUFFER) store_queue.push_back(StoreQueueEntry(addr, 16, data));
    node_writes_inflight_++;
    stat_olc_node_writes->addData(1);
    if (tracer_) tracer_->emitMem(true, addr, 16);
    memory->send(new SST::Interfaces::StandardMem::Write(addr, 16, data));
}

// ---------------- insert contexts ----------------

void PipelinedHeap::olcStartInsert(Var v, double act, uint64_t dest) {
    sst_assert(dest >= firstOffchipSlot(), CALL_INFO, -1,
        "OLC insert handoff for on-chip dest %lu\n", dest);
    // The window must cover the new dest before its write can land there.
    tailComposeTo(dest);

    OlcInsertCtx ctx;
    ctx.id = next_ctx_id_++;
    ctx.dest = dest;
    ctx.dest_level = slotLevel(dest);
    ctx.var = v;
    ctx.act = act;
    ctx.progress = onchip_levels_;
    ctx.retired = false;
    int nlevels = ctx.dest_level - onchip_levels_;
    ctx.path.resize(nlevels > 0 ? nlevels : 0);
    insert_ctxs_.push_back(std::move(ctx));
    OlcInsertCtx& c = insert_ctxs_.back();

    for (int i = 0; i < (int)c.path.size(); i++) {
        int level = onchip_levels_ + i;
        c.path[i].slot = dest >> (c.dest_level - level);
        // Shared-prefix scoreboard: if any older unretired context's path (or
        // dest) covers this slot, defer the read -- it re-issues at consume
        // time, ordered behind the older context's write.
        for (size_t j = 0; j + 1 < insert_ctxs_.size(); j++) {
            const OlcInsertCtx& o = insert_ctxs_[j];
            if (o.retired || level > o.dest_level) continue;
            if ((o.dest >> (o.dest_level - level)) == c.path[i].slot) {
                c.path[i].conflict = true;
                break;
            }
        }
        if (!c.path[i].conflict && olcMemBudgetOk(OLC_RESERVE_SIFT)) olcIssueInsertRead(c, level);
        // over-budget reads stay unissued; olcIssuePendingReads retries
    }
    stat_olc_boundary_crossings->addData(1);
    stat_olc_insert_ctx_sample->addData(insert_ctxs_.size());
    maybe_active_ = true;
}

void PipelinedHeap::olcIssueInsertRead(OlcInsertCtx& ctx, int level) {
    OlcPathRead& pr = ctx.path[level - onchip_levels_];
    pr.gen++;
    pr.ready = false;
    pr.issued = true;
    OlcNode n;
    int r = nodeReadInstant(pr.slot, n);
    if (r == 1) {
        pr.data = n;
        pr.ready = true;
        return;
    }
    sst_assert(r == 0, CALL_INFO, -1,
        "Insert path read of slot %lu hit an unfilled tail-buffer slot\n", pr.slot);
    uint64_t addr = nodeAddr(pr.slot);
    auto* req = new SST::Interfaces::StandardMem::Read(addr, 16);
    olc_pending[req->getID()] =
        OlcPendingRead(OlcMemType::INSERT_PATH, ctx.id, level, pr.gen);
    stat_olc_node_reads->addData(1);
    if (tracer_) tracer_->emitMem(false, addr, 16);
    memory->send(req);
}

void PipelinedHeap::olcProcessInserts() {
    // Globally ordered consumption: context i touches level l only after
    // every older context has passed l or retired (a younger insert can never
    // observe a slot an older one has not finalized). One level per context
    // per tick, mirroring the on-chip 1 level/cycle descent.
    for (size_t i = 0; i < insert_ctxs_.size(); i++) {
        OlcInsertCtx& c = insert_ctxs_[i];
        if (c.retired) continue;
        int l = c.progress;
        bool ordered = true;
        for (size_t j = 0; j < i; j++) {
            const OlcInsertCtx& o = insert_ctxs_[j];
            if (!o.retired && o.progress <= l) { ordered = false; break; }
        }
        if (!ordered) continue;

        if (l == c.dest_level) {
            nodeWrite(c.dest, c.var, c.act);
            c.retired = true;
            active_inserts--;
            sst_assert(active_inserts >= 0, CALL_INFO, -1, "active_inserts became negative\n");
            output.verbose(CALL_INFO, 6, 0, "OLC-INSERT: var %d (%.2f) landed at slot %lu\n",
                           c.var, c.act, c.dest);
            continue;
        }

        OlcPathRead& pr = c.path[l - onchip_levels_];
        if (pr.conflict && !pr.reissued) {
            // All olders have passed this level: safe to (re-)issue now; the
            // read is ordered behind their writes in the same hierarchy.
            if (!olcMemBudgetOk(OLC_RESERVE_SIFT)) continue;
            pr.reissued = true;
            olcIssueInsertRead(c, l);
        }
        if (!pr.ready) continue;

        sst_assert(pr.data.var != var_Undef, CALL_INFO, -1,
            "OLC insert read unwritten path slot %lu (level %d)\n", pr.slot, l);
        if (c.act > pr.data.act) {
            // Carried value wins: it settles here, the resident descends.
            nodeWrite(pr.slot, c.var, c.act);
            c.var = pr.data.var;
            c.act = pr.data.act;
        }
        c.progress++;
    }
    while (!insert_ctxs_.empty() && insert_ctxs_.front().retired)
        insert_ctxs_.pop_front();
}

// ---------------- sift ----------------

void PipelinedHeap::olcStartSift(uint64_t boundary_slot, Var v, double act) {
    sift_.active = true;
    sift_.slot = boundary_slot;
    sift_.var = v;
    sift_.act = act;
    sift_.boundary_write_pending = true;
    sift_.gen++;
    stat_olc_boundary_crossings->addData(1);
    olcSiftIssueReads();
    maybe_active_ = true;
}

void PipelinedHeap::olcSiftIssueReads() {
    uint64_t s = sift_.slot;
    uint64_t c0 = 2 * s;
    sift_.hs_at_issue = heap_size;
    sift_.children_ready = false;
    sift_.child[0] = OlcNode();
    sift_.child[1] = OlcNode();
    for (int i = 0; i < 4; i++) sift_.grand[i] = OlcNode();

    uint64_t c_line = lineOf(c0);
    OlcNode buf[4];
    if (lineReadInstant(c_line, buf) == 1) {
        uint64_t base = lineBaseSlot(c_line);
        sift_.child[0] = buf[(c0 - base) & 3];
        sift_.child[1] = buf[(c0 + 1 - base) & 3];
        sift_.children_ready = true;
    } else {
        uint64_t addr = lineAddr(c_line);
        auto* req = new SST::Interfaces::StandardMem::Read(addr, 64);
        olc_pending[req->getID()] = OlcPendingRead(OlcMemType::SIFT_CHILDREN, 0, 0, sift_.gen);
        stat_olc_node_reads->addData(1);
        if (tracer_) tracer_->emitMem(false, addr, 64);
        memory->send(req);
    }

    uint64_t g0 = 4 * s;
    sift_.grand_needed = g0 <= heap_size;
    sift_.grand_ready = !sift_.grand_needed;
    if (sift_.grand_needed) {
        // 4s..4s+3 is one aligned 64 B line: the next step's children line,
        // fetched a round trip early at zero bandwidth overhead.
        uint64_t g_line = lineOf(g0);
        if (lineReadInstant(g_line, buf) == 1) {
            for (int i = 0; i < 4; i++) sift_.grand[i] = buf[i];
            sift_.grand_ready = true;
        } else {
            uint64_t addr = lineAddr(g_line);
            auto* req = new SST::Interfaces::StandardMem::Read(addr, 64);
            olc_pending[req->getID()] = OlcPendingRead(OlcMemType::SIFT_GRAND, 0, 0, sift_.gen);
            stat_olc_node_reads->addData(1);
            if (tracer_) tracer_->emitMem(false, addr, 64);
            memory->send(req);
        }
    }
}

void PipelinedHeap::siftWriteSlot(uint64_t slot, Var v, double act) {
    if (slot < firstOffchipSlot()) {
        // Boundary slot: dedicated write port into the level K-1 SRAM.
        int level = slotLevel(slot);
        int idx = (int)(slot & ~(1ull << level));
        setVar(level, idx, v);
        setActivity(level, idx, act);
        sift_.boundary_write_pending = false;
        return;
    }
    nodeWrite(slot, v, act);
}

void PipelinedHeap::olcRetireSift() {
    sift_.active = false;
    sift_.children_ready = false;
    sift_.grand_ready = false;
    output.verbose(CALL_INFO, 6, 0, "OLC-SIFT: retired\n");
}

void PipelinedHeap::olcCompleteSiftStep() {
    uint64_t s = sift_.slot;
    // Bounds are evaluated against min(current heap_size, issue-time
    // heap_size): pops may have shrunk the heap mid-flight (cleared slots
    // also read as var_Undef and lose comparisons), and slots that appeared
    // after issue are unwritten reservations of parked inserts.
    uint64_t hs = std::min((uint64_t)heap_size, sift_.hs_at_issue);
    uint64_t c0 = 2 * s;

    if (c0 > hs) {  // no children left: settle at s
        siftWriteSlot(s, sift_.var, sift_.act);
        olcRetireSift();
        return;
    }
    bool has_r = (c0 + 1) <= hs;
    OlcNode l = sift_.child[0];
    OlcNode r = sift_.child[1];
    bool use_r = has_r && r.act > l.act;
    OlcNode maxc = use_r ? r : l;
    uint64_t cslot = use_r ? c0 + 1 : c0;

    if (!(maxc.act > sift_.act && maxc.var != var_Undef)) {
        // Carried value dominates both children: settle at s.
        siftWriteSlot(s, sift_.var, sift_.act);
        olcRetireSift();
        return;
    }
    // Hoist the winning child into s, descend into cslot.
    siftWriteSlot(s, maxc.var, maxc.act);

    uint64_t gc0 = 2 * cslot;
    bool ghas_l = sift_.grand_needed && gc0 <= hs;
    if (!ghas_l) {  // cslot is a leaf (or its children are unwritten reservations)
        siftWriteSlot(cslot, sift_.var, sift_.act);
        olcRetireSift();
        return;
    }
    bool ghas_r = (gc0 + 1) <= hs;
    OlcNode gl = sift_.grand[(int)(gc0 - 4 * s)];
    OlcNode gr = sift_.grand[(int)(gc0 + 1 - 4 * s)];
    bool guse_r = ghas_r && gr.act > gl.act;
    OlcNode gmax = guse_r ? gr : gl;
    uint64_t gslot = guse_r ? gc0 + 1 : gc0;

    if (!(gmax.act > sift_.act && gmax.var != var_Undef)) {
        // Carried dominates the grandchildren pair: settle at cslot.
        siftWriteSlot(cslot, sift_.var, sift_.act);
        olcRetireSift();
        return;
    }
    // Hoist the winning grandchild into cslot and continue two levels down.
    siftWriteSlot(cslot, gmax.var, gmax.act);
    sift_.slot = gslot;
    output.verbose(CALL_INFO, 6, 0, "OLC-SIFT: descended to slot %lu (var %d %.2f)\n",
                   gslot, sift_.var, sift_.act);
    olcSiftIssueReads();
}

// ---------------- per-tick processing & response handling ----------------

void PipelinedHeap::olcTick() {
    if (sift_.active && sift_.children_ready && sift_.grand_ready)
        olcCompleteSiftStep();
    if (!insert_ctxs_.empty()) {
        olcIssuePendingReads();
        olcProcessInserts();
    }
    tailRefillTick();
}

void PipelinedHeap::olcIssuePendingReads() {
    // Issue budget-deferred path reads, oldest context first.
    for (auto& c : insert_ctxs_) {
        if (c.retired) continue;
        for (size_t i = 0; i < c.path.size(); i++) {
            OlcPathRead& pr = c.path[i];
            if (pr.issued || pr.conflict) continue;
            if (!olcMemBudgetOk(OLC_RESERVE_SIFT)) return;
            olcIssueInsertRead(c, onchip_levels_ + (int)i);
        }
    }
}

void PipelinedHeap::olcHandleMem(SST::Interfaces::StandardMem::ReadResp* resp,
                                 const OlcPendingRead& p) {
    maybe_active_ = true;
    std::vector<uint8_t> data = resp->data;
    // Line reads can straddle newer 16 B node writes (and vice versa):
    // overlay the freshest store-queue bytes onto the response.
    overlayStoreQueue(resp->pAddr, data);

    switch (p.type) {
        case OlcMemType::INSERT_PATH: {
            for (auto& c : insert_ctxs_) {
                if (c.id != p.ctx_id) continue;
                OlcPathRead& pr = c.path[p.level - onchip_levels_];
                if (pr.gen == p.gen) {
                    pr.data = unpackNode(data.data());
                    pr.ready = true;
                }
                break;
            }
            break;  // retired/absent context: stale response, drop
        }
        case OlcMemType::SIFT_CHILDREN: {
            if (!sift_.active || p.gen != sift_.gen) break;
            uint64_t c0 = 2 * sift_.slot;
            uint64_t base = lineBaseSlot(lineOf(c0));
            sift_.child[0] = unpackNode(data.data() + ((c0 - base) & 3) * 16);
            sift_.child[1] = unpackNode(data.data() + ((c0 + 1 - base) & 3) * 16);
            sift_.children_ready = true;
            break;
        }
        case OlcMemType::SIFT_GRAND: {
            if (!sift_.active || p.gen != sift_.gen) break;
            for (int i = 0; i < 4; i++)
                sift_.grand[i] = unpackNode(data.data() + i * 16);
            sift_.grand_ready = true;
            break;
        }
        case OlcMemType::TAIL_REFILL: {
            tail_refills_inflight_--;
            refill_done_[p.ctx_id] = data;  // ctx_id carries the line index
            installRefills();
            break;
        }
        case OlcMemType::READ_PEEK: {
            sendResp(unpackNode(data.data()).var);
            break;
        }
    }
}
