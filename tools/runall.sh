#!/bin/zsh

# Create logs directory if it doesn't exist
# Parse folder name argument (default to 'logs')
FOLDER_NAME="logs"
if [[ $# -gt 0 && "$1" != -* ]]; then
    FOLDER_NAME="$1"
    shift
fi

LOGS_DIR="./runs/$FOLDER_NAME"
mkdir -p "$LOGS_DIR"

# Create a timestamp for this run
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
LOG_FILE="$LOGS_DIR/summary_$TIMESTAMP.log"

# Initialize counters
sat_passed=0
sat_timedout=0
sat_failed=0
sat_total=0

unsat_passed=0
unsat_timedout=0
unsat_failed=0
unsat_total=0

# Define directories
SAT_DIR=~/michael_sat_solver/SAT_test_cases/sat
UNSAT_DIR=~/michael_sat_solver/SAT_test_cases/unsat
DECISION_DIR="" # Default to empty

# Default number of parallel jobs (use available CPU cores)
MAX_JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
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
        *)
            echo "$result"
            ;;
    esac
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -j|--jobs)
            if [[ "$2" =~ ^[0-9]+$ ]]; then
                MAX_JOBS=$2
                shift 2
            else
                echo "Error: -j/--jobs requires a number argument"
                exit 1
            fi
            ;;
        *)
            DECISION_DIR="$1"
            shift
            ;;
    esac
done

echo "Starting tests at $(date)"
echo "Logs will be saved to $LOGS_DIR"
echo "Summary will be saved to $LOG_FILE"
echo "Running with $MAX_JOBS parallel jobs"
[[ -n "$DECISION_DIR" ]] && echo "Using decision files from $DECISION_DIR"

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
log_message "Test run started at $(date)"
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

