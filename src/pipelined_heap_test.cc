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

} // namespace

PipelinedHeapTest::PipelinedHeapTest(SST::ComponentId_t id, SST::Params& params) :
    SST::Component(id),
    script_path(params.find<std::string>("script_path", "")),
    script_index(0),
    var_inc_value(params.find<double>("var_inc", 1.0)),
    var_mem_base_addr(0x70000000),
    script_completed(false),
    sim_finish_requested(false),
    resp_cnt(0),
    pending_insert_responses(0),
    stat_successful_ops(0),
    stat_failed_ops(0) {

    verbose = params.find<int>("verbose", 1);
    output.init("HEAPTEST-> ", verbose, 0, SST::Output::STDOUT);
    output.verbose(CALL_INFO, 1, 0, "Initializing PipelinedHeapTest in manual verification mode\n");

    sst_assert(!script_path.empty(), CALL_INFO, -1,
               "PipelinedHeapTest requires a 'script_path' parameter referencing the input script file\n");

    registerClock(params.find<std::string>("clock", "1GHz"),
                  new SST::Clock::Handler2<PipelinedHeapTest, &PipelinedHeapTest::tick>(this));

    global_memory = loadUserSubComponent<SST::Interfaces::StandardMem>(
        "global_memory",
        SST::ComponentInfo::SHARE_NONE,
        getTimeConverter("1GHz"),
        new SST::Interfaces::StandardMem::Handler2<PipelinedHeapTest, &PipelinedHeapTest::handleGlobalMemEvent>(this));
    sst_assert(global_memory != nullptr, CALL_INFO, -1,
               "Unable to load StandardMem subcomponent for global memory\n");

    heap_link = configureLink("heap_port",
        new SST::Event::Handler2<PipelinedHeapTest, &PipelinedHeapTest::handleHeapResponse>(this));
    sst_assert(heap_link != nullptr, CALL_INFO, -1, "Failed to configure heap_port\n");

    heap = loadUserSubComponent<PipelinedHeap>(
        "heap",
        SST::ComponentInfo::SHARE_PORTS | SST::ComponentInfo::SHARE_STATS,
        global_memory, var_mem_base_addr);
    sst_assert(heap != nullptr, CALL_INFO, -1, "Unable to load PipelinedHeap subcomponent\n");

    loadScriptFromFile(script_path);
    script_completed = script.empty();

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

void PipelinedHeapTest::init(unsigned int phase) {
    global_memory->init(phase);
    
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

        active_vars.clear();
        activities.clear();
        for (int var : tracked_vars) {
            activities[var] = 0.0;
            active_vars.insert(var);
        }
    }
}

void PipelinedHeapTest::setup() {
    global_memory->setup();

    size_t line_size = global_memory->getLineSize();
    line_size = std::max(line_size, static_cast<size_t>(64));
    output.verbose(CALL_INFO, 1, 0, "Cache line size: %zu bytes\n", line_size);
    heap->setLineSize(line_size);
}

void PipelinedHeapTest::complete(unsigned int phase) {
    global_memory->complete(phase);
}

void PipelinedHeapTest::finish() {
    global_memory->finish();
    output.verbose(CALL_INFO, 1, 0, "Manual test finished. Successful: %lu, Failed: %lu\n",
                   stat_successful_ops, stat_failed_ops);
}

void PipelinedHeapTest::handleGlobalMemEvent(SST::Interfaces::StandardMem::Request* req) {
    heap->handleMem(req);
}

bool PipelinedHeapTest::tick(SST::Cycle_t) {
    if (script_index < script.size()) {
        const Step& next_step = script[script_index];
        // if (next_step.type == Step::Type::Remove && pending_insert_responses > 0) {
        //     finalizeIfDone();
        //     return false;
        // }

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
    }
}

void PipelinedHeapTest::issueInsert(int var) {
    sst_assert(activities.count(var) > 0, CALL_INFO, -1, "Insert requested for untracked var %d\n", var);

    if (active_vars.count(var) == 0)
        active_vars.insert(var);

    heap->handleRequest(new HeapReqEvent(HeapReqEvent::INSERT, var));
    // pending_responses.push(ResponseKind::Insert);
    // pending_insert_responses++;
    output.verbose(CALL_INFO, 2, 0, "Issued INSERT for var %d\n", var);
}

void PipelinedHeapTest::issueBump(int var) {
    if (active_vars.count(var) > 0)
        activities[var] += var_inc_value;

    heap->handleRequest(new HeapReqEvent(HeapReqEvent::BUMP, var));
    // pending_responses.push(ResponseKind::Insert);
    // pending_insert_responses++;
    output.verbose(CALL_INFO, 2, 0, "Issued BUMP for var %d (activity now %.2f)\n", var, activities[var]);
}

