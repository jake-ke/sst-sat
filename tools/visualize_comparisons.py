#!/usr/bin/env python3
"""
Visualize SAT solver performance comparisons with scatter plots.
Shows current vs backup performance for key metrics.
"""

import os
import re
import sys
import argparse
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path
from mpl_toolkits.axes_grid1.inset_locator import inset_axes, mark_inset
from unified_parser import parse_log_file as parse_satsolver_log


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


def parse_minisat_log(filepath):
    """Parse minisat format log file."""
    stats = {}
    try:
        with open(filepath, 'r') as f:
            content = f.read()

        # Extract problem name from filename: <problem_base>_YYYYMMDD_HHMMSS.log
        basename = os.path.basename(filepath)
        base_no_log = re.sub(r'\.log$', '', basename)
        problem_name = re.sub(r'_\d{8}_\d{6}$', '', base_no_log)
        stats['problem'] = problem_name

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

        # Extract CPU time (seconds) and convert to milliseconds as sim_time_ms for consistency
        cpu_match = re.search(r'CPU time\s*:\s*([\d.]+)\s*s', content, re.IGNORECASE)
        if cpu_match:
            try:
                cpu_seconds = float(cpu_match.group(1))
                stats['sim_time_ms'] = cpu_seconds * 1000.0
            except ValueError:
                pass
        else:
            # Fallback: if a raw number appears without unit, still attempt parse
            cpu_match_alt = re.search(r'CPU time\s*:\s*([\d.]+)', content, re.IGNORECASE)
            if cpu_match_alt:
                try:
                    cpu_seconds = float(cpu_match_alt.group(1))
                    stats['sim_time_ms'] = cpu_seconds * 1000.0
                except ValueError:
                    pass
        # Ensure sim_time_ms exists for downstream code
        if 'sim_time_ms' not in stats:
            stats['sim_time_ms'] = 0.0
    except Exception as e:
        print(f"Error parsing minisat log {filepath}: {e}")
        return None
    return stats


def parse_log_file(filepath):
    """Parse a single log file and extract statistics."""
    try:
        with open(filepath, 'r') as f:
            content = f.read()
        
        log_format = detect_log_format(content)
        
        if log_format == 'satsolver':
            # Use unified parser for satsolver logs
            result = parse_satsolver_log(filepath)
            if result:
                # Convert to expected format
                stats = {
                    'problem': result['test_case'],
                    'decisions': result.get('decisions', 0),
                    'propagations': result.get('propagations', 0),
                    'conflicts': result.get('conflicts', 0),
                    'learned': result.get('learned', 0),
                    'removed': result.get('removed', 0),
                    'db_reductions': result.get('db_reductions', 0),
                    'minimized': result.get('minimized', 0),
                    'restarts': result.get('restarts', 0),
                    'sim_time_ms': result.get('sim_time_ms', 0)
                }

                # Include L1 miss rate metrics (total and per-component)
                for k, v in result.items():
                    if isinstance(k, str) and k.startswith('l1_') and k.endswith('_miss_rate'):
                        stats[k] = v

                # Include propagation detail statistics (pct and cycles)
                for k, v in result.items():
                    if isinstance(k, str) and k.startswith('prop_') and (k.endswith('_pct') or k.endswith('_cycles')):
                        stats[k] = v
                return stats
            return None
                
        elif log_format == 'minisat':
            return parse_minisat_log(filepath)
            
        else:
            # Unknown format - try to extract filename from filepath
            basename = os.path.basename(filepath)
            problem_name = re.sub(r'_(sat|unsat)_\d+.*\.log$', '', basename)
            return {'problem': problem_name}
            
    except Exception as e:
        print(f"Error parsing {filepath}: {e}")
        return None


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

def _plot_metrics_figure(df_merged, metrics, title, output_path):
    """Reusable plotting helper for a list of metrics into a single figure."""
    if not metrics:
        return
    n = len(metrics)
    cols = 3
    rows = int(np.ceil(n / cols))
    fig, axes = plt.subplots(rows, cols, figsize=(6 * cols, 5 * rows))
    # Normalize axes to 2D array
    if rows == 1 and cols == 1:
        axes = np.array([[axes]])
    elif rows == 1:
        axes = np.array([axes])
    fig.suptitle(title, fontsize=16, fontweight='bold')
    
    for i, metric in enumerate(metrics):
        row = i // cols
        col = i % cols
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

    # Hide any unused subplots
    total_axes = rows * cols
    if total_axes > n:
        # Flatten axes for easy hiding
        flat_axes = axes.flatten() if isinstance(axes, np.ndarray) else [axes]
        for j in range(n, total_axes):
            flat_axes[j].axis('off')
    
    plt.tight_layout(rect=[0, 0, 1, 0.97])
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Saved plot: {output_path}")
    plt.close(fig)


