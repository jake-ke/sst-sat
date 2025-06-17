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
from mpl_toolkits.axes_grid1.inset_locator import inset_axes, mark_inset

def detect_log_format(content):
    """Detect whether this is a minisat or satsolver log."""
    # Check for satsolver format indicators
    if 'Using CNF file:' in content and 'Simulation is complete' in content:
        return 'satsolver'
    # Check for minisat format indicators
    elif 'Problem Statistics' in content and 'conflicts             :' in content:
        return 'minisat'
    else:
        return 'unknown'

def parse_log_file(filepath):
    """Parse a single log file and extract statistics."""
    stats = {}
    try:
        with open(filepath, 'r') as f:
            content = f.read()
        
        log_format = detect_log_format(content)
        
        if log_format == 'satsolver':
            # Extract filename for satsolver logs
            filename_match = re.search(r'Using CNF file: .*/([^/\n]+)', content)
            if filename_match:
                stats['problem'] = filename_match.group(1)
            else:
                return None
            
            # Extract statistics using regex for satsolver format
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
                
        elif log_format == 'minisat':
            # Extract problem name from filename for minisat logs
            basename = os.path.basename(filepath)
            # Remove timestamp pattern and .log extension
            problem_name = re.sub(r'_(sat|unsat)_\d+.*\.log$', '', basename)
            stats['problem'] = problem_name
            
            # Extract statistics using regex for minisat format
            minisat_patterns = {
                'decisions': r'decisions\s*:\s*(\d+)',
                'propagations': r'propagations\s*:\s*(\d+)',
                'conflicts': r'conflicts\s*:\s*(\d+)',
                'learned': r'learned\s*:\s*(\d+)',
                'removed': r'removed\s*:\s*(\d+)',
                'db_reductions': r'db_reductions\s*:\s*(\d+)',
                'minimized': r'minimized\s*:\s*(\d+)',
                'restarts': r'restarts\s*:\s*(\d+)',
            }
            
            for stat, pattern in minisat_patterns.items():
                match = re.search(pattern, content, re.IGNORECASE)
                if match:
                    stats[stat] = int(match.group(1))
            
            # Minisat doesn't have simulation time, but we can estimate from CPU time if available
            # For now, we'll leave it as None to avoid comparison issues
            
        else:
            # Unknown format - try to extract filename from filepath
            basename = os.path.basename(filepath)
            problem_name = re.sub(r'_(sat|unsat)_\d+.*\.log$', '', basename)
            stats['problem'] = problem_name
            return None
            
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

def create_scatter_plots(df_merged):
    """Create scatter plots comparing current vs backup performance."""
    
    print(f"Creating plots for {len(df_merged)} matching problems")
    
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
                
                # Add zoom inset for lower region (skip for log scale)
                if not (min_val > 0 and metric == 'sim_time_ms'):
                    # Calculate tight zoom region based on 0th-5th percentile
                    x_5th = np.percentile(x_vals, 5)
                    y_5th = np.percentile(y_vals, 5)
                    
                    # Set very tight zoom limits - bottom 5% of data with small fallback
                    zoom_max_x = max(x_5th, min_val + (max_val - min_val) * 0.03)
                    zoom_max_y = max(y_5th, min_val + (max_val - min_val) * 0.03)
                    
                    # Only create inset if there are enough points in the zoom region
                    zoom_mask = (x_vals <= zoom_max_x) & (y_vals <= zoom_max_y)
                    if zoom_mask.sum() > 2:  # Need at least 3 points
                        # Create 35% size inset box in lower right corner
                        axins = inset_axes(ax, width="45%", height="45%", loc='lower right')
                        
                        # Plot data in inset with transparency
                        axins.scatter(x_vals, y_vals, alpha=0.8, s=16, color='steelblue', 
                                    edgecolors='black', linewidth=0.3)
                        
                        # Set zoom limits
                        axins.set_xlim(min_val, zoom_max_x)
                        axins.set_ylim(min_val, zoom_max_y)
                        
                        # Add y=x line to inset
                        axins.plot([min_val, zoom_max_x], [min_val, zoom_max_y], 'r--', 
                                 alpha=0.9, linewidth=1.5)
                        
                        # Style inset with transparency
                        axins.patch.set_alpha(0.95)  # More opaque background
                        axins.grid(True, alpha=0.3)
                        axins.tick_params(labelsize=7)
                        
                        # Add subtle indication lines from main plot to inset
                        mark_inset(ax, axins, loc1=2, loc2=4, fc="none", ec="gray", 
                                 alpha=0.3, linestyle=':', linewidth=0.8)

                # Add text showing improvement/regression stats
                if len(x_vals) > 0:
                    improvements = (y_vals < x_vals).sum()
                    regressions = (y_vals > x_vals).sum()
                    ax.text(0.05, 0.85, f'Better: {improvements}\nWorse: {regressions}', 
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
    print(f"Loaded {len(df_current)} current log files")
    print(f"Current dataframe columns: {list(df_current.columns) if not df_current.empty else 'Empty dataframe'}")
    
    print(f"Loading backup logs from: {backup_dir}")
    df_backup = load_all_logs(backup_dir)
    print(f"Loaded {len(df_backup)} backup log files")
    print(f"Backup dataframe columns: {list(df_backup.columns) if not df_backup.empty else 'Empty dataframe'}")
    
    # Check if both dataframes have the 'problem' column before merging
    if df_current.empty:
        print("Error: No current logs loaded!")
        return
    if df_backup.empty:
        print("Error: No backup logs loaded!")
        return
    if 'problem' not in df_current.columns:
        print("Error: 'problem' column missing from current logs!")
        return
    if 'problem' not in df_backup.columns:
        print("Error: 'problem' column missing from backup logs!")
        return
    
    df_merged = pd.merge(df_current, df_backup, on='problem', suffixes=('_current', '_backup'))
    print(f"Found {len(df_merged)} matching problems")
    
    if not df_merged.empty:
        create_scatter_plots(df_merged)
        print_summary_stats(df_merged)
    else:
        print("No data available for comparison!")

if __name__ == "__main__":
    main()