void PipelinedHeapTest::issueRemove() {
    heap->handleRequest(new HeapReqEvent(HeapReqEvent::REMOVE_MAX));
    pending_responses.push(ResponseKind::Remove);
    output.verbose(CALL_INFO, 5, 0, "Issued REMOVE_MAX\n");
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

    const int result = resp->result;
    bool success = false;

    if (kind == ResponseKind::Insert) {
        success = (result == 1);
        pending_insert_responses--;
        sst_assert(pending_insert_responses >= 0, CALL_INFO, -1,
                   "Negative pending insert/bump response count\n");
        output.verbose(CALL_INFO, 1, 0, "Heap response %lu (insert/bump): got %d -> %s\n",
                       resp_cnt, result, success ? "PASS" : "FAIL");
    } else { // Remove response
        if (active_vars.empty()) {
            success = (result == var_Undef);
        } else if (result != var_Undef) {
            double best_activity = std::numeric_limits<double>::lowest();
            for (int var : active_vars) {
                best_activity = std::max(best_activity, activities[var]);
            }
            double result_activity = activities[result];
            double diff = std::fabs(result_activity - best_activity);
            const double eps = 1e-9;
            success = (diff <= eps);
            printf("result act %.2f, best act %.2f\n", result_activity, best_activity);
        }

        output.verbose(CALL_INFO, 1, 0, "Heap response %lu (remove): got %d -> %s\n",
                       resp_cnt, result, success ? "PASS" : "FAIL");

        if (success && result != var_Undef) {
            active_vars.erase(result);
        }
    }

    if (success) stat_successful_ops++;
    else stat_failed_ops++;

    resp_cnt++;
    delete resp;

    finalizeIfDone();
}

void PipelinedHeapTest::finalizeIfDone() {
    if (sim_finish_requested) return;
    if (!script_completed) return;
    if (!pending_responses.empty()) return;
    if (pending_insert_responses != 0) return;
    // if (!active_vars.empty()) return;

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

            char* header_end = nullptr;
            long parsed_count = std::strtol(count_token.c_str(), &header_end, 0);
            sst_assert(header_end != count_token.c_str() && *header_end == '\0', CALL_INFO, -1,
                       "Invalid tracked variable count '%s' on line %zu of '%s'\n",
                       count_token.c_str(), line_number, path.c_str());
            sst_assert(parsed_count > 0 && parsed_count <= std::numeric_limits<int>::max(), CALL_INFO, -1,
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

        if (cmd_lower == "insert" || cmd_lower == "ins") {
            std::string var_token;
            if (!(iss >> var_token)) {
                sst_assert(false, CALL_INFO, -1,
                           "Missing variable id for INSERT on line %zu of '%s'\n", line_number, path.c_str());
            }
            char* endptr = nullptr;
            long parsed = std::strtol(var_token.c_str(), &endptr, 0);
            sst_assert(endptr != var_token.c_str() && *endptr == '\0', CALL_INFO, -1,
                       "Invalid variable id '%s' on line %zu of '%s'\n", var_token.c_str(), line_number, path.c_str());
            sst_assert(parsed > 0 && parsed <= std::numeric_limits<int>::max(), CALL_INFO, -1,
                       "Variable id out of range on line %zu of '%s'\n", line_number, path.c_str());
            sst_assert(tracked_var_count != 0 && static_cast<size_t>(parsed) <= tracked_var_count, CALL_INFO, -1,
                       "Variable id %ld exceeds tracked variable count %zu on line %zu of '%s'\n",
                       parsed, tracked_var_count, line_number, path.c_str());
            step.type = Step::Type::Insert;
            step.var = static_cast<int>(parsed);
        } else if (cmd_lower == "bump") {
            std::string var_token;
            if (!(iss >> var_token)) {
                sst_assert(false, CALL_INFO, -1,
                           "Missing variable id for BUMP on line %zu of '%s'\n", line_number, path.c_str());
            }
            char* endptr = nullptr;
            long parsed = std::strtol(var_token.c_str(), &endptr, 0);
            sst_assert(endptr != var_token.c_str() && *endptr == '\0', CALL_INFO, -1,
                       "Invalid variable id '%s' on line %zu of '%s'\n", var_token.c_str(), line_number, path.c_str());
            sst_assert(parsed > 0 && parsed <= std::numeric_limits<int>::max(), CALL_INFO, -1,
                       "Variable id out of range on line %zu of '%s'\n", line_number, path.c_str());
            sst_assert(tracked_var_count != 0 && static_cast<size_t>(parsed) <= tracked_var_count, CALL_INFO, -1,
                       "Variable id %ld exceeds tracked variable count %zu on line %zu of '%s'\n",
                       parsed, tracked_var_count, line_number, path.c_str());
            step.type = Step::Type::Bump;
            step.var = static_cast<int>(parsed);
        } else if (cmd_lower == "remove" || cmd_lower == "rem") {
            std::string extra;
            sst_assert(!(iss >> extra), CALL_INFO, -1,
                       "Unexpected token '%s' for REMOVE on line %zu of '%s'\n", extra.c_str(), line_number, path.c_str());
            step.type = Step::Type::Remove;
            step.var = 0;
        } else {
            sst_assert(false, CALL_INFO, -1,
                       "Unrecognized command '%s' on line %zu of '%s'\n", command.c_str(), line_number, path.c_str());
        }

        script.push_back(step);

        if (step.type != Step::Type::Remove) {
            unique_vars.insert(step.var);
        }
    }

    sst_assert(header_parsed, CALL_INFO, -1,
               "Script '%s' did not provide a tracked variable count header\n", path.c_str());

    output.verbose(CALL_INFO, 1, 0,
                   "Loaded %zu steps with %zu tracked vars (%zu touched) from script '%s'\n",
                   script.size(), tracked_vars.size(), unique_vars.size(), path.c_str());
}

