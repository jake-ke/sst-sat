#!/bin/zsh

# Create logs directory if it doesn't exist
LOGS_DIR="./logs"
# LOGS_DIR="./logs_dec"
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

# Check if decision directory was provided
if [[ $# -gt 0 ]]; then
    DECISION_DIR="$1"
fi

echo "Starting tests at $(date)"
echo "Logs will be saved to $LOGS_DIR"
echo "Summary will be saved to $LOG_FILE"
[[ -n "$DECISION_DIR" ]] && echo "Using decision files from $DECISION_DIR"

# Function to log messages to both console and log file
log_message() {
    echo "$1"
    echo "$1" >> "$LOG_FILE"
}

# Write header to log file
log_message "Test run started at $(date)"
log_message "==============================================="

# Function to run tests for a given directory
run_tests_for_directory() {
    local dir=$1
    local dir_type=$2
    local passed_var="${dir_type}_passed"
    local timedout_var="${dir_type}_timedout"
    local failed_var="${dir_type}_failed"
    local total_var="${dir_type}_total"
    
    log_message "Testing files from: $dir (${dir_type})"
    log_message "==============================================="
    
    # Store files in an array (handling spaces in filenames)
    setopt extended_glob nullglob
    local files=($dir/**/*(.))  # (.) qualifier matches regular files
    eval "${total_var}=\${#files}"
    
    log_message "Found ${#files} files to test"
    
    # Process each file
    for file in "${files[@]}"; do
        filename=$(basename "$file")
        
        start_time=$(date +"%H:%M:%S")
        log_message "[$start_time] Testing $filename..."
        
        # Check if decision file exists and should be used
        command="timeout 1800 sst ../tests/test_basic.py -- \"$file\""
        if [[ -n "$DECISION_DIR" ]]; then
            decision_file="${DECISION_DIR}/${filename}.dec"
            if [[ -f "$decision_file" ]]; then
                log_message "Using decision file: $decision_file"
                command="timeout 1800 sst ../tests/test_basic.py -- \"$file\" \"$decision_file\""
            else
                log_message "Decision file not found for $filename"
            fi
        fi

        # Run the test with proper command
        eval $command > "$LOGS_DIR/${filename}_${dir_type}_$TIMESTAMP.log" 2>&1
        
        end_time=$(date +"%H:%M:%S")
        
        # Check exit status
        exit_status=$?
        if [ $exit_status -eq 0 ]; then
            # Check for ERROR occurrences even in passed tests
            log_file="$LOGS_DIR/${filename}_${dir_type}_$TIMESTAMP.log"
            error_count=$(grep -c "ERROR" "$log_file")
            if [ $error_count -gt 0 ]; then
                log_message "[$end_time]   FAILED (found $error_count errors in $log_file)"
                eval "$failed_var=\$(($failed_var + 1))"
            else
                log_message "[$end_time]   PASSED"
                eval "$passed_var=\$(($passed_var + 1))"
            fi
        elif [ $exit_status -eq 124 ] || [ $exit_status -eq 142 ]; then
            # 124 and 142 are timeout's exit codes
            log_message "[$end_time]   TIMED OUT"
            eval "$timedout_var=\$(($timedout_var + 1))"
        else
            log_message "[$end_time]   FAILED with exit code $exit_status"
            eval "$failed_var=\$(($failed_var + 1))"
        fi
    done
}

# Run tests for SAT directory
run_tests_for_directory "$SAT_DIR" "sat"

# Run tests for UNSAT directory
run_tests_for_directory "$UNSAT_DIR" "unsat"

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
  Total:      $sat_total
  Passed:     $sat_passed
  Timed out:  $sat_timedout
  Failed:     $sat_failed

UNSAT Tests:
  Total:      $unsat_total
  Passed:     $unsat_passed
  Timed out:  $unsat_timedout
  Failed:     $unsat_failed

Overall:
  Total tests: $total
  Passed:      $passed
  Timed out:   $timedout
  Failed:      $failed
===============================================
Test run completed at $(date)
"

log_message "$summary"