# Function to find and kill all processes matching a pattern
kill_matching_processes() {
    local pattern=$1
    local pids=$(ps -eo pid,comm | grep "$pattern" | awk '{print $1}')
    
    if [[ -n "$pids" ]]; then
        log_message "Found ${#pids} $pattern processes to terminate"
        for pid in $pids; do
            # Skip our own grep/awk processes
            if ps -p $pid >/dev/null 2>&1; then
                kill -9 $pid 2>/dev/null
                log_message "Killed $pattern process with PID $pid"
            fi
        done
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

# Function to run a single test case and report results
run_single_test() {
    local file=$1
    local dir_type=$2
    local temp_result_file=$3
    local progress_file=$4
    
    filename=$(basename "$file")
    local log_file="$LOGS_DIR/${filename}_${dir_type}_$TIMESTAMP.log"
    local stats_file="$LOGS_DIR/${filename}_${dir_type}_$TIMESTAMP.stats.csv"
    local start_time=$(date +"%H:%M:%S")
    
    # Report test started - using safe file append
    append_to_file_safely "$progress_file" "START|$filename|$start_time"
    
    # Build command with timeout and basic arguments
    local command="timeout 18000 sst ./tests/test_basic.py -- --cnf \"$file\" --stats-file \"$stats_file\""
    
    # Add decision file if directory specified and file exists
    if [[ -n "$DECISION_DIR" ]]; then
        local decision_file="${DECISION_DIR}/${filename}.dec"
        [[ -f "$decision_file" ]] && command+=" --dec \"$decision_file\""
    fi

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
    
    if [ "$dir_type" = "sat" ] && [ $exit_status -eq 0 ] && grep -q "SATISFIABLE: All variables assigned" "$log_file"; then
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
            fi
        fi
        
        # Clean up the temporary file
        rm -f "$solution_file"
    fi
    
    local end_time=$(date +"%H:%M:%S")
    local result=""
    
    # Check exit status
    if [ $exit_status -eq 0 ]; then
        # Check if the result matches the expected SAT/UNSAT status
        if [ "$dir_type" = "sat" ]; then
            expected="SATISFIABLE"
            
            # Check for errors first
            error_count=$(grep -i -c -E "error|fault" "$log_file")
            if [ $error_count -gt 0 ]; then
                result="FAILED"
            # Then check if expected result is present and verification passed
            elif grep -q -w "$expected" "$log_file" && [ $verifier_status -eq 0 ]; then
                result="PASSED"
            else
                result="FAILED"
            fi
        else
            expected="UNSATISFIABLE"
            
            # Check for errors first
            error_count=$(grep -i -c -E "error|fault" "$log_file")
            if [ $error_count -gt 0 ]; then
                result="FAILED"
            # Then check if expected result is present
            elif ! grep -q -w "$expected" "$log_file"; then
                result="FAILED"
            else
                result="PASSED"
            fi
        fi
    elif [ $exit_status -eq 124 ] || [ $exit_status -eq 142 ]; then
        # 124 and 142 are timeout's exit codes
        result="TIMEOUT"
    else
        result="FAILED"
    fi
    
    # Signal that this test is complete for progress reporting
    append_to_file_safely "$progress_file" "DONE|$filename|$result|$start_time|$end_time"
    
    # Write result to temporary file (thread-safe way to collect results)
    append_to_file_safely "$temp_result_file" "$filename|$dir_type|$start_time|$end_time|$result"
}

# Function to clean up all child processes
cleanup() {
    log_message "Cleaning up..."
    
    # First try to kill processes we're tracking
    for pid in ${ALL_PIDS[@]}; do
        kill_process_tree "$pid"
    done
    
    # Then specifically target any remaining SST and timeout processes
    killall "sstsim.x"
    killall "timeout"
    
    # Verify all SST processes are gone
    local remaining_sst=$(ps -eo pid,comm | grep -c "sstsim.x")
    if [[ $remaining_sst -gt 0 ]]; then
        log_message "WARNING: $remaining_sst sstsim.x processes may still be running"
        # Last resort - more aggressive pattern matching and killing
        pkill -9 -f "sstsim.x" 2>/dev/null
    fi
    
    local remaining_timeout=$(ps -eo pid,comm | grep -c "timeout")
    if [[ $remaining_timeout -gt 0 ]]; then
        log_message "WARNING: $remaining_timeout timeout processes may still be running"
        # Last resort - more aggressive pattern matching and killing
        pkill -9 -f "timeout" 2>/dev/null
    fi
    
    log_message "All child processes terminated. Exiting."
    exit 1
}

# Set trap to catch SIGINT (Ctrl+C)
trap cleanup INT
# trap cleanup EXIT  # Also clean up on normal exit

# Function for thread-safe file operations
safe_read_line() {
    local file=$1
    local line=""
    
    # Use a lock file to prevent race conditions
    local lockfile="${file}.lock"
    
    # Try to acquire lock (non-blocking)
    if mkdir "$lockfile" 2>/dev/null; then
        # We got the lock, read the first line if file exists and has content
        if [[ -s "$file" ]]; then
            line=$(head -n 1 "$file")
            # Remove first line without using temporary files
            if [[ -n "$line" ]]; then
                tail -n +2 "$file" > "${file}.new"
                mv "${file}.new" "$file"
            fi
        fi
        # Release the lock
        rmdir "$lockfile"
        echo "$line"
    fi
}

# Function to run tests for both SAT and UNSAT directories together
run_tests_for_directory() {
    local sat_dir=$1
    local unsat_dir=$2
    
    log_message "Testing files from both directories:"
    log_message "  SAT: $sat_dir"
    log_message "  UNSAT: $unsat_dir"
    log_message "==============================================="
    
    # Store files in arrays (handling spaces in filenames)
    setopt extended_glob nullglob
    local sat_files=($sat_dir/**/*(.))  # (.) qualifier matches regular files
    local unsat_files=($unsat_dir/**/*(.))
    
    # Set totals
    sat_total=${#sat_files}
    unsat_total=${#unsat_files}
    local total_files=$((sat_total + unsat_total))
    
    log_message "Found $sat_total SAT files and $unsat_total UNSAT files to test ($total_files total)"
    echo "Starting tests (0/$total_files)"
    
    # Create temporary files for results and progress tracking
    local temp_result_file=$(mktemp)
    local progress_file=$(mktemp)
    
    # Variables for progress tracking
    local completed=0
    
    # Start a background process to monitor progress
    {
        while true; do
            if [[ -f "$progress_file" ]]; then
                line=$(safe_read_line "$progress_file")
                
                if [[ -n "$line" ]]; then
                    # Parse the progress info
                    IFS='|' read -r action filename remaining <<< "$line"
                    
                    if [[ "$action" == "START" ]]; then
                        # Extract start time
                        start_time=$remaining
                        echo "LAUNCHED: $filename"
                    elif [[ "$action" == "DONE" ]]; then
                        # Extract result and times
                        IFS='|' read -r result start_time end_time <<< "$remaining"
                        
                        # Update counters
                        completed=$((completed + 1))
                        
                        # Log the result to the main log with color for console
                        local colored_result=$(colorize_result "$result")
                        local plain_message="$filename: $result ($start_time-$end_time) ($completed/$total_files)"
                        local colored_message="$filename: $colored_result ($start_time-$end_time) ($completed/$total_files)"
                        log_message_with_color "$plain_message" "$colored_message"
                    fi
                fi
            fi
            
            sleep 0.1
            
            # Check if all tests are complete
            [[ $completed -eq $total_files ]] && break
        done
    } &
    local monitor_pid=$!
    ALL_PIDS+=($monitor_pid)
    
    # Process files in parallel using a more compatible approach
    local running_jobs=0
    local job_pids=()
    local test_map=() # Maps PIDs to test names
    
    # Combine all files with their types for processing
    local all_files=()
    local file_types=()
    
    # Add SAT files
    for file in "${sat_files[@]}"; do
        all_files+=("$file")
        file_types+=("sat")
    done
    
    # Add UNSAT files
    for file in "${unsat_files[@]}"; do
        all_files+=("$file")
        file_types+=("unsat")
    done
    
    # Process all files together with dynamic job management
    local file_index=1
    
    # Launch initial batch of jobs up to MAX_JOBS
    while (( file_index <= ${#all_files[@]} && running_jobs < MAX_JOBS )); do
        local file="${all_files[$file_index]}"
        local dir_type="${file_types[$file_index]}"
        
        # Run the test in the background
        run_single_test "$file" "$dir_type" "$temp_result_file" "$progress_file" &
        local pid=$!
        
        # Track this job
        job_pids+=($pid)
        test_map[$pid]=$(basename "$file")
        ALL_PIDS+=($pid)
        running_jobs=$((running_jobs + 1))
        file_index=$((file_index + 1))
    done
    
    # Continue launching jobs as others complete
    while (( running_jobs > 0 )); do
        # Wait for any job to complete (non-blocking check)
        local completed_pid=""
        local new_job_pids=()
        
        # Check which jobs are still running
        for pid in "${job_pids[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                # Job is still running
                new_job_pids+=($pid)
            else
                # Job has completed
                if [[ -z "$completed_pid" ]]; then
                    completed_pid=$pid
                fi
                # Clean up tracking for this PID
                ALL_PIDS=("${(@)ALL_PIDS:#$pid}")
                unset "test_map[$pid]"
                running_jobs=$((running_jobs - 1))
            fi
        done
        
        # Update job_pids array to only include running jobs
        job_pids=("${new_job_pids[@]}")
        
        # Launch new job if we have capacity and more files to process
        if (( file_index <= ${#all_files[@]} && running_jobs < MAX_JOBS )); then
            local file="${all_files[$file_index]}"
            local dir_type="${file_types[$file_index]}"
            
            # Run the test in the background
            run_single_test "$file" "$dir_type" "$temp_result_file" "$progress_file" &
            local pid=$!
            
            # Track this job
            job_pids+=($pid)
            test_map[$pid]=$(basename "$file")
            ALL_PIDS+=($pid)
            running_jobs=$((running_jobs + 1))
            file_index=$((file_index + 1))
        fi
        
        # Small sleep to avoid busy waiting
        sleep 0.1
    done
    
    # Wait for all background jobs to complete
    wait ${job_pids[@]} 2>/dev/null || true
    
    # Wait for the monitor to finish
    wait $monitor_pid 2>/dev/null || true
    kill $monitor_pid 2>/dev/null || true
    ALL_PIDS=("${(@)ALL_PIDS:#$monitor_pid}")
    
    # Clear the job PIDs from global array
    for pid in ${job_pids[@]}; do
        ALL_PIDS=("${(@)ALL_PIDS:#$pid}")
    done
    
    # Process results from temporary file for statistics
    while IFS="|" read -r filename type start_time end_time result; do
        case "$type" in
            "sat")
                case "$result" in
                    "PASSED")
                        sat_passed=$((sat_passed + 1))
                        ;;
                    "TIMEOUT")
                        sat_timedout=$((sat_timedout + 1))
                        ;;
                    "FAILED")
                        sat_failed=$((sat_failed + 1))
                        ;;
                esac
                ;;
            "unsat")
                case "$result" in
                    "PASSED")
                        unsat_passed=$((unsat_passed + 1))
                        ;;
                    "TIMEOUT")
                        unsat_timedout=$((unsat_timedout + 1))
                        ;;
                    "FAILED")
                        unsat_failed=$((unsat_failed + 1))
                        ;;
                esac
                ;;
        esac
    done < "$temp_result_file"
    
    # Clean up temporary files
    rm -f "$temp_result_file"
    rm -f "$progress_file"
    
    # Display completion message
    log_message "Completed all tests: SAT($sat_passed passed, $sat_timedout timed out, $sat_failed failed) UNSAT($unsat_passed passed, $unsat_timedout timed out, $unsat_failed failed)"
}

# Run tests for both SAT and UNSAT directories together
run_tests_for_directory "$SAT_DIR" "$UNSAT_DIR"

# Calculate totals
total=$((sat_total + unsat_total))
passed=$((sat_passed + unsat_passed))
timedout=$((sat_timedout + unsat_timedout))
failed=$((sat_failed + unsat_failed))

# Display summary
summary="
Test Summary
===============================================
SAT Tests:
    Passed:     $sat_passed/$sat_total
    Timed out:  $sat_timedout/$sat_total
    Failed:     $sat_failed/$sat_total

UNSAT Tests:
    Passed:     $unsat_passed/$unsat_total
    Timed out:  $unsat_timedout/$unsat_total
    Failed:     $unsat_failed/$unsat_total

Overall:
    Passed:      $passed/$total
    Timed out:   $timedout/$total
    Failed:      $failed/$total
===============================================
Test run completed at $(date)
"

log_message "$summary"