import matplotlib.pyplot as plt

# Initialize data structures
times = []
decisions = []
propagations = []
backtracks = []
assigns = []

# Read and parse CSV file
with open('/home/jakeke/sst/scratch/src/sst-sat/src/stats.csv', 'r') as f:
    header = f.readline()  # Skip header
    for line in f:
        parts = line.strip().split(',')
        component = parts[0]
        stat = parts[1]
        simtime = int(parts[4])
        count = int(parts[6])  # Using Sum.u64 column
        
        if component == 'solver':
            if stat == 'decisions':
                decisions.append((simtime, count))
            elif stat == 'propagations':
                propagations.append((simtime, count))
            elif stat == 'backtracks':
                backtracks.append((simtime, count))
            elif stat == 'assigned_vars':
                assigns.append((simtime, count))

# Find first non-zero value time for each series
def find_first_activity(series):
    for t, c in sorted(series):
        if c > 0:
            return t
    return float('inf')

start_time = min(
    find_first_activity(decisions),
    find_first_activity(propagations), 
    find_first_activity(backtracks),
    find_first_activity(assigns)
)

print(f"First solver activity at {start_time} ps")

# Adjust all times relative to start_time
decisions = [(t - start_time, c) for t, c in decisions if t >= start_time]
propagations = [(t - start_time, c) for t, c in propagations if t >= start_time]
backtracks = [(t - start_time, c) for t, c in backtracks if t >= start_time]
assigns = [(t - start_time, c) for t, c in assigns if t >= start_time]

# Sort by time and unzip into separate lists
times_d, counts_d = zip(*sorted(decisions))
times_p, counts_p = zip(*sorted(propagations))
times_b, counts_b = zip(*sorted(backtracks))
times_a, counts_a = zip(*sorted(assigns))

# Print final statistics
print("\nFinal Statistics:")
print("================")
print(f"Final decisions: {counts_d[-1]}")
print(f"Final propagations: {counts_p[-1]}")
print(f"Final backtracks: {counts_b[-1]}")
print(f"Final assigned variables: {counts_a[-1]}")

# Create figure with two subplots
plt.figure(figsize=(12, 8))

# Top subplot - Cumulative counts
plt.subplot(2, 1, 1)
plt.plot(times_d, counts_d, label='Decisions', marker='.', color='blue')
plt.plot(times_p, counts_p, label='Propagations', marker='.', color='red') 
plt.plot(times_b, counts_b, label='Backtracks', marker='.', color='green')
plt.plot(times_a, counts_a, label='Assigned Variables', marker='.', color='purple')
plt.xlabel('Simulation Time (ps)')
plt.ylabel('Cumulative Count')
plt.title('SAT Solver Progress (Cumulative)')
plt.legend()
plt.grid(True)

# Bottom subplot - Differences between consecutive points
plt.subplot(2, 1, 2)
def get_diffs(times, counts):
    diffs = [j-i for i, j in zip(counts[:-1], counts[1:])]
    return times[1:], diffs

times_d_diff, counts_d_diff = get_diffs(times_d, counts_d)
times_p_diff, counts_p_diff = get_diffs(times_p, counts_p)
times_b_diff, counts_b_diff = get_diffs(times_b, counts_b)
times_a_diff, counts_a_diff = get_diffs(times_a, counts_a)

plt.plot(times_d_diff, counts_d_diff, label='Decisions Δ', marker='.', color='blue')
plt.plot(times_p_diff, counts_p_diff, label='Propagations Δ', marker='.', color='red')
plt.plot(times_b_diff, counts_b_diff, label='Backtracks Δ', marker='.', color='green')
plt.plot(times_a_diff, counts_a_diff, label='Assigned Vars Δ', marker='.', color='purple')
plt.xlabel('Simulation Time (ps)')
plt.ylabel('Change per Step')
plt.title('SAT Solver Progress (Changes)')
plt.legend()
plt.grid(True)

plt.tight_layout()
plt.savefig('sat_stats.png')
plt.show()
