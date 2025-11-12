#!/bin/zsh

# Preserve script name for usage printing (zsh sets $0 to function name inside functions)
SCRIPT_NAME=$(basename "$0")

# Check if help is requested or show usage
show_usage() {
    echo "Usage: $SCRIPT_NAME --bench-dir DIR [--ram2-cfg FILE] [--classic-heap] [--l1-size SIZE] [--l1-latency LATENCY] [--mem-latency LATENCY] [--prefetch] [--spec] [--folder FOLDER] [--num-seeds NUM] [--timeout-cycles CYCLES] [-j jobs]"
    echo "Options:"
    echo "  -b, --bench-dir DIR  Directory containing benchmark CNF files (required)"
    echo "  --ram2-cfg FILE       Ramulator2 configuration file"
    echo "  --classic-heap        Use classic heap implementation instead of pipelined"
    echo "  --l1-size SIZE        L1 cache size"
    echo "  --l1-latency LATENCY  L1 cache latency cycles"
    echo "  --l1-bw BW            L1 cache bandwidth (max requests per cycle)"
    echo "  --l2-latency LATENCY  L2 cache latency cycles"
    echo "  --l2-bw BW            L2 cache bandwidth (max requests per cycle)"
    echo "  --mem-latency LATENCY External memory latency"
    echo "  --prefetch            Enable directed prefetching"
    echo "  --spec                Enable speculative propagation"
    echo "  --folder FOLDER       Name for the logs folder (default: logs)"
    echo "  --num-seeds NUM       Number of random seeds to run (default: 1)"
    echo "  --timeout-cycles N    Maximum solver cycles before timing out (0 or omit for unlimited)"
    echo "  -j, --jobs JOBS       Number of parallel jobs"
    echo "Example: $SCRIPT_NAME --bench-dir /path/to/benchmarks --l1-size 32KiB --l1-latency 2 --mem-latency 200ns --prefetch --timeout-cycles 100000000 --folder quick_test --num-seeds 5 -j 8"
}

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    show_usage
    exit 0
fi

# Optional values (no defaults - let Python script handle defaults)
BENCHMARK_DIR=""
RAM2_CFG=""
CLASSIC_HEAP=""
L1_SIZE=""
L1_LATENCY=""
L1_BW=""
L2_LATENCY=""
L2_BW=""
MEM_LATENCY=""
FOLDER_NAME="logs"
PREFETCH=""
SPEC=""
NUM_SEEDS=1
TIMEOUT_CYCLES=""

# Default number of parallel jobs (use available CPU cores)
MAX_JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

