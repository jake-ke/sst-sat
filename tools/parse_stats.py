import csv
import sys

def parse_statistics(stats_file):
    """Parse the statistics file and print cache statistics"""
    try:
        # Read the CSV file
        cache_stats = {
            "global_l1cache": {},
            "global_l2cache": {},
            "global_l3cache": {}
        }
        
        with open(stats_file, 'r') as csvfile:
            reader = csv.reader(csvfile)
            header = next(reader)  # Skip header row
            for row in reader:
                if len(row) < 7:  # Ensure we have enough columns
                    continue
                
                # Extract component and statistic directly from their columns
                component = row[0]
                statistic = row[1]
                
                # Use Sum.u64 (column 6) as the value
                value = row[6]
                
                # Collect stats for all cache levels
                if component in cache_stats and statistic in ["CacheHits", "CacheMisses"]:
                    cache_stats[component][statistic] = value

        # Print cache statistics for all levels
        print("\n============================[ Cache Statistics ]============================")
        
        cache_levels = ["L1 Cache", "L2 Cache", "L3 Cache"]
        components = ["global_l1cache", "global_l2cache", "global_l3cache"]
        
        for level, component in zip(cache_levels, components):
            hits = float(cache_stats[component].get("CacheHits", "0"))
            misses = float(cache_stats[component].get("CacheMisses", "0"))
            total = hits + misses
            
            if total > 0:
                miss_rate = (misses / total) * 100
                hit_rate = (hits / total) * 100
            else:
                miss_rate = 0.0
                hit_rate = 0.0
            
            print(f"\n{level} Statistics:")
            print(f"  Cache Hits:       {int(hits)}")
            print(f"  Cache Misses:     {int(misses)}")
            print(f"  Total Requests:   {int(total)}")
            print(f"  Hit Rate:         {hit_rate:.2f}%")
            print(f"  Miss Rate:        {miss_rate:.2f}%")
        
        print("\n===========================================================================\n")
    except Exception as e:
        print(f"Error parsing statistics file: {e}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        stats_file = sys.argv[1]
    else:
        print("Usage: parse_stats.py <stats_file>")
        sys.exit(1)
    parse_statistics(stats_file)