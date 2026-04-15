"""Streaming reader for SST SAT solver binary traces.

Format reference: TRACE_FORMAT.md at repo root.

Typical usage:

    from trace_lib import iter_events, parse_header, VIEWS

    hdr = parse_header('/path/to/run.trace.bin')
    for ev in iter_events('/path/to/run.trace.bin', view='memory'):
        ...  # ev is a dict: {'kind', 'cycle', 'phase', 'level', ...}

Views:
    all      - every decoded event
    memory   - only MEM_READ / MEM_WRITE
    algo     - only algorithm events (DECIDE ENQUEUE CONFLICT LEARN BACKTRACK RESTART REDUCE)
    literal  - only DECIDE + ENQUEUE (literal-level activity)
    clause   - only CONFLICT + LEARN + REDUCE + mem events touching clause/clauses_cmd DSes
    phase    - only PHASE records (phase-change timeline)
"""
import os
import struct
import zlib


# Phase enum (matches SolverState in src/satsolver.h)
PHASE_NAMES = [
    'IDLE', 'INIT', 'STEP', 'PROPAGATE', 'DECIDE', 'ANALYZE',
    'MINIMIZE', 'BTLEVEL', 'BACKTRACK', 'REDUCE', 'RESTART',
    'WAIT_HEAP', 'DONE',
]

DS_NAMES = [
    'heap', 'indices', 'variables', 'watches', 'watch_nodes',
    'clauses_cmd', 'clauses', 'var_activity', 'unknown',
]

TAG_TICK, TAG_PHASE, TAG_LEVEL = 0x00, 0x01, 0x02
TAG_MEM_READ, TAG_MEM_WRITE = 0x10, 0x11
TAG_DECIDE, TAG_ENQUEUE, TAG_CONFLICT, TAG_LEARN = 0x20, 0x21, 0x22, 0x23
TAG_BACKTRACK, TAG_RESTART, TAG_REDUCE = 0x24, 0x25, 0x26
TAG_FINISH = 0x7f

VIEWS = ('all', 'memory', 'algo', 'literal', 'clause', 'phase')

ALGO_KINDS = frozenset(('decide', 'enqueue', 'conflict', 'learn',
                        'backtrack', 'restart', 'reduce'))
LITERAL_KINDS = frozenset(('decide', 'enqueue'))
CLAUSE_ALGO_KINDS = frozenset(('conflict', 'learn', 'reduce'))
CLAUSE_DS_IDS = frozenset((5, 6))  # clauses_cmd, clauses


def _uvarint(buf, i):
    r = 0
    s = 0
    while True:
        b = buf[i]
        i += 1
        r |= (b & 0x7f) << s
        if (b & 0x80) == 0:
            return r, i
        s += 7


def _svarint(buf, i):
    u, i = _uvarint(buf, i)
    return (u >> 1) ^ -(u & 1), i


def parse_header(path):
    """Read magic + text header. Returns dict of header fields."""
    with open(path, 'rb') as f:
        magic = f.read(8)
        if magic != b'SSTSAT\x00\x00':
            raise ValueError(f'{path}: bad magic {magic!r}')
        lines = []
        while True:
            line = f.readline()
            if not line:
                raise ValueError(f'{path}: header terminator (---) not found')
            if line == b'---\n':
                break
            lines.append(line.decode().rstrip('\n'))
        hdr = {'magic': 'SSTSAT', 'body_offset': f.tell()}
        for line in lines:
            if '=' in line:
                k, _, v = line.partition('=')
                hdr[k] = v
        return hdr