# Function to colorize result text
colorize_result() {
    local result=$1
    case "$result" in
        "PASSED")
            echo "${GREEN}${result}${NC}"
            ;;
        "FAILED"|"TIMEOUT")
            echo "${RED}${result}${NC}"
            ;;
        "SKIPPED")
            echo "${YELLOW}${result}${NC}"
            ;;
        *)
            echo "$result"
            ;;
    esac
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--bench-dir|--benchmark-dir)
            if [[ -n "$2" && "$2" != -* ]]; then
                BENCHMARK_DIR="$2"
                shift 2
            else
                echo "Error: --bench-dir requires a directory argument"
                show_usage
                exit 1
            fi
            ;;
        --ram2-cfg)
            if [[ -n "$2" && "$2" != -* ]]; then
                RAM2_CFG="$2"
                shift 2
            else
                echo "Error: --ram2-cfg requires a configuration file argument"
                show_usage
                exit 1
            fi
            ;;
        --classic-heap)
            CLASSIC_HEAP=1
            shift
            ;;
        --l1-size)
            if [[ -n "$2" && "$2" != -* ]]; then
                L1_SIZE="$2"
                shift 2
            else
                echo "Error: --l1-size requires a size argument"
                show_usage
                exit 1
            fi
            ;;
        --l1-latency)
            if [[ -n "$2" && "$2" != -* ]]; then
                L1_LATENCY="$2"
                shift 2
            else
                echo "Error: --l1-latency requires a latency argument"
                show_usage
                exit 1
            fi
            ;;
        --l1-bw)
            if [[ -n "$2" && "$2" != -* ]]; then
                L1_BW="$2"
                shift 2
            else
                echo "Error: --l1-bw requires a bandwidth argument"
                show_usage
                exit 1
            fi
            ;;
        --l2-latency)
            if [[ -n "$2" && "$2" != -* ]]; then
                L2_LATENCY="$2"
                shift 2
            else
                echo "Error: --l2-latency requires a latency argument"
                show_usage
                exit 1
            fi
            ;;
        --l2-bw)
            if [[ -n "$2" && "$2" != -* ]]; then
                L2_BW="$2"
                shift 2
            else
                echo "Error: --l2-bw requires a bandwidth argument"
                show_usage
                exit 1
            fi
            ;;
        --mem-latency)
            if [[ -n "$2" && "$2" != -* ]]; then
                MEM_LATENCY="$2"
                shift 2
            else
                echo "Error: --mem-latency requires a latency argument"
                show_usage
                exit 1
            fi
            ;;
        --prefetch)
            PREFETCH="yes"
            shift
            ;;
        --spec)
            SPEC="yes"
            shift
            ;;
        --folder)
            if [[ -n "$2" && "$2" != -* ]]; then
                FOLDER_NAME="$2"
                shift 2
            else
                echo "Error: --folder requires a folder name argument"
                show_usage
                exit 1
            fi
            ;;
        --num-seeds)
            if [[ "$2" =~ ^[0-9]+$ ]]; then
                NUM_SEEDS=$2
                shift 2
            else
                echo "Error: --num-seeds requires a number argument"
                show_usage
                exit 1
            fi
            ;;
        --timeout-cycles)
            if [[ -n "$2" && "$2" =~ ^[0-9]+$ ]]; then
                TIMEOUT_CYCLES=$2
                shift 2
            else
                echo "Error: --timeout-cycles requires a numeric cycles argument"
                show_usage
                exit 1
            fi
            ;;
        -j|--jobs)
            if [[ "$2" =~ ^[0-9]+$ ]]; then
                MAX_JOBS=$2
                shift 2
            else
                echo "Error: -j/--jobs requires a number argument"
                exit 1
            fi
            ;;
        -*)
            echo "Error: Unknown option $1"
            show_usage
            exit 1
            ;;
        *)
            echo "Error: Unexpected argument '$1'. Use --folder for folder name."
            show_usage
            exit 1
            ;;
    esac
done

# Create logs directory if it doesn't exist
LOGS_DIR="./runs/$FOLDER_NAME"
mkdir -p "$LOGS_DIR"

# Remove timestamp-related variables
LOG_FILE="$LOGS_DIR/summary.log"

# Initialize counters
passed=0
failed=0
timedout=0
skipped=0
total=0

# Ensure benchmark directory was provided and exists
if [[ -z "$BENCHMARK_DIR" ]]; then
    echo "Error: --bench-dir is required and must point to a directory containing benchmark files"
    show_usage
    exit 1
fi

if [[ ! -d "$BENCHMARK_DIR" ]]; then
    echo "Error: Benchmark directory not found: $BENCHMARK_DIR"
    exit 1
fi

# Function to log messages to both console and log file
log_message() {
    echo "$1"
    echo "$1" >> "$LOG_FILE"
}

# Function to log with color to console but plain text to file
log_message_with_color() {
    local message=$1
    local colored_message=$2
    echo -e "$colored_message"
    echo "$message" >> "$LOG_FILE"
}

# Write header to log file
log_message "==============================================="
log_message "SAT Competition Quick Test"
log_message "Test run started at $(date)"
log_message "Output directory: $LOGS_DIR"
log_message "Parallel jobs: $MAX_JOBS"
log_message "Benchmark directory: $BENCHMARK_DIR"
log_message "RAMULATOR2 configuration: ${RAM2_CFG:-default}"
log_message "L1 cache size: ${L1_SIZE:-default}"
log_message "L1 cache latency: ${L1_LATENCY:-default}"
log_message "L1 cache bandwidth: ${L1_BW:-default}"
log_message "L2 cache latency: ${L2_LATENCY:-default}"
log_message "L2 cache bandwidth: ${L2_BW:-default}"
log_message "Memory latency: ${MEM_LATENCY:-default}"
log_message "Number of random seeds: $NUM_SEEDS"
if [[ -n "$TIMEOUT_CYCLES" ]]; then
    log_message "Timeout cycles: $TIMEOUT_CYCLES"