def create_scatter_plots(df_merged, output_dir='.'):
    """Create scatter plots comparing current vs backup performance. Generates separate figures for baseline, L1 miss rates, and propagation details."""
    print(f"Creating plots for {len(df_merged)} matching problems")
    if df_merged.empty:
        print("No matching problems found!")
        return

    # Baseline metrics
    baseline = ['decisions', 'propagations', 'conflicts', 'learned', 'removed',
                'db_reductions', 'minimized', 'restarts', 'sim_time_ms']
    # Discover additional metrics present in both current and backup
    candidate_metrics = set()
    for col in df_merged.columns:
        if col.endswith('_current'):
            base = col[:-8]
            if f'{base}_backup' in df_merged.columns:
                candidate_metrics.add(base)
    l1_metrics = sorted([m for m in candidate_metrics if (m.startswith('l1_') and m.endswith('_miss_rate'))])
    prop_metrics = sorted([m for m in candidate_metrics if m.startswith('prop_')])

    os.makedirs(output_dir, exist_ok=True)
    # Plot and save each figure
    _plot_metrics_figure(df_merged, baseline, 'SAT Solver Performance Comparison: Current vs Backup', os.path.join(output_dir, 'performance_scatter_plots.png'))
    if l1_metrics:
        _plot_metrics_figure(df_merged, l1_metrics, 'L1 Miss Rates: Current vs Backup', os.path.join(output_dir, 'l1_miss_rates_scatter_plots.png'))
    if prop_metrics:
        _plot_metrics_figure(df_merged, prop_metrics, 'Propagation Detail: Current vs Backup', os.path.join(output_dir, 'propagation_detail_scatter_plots.png'))

def save_comparison_to_csv(df_merged, output_file='comparison_results.csv'):
    """Save the comparison results to a CSV file."""
    if df_merged.empty:
        print("No data to save to CSV")
        return
    
    # Create a summary dataframe for CSV output
    baseline = ['sim_time_ms', 'decisions', 'propagations', 'conflicts', 'learned', 'removed', 
                'db_reductions', 'minimized', 'restarts']
    candidate_metrics = set()
    for col in df_merged.columns:
        if col.endswith('_current'):
            base = col[:-8]
            if f'{base}_backup' in df_merged.columns:
                candidate_metrics.add(base)
    extra = [m for m in candidate_metrics if (m.startswith('l1_') and m.endswith('_miss_rate')) or m.startswith('prop_')]
    metrics = baseline + sorted(extra)
    
    # Start with problem names
    csv_data = df_merged[['problem']].copy()
    
    # Add current and backup values for each metric
    for metric in metrics:
        current_col = f'{metric}_current'
        backup_col = f'{metric}_backup'
        
        if current_col in df_merged.columns and backup_col in df_merged.columns:
            csv_data[f'{metric}_current'] = df_merged[current_col]
            csv_data[f'{metric}_backup'] = df_merged[backup_col]
            
            if metric == 'sim_time_ms':
                # Speedup = backup/current (speedup > 1 => current is faster)
                cur = df_merged[current_col].astype(float)
                bak = df_merged[backup_col].astype(float)
                with np.errstate(divide='ignore', invalid='ignore'):
                    speedup = bak / cur
                    # Only keep valid positive ratios when both are > 0
                    invalid = (~np.isfinite(speedup)) | (cur <= 0) | (bak <= 0)
                    speedup[invalid] = np.nan
                csv_data['sim_time_ms_speedup'] = speedup
            elif metric.endswith('_miss_rate') or metric.endswith('_pct'):
                # For percentages, keep using difference
                csv_data[f'{metric}_diff'] = df_merged[current_col] - df_merged[backup_col]
            else:
                # For other metrics, use ratio (current/backup)
                cur = df_merged[current_col].astype(float)
                bak = df_merged[backup_col].astype(float)
                with np.errstate(divide='ignore', invalid='ignore'):
                    ratio = cur / bak
                    # Only keep valid positive ratios when both values are valid
                    invalid = (~np.isfinite(ratio)) | (bak == 0)
                    ratio[invalid] = np.nan
                csv_data[f'{metric}_ratio'] = ratio
    
    # Keep test cases with empty results at the bottom (based on sim_time_ms_speedup availability)
    if 'sim_time_ms_speedup' in csv_data.columns:
        csv_data['_missing'] = csv_data['sim_time_ms_speedup'].isna()
        csv_data = csv_data.sort_values(by=['_missing', 'problem'], ascending=[True, True]).drop(columns=['_missing'])
    else:
        csv_data = csv_data.sort_values(by=['problem'])

    # Save to CSV
    csv_data.to_csv(output_file, index=False)
    print(f"\nComparison data saved to: {output_file}")
    print(f"Saved {len(csv_data)} problem comparisons with detailed metrics")

