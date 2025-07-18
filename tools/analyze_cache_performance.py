import argparse
import numpy as np
import matplotlib.pyplot as plt
from unified_parser import parse_log_directory


def create_miss_rate_boxplot(data_points, output_file="miss_rates_comparison.png"):
    """Create a box plot comparing miss rates across cache levels."""
    print("Generating miss rate box plot...")
    plt.figure(figsize=(10, 6))
    
    # Prepare data for box plot
    l1_data = [d.get('l1_total_miss_rate', 0) for d in data_points if d.get('l1_total_miss_rate', 0) > 0]
    
    if not l1_data:
        print("No L1 cache data available for box plot")
        return
    
    data = [l1_data]
    labels = ["L1 Cache"]
    
    # Create box plot
    bp = plt.boxplot(data, labels=labels, showfliers=True)
    
    # Add grid lines for better readability
    plt.grid(True, linestyle='--', alpha=0.7)
    
    # Add title and labels
    plt.title('L1 Cache Miss Rate Distribution', fontsize=15)
    plt.ylabel('Miss Rate (%)', fontsize=12)
    plt.xlabel('Cache Level', fontsize=12)
    
    # Adjust layout and save
    plt.tight_layout()
    plt.savefig(output_file, dpi=300)
    print(f"Box plot saved as {output_file}")
    plt.close()
    

def create_multiplot_figure(data_points, output_file="cache_metrics_analysis.png"):
    """Create plots showing miss rates vs. different metrics."""
    print("Generating metric correlation plots...")
    fig, axes = plt.subplots(1, 3, figsize=(18, 5))
    
    # Check if we have necessary metrics
    l1_data = [d for d in data_points if d.get('l1_total_miss_rate', 0) > 0]
    
    if not l1_data:
        print("No L1 cache data available for correlation plots")
        return
    
    # Metrics for columns
    x_metrics = ['variables', 'clauses', 'sim_time_ms']
    x_labels = ['Number of Variables', 'Number of Clauses', 'Runtime (ms)']
    
    # Extract data for plotting
    miss_rates = [d['l1_total_miss_rate'] for d in l1_data]
    
    # Create each subplot
    for j, (metric, label) in enumerate(zip(x_metrics, x_labels)):
        ax = axes[j]
        
        # Get data, filtering out invalid values
        x_data = [d.get(metric, 0) for d in l1_data]
        valid_pairs = [(x, y) for x, y in zip(x_data, miss_rates) if x > 0]
        
        if valid_pairs:
            x_clean, y_clean = zip(*valid_pairs)
            x_clean = np.array(x_clean)
            y_clean = np.array(y_clean)
            
            # Plot miss rate vs the metric
            ax.scatter(x_clean, y_clean, alpha=0.7, edgecolors='k', linewidths=0.5)
            
            # Add linear trendline if enough data points
            if len(x_clean) > 2:
                try:
                    z = np.polyfit(x_clean, y_clean, 1)
                    p = np.poly1d(z)
                    x_line = np.linspace(min(x_clean), max(x_clean), 100)
                    ax.plot(x_line, p(x_line), '--', color='r', alpha=0.7)
                except:
                    pass
        else:
            ax.text(0.5, 0.5, f'No data available\nfor {metric}', 
                    ha='center', va='center', transform=ax.transAxes)
        
        # Set labels and title
        ax.set_xlabel(label)
        ax.set_ylabel('L1 Miss Rate (%)')
        ax.set_title(f'L1 Cache Miss Rate vs {label}')
        ax.grid(True, linestyle='--', alpha=0.7)
    
    # Add overall title
    plt.suptitle("L1 Cache Miss Rate Analysis", fontsize=16)
    
    # Adjust layout
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    plt.savefig(output_file, dpi=300)
    print(f"Multi-plot figure saved as {output_file}")
    plt.close()


def main():
    """Main function to process log files and create plots."""
    parser = argparse.ArgumentParser(
        description="Analyze cache performance from SAT solver log files"
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
    
    # Process log files using unified parser
    data_points = parse_log_directory(args.log_dir, exclude_summary=True)
    
    if not data_points:
        print("No valid data found in the log files.")
        return 1
    
    print(f"Successfully processed {len(data_points)} log files")
    
    # Generate requested plots
    if not args.no_boxplot:
        create_miss_rate_boxplot(data_points, args.boxplot)
    
    if not args.no_metrics_plot:
        create_multiplot_figure(data_points, args.metrics_plot)
    
    print("Analysis complete!")


if __name__ == "__main__":
    main()