else
    log_message "Timeout cycles: unlimited"
fi
if [[ -n "$CLASSIC_HEAP" ]]; then
    log_message "Using Classic heap"
else
    log_message "Using Pipelined heap"
fi
if [[ -n "$PREFETCH" ]]; then
    log_message "Directed prefetching: enabled"
else
    log_message "Directed prefetching: disabled"
fi
if [[ -n "$SPEC" ]]; then
    log_message "Speculative propagation: enabled"
else
    log_message "Speculative propagation: disabled"
fi
log_message "==============================================="

# Array to store all child PIDs for cleanup
ALL_PIDS=()

# Function to kill a process and all its descendants
kill_process_tree() {
    local pid=$1
    local child_pids=$(ps -o pid --no-headers --ppid "$pid" 2>/dev/null)
    
    # First kill children recursively
    for child_pid in $child_pids; do
        kill_process_tree "$child_pid"
    done
    
    # Then kill the parent
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
    fi
}

# Function for thread-safe file operations with proper locking
append_to_file_safely() {
    local file=$1
    local content=$2
    local lockfile="${file}.lock"
    
    # Try to acquire lock (non-blocking)
    until mkdir "$lockfile" 2>/dev/null; do
        sleep 0.1  # Small delay before retry
    done
    
    # We got the lock, append content
    echo "$content" >> "$file"
    
    # Release the lock
    rmdir "$lockfile"
}

# Function to check if a test has already been run (has existing log file)
is_test_already_run() {
    local filename=$1
    # Check if the specific log file exists
    local log_file="$LOGS_DIR/${filename}.log"
    if [[ -f "$log_file" ]]; then
        return 0  # Test already run
    fi
    return 1  # Test not yet run
}

