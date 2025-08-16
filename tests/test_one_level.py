import sst
import os
import sys
import argparse
import lzma
import tempfile
import atexit

def parse_args():
    """Parse command line arguments"""
    parser = argparse.ArgumentParser(
        description='Run SAT solver simulation with SST',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    
    # File paths
    parser.add_argument('--cnf', dest='cnf_path', 
                        default=os.path.join(os.path.dirname(__file__), "test.cnf"),
                        help='Path to the CNF file')
    parser.add_argument('--ram2-cfg', dest='ram2_config',
                        help='Path to the ramulator2 configuration file')
    parser.add_argument('--decisions-in', dest='decision_path',
                        help='Path to input decision sequence file')
    parser.add_argument('--decisions-out', dest='decision_output_path',
                        help='Path to output decision sequence file')
    
    # Solver parameters
    parser.add_argument('--verbose', '-v', dest='verbose', type=int, default=1,
                        help='Verbosity level (0-7)')
    parser.add_argument('--sort-clauses', dest='sort_clauses', 
                        action='store_true', default=True,
                        help='Sort literals in clauses')
    parser.add_argument('--no-sort-clauses', dest='sort_clauses',
                        action='store_false',
                        help='Do not sort literals in clauses')
    parser.add_argument('--random-freq', dest='random_var_freq', 
                        type=float, default=0.0,
                        help='Frequency of random decisions (0.0-1.0)')
    parser.add_argument('--var-decay', dest='var_decay',
                        type=float, default=0.95,
                        help='Variable activity decay factor')
    parser.add_argument('--clause-decay', dest='clause_decay',
                        type=float, default=0.999,
                        help='Clause activity decay factor')
    parser.add_argument('--stats-file', dest='stats_file',
                        default="stats.csv",
                        help='Output file for statistics')
    parser.add_argument('--l1-size', dest='l1_size',
                        type=str, default="64KiB",
                        help='L1 cache size')
    parser.add_argument('--l1-latency', dest='l1_latency',
                        type=str, default="1",
                        help='L1 cache latency cycles (1GHz)')
    parser.add_argument('--mem-latency', dest='mem_latency',
                        type=str, default="100ns",
                        help='External Memory latency')
    parser.add_argument('--prefetch', dest='enable_prefetch', 
                        action='store_true', default=False,
                        help='Enable directed prefetching')
                        
    args = parser.parse_args()
    
    # Validate file existence
    if not os.path.exists(args.cnf_path):
        parser.error(f"CNF file not found: {args.cnf_path}")
    
    if args.decision_path and not os.path.exists(args.decision_path):
        parser.error(f"Decision file not found: {args.decision_path}")
    
    return args

def decompress_xz_file(xz_path):
    """
    Decompress .xz file to a temporary file and return the path.
    The temporary file will be automatically cleaned up on exit.
    """
    print(f"Decompressing .xz file: {xz_path}")
    
    # Create a temporary file
    temp_fd, temp_path = tempfile.mkstemp(suffix='.cnf', prefix='sst_cnf_')
    
    try:
        # Read and decompress the .xz file
        with lzma.open(xz_path, 'rt') as xz_file:
            with os.fdopen(temp_fd, 'w') as temp_file:
                # Copy content in chunks to handle large files efficiently
                while True:
                    chunk = xz_file.read(8192)  # 8KB chunks
                    if not chunk:
                        break
                    temp_file.write(chunk)
        
        print(f"Decompressed to temporary file: {temp_path}")
        
        # Register cleanup function to remove temp file on exit
        atexit.register(lambda: os.unlink(temp_path) if os.path.exists(temp_path) else None)
        
        return temp_path
        
    except Exception as e:
        # Clean up the temp file if decompression failed
        try:
            os.close(temp_fd)
        except:
            pass
        try:
            os.unlink(temp_path)
        except:
            pass
        raise Exception(f"Failed to decompress {xz_path}: {e}")

def get_cnf_path_and_size(original_path):
    """
    Get the actual CNF path and file size, handling .xz decompression if needed.
    Returns cnf_path.
    """
    if original_path.endswith('.xz'):
        # Decompress .xz file to temporary location
        cnf_path = decompress_xz_file(original_path)
        return cnf_path
    else:
        return original_path


# Parse command line arguments
args = parse_args()

# Handle .xz decompression if needed
actual_cnf_path = get_cnf_path_and_size(args.cnf_path)

print(f"Using CNF file: {args.cnf_path}")
if actual_cnf_path != args.cnf_path:
    print(f"Decompressed to: {actual_cnf_path}")
if args.decision_path:
    print(f"Using decision file: {args.decision_path}")
if args.decision_output_path:
    print(f"Will output decisions to: {args.decision_output_path}")
print(f"L1 cache size: {args.l1_size}")
print(f"L1 cache latency: {args.l1_latency} cycles")
if (args.ram2_config):
    print(f"Using ramulator2 config: {args.ram2_config}")
else:
    print(f"Using simple memory latency: {args.mem_latency}")
if args.enable_prefetch:
    print(f"Directed prefetching enabled")
print()

# Create the SAT solver component
solver = sst.Component("solver", "satsolver.SATSolver")

# Define memory addresses for global memory operations
heap_base_addr          = 0x00000000
indices_base_addr       = 0x10000000
variables_base_addr     = 0x20000000
watches_base_addr       = 0x30000000
watch_nodes_base_addr   = 0x40000000
clauses_cmd_base_addr   = 0x50000000
clauses_base_addr       = 0x60000000
var_act_base_addr       = 0x70000000
# clause_act_base_addr    = 0x80000000

# Get file size and pass it to solver
params = {
    "clock" : "1GHz",
    "verbose" : str(args.verbose),
    "sort_clauses": args.sort_clauses,
    "cnf_file" : actual_cnf_path,
    "heap_base_addr" : hex(heap_base_addr),
    "indices_base_addr" : hex(indices_base_addr),
    "variables_base_addr" : hex(variables_base_addr),
    "watches_base_addr" : hex(watches_base_addr),
    "watch_nodes_base_addr" : hex(watch_nodes_base_addr),
    "clauses_cmd_base_addr" : hex(clauses_cmd_base_addr),
    "clauses_base_addr" : hex(clauses_base_addr),
    "var_act_base_addr" : hex(var_act_base_addr),
    "random_var_freq": str(args.random_var_freq),
    "var_decay": str(args.var_decay),
    "clause_decay": str(args.clause_decay),
    "prefetch_enabled": str(args.enable_prefetch),
}
if args.decision_path:
    params["decision_file"] = args.decision_path
if args.decision_output_path:
    params["decision_output_file"] = args.decision_output_path
solver.addParams(params)


# Create the external heap subcomponent
heap = solver.setSubComponent("order_heap", "satsolver.Heap")
heap.addParams({
    "verbose" : str(args.verbose),
})

# Configure memory interface for global operations (heap and variables)
global_iface = solver.setSubComponent("global_memory", "memHierarchy.standardInterface")

# Create L1 cache for global operations
global_cache = sst.Component("global_l1cache", "memHierarchy.Cache")
global_cache.addParams({
    "cache_frequency"    : "1GHz",
    "cache_size"         : args.l1_size,
    "cache_line_size"    : "64",
    "associativity"      : "8",
    "access_latency_cycles" : args.l1_latency,
    "max_requests_per_cycle" : "-1",
    "request_link_width" : "64B",
    "response_link_width" : "64B",
    "L1"                 : "1",
    "replacement_policy" : "lru",
    "coherence_protocol" : "MSI",
    "prefetch_delay_cycles" : "0",
    "statistics" : "1",           # Enable statistics for cache
    "collect_stats" : "1"         # Make sure stats are collected
})

# prefetcher1 = global_cache.setSubComponent("prefetcher", "cassini.NextBlockPrefetcher", 1)
# prefetcher1 = global_cache.setSubComponent("prefetcher", "cassini.StridePrefetcher", 1)
# prefetcher1 = global_cache.setSubComponent("prefetcher", "cassini.PalaPrefetcher", 1)

# Add CacheProfiler to L1 cache
global_cache_profiler = global_cache.setSubComponent("prefetcher", "satsolver.CacheProfiler", 0)
global_cache_profiler.addParams({
    "cache_level": "L1",
    "heap_base_addr": hex(heap_base_addr),
    "variables_base_addr": hex(variables_base_addr),
    "watches_base_addr": hex(watches_base_addr),
    "clauses_cmd_base_addr": hex(clauses_cmd_base_addr),
    "var_act_base_addr": hex(var_act_base_addr),
    "verbose": str(args.verbose),
    "exclude_cold_misses": "1"
})

# Create memory controller for global operations
global_memctrl = sst.Component("global_memory", "memHierarchy.MemController")
global_memctrl.addParams({
    "clock" : "1GHz",
    "debug" : "0",
    "debug_level" : "10",
    "verbose" : "0",
    "addr_range_start" : "0",
    "addr_range_end" : "0xFFFFFFFF",
    "mem_size" : "4GiB",
})

# Create memory backend for global operations
if (args.ram2_config):
    global_memory = global_memctrl.setSubComponent("backend", "memHierarchy.ramulator2")
    global_memory.addParams({
        "mem_size" : "4GiB",
        "configFile" : args.ram2_config,
        "max_requests_per_cycle" : "1",
        "debug_level" : "10",
        "debug" : "0",
        "verbose" : "10",
    })
else:
    global_memory = global_memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
    global_memory.addParams({
        "access_time" : args.mem_latency,
        "mem_size" : "4GiB",
        "max_requests_per_cycle" : "-1",
        "request_width" : "64",
    })

# Connect solver to heap
solver_heap_link = sst.Link("solver_heap_link")
solver_heap_link.connect((solver, "heap_port", "1ns"), (heap, "response", "1ns"))

# Connect solver to L1 cache
cpu_to_cache_link = sst.Link("cpu_to_cache_link")
cpu_to_cache_link.connect((global_iface, "lowlink", "1ns"), (global_cache, "highlink", "1ns"))

# Connect L1 cache to mem
l1_to_mem_link = sst.Link("l1_to_mem_link")
l1_to_mem_link.connect((global_cache, "lowlink", "1ns"), (global_memctrl, "highlink", "1ns"))

# Create the directed prefetcher if enabled
if args.enable_prefetch:
    prefetcher = global_cache.setSubComponent("prefetcher", "satsolver.DirectedPrefetcher", 1)
    prefetcher.addParams({"cache_line_size": "64"})
    
    # Connect prefetcher to solver
    prefetch_link = sst.Link("prefetch_link")
    prefetch_link.connect((solver, "prefetch_port", "1ns"), (prefetcher, "cmd_port", "1ns"))

    sst.enableAllStatisticsForComponentType("satsolver.DirectedPrefetcher")

# Enable statistics - different types for different stats
sst.setStatisticLoadLevel(7)

# Enable statistics for all metrics
sst.enableStatisticsForComponentName("solver", [
    "decisions",
    "propagations", 
    "assigns",
    "unassigns",
    "conflicts",
    "learned",
    "removed",
    "db_reductions",
    "minimized_literals",
    "restarts"
], {
    "type": "sst.AccumulatorStatistic",
    "rate": "1s"
})

# Enable histogram statistic for watchers occupancy
sst.enableStatisticsForComponentName("solver", [
    "watcher_occ"
], {
    "type": "sst.HistogramStatistic",
    "minvalue": "0",
    "binwidth": "1", 
    "numbins": "20",
    "dumpbinsonoutput": "1",
    "includeoutofbounds": "1",
    "rate": "1s"
})

# Enable histogram statistic for watchers inspected
sst.enableStatisticsForComponentName("solver", [
    "para_watchers"
], {
    "type": "sst.HistogramStatistic",
    "minvalue": "0",
    "binwidth": "1", 
    "numbins": "20",
    "dumpbinsonoutput": "1",
    "includeoutofbounds": "1",
    "rate": "1s"
})

# Enable histogram statistic for parallel variables
sst.enableStatisticsForComponentName("solver", [
    "para_vars"
], {
    "type": "sst.HistogramStatistic",
    "minvalue": "0",
    "binwidth": "1", 
    "numbins": "20",
    "dumpbinsonoutput": "1",
    "includeoutofbounds": "1",
    "rate": "1s"
})

# Enable cache statistics for the L1 cache
sst.enableStatisticsForComponentType("memHierarchy.Cache", [
    "CacheHits", 
    "CacheMisses",
    "Prefetch_requests",
    "Prefetch_drops",
], {
    "type": "sst.AccumulatorStatistic",
    "rate": "1s"
})

sst.enableStatisticsForComponentType("satsolver.CacheProfiler", [
    "heap_hits",
    "heap_misses",
    "variables_hits",
    "variables_misses",
    "watches_hits",
    "watches_misses",
    "clauses_hits",
    "clauses_misses",
    "var_activity_hits",
    "var_activity_misses",
    "cla_activity_hits",
    "cla_activity_misses",
], {
    "type": "sst.AccumulatorStatistic",
    "rate": "1s"
})

# Set statistics output to CSV file
sst.setStatisticOutput("sst.statOutputCSV", 
    {"filepath": args.stats_file, "separator": ","})
