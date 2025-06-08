import os
import re
import argparse
import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path

def parse_log_data(log_file):
    """Extract all required metrics from log file."""
    try:
        with open(log_file, 'r', errors='ignore') as f:
            content = f.read()
        
        data = {}
        
        # Extract miss rates
        miss_rate_pattern = r"(L\d Cache) Statistics:.*?Miss Rate:\s+(\d+\.\d+)%"
        miss_rate_matches = re.findall(miss_rate_pattern, content, re.DOTALL)
        
        for cache_level, miss_rate in miss_rate_matches:
            data[f"{cache_level.split()[0]}_miss_rate"] = float(miss_rate)
        
        # Extract vars and clauses
        problem_pattern = r"MAIN-> Problem: vars=(\d+) clauses=(\d+)"
        problem_match = re.search(problem_pattern, content)
        if problem_match:
            data['vars'] = int(problem_match.group(1))
            data['clauses'] = int(problem_match.group(2))
        
        # Extract runtime
        runtime_pattern = r"Simulation is complete, simulated time: ([\d\.]+) ([a-z]+)"
        runtime_match = re.search(runtime_pattern, content)
        if runtime_match:
            time_value = float(runtime_match.group(1))
            time_unit = runtime_match.group(2)
            
            # Convert everything to seconds
            if time_unit == "ms":
                data['runtime'] = time_value / 1000
            elif time_unit == "us":
                data['runtime'] = time_value / 1000000
            else:  # assume seconds
                data['runtime'] = time_value
        
        # Check if we have all required data
        required_fields = ['L1_miss_rate', 'L2_miss_rate', 'L3_miss_rate']
        if all(field in data for field in required_fields):
            data['filename'] = os.path.basename(log_file)
            return data
        else:
            missing = [f for f in required_fields if f not in data]
            print(f"Warning: Missing fields in {log_file}: {missing}")
            return None
            
    except Exception as e:
        print(f"Error parsing {log_file}: {e}")
        return None

def process_log_directory(log_dir):
    """Process all log files and extract data."""
    log_files = list(Path(log_dir).glob("*.log"))
    
    if not log_files:
        print(f"No log files found in {log_dir}")
        return None
    
    print(f"Found {len(log_files)} log files")
    
    # Process each log file
    data_points = []
    processed_files = 0
    for log_file in log_files:
        data = parse_log_data(log_file)
        if data:
            data_points.append(data)
            processed_files += 1
    
    print(f"Successfully processed {processed_files} out of {len(log_files)} log files")
    
    return data_points if data_points else None

def create_miss_rate_boxplot(data_points, output_file="miss_rates_comparison.png"):
    """Create a box plot comparing miss rates across cache levels."""
    print("Generating miss rate box plot...")
    plt.figure(figsize=(10, 6))
    
    # Prepare data for box plot
    l1_data = [d['L1_miss_rate'] for d in data_points]
    l2_data = [d['L2_miss_rate'] for d in data_points]
    l3_data = [d['L3_miss_rate'] for d in data_points]
    data = [l1_data, l2_data, l3_data]
    
    # Create box plot with default colors
    bp = plt.boxplot(data, labels=["L1 Cache", "L2 Cache", "L3 Cache"], 
                    showfliers=True)
    
    # Add grid lines for better readability
    plt.grid(True, linestyle='--', alpha=0.7)
    
    # Add title and labels
    plt.title('Cache Miss Rate Comparison', fontsize=15)
    plt.ylabel('Miss Rate (%)', fontsize=12)
    plt.xlabel('Cache Level', fontsize=12)
    
    # Adjust layout and save
    plt.tight_layout()
    plt.savefig(output_file, dpi=300)
    print(f"Box plot saved as {output_file}")
    
    # Close the figure to free memory
    plt.close()
    