# Function to run a single (file, seed) task and report results
run_one_seed() {
    local file=$1
    local seed=$2
    local temp_result_file=$3
    local progress_file=$4

    local filename=$(basename "$file")

    # Define file paths for this seed run
    local seed_dir="$LOGS_DIR/seed$seed"
    mkdir -p "$seed_dir"
    local log_file="$seed_dir/${filename}.log"
    local stats_file="$seed_dir/${filename}.stats.csv"

    # Check if test already has existing log files for this seed
    if [[ -f "$log_file" ]]; then
        # Mark skipped runs as completed so progress and summary are correct
        append_to_file_safely "$progress_file" "DONE|$filename (seed $seed)|SKIPPED|-|-"
        append_to_file_safely "$temp_result_file" "$filename|SKIPPED|-|-"
        return 0
    fi

    local start_time=$(date +"%H:%M:%S")

    # Report test started with seed
    append_to_file_safely "$progress_file" "START|$filename (seed $seed)|$start_time"

    # Build command. Use wall-clock timeout only if cycle timeout isn't set.
    local sst_invocation="sst ./tests/test_two_level.py -- --cnf \"$file\" --stats-file \"$stats_file\" --rand $seed"
    local command=""
    if [[ -z "$TIMEOUT_CYCLES" ]]; then
        # Apply 2-hour wall-clock timeout only when no simulation cycle timeout is provided
        command="timeout 7200 $sst_invocation"
    else
        command="$sst_invocation"
    fi

    # Add optional cache/memory parameters only if provided
    [[ -n "$RAM2_CFG" ]] && command+=" --ram2-cfg $RAM2_CFG"
    [[ -n "$CLASSIC_HEAP" ]] && command+=" --classic-heap"
    [[ -n "$L1_SIZE" ]] && command+=" --l1-size $L1_SIZE"
    [[ -n "$L1_LATENCY" ]] && command+=" --l1-latency $L1_LATENCY"
    [[ -n "$L1_BW" ]] && command+=" --l1-bw $L1_BW"
    [[ -n "$L2_LATENCY" ]] && command+=" --l2-latency $L2_LATENCY"
    [[ -n "$L2_BW" ]] && command+=" --l2-bw $L2_BW"
    [[ -n "$MEM_LATENCY" ]] && command+=" --mem-latency $MEM_LATENCY"
    [[ -n "$PREFETCH" ]] && command+=" --prefetch"
    [[ -n "$SPEC" ]] && command+=" --spec"
    [[ -n "$TIMEOUT_CYCLES" ]] && command+=" --timeout-cycles $TIMEOUT_CYCLES"

    # Run the test with proper command
    eval $command > "$log_file" 2>&1
    local exit_status=$?

    # Run the parse_stats.py script with the unique stats file
    if [ -f "$stats_file" ]; then
        echo -e "\nParsing statistics file: $stats_file" >> "$log_file"
        python3 ./tools/parse_stats.py "$stats_file" >> "$log_file" 2>&1
    else
        echo -e "\nWarning: Statistics file not found at $stats_file" >> "$log_file"
    fi

    # Extract and verify solution for SAT cases
    local verifier_status=1  # Default to failed
    if [ $exit_status -eq 0 ] && grep -q "SATISFIABLE: All variables assigned" "$log_file"; then
        # Create a temporary solution file
        local solution_file=$(mktemp)

        # Extract the solution line after "SATISFIABLE: All variables assigned"
        grep -A 2 "SATISFIABLE: All variables assigned" "$log_file" | grep -E 'x[0-9]+=' > "$solution_file"

        # Run the verifier if we got a solution
        if [ -s "$solution_file" ]; then
            python3 ./tools/verifier.py "$solution_file" "$file" > /dev/null 2>&1
            verifier_status=$?

            # If verification failed, log the output
            if [ $verifier_status -ne 0 ]; then
                echo -e "\nVerification FAILED." >> "$log_file"
            else
                echo -e "\nVerification PASSED." >> "$log_file"
            fi
        fi

        # Clean up the temporary file
        rm -f "$solution_file"
    fi

    local end_time=$(date +"%H:%M:%S")
    local result=""

    # Check exit status and determine result
    if [ $exit_status -eq 0 ]; then
        # Check for successful completion (either SAT or UNSAT)
        local error_count
        error_count=$(grep -i -c -E "error|fault" "$log_file")
        if [ $error_count -gt 0 ]; then
            result="FAILED"
        elif grep -q "SATISFIABLE: All variables assigned" "$log_file" && [ $verifier_status -eq 0 ]; then
            result="PASSED"
        elif grep -q "UNSATISFIABLE" "$log_file"; then
            result="PASSED"
        else
            result="FAILED"
        fi
    elif [ $exit_status -eq 124 ] || [ $exit_status -eq 142 ]; then
        # 124 and 142 are timeout's exit codes
        result="TIMEOUT"
    else
        result="FAILED"
    fi

    # Signal that this test is complete (include seed for clarity)
    append_to_file_safely "$progress_file" "DONE|$filename (seed $seed)|$result|$start_time|$end_time"

    # Write result to temporary file (thread-safe way to collect results)
    append_to_file_safely "$temp_result_file" "$filename|$result|$start_time|$end_time"
}

# Function for thread-safe file operations
safe_read_line() {
    local file=$1
    local line=""
    
    # Use a lock file to prevent race conditions
    local lockfile="${file}.lock"
    if mkdir "$lockfile" 2>/dev/null; then
        if [[ -s "$file" ]]; then
            line=$(head -n 1 "$file")
            if [[ -n "$line" ]]; then
                tail -n +2 "$file" > "${file}.new"
                mv "${file}.new" "$file"
            fi
        fi
        rmdir "$lockfile"
        echo "$line"
    fi
}