def iter_events(path, view='all'):
    """Yield decoded events as dicts.

    Each dict has at minimum a `kind` key. Memory events also carry
    `cycle, phase, level, ds, ds_name, addr, size, is_write`. Algorithm
    events carry kind-specific payload plus the current `cycle/phase/level`.
    """
    if view not in VIEWS:
        raise ValueError(f'view must be one of {VIEWS}')

    hdr = parse_header(path)
    with open(path, 'rb') as f:
        f.seek(hdr['body_offset'])
        buf = f.read()

    i = 0
    n = len(buf)
    cur_cycle = 0
    cur_phase = 0
    cur_level = 0
    last_addr = [0] * 9

    SIZE_CLASSES = (1, 2, 4, 8, 16, 32, 64, None)

    while i < n:
        tag = buf[i]
        i += 1

        if tag == TAG_TICK:
            delta, i = _uvarint(buf, i)
            cur_cycle += delta
            continue

        if tag == TAG_PHASE:
            cur_phase = buf[i]
            i += 1
            if view in ('all', 'phase'):
                yield {'kind': 'phase', 'cycle': cur_cycle, 'phase': cur_phase,
                       'phase_name': PHASE_NAMES[cur_phase] if cur_phase < len(PHASE_NAMES) else str(cur_phase),
                       'level': cur_level}
            continue

        if tag == TAG_LEVEL:
            cur_level, i = _uvarint(buf, i)
            continue

        if tag in (TAG_MEM_READ, TAG_MEM_WRITE):
            packed = buf[i]
            i += 1
            sc = (packed >> 4) & 0x7
            ds = packed & 0xF
            delta, i = _svarint(buf, i)
            last_addr[ds] += delta
            size = SIZE_CLASSES[sc]
            if size is None:
                size, i = _uvarint(buf, i)
            # View filter
            if view == 'memory' or view == 'all':
                yield {
                    'kind': 'mem_write' if tag == TAG_MEM_WRITE else 'mem_read',
                    'cycle': cur_cycle, 'phase': cur_phase,
                    'phase_name': PHASE_NAMES[cur_phase] if cur_phase < len(PHASE_NAMES) else str(cur_phase),
                    'level': cur_level,
                    'ds': ds, 'ds_name': DS_NAMES[ds] if ds < len(DS_NAMES) else str(ds),
                    'addr': last_addr[ds], 'size': size,
                    'is_write': tag == TAG_MEM_WRITE,
                }
            elif view == 'clause' and ds in CLAUSE_DS_IDS:
                yield {
                    'kind': 'mem_write' if tag == TAG_MEM_WRITE else 'mem_read',
                    'cycle': cur_cycle, 'phase': cur_phase,
                    'phase_name': PHASE_NAMES[cur_phase] if cur_phase < len(PHASE_NAMES) else str(cur_phase),
                    'level': cur_level,
                    'ds': ds, 'ds_name': DS_NAMES[ds],
                    'addr': last_addr[ds], 'size': size,
                    'is_write': tag == TAG_MEM_WRITE,
                }
            continue

        if tag == TAG_DECIDE:
            var, i = _uvarint(buf, i)
            sign = buf[i]; i += 1
            new_level, i = _uvarint(buf, i)
            if view in ('all', 'algo', 'literal'):
                yield {'kind': 'decide', 'cycle': cur_cycle, 'phase': cur_phase,
                       'level': cur_level, 'var': var, 'sign': sign, 'new_level': new_level}
            continue

        if tag == TAG_ENQUEUE:
            var, i = _uvarint(buf, i)
            sign = buf[i]; i += 1
            reason, i = _svarint(buf, i)
            if view in ('all', 'algo', 'literal'):
                yield {'kind': 'enqueue', 'cycle': cur_cycle, 'phase': cur_phase,
                       'level': cur_level, 'var': var, 'sign': sign, 'reason_cref': reason}
            continue

        if tag == TAG_CONFLICT:
            cref, i = _uvarint(buf, i)
            if view in ('all', 'algo', 'clause'):
                yield {'kind': 'conflict', 'cycle': cur_cycle, 'phase': cur_phase,
                       'level': cur_level, 'cref': cref}
            continue

        if tag == TAG_LEARN:
            lbd, i = _uvarint(buf, i)
            clause_size, i = _uvarint(buf, i)
            bt_level, i = _uvarint(buf, i)
            new_cref, i = _uvarint(buf, i)
            if view in ('all', 'algo', 'clause'):
                yield {'kind': 'learn', 'cycle': cur_cycle, 'phase': cur_phase,
                       'level': cur_level, 'lbd': lbd, 'clause_size': clause_size,
                       'bt_level': bt_level, 'new_cref': new_cref}
            continue

        if tag == TAG_BACKTRACK:
            frm, i = _uvarint(buf, i)
            to, i = _uvarint(buf, i)
            if view in ('all', 'algo'):
                yield {'kind': 'backtrack', 'cycle': cur_cycle, 'phase': cur_phase,
                       'level': cur_level, 'from_level': frm, 'to_level': to}
            continue

        if tag == TAG_RESTART:
            idx, i = _uvarint(buf, i)
            if view in ('all', 'algo'):
                yield {'kind': 'restart', 'cycle': cur_cycle, 'phase': cur_phase,
                       'level': cur_level, 'restart_idx': idx}
            continue

        if tag == TAG_REDUCE:
            removed, i = _uvarint(buf, i)
            kept, i = _uvarint(buf, i)
            if view in ('all', 'algo', 'clause'):
                yield {'kind': 'reduce', 'cycle': cur_cycle, 'phase': cur_phase,
                       'level': cur_level, 'removed': removed, 'kept': kept}
            continue

        if tag == TAG_FINISH:
            total_cycles = int.from_bytes(buf[i:i+8], 'little'); i += 8
            events_written = int.from_bytes(buf[i:i+8], 'little'); i += 8
            crc32 = int.from_bytes(buf[i:i+4], 'little'); i += 4
            if view == 'all':
                yield {'kind': 'finish', 'total_cycles': total_cycles,
                       'events_written': events_written, 'crc32': crc32}
            return

        raise ValueError(f'unknown tag 0x{tag:02x} at body offset {i-1}')


def summarize(path):
    """Return a dict of per-kind counts + per-DS memory counts + header."""
    hdr = parse_header(path)
    counts = {}
    ds_counts = {}
    phase_counts = {}
    total_mem_bytes = 0
    for ev in iter_events(path, view='all'):
        k = ev['kind']
        counts[k] = counts.get(k, 0) + 1
        if k in ('mem_read', 'mem_write'):
            d = ev['ds_name']
            ds_counts[d] = ds_counts.get(d, 0) + 1
            if ev['size']:
                total_mem_bytes += ev['size']
        elif k == 'phase':
            p = ev['phase_name']
            phase_counts[p] = phase_counts.get(p, 0) + 1
    return {
        'header': hdr,
        'counts': counts,
        'ds_counts': ds_counts,
        'phase_counts': phase_counts,
        'total_mem_bytes': total_mem_bytes,
    }