def print_summary_stats(df_merged):
    """Print summary statistics for the comparison."""
    baseline = ['decisions', 'propagations', 'conflicts', 'learned', 'removed', 
                'db_reductions', 'minimized', 'restarts', 'sim_time_ms']
    candidate_metrics = set()
    for col in df_merged.columns:
        if col.endswith('_current'):
            base = col[:-8]
            if f'{base}_backup' in df_merged.columns:
                candidate_metrics.add(base)
    extra = [m for m in candidate_metrics if (m.startswith('l1_') and m.endswith('_miss_rate')) or m.startswith('prop_')]
    metrics = baseline + sorted(extra)
    
    # Compute dynamic width for the metric column to reduce misalignment
    metric_titles = [m.replace('_', ' ').title() for m in metrics]
    metric_col_width = max(28, min(48, max(len(t) for t in metric_titles)))
    print("\n" + "=" * (metric_col_width + 44))
    print("PERFORMANCE COMPARISON SUMMARY")
    print("=" * (metric_col_width + 44))
    header = f"{'Metric':<{metric_col_width}} {'Improved':>9} {'Worse':>9} {'Same':>7} {'Avg Measure':>14}"
    print(header)
    print("-" * (metric_col_width + 44))
    
    for metric in metrics:
        current_col = f'{metric}_current'
        backup_col = f'{metric}_backup'
        
        if current_col in df_merged.columns and backup_col in df_merged.columns:
            valid_idx = (df_merged[current_col] >= 0) & (df_merged[backup_col] >= 0)
            
            if metric == 'sim_time_ms':
                valid_idx = valid_idx & (df_merged[current_col] > 0) & (df_merged[backup_col] > 0)
            
            if valid_idx.sum() > 0:
                current_vals = df_merged.loc[valid_idx, current_col].astype(float)
                backup_vals = df_merged.loc[valid_idx, backup_col].astype(float)
                
                improved = (current_vals < backup_vals).sum()
                worse = (current_vals > backup_vals).sum() 
                same = (current_vals == backup_vals).sum()

                if metric == 'sim_time_ms':
                    # GeoMean speedup = exp(mean(log(backup/current)))
                    with np.errstate(divide='ignore', invalid='ignore'):
                        ratios = backup_vals / current_vals
                        ratios = ratios[(ratios > 0) & np.isfinite(ratios)]
                        if len(ratios) > 0:
                            geo_speedup = float(np.exp(np.log(ratios).mean()))
                        else:
                            geo_speedup = float('nan')
                    metric_name = metric.replace('_', ' ').title()
                    print(f"{metric_name:<{metric_col_width}} {improved:>9} {worse:>9} {same:>7} {geo_speedup:>14.3f}")
                elif metric.endswith('_miss_rate') or metric.endswith('_pct'):
                    # For percentages, use arithmetic mean of differences
                    diffs = current_vals - backup_vals
                    avg_diff = diffs.mean()
                    metric_name = metric.replace('_', ' ').title()
                    print(f"{metric_name:<{metric_col_width}} {improved:>9} {worse:>9} {same:>7} {avg_diff:>14.2f}")
                else:
                    # For other metrics, use geometric mean of ratios (current/backup)
                    with np.errstate(divide='ignore', invalid='ignore'):
                        ratios = current_vals / backup_vals
                        ratios = ratios[(ratios > 0) & np.isfinite(ratios)]
                        if len(ratios) > 0:
                            geo_ratio = float(np.exp(np.log(ratios).mean()))
                        else:
                            geo_ratio = float('nan')
                    metric_name = metric.replace('_', ' ').title()
                    print(f"{metric_name:<{metric_col_width}} {improved:>9} {worse:>9} {same:>7} {geo_ratio:>14.3f}")