# Run tests (flattened tasks implementation)
run_tests() {
    local benchmark_dir=$1

    log_message "Searching for benchmark files in: $benchmark_dir"

    # Combine all files
    local all_files=($benchmark_dir/*)
    total=${#all_files}
    log_message "Found $total benchmark files to test"

    # Build flattened list of (file, seed) tasks
    local tasks=()
    local num_tasks=0
    for file in ${all_files[@]}; do
        for ((seed=0; seed<NUM_SEEDS; seed++)); do
            tasks+=("$file|$seed")
            num_tasks=$((num_tasks + 1))
        done
    done

    local total_runs=$num_tasks
    log_message "Total runs to execute: $total_runs"
    [[ $total_runs -eq 0 ]] && { log_message "No benchmark runs to execute. Exiting."; return; }

    echo "Starting tests (0/$total_runs)"

    local temp_result_file=$(mktemp)
    local progress_file=$(mktemp)
    local completed=0

    {
        while true; do
            if [[ -f "$progress_file" ]]; then
                line=$(safe_read_line "$progress_file")
                if [[ -n "$line" ]]; then
                    IFS='|' read -r action filename remaining <<< "$line"
                    if [[ "$action" == "START" ]]; then
                        start_time=$remaining
                        echo "LAUNCHED: $filename at $start_time"
                    elif [[ "$action" == "DONE" ]]; then
                        IFS='|' read -r result start_time end_time <<< "$remaining"
                        completed=$((completed + 1))
                        local colored_result=$(colorize_result "$result")
                        local plain_message="$filename: $result ($start_time-$end_time) ($completed/$total_runs)"
                        local colored_message="$filename: $colored_result ($start_time-$end_time) ($completed/$total_runs)"
                        log_message_with_color "$plain_message" "$colored_message"
                    fi
                fi
            fi
            sleep 0.1
            [[ $completed -eq $total_runs ]] && break
        done
    } &
    local monitor_pid=$!
    ALL_PIDS+=($monitor_pid)

    local running_jobs=0
    local job_pids=()
    local task_index=1
    while (( task_index <= ${#tasks[@]} && running_jobs < MAX_JOBS )); do
        local task="${tasks[$task_index]}"
        local file="${task%%|*}"
        local seed="${task##*|}"
        run_one_seed "$file" "$seed" "$temp_result_file" "$progress_file" &
        local pid=$!
        job_pids+=($pid)
        ALL_PIDS+=($pid)
        running_jobs=$((running_jobs + 1))
        task_index=$((task_index + 1))
    done

    while (( running_jobs > 0 )); do
        local new_job_pids=()
        for pid in "${job_pids[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                new_job_pids+=($pid)
            else
                ALL_PIDS=(${(@)ALL_PIDS:#$pid})
                running_jobs=$((running_jobs - 1))
            fi
        done
        job_pids=("${new_job_pids[@]}")
        if (( task_index <= ${#tasks[@]} && running_jobs < MAX_JOBS )); then
            local task="${tasks[$task_index]}"
            local file="${task%%|*}"
            local seed="${task##*|}"
            run_one_seed "$file" "$seed" "$temp_result_file" "$progress_file" &
            local pid=$!
            job_pids+=($pid)
            ALL_PIDS+=($pid)
            running_jobs=$((running_jobs + 1))
            task_index=$((task_index + 1))
        fi
        sleep 0.1
    done

    wait ${job_pids[@]} 2>/dev/null || true
    wait $monitor_pid 2>/dev/null || true
    kill $monitor_pid 2>/dev/null || true
    ALL_PIDS=(${(@)ALL_PIDS:#$monitor_pid})

    while IFS="|" read -r filename result start_time end_time; do
        case "$result" in
            "PASSED") passed=$((passed + 1)) ;;
            "TIMEOUT") timedout=$((timedout + 1)) ;;
            "FAILED") failed=$((failed + 1)) ;;
            "SKIPPED") skipped=$((skipped + 1)) ;;
        esac
    done < "$temp_result_file"

    rm -f "$temp_result_file" "$progress_file"
    log_message "Completed all tests: $passed passed, $timedout timed out, $failed failed, $skipped skipped"
}

# Run tests
run_tests "$BENCHMARK_DIR"

# Display summary
summary="
Test Summary
===============================================
Benchmark Directory: $BENCHMARK_DIR
Total Files:         $total
Passed:              $passed
Timed out:           $timedout  
Failed:              $failed
Skipped:             $skipped
===============================================
Test run completed at $(date)
"

log_message "$summary"
