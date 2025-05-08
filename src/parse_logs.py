import os
import re
import csv
import glob

# Metrics to extract
METRICS = ['decisions', 'propagations', 'conflicts', 'learned', 'removed', 'db_reductions', 'restarts']

def extract_metrics_from_log(log_path):
    """Extract metrics from a log file."""
    metrics = {metric: 0 for metric in METRICS}
    
    try:
        with open(log_path, 'r') as f:
            content = f.read()
            
            # Use regular expressions to find the metrics
            for metric in METRICS:
                pattern = rf'{metric}\s*:\s*(\d+)'
                matches = re.findall(pattern, content, re.IGNORECASE)
                if matches:
                    metrics[metric] = int(matches[-1])  # Take the last occurrence
    except Exception as e:
        print(f"Error processing {log_path}: {e}")
    
    return metrics

def main():
    # Define directories
    current_logs_dir = 'logs'
    reference_logs_dir = os.path.expanduser('~/minisat/logs/')
    
    # Check if directories exist
    for directory in [current_logs_dir, reference_logs_dir]:
        if not os.path.exists(directory):
            print(f"Error: Directory {directory} does not exist.")
            return
    
    # Get all log files (excluding summary)
    current_log_files = [f for f in glob.glob(f"{current_logs_dir}/*") if os.path.isfile(f) and 'summary' not in os.path.basename(f).lower()]
    reference_log_files = [f for f in glob.glob(f"{reference_logs_dir}/*") if os.path.isfile(f) and 'summary' not in os.path.basename(f).lower()]
    
    if not current_log_files:
        print(f"No log files found in {current_logs_dir}.")
        return
    
    # Extract test names without dates
    def get_test_name(filename):
        # Assuming format like "test_name_YYYY-MM-DD.log" or "test_name_timestamp.log"
        # Strip out date/timestamp parts (customize this pattern based on your actual filenames)
        base_name = os.path.basename(filename)
        # Match everything up to the last underscore or till the extension
        match = re.match(r'(.+?)(?:_\d|\.)', base_name)
        return match.group(1) if match else base_name
    
    # Map reference files by their test names
    reference_map = {}
    for ref_log in reference_log_files:
        test_name = get_test_name(ref_log)
        reference_map[test_name] = ref_log
    
    # Prepare CSV output
    csv_file = 'log_comparison.csv'
    fieldnames = ['test'] + [f'{metric}_{suffix}' for metric in METRICS for suffix in ['current', 'reference', 'diff']]
    
    with open(csv_file, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        
        # For calculating averages
        total_diffs = {metric: 0 for metric in METRICS}
        count = 0
        
        # Process each log file
        for log_file in sorted(current_log_files):
            test_name = get_test_name(log_file)
            
            if test_name not in reference_map:
                print(f"No reference log found for test {test_name}. Skipping.")
                continue
                
            reference_log = reference_map[test_name]
            
            # Extract metrics
            current_metrics = extract_metrics_from_log(log_file)
            reference_metrics = extract_metrics_from_log(reference_log)
            
            # Calculate differences
            differences = {metric: current_metrics[metric] - reference_metrics[metric] for metric in METRICS}
            
            # Update totals for averages
            for metric in METRICS:
                total_diffs[metric] += differences[metric]
            
            # Prepare row for CSV
            row = {'test': os.path.basename(log_file)}
            for metric in METRICS:
                row[f'{metric}_current'] = current_metrics[metric]
                row[f'{metric}_reference'] = reference_metrics[metric]
                row[f'{metric}_diff'] = differences[metric]
            
            writer.writerow(row)
            count += 1

if __name__ == "__main__":
    main()