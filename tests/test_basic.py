import sst
import os
import sys

# Get CNF file path from command line or use default
if len(sys.argv) > 1:
    cnf_path = sys.argv[1]
    if not os.path.exists(cnf_path):
        print(f"Error: CNF file '{cnf_path}' not found")
        sys.exit(1)
else:
    cnf_path = os.path.join(os.path.dirname(__file__), "test.cnf")

print(f"Using CNF file: {cnf_path}")

# Read CNF file content
with open(cnf_path, 'r') as f:
    cnf_content = f.read()

# Create the SAT solver component
solver = sst.Component("solver", "satsolver.SATSolver")

# Get file size and pass it to solver
file_size = os.path.getsize(cnf_path)
solver.addParams({
    "clock" : "1GHz",
    "verbose" : "1",
    "filesize" : str(file_size)  # Add file size parameter
})

# Configure memory interface
iface = solver.setSubComponent("memory", "memHierarchy.standardInterface")

# Create memory controller with proper backing store configuration
memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock" : "1GHz",
    "backing" : "mmap",  # Changed back to mmap
    "backing_size_unit" : "1B",
    "memory_file" : cnf_path,
    "debug" : "1",
    "debug_level" : "10",
    "verbose" : "0",
    "addr_range_start" : "0",
    "addr_range_end" : str(512*1024*1024-1),
    "mem_size" : "512MiB",
    "initBacking" : "1",  # Added - explicitly enable backing store initialization
})

# Create memory backend
memory = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
memory.addParams({
    "access_time" : "1000ns",
    "mem_size" : "512MiB",
})


# Connect solver to memory controller
link = sst.Link("mem_link")
link.connect((iface, "lowlink", "1ns"), (memctrl, "highlink", "1ns"))

print(f"CNF content ({len(cnf_content)} bytes)")

# Enable statistics - different types for different stats
sst.setStatisticLoadLevel(7)

# Enable statistics for all metrics including assigned_vars
sst.enableStatisticsForComponentName("solver", [
    "decisions",
    "propagations", 
    "backtracks",
    "assigned_vars",
    "conflicts"
], {
    "type": "sst.AccumulatorStatistic",
    "rate": "1ns"
})

sst.setStatisticOutput("sst.statOutputCSV", 
    {"filepath": "stats.csv", "separator": "," })
