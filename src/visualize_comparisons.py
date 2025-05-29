#!/usr/bin/env python3
"""
Visualize SAT solver performance comparisons with scatter plots.
Shows current vs backup performance for key metrics.
"""

import os
import re
import sys
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path

def parse_log_file(filepath):
    """Parse a single log file and extract statistics."""
    stats = {}
    try:
        with open(filepath, 'r') as f:
            content = f.read()
        
        # Extract filename
        filename_match = re.search(r'Using CNF file: .*/([^/\n]+)', content)
        if filename_match:
            stats['problem'] = filename_match.group(1)
        else:
            # Fallback: try to extract from first line
            first_line = content.split('\n')[0] if content else ""
            if first_line and not first_line.startswith('Using CNF file:'):
                stats['problem'] = first_line.strip()
            else:
                return None
        
        # Extract statistics using regex
        stat_patterns = {
            'decisions': r'Decisions\s*:\s*(\d+)',
            'propagations': r'Propagations\s*:\s*(\d+)',
            'conflicts': r'Conflicts\s*:\s*(\d+)', 
            'learned': r'Learned\s*:\s*(\d+)',
            'removed': r'Removed\s*:\s*(\d+)',
            'db_reductions': r'DB_Reductions\s*:\s*(\d+)',
            'minimized': r'Minimized\s*:\s*(\d+)',
            'restarts': r'Restarts\s*:\s*(\d+)',
        }
        
        for stat, pattern in stat_patterns.items():
            match = re.search(pattern, content)
            if match:
                stats[stat] = int(match.group(1))
        
        # Extract simulation time
        time_match = re.search(r'Simulation is complete, simulated time: ([\d.]+)\s*(\w+)', content)
        if time_match:
            time_val = float(time_match.group(1))
            time_unit = time_match.group(2)
            # Convert to milliseconds
            if time_unit == 'us':
                time_val *= 0.001
            elif time_unit == 's':
                time_val *= 1000
            stats['sim_time_ms'] = time_val
            
    except Exception as e:
        print(f"Error parsing {filepath}: {e}")
        return None
    
    return stats

def load_all_logs(logs_dir):
    """Load and parse all log files in a directory."""
    all_stats = []
    
    for log_file in Path(logs_dir).glob("*.log"):
        if "summary" in log_file.name:  # Skip summary files
            continue
            
        stats = parse_log_file(log_file)
        if stats:
            all_stats.append(stats)
    
    return pd.DataFrame(all_stats)

def create_scatter_plots(logs_dir, backup_dir):
    """Create scatter plots comparing current vs backup performance."""
    
    print("Loading current logs...")
    df_current = load_all_logs(logs_dir)
    print(f"Loaded {len(df_current)} current log files")
    
    print("Loading backup logs...")
    df_backup = load_all_logs(backup_dir)
    print(f"Loaded {len(df_backup)} backup log files")
    
    # Merge dataframes on problem name
    df_merged = pd.merge(df_current, df_backup, on='problem', suffixes=('_current', '_backup'))
    print(f"Found {len(df_merged)} matching problems")
    
    if df_merged.empty:
        print("No matching problems found!")
        return
    
    # Metrics to compare
    metrics = ['decisions', 'propagations', 'conflicts', 'learned', 'removed', 
               'db_reductions', 'minimized', 'restarts', 'sim_time_ms']
    
    # Create 3x3 subplot grid
    fig, axes = plt.subplots(3, 3, figsize=(18, 15))
    fig.suptitle('SAT Solver Performance Comparison: Current vs Backup', fontsize=16, fontweight='bold')
    
    # Colors for different result types
    colors = {'SAT': 'green', 'UNSAT': 'red', 'UNKNOWN': 'gray'}
    
    for i, metric in enumerate(metrics):
        row = i // 3
        col = i % 3
        ax = axes[row, col]
        
        current_col = f'{metric}_current'
        backup_col = f'{metric}_backup'
        
        if current_col in df_merged.columns and backup_col in df_merged.columns:
            # Filter out invalid values
            valid_idx = (df_merged[current_col] >= 0) & (df_merged[backup_col] >= 0)
            
            if metric == 'sim_time_ms':
                # For simulation time, also filter out zero values for log scale
                valid_idx = valid_idx & (df_merged[current_col] > 0) & (df_merged[backup_col] > 0)
            
            if valid_idx.sum() > 0:
                x_vals = df_merged.loc[valid_idx, backup_col]
                y_vals = df_merged.loc[valid_idx, current_col]
                
                # Create scatter plot
                ax.scatter(x_vals, y_vals, alpha=0.6, s=40, color='steelblue', edgecolors='black', linewidth=0.5)
                
                # Add y=x reference line
                if len(x_vals) > 0 and len(y_vals) > 0:
                    min_val = min(x_vals.min(), y_vals.min())
                    max_val = max(x_vals.max(), y_vals.max())
                    
                    if min_val > 0 and metric == 'sim_time_ms':
                        # Use log scale for simulation time
                        ax.set_xscale('log')
                        ax.set_yscale('log')
                    
                    # Draw y=x line
                    ax.plot([min_val, max_val], [min_val, max_val], 'r--', alpha=0.7, linewidth=2, label='y=x (no change)')
                
                # Formatting
                metric_title = metric.replace('_', ' ').replace('sim time ms', 'simulation time (ms)').title()
                ax.set_xlabel(f'{metric_title} (Backup)', fontsize=11)
                ax.set_ylabel(f'{metric_title} (Current)', fontsize=11)
                ax.set_title(f'{metric_title}', fontsize=12, fontweight='bold')
                ax.grid(True, alpha=0.3)
                ax.legend(fontsize=9)
                
                # Add text showing improvement/regression stats
                if len(x_vals) > 0:
                    improvements = (y_vals < x_vals).sum()
                    regressions = (y_vals > x_vals).sum()
                    ax.text(0.05, 0.95, f'Better: {improvements}\nWorse: {regressions}', 
                           transform=ax.transAxes, fontsize=9, verticalalignment='top',
                           bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))
            else:
                ax.text(0.5, 0.5, 'No valid data', transform=ax.transAxes, 
                       horizontalalignment='center', verticalalignment='center', fontsize=12)
                ax.set_title(f'{metric.replace("_", " ").title()}', fontsize=12)
        else:
            ax.text(0.5, 0.5, f'Data not available', transform=ax.transAxes, 
                   horizontalalignment='center', verticalalignment='center', fontsize=12)
            ax.set_title(f'{metric.replace("_", " ").title()}', fontsize=12)
    
    plt.tight_layout()
    plt.savefig('/home/jakeke/sst/scratch/src/sst-sat/src/performance_scatter_plots.png', 
                dpi=300, bbox_inches='tight')
    print("\nScatter plots saved to: performance_scatter_plots.png")
    plt.show()