def main():
    print("Starting visualization script...")
    print("Creating scatter plot comparisons...")
    parser = argparse.ArgumentParser(description='Visualize SAT solver performance comparisons')
    parser.add_argument('logs_dir', help='Directory containing current logs')
    parser.add_argument('backup_dir', help='Directory containing backup logs')
    parser.add_argument('--output-dir', default='.', help='Directory to save figures and CSV')
    args = parser.parse_args()
    
    # Load and merge data
    logs_dir = args.logs_dir
    backup_dir = args.backup_dir
    output_dir = args.output_dir
    
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
    
    # Normalize problem names so current satsolver logs (which may have .xz in name)
    # match minisat logs (which add a timestamp suffix).
    def _normalize(name: str) -> str:
        if not isinstance(name, str):
            return name
        name = name.strip()
        # Remove .log if still present
        name = re.sub(r'\.log$', '', name)
        # Remove timestamp pattern _YYYYMMDD_HHMMSS if at end or before an extension
        name = re.sub(r'_\d{8}_\d{6}(?=$|\.[A-Za-z0-9]+$)', '', name)
        # Remove compression extensions (xz, gz, bz2)
        name = re.sub(r'\.(?:xz|gz|bz2)$', '', name)
        # Collapse any accidental double extensions like .cnf.cnf
        name = re.sub(r'(\.cnf)+$', '.cnf', name)
        return name

    df_current['problem_norm'] = df_current['problem'].apply(_normalize)
    df_backup['problem_norm'] = df_backup['problem'].apply(_normalize)

    # Identify problems that won't be compared
    current_set = set(df_current['problem_norm'])
    backup_set = set(df_backup['problem_norm'])
    only_current = sorted(current_set - backup_set)
    only_backup = sorted(backup_set - current_set)
    # Attempt secondary reconciliation: if difference is only due to trailing .cnf vs no extension, adjust
    if only_current or only_backup:
        # Build maps by base (strip trailing .cnf for comparison purposes)
        def base_no_cnf(n):
            return re.sub(r'\.cnf$', '', n)
        unmatched_pairs = []
        backup_bases = {base_no_cnf(b): b for b in only_backup}
        for c in list(only_current):
            base = base_no_cnf(c)
            if base in backup_bases:
                # They actually match after base normalization; remove from diff lists
                only_backup.remove(backup_bases[base])
                only_current.remove(c)
                unmatched_pairs.append((c, backup_bases[base]))
        if unmatched_pairs:
            print(f"Resolved {len(unmatched_pairs)} previously unmatched problems by base name normalization.")
        if only_current:
            print(f"Problems only in CURRENT ({len(only_current)}):")
            for p in only_current:
                print(f"  {p}")
        if only_backup:
            print(f"Problems only in BACKUP ({len(only_backup)}):")
            for p in only_backup:
                print(f"  {p}")
        # Diagnostic: show any names still containing timestamp pattern (should be none)
        residual = [n for n in list(current_set | backup_set) if re.search(r'_\d{8}_\d{6}$', n)]
        if residual:
            print(f"Warning: {len(residual)} names still contain timestamp suffix after normalization:")
            for n in residual:
                print(f"  {n}")
        if not only_current and not only_backup:
            print("All problems overlap between current and backup after normalization.")
    else:
        print("All problems overlap between current and backup.")

    df_merged = pd.merge(df_current, df_backup, on='problem_norm', suffixes=('_current', '_backup'))
    # Provide a canonical problem column (normalized)
    df_merged['problem'] = df_merged['problem_norm']
    print(f"Found {len(df_merged)} matching problems")
    
    if not df_merged.empty:
        create_scatter_plots(df_merged, output_dir=output_dir)
        print_summary_stats(df_merged)
        # Save CSV into output directory
        os.makedirs(output_dir, exist_ok=True)
        save_comparison_to_csv(df_merged, os.path.join(output_dir, 'comparison_results.csv'))
    else:
        print("No data available for comparison!")

if __name__ == "__main__":
    main()
