#include <sst/core/sst_config.h> // Required for all SST implementation files
#include "pipelined_heap.h"
#include <algorithm>
#include <random>
#include <cmath>

PipelinedHeap::PipelinedHeap(
    SST::ComponentId_t id, SST::Params& params,
    SST::Interfaces::StandardMem* mem, uint64_t var_ptr_base_addr
) : SST::SubComponent(id),
    memory(mem),
    line_size(64),
    var_ptr_base_addr(var_ptr_base_addr),
    heap_size(0),
    var_inc_ptr(nullptr),
    bump_active(false),
    bump_mem_inflight(false),
    active_inserts(0),
    rescale(false),
    rescale_pending_reads(0),
    debug_heap_pending(false),
    debug_heap_errors(0) {

    output.init("PHEAP-> ", params.find<int>("verbose", 0), 0, SST::Output::STDOUT);

    registerClock(params.find<std::string>("clock", "1GHz"),
                 new SST::Clock::Handler2<PipelinedHeap, &PipelinedHeap::tick>(this));

    response_port = configureLink("response");
    sst_assert(response_port != nullptr, CALL_INFO, -1, 
              "Error: 'response_port' is not connected to a link\n");

    output.verbose(CALL_INFO, 1, 0, "var ptr address: 0x%lx\n", var_ptr_base_addr);

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
            bypass_data[level].reset();
        }
    }
}

