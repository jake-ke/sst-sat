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

# Read CNF file content
with open(args.cnf_path, 'r') as f:
    cnf_content = f.read()

# Create the SAT solver component
solver = sst.Component("solver", "satsolver.SATSolver")

# Define memory addresses for heap operations
heap_base_addr = 0x00000000
indices_base_addr = 0x10000000

# Get file size and pass it to solver
file_size = os.path.getsize(args.cnf_path)
params = {
    "clock" : "1GHz",
    "verbose" : str(args.verbose),
    "sort_clauses": args.sort_clauses,
    "filesize" : str(file_size),
    "heap_base_addr" : hex(heap_base_addr),
    "indices_base_addr" : hex(indices_base_addr),
    "random_var_freq": str(args.random_var_freq),
    "var_decay": str(args.var_decay),
    "clause_decay": str(args.clause_decay)
}
if args.decision_path:
    params["decision_file"] = args.decision_path
if args.decision_output_path:
    params["decision_output_file"] = args.decision_output_path
solver.addParams(params)

# Configure memory interface for CNF data
iface = solver.setSubComponent("memory", "memHierarchy.standardInterface")

# Create memory controller for CNF data
memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock" : "1GHz",
    "backing" : "mmap",
    "backing_size_unit" : "1B",
    "memory_file" : args.cnf_path,
    "debug" : "1",
    "debug_level" : "10",
    "verbose" : "0",
    "addr_range_start" : "0",
    "addr_range_end" : "0x1FFFFFFF",
    "mem_size" : "512MiB",
    "initBacking" : "1",  # enable backing store initialization
})

# Create memory backend for CNF data
memory = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
memory.addParams({
    "access_time" : "100ns",
    "mem_size" : "512MiB",
})

# Configure memory interface for heap operations
heap_iface = solver.setSubComponent("heap_memory", "memHierarchy.standardInterface")

# Create memory controller for heap operations
heap_memctrl = sst.Component("heap_memory", "memHierarchy.MemController")
heap_memctrl.addParams({
    "clock" : "1GHz",
    "debug" : "1",
    "debug_level" : "10",
    "verbose" : "10",
    "addr_range_start" : "0",
    "addr_range_end" : "0x3FFFFFFF",
    "mem_size" : "1GiB",
})

# Create memory backend for heap operations
heap_memory = heap_memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
heap_memory.addParams({
    "access_time" : "1ns",
    "mem_size" : "1GiB",
})

# Create the external heap subcomponent
heap = solver.setSubComponent("order_heap", "satsolver.Heap")

# Connect solver to heap
solver_heap_link = sst.Link("solver_heap_link")
solver_heap_link.connect((solver, "heap_port", "1ns"), (heap, "response", "1ns"))

# Connect solver to main memory controller
link = sst.Link("mem_link")
link.connect((iface, "lowlink", "1ns"), (memctrl, "highlink", "1ns"))

# Connect solver to heap memory controller
heapmem_link = sst.Link("heapmem_link")
heapmem_link.connect((heap_iface, "lowlink", "1ns"), (heap_memctrl, "highlink", "1ns"))

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
    "rate": "100ns"
})

sst.setStatisticOutput("sst.statOutputCSV", 
    {"filepath": args.stats_file, "separator": "," })