def create_multiplot_figure(data_points, output_file="cache_metrics_analysis.png"):
    """Create a 3x3 grid of plots showing miss rates vs. different metrics."""
    print("Generating metric correlation plots...")
    fig, axes = plt.subplots(3, 3, figsize=(18, 15))
    
    # Check if we have necessary metrics
    metrics_to_check = ['vars', 'clauses', 'runtime']
    missing_metrics = [m for m in metrics_to_check if not all(m in d for d in data_points)]
    
    if missing_metrics:
        print(f"Warning: Missing metrics in data: {missing_metrics}")
        print("Some plots may be incomplete.")
    
    # Metrics for columns
    x_metrics = ['vars', 'clauses', 'runtime']
    x_labels = ['Number of Variables', 'Number of Clauses', 'Runtime (seconds)']
    
    # Cache levels for rows
    cache_levels = ['L1', 'L2', 'L3'] 
    
    # Extract data for plotting
    metrics_data = {}
    for metric in x_metrics:
        metrics_data[metric] = [d.get(metric, np.nan) for d in data_points]
    
    metrics_data['L1_miss_rate'] = [d['L1_miss_rate'] for d in data_points]
    metrics_data['L2_miss_rate'] = [d['L2_miss_rate'] for d in data_points]
    metrics_data['L3_miss_rate'] = [d['L3_miss_rate'] for d in data_points]
    
    # Create each subplot
    for i, cache_level in enumerate(cache_levels):
        for j, (metric, label) in enumerate(zip(x_metrics, x_labels)):
            ax = axes[i, j]
            
            # Get data, filtering out NaN values
            x_data = np.array(metrics_data[metric])
            y_data = np.array(metrics_data[f'{cache_level}_miss_rate'])
            mask = ~np.isnan(x_data)
            x_clean = x_data[mask]
            y_clean = y_data[mask]
            
            if len(x_clean) > 0:
                # Plot miss rate vs the metric
                ax.scatter(x_clean, y_clean, alpha=0.7, edgecolors='k', linewidths=0.5)
                
                # Add linear trendline if enough data points
                if len(x_clean) > 2:
                    try:
                        # Use numpy to calculate trendline
                        z = np.polyfit(x_clean, y_clean, 1)
                        p = np.poly1d(z)
                        
                        # Add trendline to plot
                        x_line = np.linspace(min(x_clean), max(x_clean), 100)
                        ax.plot(x_line, p(x_line), '--', color='r', alpha=0.7)
                    except:
                        # Skip trendline if calculation fails
                        pass
                
                # Set logarithmic scale for x-axis if there's a wide range of values
                if len(set(x_clean)) > 5 and max(x_clean) / (min(x_clean) or 1) > 100:
                    ax.set_xscale('log')
            else:
                ax.text(0.5, 0.5, f'No data available\nfor {metric}', 
                        ha='center', va='center', transform=ax.transAxes)
            
            # Set labels and title
            ax.set_xlabel(label)
            ax.set_ylabel(f'{cache_level} Miss Rate (%)')
            ax.set_title(f'{cache_level} Cache Miss Rate vs {label}')
            
            # Add grid for better readability
            ax.grid(True, linestyle='--', alpha=0.7)
    
    # Add overall title
    plt.suptitle("Cache Miss Rate Analysis", fontsize=20)
    
    # Adjust layout
    plt.tight_layout(rect=[0, 0, 1, 0.97])  # Make room for suptitle
    plt.savefig(output_file, dpi=300)
    print(f"Multi-plot figure saved as {output_file}")
    
    # Close the figure to free memory
    plt.close()

def main():
    """Main function to process log files and create plots."""
    parser = argparse.ArgumentParser(
        description="Analyze cache performance from SST log files"
    )
    parser.add_argument("log_dir", help="Directory containing log files to analyze")
    parser.add_argument("--boxplot", dest="boxplot", default="miss_rates_comparison.png",
                        help="Generate box plot and specify output filename")
    parser.add_argument("--no-boxplot", dest="no_boxplot", action="store_true",
                        help="Skip box plot generation")
    parser.add_argument("--metrics-plot", dest="metrics_plot", default="cache_metrics_analysis.png",
                        help="Generate metrics correlation plots and specify output filename")
    parser.add_argument("--no-metrics-plot", dest="no_metrics_plot", action="store_true",
                        help="Skip metrics correlation plots generation")
    
    args = parser.parse_args()
    
    # Process log files
    data_points = process_log_directory(args.log_dir)
    
    if not data_points:
        print("No valid data found in the log files.")
        return 1
    
    # Generate requested plots
    if not args.no_boxplot:
        create_miss_rate_boxplot(data_points, args.boxplot)
    
    if not args.no_metrics_plot:
        create_multiplot_figure(data_points, args.metrics_plot)
    
    print("Analysis complete!")

if __name__ == "__main__":
    main()