def print_summary_stats(df_merged):
    """Print summary statistics for the comparison."""
    metrics = ['decisions', 'propagations', 'conflicts', 'learned', 'removed', 
               'db_reductions', 'minimized', 'restarts', 'sim_time_ms']
    
    print("\n" + "="*80)
    print("PERFORMANCE COMPARISON SUMMARY")
    print("="*80)
    print(f"{'Metric':<20} {'Improved':<10} {'Worse':<10} {'Same':<8} {'Avg Ratio':<12}")
    print("-"*80)
    
    for metric in metrics:
        current_col = f'{metric}_current'
        backup_col = f'{metric}_backup'
        
        if current_col in df_merged.columns and backup_col in df_merged.columns:
            valid_idx = (df_merged[current_col] >= 0) & (df_merged[backup_col] >= 0)
            
            if metric == 'sim_time_ms':
                valid_idx = valid_idx & (df_merged[current_col] > 0) & (df_merged[backup_col] > 0)
            
            if valid_idx.sum() > 0:
                current_vals = df_merged.loc[valid_idx, current_col]
                backup_vals = df_merged.loc[valid_idx, backup_col]
                
                improved = (current_vals < backup_vals).sum()
                worse = (current_vals > backup_vals).sum() 
                same = (current_vals == backup_vals).sum()
                
                # Calculate average ratio (current/backup)
                ratios = current_vals / backup_vals.replace(0, 1)  # Avoid division by zero
                avg_ratio = ratios.mean()
                
                metric_name = metric.replace('_', ' ').title()
                print(f"{metric_name:<20} {improved:<10} {worse:<10} {same:<8} {avg_ratio:<12.2f}")

def main():
    print("Starting visualization script...")
    print("Creating scatter plot comparisons...")
    assert len(sys.argv) == 3, "Usage: python visualize_comparisons.py <logs_dir> <logs_dir_2>"
    
    # Load and merge data
    logs_dir = sys.argv[1]
    backup_dir = sys.argv[2]
    
    print(f"Loading logs from: {logs_dir}")
    df_current = load_all_logs(logs_dir)
    print(f"Loading backup logs from: {backup_dir}")
    df_backup = load_all_logs(backup_dir)
    df_merged = pd.merge(df_current, df_backup, on='problem', suffixes=('_current', '_backup'))
    
    if not df_merged.empty:
        create_scatter_plots(logs_dir, backup_dir)
        print_summary_stats(df_merged)
    else:
        print("No data available for comparison!")

if __name__ == "__main__":
    main()