bool PipelinedHeap::tick(SST::Cycle_t cycle) {
    // output.verbose(CALL_INFO, 7, 0, "=============== Tick %lu =============== \n", cycle);

    if (!insert_queue.empty() && canStartOperation(HEAP_OP_INSERT) && !rescale) {
        InsReq& op = insert_queue.front();

        if (op.bump) {
            // check for rescale before popping
            if (op.activity + *(var_inc_ptr) > 1e100) {
                // if (!store_queue.empty()) return false;

                output.verbose(CALL_INFO, 2, 0, "Rescaling variable activities\n");
                rescale = true;
                for (int level = 0; level < MAX_HEAP_LEVELS; level++) {
                    for (int i = 0; i < (1 << level); i++){
                        heap_activities[level][i] *= 1e-100;
                    }
                    assert(req_to_op.empty());
                }
                op.activity *= 1e-100;
                // rescale all var mem
                readBurstAll(var_ptr_base_addr, (num_vars + 1) * sizeof(VarMem));
                *var_inc_ptr *= 1e-100;
                output.verbose(CALL_INFO, 2, 0, "Rescaled Var Inc %f\n", *var_inc_ptr);
                return false;
            }

            // update ptr and activity
            op.activity += *(var_inc_ptr);
        } else active_inserts++;
        startOperation(HEAP_OP_INSERT, op.arg, op.activity, op.bump, op.dest);
        insert_queue.pop_front();
    }

    if (!request_queue.empty()) {
        const PendingRequest& pending = request_queue.front();
        switch (pending.op) {
            case HeapReqEvent::BUMP:
                // can only start with an empty pipeline
                if (!bump_active && req_to_op.empty() && isPipelineIdle()) {
                    bump_active = true;
                    bump_mem_inflight = true;
                    getVarMem(pending.arg, true);
                    request_queue.pop_front();
                }
                break;
            case HeapReqEvent::INSERT:
                // can fetch var mem when not bump or bump has started
                if (!bump_mem_inflight && !rescale){
                    // discards the requests for vars already in progress
                    if (in_progress_vars.find(pending.arg) == in_progress_vars.end()) {
                        in_progress_vars.insert(pending.arg);
                        getVarMem(pending.arg, false);
                    }
                    request_queue.pop_front();
                }
                break;
            case HeapReqEvent::REMOVE_MAX:
                // after previous insert/bump have started
                if (!bump_active && (active_inserts == 0) && req_to_op.empty()
                    && canStartOperation(HEAP_OP_REPLACE)) {
                    startOperation(HEAP_OP_REPLACE, 0, 0, 0, 0);
                    request_queue.pop_front();
                }
                break;
            case HeapReqEvent::DEBUG_HEAP:
                // Wait for all previous requests and pipeline to finish
                if (active_inserts == 0 && !bump_active && req_to_op.empty() && isPipelineIdle() && !rescale) {
                // if (active_inserts == 0 && !bump_active && req_to_op.empty() && isPipelineIdle() && store_queue.empty() && !rescale) {
                    // Start debug heap check
                    debug_heap_pending = true;
                    debug_heap_errors = 0;
                    debug_heap_varmem.clear();
                    debug_heap_varmem.reserve(num_vars + 1);  // Reserve enough space for all variables
                    
                    // Use readBurstAll to read all VarMem entries
                    if (heap_size == 0) {
                        sendResp(0);
                        debug_heap_pending = false;
                    } else {
                        output.verbose(CALL_INFO, 5, 0, "DEBUG_HEAP: Reading memory for heap verification\n");
                        readBurstAll(var_ptr_base_addr, (num_vars + 1) * sizeof(VarMem));
                    }
                    request_queue.pop_front();
                }
                break;
            default:
                request_queue.pop_front();
                break;
        }
    }

    advancePipeline();
    return false;
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

bool PipelinedHeap::canStartOperation(HeapOpType op) {
    // 1 insertion / 1 cycle, 1 removal / 2 cycles

    // Check if the first stage (READ) of the first level is ready to receive new data
    if (!stages[0][STAGE_READ].ready) return false;
    
    if (op == HEAP_OP_REPLACE) {
        uint32_t last_level = priority_encoder(heap_size);
        // Check if the previous operation in the READ stage is also a REPLACE
        if ((stages[0][STAGE_COMPARE].valid && stages[0][STAGE_COMPARE].op_type == HEAP_OP_REPLACE))
            return false;
    }
    
    return true;
}

void PipelinedHeap::startOperation(HeapOpType op, Var arg, double activity, bool bump, int dest) {
    // Initialize READ stage at level 0 with the new operation
    stages[0][STAGE_READ].op_type = op;
    
    if (op == HEAP_OP_INSERT) {
        if (!bump) {
            // insertion at the last position
            heap_size++;
            sst_assert(heap_size <= num_vars, CALL_INFO, -1, 
                "Failed to insert var %d: heap size exceeds number of variables\n", arg);
            sst_assert(heap_size <= (size_t) MAX_HEAP_SIZE, CALL_INFO, -1, 
                "Failed to insert var %d: heap size overflow\n", arg);
            dest = heap_size;
        } 

        uint32_t target_level = priority_encoder(dest);
        // need to pass down depth for termination condition
        stages[0][STAGE_READ].depth = target_level;
        // Normalize path by shifting so the leading 1 is at the MSB position
        uint32_t path = dest << (31 - target_level);
        stages[0][STAGE_READ].path = path << 1;  // remove the leading 1 bit

        output.verbose(CALL_INFO, 5, 0,
            "Start INSERT: heap_size=%lu, var %d (%.2f), idx=%u, path=0x%x, depth=%d, bump=%d\n", 
            heap_size, arg, activity, dest, path, target_level, bump);

        if (bump) {
            sst_assert(dest <= heap_size, CALL_INFO, -1, "var %d's idx %d > heap size %lu\n", arg, dest, heap_size);
            Var v = getVar(target_level, ~(1 << target_level) & dest);
            sst_assert(v == arg, CALL_INFO, -1, "bump var %d is not located at idx %d which has var %d\n", arg, dest, v);
        }
    } else if (op == HEAP_OP_REPLACE) {
        if (heap_size == 0) {
            sendResp(var_Undef);
            return;
        }

        // determine the last level and node idx
        uint32_t heap_r = bit_reverse(heap_size);
        uint32_t heap_one_hot = bit_reverse(heap_r & ~(heap_r - 1));
        uint32_t last_node_idx = (heap_size) & (~heap_one_hot);
        uint32_t last_level = priority_encoder(heap_size);

        // determine the replacement var
        if (stages[last_level][STAGE_WRITE].valid && last_node_idx == stages[last_level][STAGE_WRITE].node_idx) {
            // bypass the WRITE_STAGE node if match
            arg = stages[last_level][STAGE_WRITE].var;
            activity = stages[last_level][STAGE_WRITE].act;
            stages[last_level][STAGE_WRITE].reset();
        } else if (stages[last_level][STAGE_COMPARE].valid && last_node_idx == stages[last_level][STAGE_COMPARE].node_idx) {
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
        output.verbose(CALL_INFO, 5, 0, "set last level %d, idx %d, addr %d, to var_Undef\n", last_level, last_node_idx, (1 << last_level) | last_node_idx);
        if (last_node_idx > 1)
            output.verbose(CALL_INFO, 5, 0, "current last level %d, idx %d, addr %d, is var %d\n", last_level, last_node_idx - 1, (1 << last_level) | (last_node_idx - 1), getVar(last_level, last_node_idx - 1));
        output.verbose(CALL_INFO, 5, 0, "Start REPLACE: heap_size=%lu, last var %d (%.2f)\n",
            heap_size, arg, activity);

        heap_size--;
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
            output.verbose(CALL_INFO, 5, 0, "INSERT[L%d-READ]: var %d (%.2f), node %d, depth %d, path 0x%x\n",
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
                output.verbose(CALL_INFO, 5, 0, "INSERT[L%d-READ]: inducing L%d node %d, %s child\n",
                               level, level+1, child_idx, go_left ? "left" : "right");
            } else output.verbose(CALL_INFO, 5, 0, "INSERT[L%d-READ]: insertion ends this level\n", level);

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
            // or bypass if write stage is writing back but not yet visible in memory
            // if (bypass_data[level].valid && bypass_data[level].node_idx == node_idx) {
            //     curr_var = bypass_data[level].var;
            //     curr_act = bypass_data[level].act;
            // }

            int new_var = curr_stage.var;
            double new_act = curr_stage.act;

            output.verbose(CALL_INFO, 5, 0, "INSERT[L%d-COMP]: new var %d (%.2f), cur_var %d (%.2f) node %d, depth %d\n",
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

            // bypass the write back var for the next operation if needed
            // bypass_data[level].valid = true;
            // bypass_data[level].node_idx = node_idx;
            // bypass_data[level].var = curr_var;
            // bypass_data[level].act = curr_act;

            stages[level][stage].reset();
            break;
        }

        case STAGE_WRITE: {
            // always ready for new operations
            // Update the memory with the new value and activity
            setVar(level, node_idx, curr_stage.var);
            setActivity(level, node_idx, curr_stage.act);
            setVarMem(curr_stage.var, VarMem((1 << level) | node_idx, curr_stage.act));
            output.verbose(CALL_INFO, 5, 0, "INSERT[L%d-WRITE]: Write back node %d: var=%d (%.2f)\n",
                           level, node_idx, curr_stage.var, curr_stage.act);

            if (in_progress_vars.find(curr_stage.var) != in_progress_vars.end())
                in_progress_vars.erase(curr_stage.var);

            // If this is the root level, we've completed the operation
            if (level == curr_stage.depth) {
                if (bump_active) bump_active = false;
                else {
                    active_inserts--;
                    sst_assert(active_inserts >= 0, CALL_INFO, -1, "active_inserts became negative\n");
                }
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
                sendResp(root);
                setVarMem(root, VarMem(0, getActivity(0, 0)));
                output.verbose(CALL_INFO, 5, 0, "REPLACE[L%d-READ]: removing Min %d\n", level, root);
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
                output.verbose(CALL_INFO, 5, 0, "REPLACE[L%d-READ]: inducing L%d children of node %d\n",
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

            Var left_child, right_child; double left_act, right_act;
            // bypass from next level's WRITE is omitted for simplicity
            // because next level always executes before this stage and has updated the memory
            // node_idx is updated by previous level's COMPARE
            uint32_t lchild_idx = getChildIdx(level, node_idx, true);
            left_child = getVar(level+1, lchild_idx);
            left_act = getActivity(level+1, lchild_idx);
            bool has_right = heap_size >= ((lchild_idx + 1) | (1 << (level + 1)));
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

                output.verbose(CALL_INFO, 5, 0, "REPLACE[L%d-COMP]: %s child %d (%.2f) > repl_var %d (%.2f), swapping\n",
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
                
                output.verbose(CALL_INFO, 5, 0, "REPLACE[L%d-COMP]: repl_var %d (%.2f) >= both children, ends here\n",
                    level, repl_var, repl_act);
            }

            // Mark COMPARE stage as ready for new operations
            stages[level][stage].reset();
            break;
        }
            
        case STAGE_WRITE: {
            // Update the memory with the replacement value
            setVar(level, node_idx, curr_stage.var);
            setActivity(level, node_idx, curr_stage.act);
            setVarMem(curr_stage.var, VarMem((1 << level) | node_idx, curr_stage.act));

            output.verbose(CALL_INFO, 5, 0, "REPLACE[L%d-WRITE]: Write back node %d: var=%d (%.2f)\n",
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

    sst_assert(req->op != HeapReqEvent::READ, CALL_INFO, -1, "READ operation not supported in PipelinedHeap\n");

    // Assert var is valid
    sst_assert(req->arg != var_Undef || (req->op != HeapReqEvent::INSERT || req->op != HeapReqEvent::BUMP),
        CALL_INFO, -1, "Attempting to insert undefined variable");
    sst_assert(req->arg <= num_vars, CALL_INFO, -1, 
        "Attempting to insert var %d which exceeds num_vars %zu", req->arg, num_vars);
    request_queue.emplace_back(req->op, req->arg);
    delete req;
}

void PipelinedHeap::sendResp(int result) {
    response_port->send(new HeapRespEvent(result));
}

void PipelinedHeap::handleMem(SST::Interfaces::StandardMem::Request* req) {
    if (auto* read_resp = dynamic_cast<SST::Interfaces::StandardMem::ReadResp*>(req)) {
        auto it = req_to_op.find(read_resp->getID());
        sst_assert(it != req_to_op.end(), CALL_INFO, -1, "Unexpected memory response ID %lu", read_resp->getID());

        PendingMemOp pending = it->second;
        req_to_op.erase(it);

        if (pending.type == PendingMemOpType::INSERT_FETCH) {
            VarMem data;
            sst_assert(read_resp->data.size() >= sizeof(VarMem), CALL_INFO, -1,
                "Memory response data size too small: %zu\n", read_resp->data.size());
                      
            memcpy(&data, read_resp->data.data(), sizeof(VarMem));

            InsReq op = pending.insert_req;
            op.dest = data.addr;
            op.activity = data.act;

            if ((!op.bump && data.addr == 0)  // insert and var not in heap
                || (op.bump && data.addr != 0))  // bump and var in the heap
                insert_queue.emplace_back(op);
            else if (bump_active && bump_mem_inflight) bump_active = false;
            else if (in_progress_vars.find(op.arg) != in_progress_vars.end())
                in_progress_vars.erase(op.arg);

            bump_mem_inflight = false;
        } else if (pending.type == PendingMemOpType::RESCALE) {
            const size_t chunk_size = pending.size;
            const size_t entry_size = sizeof(VarMem);
            sst_assert(chunk_size % entry_size == 0, CALL_INFO, -1,
                       "Rescale chunk size %zu is not aligned to VarMem size %zu",
                       chunk_size, entry_size);

            const VarMem* resp_entries = reinterpret_cast<const VarMem*>(read_resp->data.data());
            std::vector<VarMem> scaled_entries(resp_entries, resp_entries + (chunk_size / entry_size));
            for (auto& entry : scaled_entries) {
                entry.act *= 1e-100;
            }

            std::vector<uint8_t> write_data(chunk_size);
            memcpy(write_data.data(), scaled_entries.data(), chunk_size);

            uint64_t write_addr = read_resp->pAddr;
            memory->send(new SST::Interfaces::StandardMem::Write(write_addr, chunk_size, write_data));

            if (rescale_pending_reads > 0) {
                rescale_pending_reads--;
                if (rescale_pending_reads == 0) rescale = false;
            }
        } else if (pending.type == PendingMemOpType::DEBUG) {
            // Handle debug heap verification - collect all VarMem data first
            const size_t chunk_size = pending.size;
            const size_t entry_size = sizeof(VarMem);
            sst_assert(chunk_size % entry_size == 0, CALL_INFO, -1,
                       "Chunk size %zu is not aligned to VarMem size %zu",
                       chunk_size, entry_size);

            // Get base address to calculate var indices
            uint64_t base_offset = read_resp->pAddr - var_ptr_base_addr;
            size_t start_idx = base_offset / entry_size;

            // Store all VarMem entries in debug_heap_varmem
            const VarMem* resp_entries = reinterpret_cast<const VarMem*>(read_resp->data.data());
            for (size_t i = 0; i < (chunk_size / entry_size); i++) {
                Var curr_var = start_idx + i;
                debug_heap_varmem[curr_var] = resp_entries[i];
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
        if (!WRITE_BUFFER) return;

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

bool PipelinedHeap::isPipelineIdle() const {
    for (int level = 0; level < MAX_HEAP_LEVELS; ++level) {
        for (int stage = 0; stage < PIPELINE_DEPTH; ++stage) {
            if (stages[level][stage].valid) {
                return false;
            }
        }
    }

    // Verify that no variables are left in progress when pipeline is idle
    sst_assert(in_progress_vars.empty(), CALL_INFO, -1, 
        "in_progress_vars [%d,...] not empty when pipeline is idle\n",
        in_progress_vars.size() > 0 ? *in_progress_vars.begin() : 0);

    // Verify active_inserts is consistent
    sst_assert(active_inserts == 0, CALL_INFO, -1, "active_inserts not 0 when pipeline is idle\n");

    return true;
}

void PipelinedHeap::initHeap(uint64_t random_seed) {
    in_progress_vars.clear();
    assert(heap_size <= (1 << MAX_HEAP_LEVELS));
    // Collect all decision variables first
    std::vector<Var> decision_vars;
    for (Var v = 1; v <= (Var)heap_size; v++) {
        if (decision[v]) {
            decision_vars.push_back(v);
        }
    }

    // Randomize if a seed is provided
    if (random_seed != 0) {
        output.verbose(CALL_INFO, 1, 0, "Randomizing heap with seed %lu\n", random_seed);
        std::mt19937 rng(random_seed);
        std::shuffle(decision_vars.begin(), decision_vars.end(), rng);
    }

    // initialize var memory
    std::vector<VarMem> values((heap_size + 1), VarMem(0, 0.0));

    // Build heap level by level
    int added = 0;
    for (int level = 0; level < MAX_HEAP_LEVELS; level++) {
        int level_size = 1 << level;
        for (int i = 0; i < level_size; i++) {
            heap_vars[level][i] = decision_vars[added];
            heap_activities[level][i] = 0.0;
            values[decision_vars[added]].addr = added + 1;  // heap index starts from 1
            added++;
            if (added >= heap_size) {
                level = MAX_HEAP_LEVELS;  // to break outer loop
                break;
            }
        }
    }

    std::vector<uint8_t> buffer((heap_size + 1) * sizeof(VarMem));
    memcpy(buffer.data(), values.data(), buffer.size());
    memory->sendUntimedData(new SST::Interfaces::StandardMem::Write(
        varMemAddr(0), buffer.size(), buffer,
        true, 0x1));  // posted, and not cacheable

    output.verbose(CALL_INFO, 1, 0, "Heap Size: %lu variables and activities, %lu bytes\n",
                   (heap_size + 1), (heap_size + 1) * (sizeof(Var) + sizeof(double)));
    output.verbose(CALL_INFO, 1, 0, "Var Mem Size: %zu, %zu bytes\n",
                   (heap_size + 1), (heap_size + 1) * sizeof(VarMem));
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
    heap_activities[level][idx] = value;
}

void PipelinedHeap::setVar(int level, int idx, Var value) {
    assert(idx >= 0 && idx < heap_vars[level].size());
    heap_vars[level][idx] = value;
}

void PipelinedHeap::setVarMem(Var v, VarMem p) {
    // update the var_act memory copy
    size_t size = sizeof(VarMem);
    uint64_t addr = varMemAddr(v);
    std::vector<uint8_t> data(size);
    memcpy(data.data(), &p, size);
    if (WRITE_BUFFER) {
        // Always add a new entry to the store queue
        StoreQueueEntry entry(addr, size, data);
        store_queue.push_back(entry);
    }

    memory->send(new SST::Interfaces::StandardMem::Write(addr, size, data));
}

void PipelinedHeap::getVarMem(Var v, bool bump) {
    output.verbose(CALL_INFO, 5, 0, "Get VarMem: var %d, bump=%d\n", v, bump);
    size_t size = sizeof(VarMem);
    uint64_t addr = varMemAddr(v);
    if (WRITE_BUFFER) {
        // forward from store queue if possible
        int idx = findStoreQueueEntry(addr, size);
        if (idx >= 0) {
            assert(size == store_queue[idx].size);
            VarMem data;
            memcpy(&data, store_queue[idx].data.data(), size);
            if ((!bump && data.addr == 0)  // insert and var not in heap
                || (bump && data.addr != 0))  // bump and var in the heap
                insert_queue.push_back(InsReq(v, data.act, bump, data.addr));
            else if (bump_active && bump_mem_inflight) bump_active = false;
            else if (in_progress_vars.find(v) != in_progress_vars.end())
                in_progress_vars.erase(v);
            bump_mem_inflight = false;
            return;
        }
    }

    // read from memory and handleMem will push to insert_queue
    auto req = new SST::Interfaces::StandardMem::Read(addr, size);
    req_to_op[req->getID()] = PendingMemOp(InsReq(v, 0.0, bump, 0));
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
        memory->send(req);

        offset += chunk_size;
    }
}

void PipelinedHeap::verifyDebugHeap() {
    output.verbose(CALL_INFO, 5, 0, "DEBUG_HEAP: Verifying heap consistency...\n");
    
    // Create a set of all variables that exist in the heap for quick lookup
    std::unordered_map<Var, std::pair<int, int>> heap_var_locations;
    for (int level = 0; level < MAX_HEAP_LEVELS; ++level) {
        for (int idx = 0; idx < heap_vars[level].size(); ++idx) {
            Var var = heap_vars[level][idx];
            if (var != var_Undef) {
                sst_assert(heap_var_locations.find(var) == heap_var_locations.end(), CALL_INFO, -1,
                    "DEBUG_HEAP ERROR: Duplicate var %d found in heap at (L%d,i%d)\n", var, level, idx);
                heap_var_locations[var] = std::make_pair(level, idx);
            }
        }
    }
    
    // Check all variables in memory against the heap
    for (const auto& [var, mem_data] : debug_heap_varmem) {
        if (var == 0) continue;  // Skip the dummy var 0
        
        auto it = heap_var_locations.find(var);
        if (it != heap_var_locations.end()) {
            // Variable exists in heap - verify addr and activity match
            int level = it->second.first;
            int idx = it->second.second;
            
            int expected_addr = (1 << level) | idx;
            double expected_act = heap_activities[level][idx];
            
            // Check for address mismatch
            if ((int)mem_data.addr != expected_addr) {
                output.verbose(CALL_INFO, 0, 0, 
                    "DEBUG_HEAP ERROR: Var %d: addr mismatch: heap=(L%d,i%d) expect=%d mem=%d\n",
                    var, level, idx, expected_addr, (int)mem_data.addr);
                debug_heap_errors++;
            }
            
            // Check for activity mismatch
            if (std::abs(mem_data.act - expected_act) > 1e-8) {
                output.verbose(CALL_INFO, 0, 0, 
                    "DEBUG_HEAP ERROR: Var %d: activity mismatch: heap=%.12g mem=%.12g\n",
                    var, expected_act, mem_data.act);
                debug_heap_errors++;
            }
        } else if (mem_data.addr != 0) {
            // Variable exists in memory but not in heap
            output.verbose(CALL_INFO, 0, 0, 
                "DEBUG_HEAP ERROR: Var %d exists in memory with addr=%d but not in heap\n",
                var, (int)mem_data.addr);
            debug_heap_errors++;
        }
    }
    
    // Check for variables in heap but not in memory
    for (const auto& [var, location] : heap_var_locations) {
        if (debug_heap_varmem.find(var) == debug_heap_varmem.end()) {
            int level = location.first;
            int idx = location.second;
            output.verbose(CALL_INFO, 0, 0,
                "DEBUG_HEAP ERROR: Var %d exists in heap at (L%d,i%d) but not in memory\n",
                var, level, idx);
            debug_heap_errors++;
        }
    }
    
    output.verbose(CALL_INFO, 5, 0, "DEBUG_HEAP: Verification complete, found %d errors\n", debug_heap_errors);
    
    // Send the response and clear the debug state
    sendResp(debug_heap_errors);
    debug_heap_pending = false;
    debug_heap_varmem.clear();
}
