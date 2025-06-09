import sst
import os
import sys
import argparse

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
                        
    args = parser.parse_args()
    
    # Validate file existence
    if not os.path.exists(args.cnf_path):
        parser.error(f"CNF file not found: {args.cnf_path}")
    
    if args.decision_path and not os.path.exists(args.decision_path):
        parser.error(f"Decision file not found: {args.decision_path}")
    
    return args

# Parse command line arguments
args = parse_args()

print(f"Using CNF file: {args.cnf_path}")
if args.decision_path:
    print(f"Using decision file: {args.decision_path}")
if args.decision_output_path:
    print(f"Will output decisions to: {args.decision_output_path}")

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
clause_act_base_addr    = 0x80000000

# Get file size and pass it to solver
file_size = os.path.getsize(args.cnf_path)
params = {
    "clock" : "1GHz",
    "verbose" : str(args.verbose),
    "sort_clauses": args.sort_clauses,
    "filesize" : str(file_size),
    "cnf_file" : args.cnf_path,
    "heap_base_addr" : hex(heap_base_addr),
    "indices_base_addr" : hex(indices_base_addr),
    "variables_base_addr" : hex(variables_base_addr),
    "watches_base_addr" : hex(watches_base_addr),
    "watch_nodes_base_addr" : hex(watch_nodes_base_addr),
    "clauses_cmd_base_addr" : hex(clauses_cmd_base_addr),
    "clauses_base_addr" : hex(clauses_base_addr),
    "var_act_base_addr" : hex(var_act_base_addr),
    "clause_act_base_addr" : hex(clause_act_base_addr),
    "random_var_freq": str(args.random_var_freq),
    "var_decay": str(args.var_decay),
    "clause_decay": str(args.clause_decay)
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

# Configure memory interface for CNF data
cnf_iface = solver.setSubComponent("cnf_memory", "memHierarchy.standardInterface")

# Create memory controller for CNF data
cnf_memctrl = sst.Component("cnf_memory", "memHierarchy.MemController")
cnf_memctrl.addParams({
    "clock" : "1GHz",
    "backing" : "mmap",
    "backing_size_unit" : "1B",
    "memory_file" : args.cnf_path,
    "debug" : "0",
    "debug_level" : "10",
    "verbose" : "0",
    "addr_range_start" : "0",
    "addr_range_end" : "0x1FFFFFFF",
    "mem_size" : "512MiB",
    "initBacking" : "1",  # enable backing store initialization
})

# Create memory backend for CNF data
cnf_memory = cnf_memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
cnf_memory.addParams({
    "access_time" : "10ns",
    "mem_size" : "512MiB",
})

# Configure memory interface for global operations (heap and variables)
global_iface = solver.setSubComponent("global_memory", "memHierarchy.standardInterface")

# Create L1 cache for global operations
global_cache = sst.Component("global_l1cache", "memHierarchy.Cache")
global_cache.addParams({
    "cache_frequency"    : "1GHz",
    "cache_size"         : "32KiB",
    "cache_line_size"    : "64",
    "associativity"      : "8",
    "access_latency_cycles" : "1",
    "max_requests_per_cycle" : "-1",
    "L1"                 : "1",
    "replacement_policy" : "lru",
    "coherence_protocol" : "MSI",
    "verbose"            : "0",
    "debug" : "0",
    "debug_level" : "10",
    "statistics" : "1",           # Enable statistics for cache
    "collect_stats" : "1"         # Make sure stats are collected
})

# Add CacheProfiler to L1 cache
global_cache_profiler = global_cache.setSubComponent("prefetcher", "satsolver.CacheProfiler")
global_cache_profiler.addParams({
    "cache_level": "L1",
    "heap_base_addr": hex(heap_base_addr),
    "variables_base_addr": hex(variables_base_addr),
    "watches_base_addr": hex(watches_base_addr),
    "clauses_cmd_base_addr": hex(clauses_cmd_base_addr),
    "var_act_base_addr": hex(var_act_base_addr),
    "clause_act_base_addr": hex(clause_act_base_addr),
    "verbose": str(args.verbose)
})

# Create L2 cache
global_l2cache = sst.Component("global_l2cache", "memHierarchy.Cache")
global_l2cache.addParams({
    "cache_frequency"    : "1GHz",
    "cache_size"         : "2MiB",
    "cache_line_size"    : "64",
    "associativity"      : "16",
    "access_latency_cycles" : "5",
    "L1"                 : "0",
    "replacement_policy" : "lru",
    "coherence_protocol" : "MSI",
    "verbose"            : "0",
    "debug" : "0",
    "debug_level" : "10",
    "statistics" : "1",
    "collect_stats" : "1"
})

# Add CacheProfiler to L2 cache
global_l2cache_profiler = global_l2cache.setSubComponent("prefetcher", "satsolver.CacheProfiler")
global_l2cache_profiler.addParams({
    "cache_level": "L2",
    "heap_base_addr": hex(heap_base_addr),
    "variables_base_addr": hex(variables_base_addr),
    "watches_base_addr": hex(watches_base_addr),
    "clauses_cmd_base_addr": hex(clauses_cmd_base_addr),
    "var_act_base_addr": hex(var_act_base_addr),
    "clause_act_base_addr": hex(clause_act_base_addr),
    "verbose": str(args.verbose)
})

# Create L3 cache
global_l3cache = sst.Component("global_l3cache", "memHierarchy.Cache")
global_l3cache.addParams({
    "cache_frequency"    : "1GHz",
    "cache_size"         : "36MiB",
    "cache_line_size"    : "64",
    "associativity"      : "12",
    "access_latency_cycles" : "10",
    "L1"                 : "0",
    "replacement_policy" : "lru",
    "coherence_protocol" : "MSI",
    "verbose"            : "0",
    "debug" : "0",
    "debug_level" : "10",
    "statistics" : "1",
    "collect_stats" : "1"
})

# Add CacheProfiler to L3 cache
global_l3cache_profiler = global_l3cache.setSubComponent("prefetcher", "satsolver.CacheProfiler")
global_l3cache_profiler.addParams({
    "cache_level": "L3",
    "heap_base_addr": hex(heap_base_addr),
    "variables_base_addr": hex(variables_base_addr),
    "watches_base_addr": hex(watches_base_addr),
    "clauses_cmd_base_addr": hex(clauses_cmd_base_addr),
    "var_act_base_addr": hex(var_act_base_addr),
    "clause_act_base_addr": hex(clause_act_base_addr),
    "verbose": str(args.verbose)
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
global_memory = global_memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
global_memory.addParams({
    "access_time" : "10ns",
    "mem_size" : "4GiB",
})

# Connect solver to heap
solver_heap_link = sst.Link("solver_heap_link")
solver_heap_link.connect((solver, "heap_port", "1ns"), (heap, "response", "1ns"))

# Connect solver to CNF memory controller
cnf_mem_link = sst.Link("cnf_mem_link")
cnf_mem_link.connect((cnf_iface, "lowlink", "1ns"), (cnf_memctrl, "highlink", "1ns"))

# Connect solver to L1 cache
cpu_to_cache_link = sst.Link("cpu_to_cache_link")
cpu_to_cache_link.connect((global_iface, "lowlink", "1ns"), (global_cache, "highlink", "1ns"))

# Connect L1 cache to L2 cache
l1_to_l2_link = sst.Link("l1_to_l2_link")
l1_to_l2_link.connect((global_cache, "lowlink", "50ps"), (global_l2cache, "highlink", "50ps"))

# Connect L2 cache to L3 cache
l2_to_l3_link = sst.Link("l2_to_l3_link")
l2_to_l3_link.connect((global_l2cache, "lowlink", "50ps"), (global_l3cache, "highlink", "50ps"))

# Connect L3 cache to global memory controller (replacing the previous direct L1-to-memory connection)
l3_to_mem_link = sst.Link("l3_to_mem_link")
l3_to_mem_link.connect((global_l3cache, "lowlink", "50ps"), (global_memctrl, "highlink", "50ps"))

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
    "rate": "1ms"
})

# Enable cache statistics for the L1 cache
sst.enableStatisticsForComponentType("memHierarchy.Cache", [
    "CacheHits", 
    "CacheMisses",
], {
    "type": "sst.AccumulatorStatistic",
    "rate": "1ms"
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
    "rate": "1ms"
})

# Set statistics output to CSV file
sst.setStatisticOutput("sst.statOutputCSV", 
    {"filepath": args.stats_file, "separator": ","})
