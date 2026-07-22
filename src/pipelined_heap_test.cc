#include <sst/core/sst_config.h>
#include "pipelined_heap_test.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <sstream>
#include <string>

namespace {

std::string trim_copy(const std::string& input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

std::string strip_comment(const std::string& input) {
    std::string result = input;
    const auto hash_pos = result.find('#');
    const auto slash_pos = result.find("//");
    size_t cut_pos = std::string::npos;
    if (hash_pos != std::string::npos) {
        cut_pos = hash_pos;
    }
    if (slash_pos != std::string::npos) {
        cut_pos = (cut_pos == std::string::npos) ? slash_pos : std::min(cut_pos, slash_pos);
    }
    if (cut_pos != std::string::npos) {
        result.erase(cut_pos);
    }
    return result;
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

long parse_positive_long(const std::string& token, size_t line_number, const std::string& path,
                         SST::Output& output) {
    char* endptr = nullptr;
    long parsed = std::strtol(token.c_str(), &endptr, 0);
    if (endptr == token.c_str() || *endptr != '\0' || parsed <= 0) {
        output.fatal(CALL_INFO, -1, "Invalid numeric token '%s' on line %zu of '%s'\n",
                     token.c_str(), line_number, path.c_str());
    }
    return parsed;
}

} // namespace

PipelinedHeapTest::PipelinedHeapTest(SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id),
    script_path(params.find<std::string>("script_path", "")),
    script_index(0),
    wait_cycles(0),
    var_inc_value(params.find<double>("var_inc", 1.0)),
    script_completed(false),
    sim_finish_requested(false),
    resp_cnt(0),
    stat_successful_ops(0),
    stat_failed_ops(0) {

    verbose = params.find<int>("verbose", 1);
    output.init("HEAPTEST-> ", verbose, 0, SST::Output::STDOUT);
    output.verbose(CALL_INFO, 1, 0, "Initializing PipelinedHeapTest in manual verification mode\n");

    sst_assert(!script_path.empty(), CALL_INFO, -1,
               "PipelinedHeapTest requires a 'script_path' parameter referencing the input script file\n");

    registerClock(params.find<std::string>("clock", "1GHz"),
                  new SST::Clock::Handler2<PipelinedHeapTest, &PipelinedHeapTest::tick>(this));

    heap_link = configureLink("heap_port",
        new SST::Event::Handler2<PipelinedHeapTest, &PipelinedHeapTest::handleHeapResponse>(this));
    sst_assert(heap_link != nullptr, CALL_INFO, -1, "Failed to configure heap_port\n");

    // The heap owns its memory interface (loaded on its "memory" slot).
    heap = loadUserSubComponent<PipelinedHeap>(
        "heap",
        SST::ComponentInfo::SHARE_PORTS | SST::ComponentInfo::SHARE_STATS);
    sst_assert(heap != nullptr, CALL_INFO, -1, "Unable to load PipelinedHeap subcomponent\n");

    loadScriptFromFile(script_path);
    script_completed = script.empty();

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

void PipelinedHeapTest::init(unsigned int phase) {
    heap->init(phase);

    if (phase == 0) {
        std::vector<bool> decision_flags;
        if (!tracked_vars.empty()) {
            int max_var = *std::max_element(tracked_vars.begin(), tracked_vars.end());
            decision_flags.assign(max_var + 1, false);
            for (int var : tracked_vars) {
                decision_flags[var] = true;
            }
            heap->setDecisionFlags(decision_flags);
        }
        heap->setHeapSize(tracked_vars.size());
        heap->setVarIncPtr(&var_inc_value);
        heap->initHeap();

        activities.clear();
        golden_copies.clear();
        golden_inheap.clear();
        for (int var : tracked_vars) {
            activities[var] = 0.0;
            golden_copies.emplace_back(var, 0.0);
            golden_inheap.insert(var);
        }
    }
}

void PipelinedHeapTest::setup() {
    heap->setup();
}

void PipelinedHeapTest::complete(unsigned int phase) {
    heap->complete(phase);
}

void PipelinedHeapTest::finish() {
    heap->finish();
    output.verbose(CALL_INFO, 1, 0, "Manual test finished. Successful: %lu, Failed: %lu\n",
                   stat_successful_ops, stat_failed_ops);
}

bool PipelinedHeapTest::tick(SST::Cycle_t) {
    if (wait_cycles > 0) {
        wait_cycles--;
        finalizeIfDone();
        return false;
    }

    // Stall the script on outstanding remove/debug responses so the golden
    // state at response time matches the request prefix the heap has seen
    // (mirrors the solver, which yields per REMOVE_MAX). Inserts and bumps
    // still stream at one per tick.
    if (!pending_responses.empty()) {
        finalizeIfDone();
        return false;
    }

    if (script_index < script.size()) {
        executeStep(script[script_index]);
        script_index++;
        if (script_index == script.size()) {
            script_completed = true;
        }
    }

    finalizeIfDone();
    return false;
}

void PipelinedHeapTest::executeStep(const Step& step) {
    switch (step.type) {
        case Step::Type::Insert:
            issueInsert(step.var);
            break;
        case Step::Type::Bump:
            issueBump(step.var);
            break;
        case Step::Type::Remove:
            issueRemove();
            break;
        case Step::Type::Debug:
            issueDebug();
            break;
        case Step::Type::Wait:
            wait_cycles = step.var;
            output.verbose(CALL_INFO, 2, 0, "Waiting %d cycles (idle-time cleanup window)\n", step.var);
            break;
        case Step::Type::Hint:
            heap->handleRequest(new HeapReqEvent(HeapReqEvent::CLEAN_HINT));
            output.verbose(CALL_INFO, 2, 0, "Issued CLEAN_HINT\n");
            break;
        case Step::Type::Rebuild:
            // Mimic the solver's rebuild-restart: wipe, then reinsert every
            // tracked var (the harness has no assignment state, so all vars
            // count as unassigned). Golden model resets the same way.
            heap->handleRequest(new HeapReqEvent(HeapReqEvent::REBUILD));
            golden_copies.clear();
            golden_inheap.clear();
            for (int var : tracked_vars) {
                issueInsert(var);
            }
            output.verbose(CALL_INFO, 2, 0, "Issued REBUILD + %zu reinserts\n", tracked_vars.size());
            break;
    }
}

void PipelinedHeapTest::issueInsert(int var) {
    sst_assert(activities.count(var) > 0, CALL_INFO, -1, "Insert requested for untracked var %d\n", var);

    // Golden model: skipped when the bit is set (fresh copy resident),
    // otherwise a new copy with the current authoritative activity.
    if (golden_inheap.count(var) == 0) {
        golden_copies.emplace_back(var, activities[var]);
        golden_inheap.insert(var);
    }

    heap->handleRequest(new HeapReqEvent(HeapReqEvent::INSERT, var));
    output.verbose(CALL_INFO, 2, 0, "Issued INSERT for var %d\n", var);
}

void PipelinedHeapTest::issueBump(int var) {
    sst_assert(activities.count(var) > 0, CALL_INFO, -1, "Bump requested for untracked var %d\n", var);

    // Golden model: authoritative activity increases; any resident copy is
    // now stale, marked by clearing the bit. Copies keep their stored act.
    activities[var] += var_inc_value;
    golden_inheap.erase(var);

    heap->handleRequest(new HeapReqEvent(HeapReqEvent::BUMP, var));
    output.verbose(CALL_INFO, 2, 0, "Issued BUMP for var %d (activity now %.2f)\n", var, activities[var]);
}

void PipelinedHeapTest::issueRemove() {
    heap->handleRequest(new HeapReqEvent(HeapReqEvent::REMOVE_MAX));
    pending_responses.push(ResponseKind::Remove);
    output.verbose(CALL_INFO, 5, 0, "Issued REMOVE_MAX\n");
}

void PipelinedHeapTest::issueDebug() {
    heap->handleRequest(new HeapReqEvent(HeapReqEvent::DEBUG_HEAP));
    pending_responses.push(ResponseKind::Debug);
    output.verbose(CALL_INFO, 2, 0, "Issued DEBUG_HEAP\n");
}

void PipelinedHeapTest::checkRemoveResponse(int result) {
    const double eps = 1e-9;
    bool success = false;

    if (golden_copies.empty()) {
        success = (result == var_Undef);
        if (!success)
            output.verbose(CALL_INFO, 0, 0, "REMOVE returned %d but golden heap is empty\n", result);
    } else if (result == var_Undef) {
        // The heap may have purged every remaining copy only if all of them
        // were stale; losing a fresh copy is a real bug.
        success = true;
        for (const auto& [v, act] : golden_copies) {
            if (golden_inheap.count(v)) {
                output.verbose(CALL_INFO, 0, 0,
                    "REMOVE returned var_Undef but fresh copy of var %d (%.2f) remains\n", v, act);
                success = false;
            }
        }
        if (success) golden_copies.clear();
    } else {
        // Find the returned var's max stored copy
        double r_act = std::numeric_limits<double>::lowest();
        bool r_found = false;
        for (const auto& [v, act] : golden_copies) {
            if (v == result) { r_found = true; r_act = std::max(r_act, act); }
        }
        if (!r_found) {
            output.verbose(CALL_INFO, 0, 0, "REMOVE returned var %d with no golden copy\n", result);
            success = false;
        } else {
            // Every golden copy above the returned one must be stale (the heap
            // is allowed to purge stale copies at any time; skipping a fresh
            // copy would violate max-ordering).
            success = true;
            for (const auto& [v, act] : golden_copies) {
                if (act > r_act + eps && golden_inheap.count(v)) {
                    output.verbose(CALL_INFO, 0, 0,
                        "REMOVE returned var %d (%.2f) but fresh var %d (%.2f) is higher\n",
                        result, r_act, v, act);
                    success = false;
                }
            }
            if (success) {
                // Reconcile: copies above r_act were purged stales; one copy
                // of the result pops; any pop of the var clears its bit.
                std::vector<std::pair<int, double>> next;
                bool popped = false;
                for (const auto& [v, act] : golden_copies) {
                    if (act > r_act + eps) continue;                       // purged stale
                    if (!popped && v == result && act >= r_act - eps) {    // the popped copy
                        popped = true;
                        continue;
                    }
                    next.emplace_back(v, act);
                }
                golden_copies.swap(next);
                golden_inheap.erase(result);
            }
        }
    }

    output.verbose(CALL_INFO, 1, 0, "Heap response %lu (remove): got %d -> %s\n",
                   resp_cnt, result, success ? "PASS" : "FAIL");
    if (success) stat_successful_ops++;
    else stat_failed_ops++;
}

void PipelinedHeapTest::handleHeapResponse(SST::Event* ev) {
    auto* resp = dynamic_cast<HeapRespEvent*>(ev);
    sst_assert(resp != nullptr, CALL_INFO, -1, "Received invalid event type on heap response\n");

    if (pending_responses.empty()) {
        stat_failed_ops++;
        output.verbose(CALL_INFO, 0, 0, "Unexpected heap response %d with no pending expectation\n", resp->result);
        delete resp;
        finalizeIfDone();
        return;
    }

    ResponseKind kind = pending_responses.front();
    pending_responses.pop();

    if (kind == ResponseKind::Remove) {
        checkRemoveResponse(resp->result);
    } else { // Debug response: error count must be zero
        bool success = (resp->result == 0);
        output.verbose(CALL_INFO, 1, 0, "Heap response %lu (debug): %d errors -> %s\n",
                       resp_cnt, resp->result, success ? "PASS" : "FAIL");
        if (success) stat_successful_ops++;
        else stat_failed_ops++;
    }

    resp_cnt++;
    delete resp;

    finalizeIfDone();
}

void PipelinedHeapTest::finalizeIfDone() {
    if (sim_finish_requested) return;
    if (!script_completed) return;
    if (wait_cycles > 0) return;
    if (!pending_responses.empty()) return;

    sim_finish_requested = true;
    output.verbose(CALL_INFO, 1, 0, "Manual verification sequence complete. Ending simulation.\n");
    primaryComponentOKToEndSim();
}

void PipelinedHeapTest::loadScriptFromFile(const std::string& path) {
    script.clear();
    tracked_vars.clear();

    std::ifstream input(path);
    sst_assert(input.good(), CALL_INFO, -1,
               "Failed to open script file '%s' for PipelinedHeapTest\n", path.c_str());

    std::unordered_set<int> unique_vars;
    std::string line;
    size_t line_number = 0;
    bool header_parsed = false;
    size_t tracked_var_count = 0;

    while (std::getline(input, line)) {
        line_number++;
        std::string stripped = strip_comment(line);
        std::string trimmed = trim_copy(stripped);
        if (trimmed.empty()) {
            continue;
        }

        if (!header_parsed) {
            std::istringstream header(trimmed);
            std::string first_token;
            header >> first_token;
            sst_assert(!first_token.empty(), CALL_INFO, -1,
                       "Missing tracked variable count on line %zu of '%s'\n", line_number, path.c_str());

            std::string count_token;
            if (first_token.size() && !std::isdigit(static_cast<unsigned char>(first_token[0]))) {
                std::string lowered = to_lower_copy(first_token);
                sst_assert(lowered == "vars" || lowered == "tracked" || lowered == "variables",
                           CALL_INFO, -1,
                           "Unrecognized header token '%s' on line %zu of '%s'\n",
                           first_token.c_str(), line_number, path.c_str());
                header >> count_token;
                sst_assert(!count_token.empty(), CALL_INFO, -1,
                           "Missing tracked variable count after '%s' on line %zu of '%s'\n",
                           first_token.c_str(), line_number, path.c_str());
            } else {
                count_token = first_token;
            }

            long parsed_count = parse_positive_long(count_token, line_number, path, output);
            sst_assert(parsed_count <= std::numeric_limits<int>::max(), CALL_INFO, -1,
                       "Tracked variable count out of range on line %zu of '%s'\n", line_number, path.c_str());

            tracked_var_count = static_cast<size_t>(parsed_count);
            tracked_vars.reserve(tracked_var_count);
            for (int var = 1; var <= parsed_count; ++var) {
                tracked_vars.push_back(var);
            }

            std::string extra;
            sst_assert(!(header >> extra), CALL_INFO, -1,
                       "Unexpected extra token '%s' after tracked variable count on line %zu of '%s'\n",
                       extra.c_str(), line_number, path.c_str());

            header_parsed = true;
            continue;
        }

        std::istringstream iss(trimmed);
        std::string command;
        iss >> command;
        std::string cmd_lower = to_lower_copy(command);

        Step step{};

        if (cmd_lower == "insert" || cmd_lower == "ins" || cmd_lower == "bump") {
            std::string var_token;
            if (!(iss >> var_token)) {
                sst_assert(false, CALL_INFO, -1,
                           "Missing variable id for %s on line %zu of '%s'\n",
                           command.c_str(), line_number, path.c_str());
            }
            long parsed = parse_positive_long(var_token, line_number, path, output);
            sst_assert(tracked_var_count != 0 && static_cast<size_t>(parsed) <= tracked_var_count, CALL_INFO, -1,
                       "Variable id %ld exceeds tracked variable count %zu on line %zu of '%s'\n",
                       parsed, tracked_var_count, line_number, path.c_str());
            step.type = (cmd_lower == "bump") ? Step::Type::Bump : Step::Type::Insert;
            step.var = static_cast<int>(parsed);
        } else if (cmd_lower == "remove" || cmd_lower == "rem") {
            std::string extra;
            sst_assert(!(iss >> extra), CALL_INFO, -1,
                       "Unexpected token '%s' for REMOVE on line %zu of '%s'\n", extra.c_str(), line_number, path.c_str());
            step.type = Step::Type::Remove;
            step.var = 0;
        } else if (cmd_lower == "debug") {
            step.type = Step::Type::Debug;
            step.var = 0;
        } else if (cmd_lower == "rebuild") {
            step.type = Step::Type::Rebuild;
            step.var = 0;
        } else if (cmd_lower == "hint") {
            // Enables targeted-clean launches (the solver sends this when it
            // enters propagation); golden state is unaffected -- cleans only
            // remove copies the model already treats as purgeable-any-time.
            step.type = Step::Type::Hint;
            step.var = 0;
        } else if (cmd_lower == "wait") {
            std::string count_token;
            if (!(iss >> count_token)) {
                sst_assert(false, CALL_INFO, -1,
                           "Missing cycle count for WAIT on line %zu of '%s'\n", line_number, path.c_str());
            }
            long parsed = parse_positive_long(count_token, line_number, path, output);
            step.type = Step::Type::Wait;
            step.var = static_cast<int>(parsed);
        } else {
            sst_assert(false, CALL_INFO, -1,
                       "Unrecognized command '%s' on line %zu of '%s'\n", command.c_str(), line_number, path.c_str());
        }

        script.push_back(step);

        if (step.type == Step::Type::Insert || step.type == Step::Type::Bump) {
            unique_vars.insert(step.var);
        }
    }

    sst_assert(header_parsed, CALL_INFO, -1,
               "Script '%s' did not provide a tracked variable count header\n", path.c_str());

    output.verbose(CALL_INFO, 1, 0,
                   "Loaded %zu steps with %zu tracked vars (%zu touched) from script '%s'\n",
                   script.size(), tracked_vars.size(), unique_vars.size(), path.c_str());
}
