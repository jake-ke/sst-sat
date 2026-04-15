#!/usr/bin/env python3
"""CLI for inspecting SST SAT solver binary traces.

Examples:
    # Print header only
    tools/query_trace.py runs/trace_db/foo/seed0.trace.bin --header

    # Stream memory events as JSONL, first 50
    tools/query_trace.py runs/trace_db/foo/seed0.trace.bin --view memory --head 50

    # Human-readable summary (counts per event kind, per DS)
    tools/query_trace.py runs/trace_db/foo/seed0.trace.bin --summary

    # Aggregate summary across the whole trace_db
    tools/query_trace.py runs/trace_db/ --summary

    # Literal-level view (decisions + enqueues)
    tools/query_trace.py runs/trace_db/foo/seed0.trace.bin --view literal --format text
"""
import argparse
import json
import os
import sys

# Make sibling imports work when script is run directly.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_lib import iter_events, parse_header, summarize, VIEWS  # noqa: E402


def _iter_traces(path):
    """Yield every *.trace.bin under `path` (or just `path` if it's a file)."""
    if os.path.isfile(path):
        yield path
        return
    for root, _, files in os.walk(path):
        for fn in sorted(files):
            if fn.endswith('.trace.bin'):
                yield os.path.join(root, fn)


def _fmt_text(ev):
    k = ev['kind']
    c = ev.get('cycle', '-')
    ph = ev.get('phase_name', ev.get('phase', '-'))
    lvl = ev.get('level', '-')
    prefix = f'[{c:>10}] ph={ph:<9} lvl={lvl:<3}'
    if k in ('mem_read', 'mem_write'):
        rw = 'W' if ev['is_write'] else 'R'
        return f"{prefix} MEM {rw} {ev['ds_name']:<12} 0x{ev['addr']:08x} +{ev['size']}"
    if k == 'phase':
        return f"{prefix} PHASE -> {ev['phase_name']}"
    if k == 'decide':
        return f"{prefix} DECIDE  var={ev['var']} sign={ev['sign']} new_level={ev['new_level']}"
    if k == 'enqueue':
        return f"{prefix} ENQUEUE var={ev['var']} sign={ev['sign']} reason={ev['reason_cref']}"
    if k == 'conflict':
        return f"{prefix} CONFLICT cref=0x{ev['cref']:x}"
    if k == 'learn':
        return (f"{prefix} LEARN lbd={ev['lbd']} size={ev['clause_size']} "
                f"bt_level={ev['bt_level']} new_cref=0x{ev['new_cref']:x}")
    if k == 'backtrack':
        return f"{prefix} BACKTRACK {ev['from_level']} -> {ev['to_level']}"
    if k == 'restart':
        return f"{prefix} RESTART #{ev['restart_idx']}"
    if k == 'reduce':
        return f"{prefix} REDUCE removed={ev['removed']} kept={ev['kept']}"
    if k == 'finish':
        return (f"FINISH total_cycles={ev['total_cycles']} "
                f"events_written={ev['events_written']} crc32=0x{ev['crc32']:08x}")
    return f"{prefix} {k} {ev}"


def cmd_header(path):
    for trace in _iter_traces(path):
        print(f'=== {trace} ===')
        hdr = parse_header(trace)
        for k, v in hdr.items():
            print(f'  {k} = {v}')


def cmd_summary(path):
    total = {}
    any_trace = False
    for trace in _iter_traces(path):
        any_trace = True
        s = summarize(trace)
        print(f'=== {trace} ===')
        h = s['header']
        print(f"  cnf={h.get('cnf','?')}  seed={h.get('seed','?')}  "
              f"vars={h.get('num_vars','?')}  clauses={h.get('num_clauses','?')}")
        print(f"  total_mem_bytes={s['total_mem_bytes']}")
        print(f"  counts={s['counts']}")
        print(f"  ds_counts={s['ds_counts']}")
        finish = s['counts'].get('finish', 0)
        if finish != 1:
            print(f"  WARNING: expected exactly 1 FINISH record, got {finish}")
        # Aggregate
        for k, v in s['counts'].items():
            total[k] = total.get(k, 0) + v
    if not any_trace:
        print(f'no *.trace.bin found under {path}')
        return
    if os.path.isdir(path):
        print(f'=== AGGREGATE across {path} ===')
        print(f'  {total}')


def cmd_stream(path, view, fmt, head):
    shown = 0
    for trace in _iter_traces(path):
        if os.path.isdir(path):
            print(f'=== {trace} ===')
        for ev in iter_events(trace, view=view):
            if fmt == 'jsonl':
                print(json.dumps(ev))
            else:
                print(_fmt_text(ev))
            shown += 1
            if head and shown >= head:
                return


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument('path', help='trace file or directory to walk')
    ap.add_argument('--view', choices=VIEWS, default='all',
                    help='event filter (default: all)')
    ap.add_argument('--format', choices=('jsonl', 'text'), default='text',
                    help='output format (default: text)')
    ap.add_argument('--head', type=int, default=0,
                    help='limit to first N events (default: unlimited)')
    ap.add_argument('--header', action='store_true',
                    help='print header only and exit')
    ap.add_argument('--summary', action='store_true',
                    help='print per-kind / per-DS counts and exit')
    args = ap.parse_args()

    if args.header:
        cmd_header(args.path)
        return
    if args.summary:
        cmd_summary(args.path)
        return
    cmd_stream(args.path, args.view, args.format, args.head)


if __name__ == '__main__':
    main()