#!/usr/bin/env python3
"""Parse and visualize PipelinedHeap activity-distribution profiles (.heapdist).

Reads the binary snapshot files written by the HeapDistProfiler (see
plans/pheap-act-dist.md) and renders the per-instance and sweep-level views
that back the lazy-op / activity-representation decisions.

Note: under --olc-stack runs the sift_boundary series is never fed (replace
descents settle at the on-chip boundary; no OLC sifts exist), so its
histograms and rates legitimately read as zero.

Outputs:

  Per-instance (input = one <case>.stats.csv.heapdist file):
    <case>_heapdist_heatmaps.pdf      gap-bin occupancy over time (live /
                                      off-chip stored / pops), log-count color
    <case>_heapdist_occ_vs_pop.pdf    stored-content vs pop-mass overlay
                                      (aggregate + final window)
    <case>_heapdist_cold_fraction.pdf per-level cold fraction (below-X / count)
                                      over time
    <case>_heapdist_margins.pdf       compare-margin histograms per class +
                                      cumulative "decided within m bits"
    <case>_heapdist_summary.csv       summary scalars (tail fractions, pop-gap
                                      percentiles, margin p99, boundary rates)
    <case>_heapdist.gif               optional --animate: per-frame evolution

  Sweep (input = run folder with seed*/<case>.stats.csv.heapdist):
    heapdist_summary.csv              one scalar row per instance
    heapdist_sweep_box.pdf            cross-instance distribution boxplots

  --check     invariant validation only (monotonic counters, histogram sums vs
              header counts within in-flight slack, level-row consistency)
  --selftest  write a synthetic multi-frame file from the format spec, parse
              it back, assert round-trip equality, and exercise every plot
              path (no C++ output needed)

Binary format (little-endian, spec: plans/pheap-act-dist.md "Frame format"):
  file header (32 B): magic 'HDST' u32, version u16 = 1, flags u16, num_vars
  u32, onchip_levels u32, gap_bins u32 = 258, margin_bins u32 = 68, gap_step
  u32 = 2, gap_min i32 = -8. Then frames: 16 x u64 header (conflicts,
  decisions, cycle, heap_size, live_count, stale_count, rescales_total,
  var_inc f64-bits, live_pops, stale_pops, purge_pops, inserts_accepted,
  insert_skips, boundary_inserts, boundary_sifts, cmp_vs_empty), num_level_rows
  u32 + pad u32, 6 gap histograms x gap_bins u32 (live, onchip_stored,
  offchip_stored, pop, insert_boundary, sift_boundary), 4 margin histograms x
  margin_bins u32 (top4, onchip_rest, olc, pop_margin), num_level_rows level
  rows of 8 u32 (level, count, live, zero, below16, below32, below64,
  below128). A truncated final frame is tolerated with a warning.

Gap bins: bin b covers exponent gap g in [gap_min + b*gap_step, +step);
bin gap_bins-2 = "ancient" (g >= 504 / denormal), bin gap_bins-1 = "zero"
(act == 0.0). Percentiles report ancient as g=504 and zero as g=506.
Margin bins: 0 = tie (a == b), 1+m = decided m relative bits below the
leading bit (m in [0,63]), 65 = exactly one side zero, 66-67 reserved.

Usage:
  python tools/plot_heap_dist.py <file.heapdist> [--out-dir DIR] [--animate]
  python tools/plot_heap_dist.py <file.heapdist> --check
  python tools/plot_heap_dist.py <run_folder> [--out-dir DIR] [--csv FILE]
  python tools/plot_heap_dist.py --selftest
"""

import sys
import csv
import math
import struct
import random
import argparse
import tempfile
from pathlib import Path
from collections import defaultdict

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import LogNorm
from matplotlib.animation import FuncAnimation, PillowWriter

MAGIC = b'HDST'
FILE_HDR = struct.Struct('<4sHHIIIIIi')   # 32 bytes
FRAME_HDR = struct.Struct('<16Q')          # 128 bytes
ROWS_HDR = struct.Struct('<II')            # num_level_rows + pad

FRAME_FIELDS = [
    'conflicts', 'decisions', 'cycle', 'heap_size', 'live_count',
    'stale_count', 'rescales_total', 'var_inc_bits', 'live_pops',
    'stale_pops', 'purge_pops', 'inserts_accepted', 'insert_skips',
    'boundary_inserts', 'boundary_sifts', 'cmp_vs_empty',
]
GAP_HISTS = ['live', 'onchip_stored', 'offchip_stored', 'pop',
             'insert_boundary', 'sift_boundary']
MARGIN_HISTS = ['top4', 'onchip_rest', 'olc', 'pop_margin']
MARGIN_LABELS = {
    'top4': 'levels 0-3 (pop-critical)',
    'onchip_rest': 'levels 4..K-1',
    'olc': 'OLC (off-chip) compares',
    'pop_margin': 'pop: root vs runner-up',
}
LEVEL_ROW_FIELDS = ['level', 'count', 'live', 'zero',
                    'below16', 'below32', 'below64', 'below128']

# Validated categorical palette (dataviz six-checks; slots 1-2 shared with
# race_stats.py). Fixed assignment by margin class, never cycled.
CLASS_COLORS = {'top4': '#4C72B0', 'onchip_rest': '#DD8452',
                'olc': '#9467BD', 'pop_margin': '#55A868'}
C_STORED = '#4C72B0'   # stored-content series
C_POP = '#DD8452'      # pop-mass series (validated pair with C_STORED)
C_LIVE = '#555555'     # live series: neutral + dashed (linestyle encodes it)


class FileHeader:
    def __init__(self, version, flags, num_vars, onchip_levels, gap_bins,
                 margin_bins, gap_step, gap_min):
        self.version = version
        self.flags = flags
        self.num_vars = num_vars
        self.onchip_levels = onchip_levels
        self.gap_bins = gap_bins
        self.margin_bins = margin_bins
        self.gap_step = gap_step
        self.gap_min = gap_min

    def __eq__(self, o):
        return isinstance(o, FileHeader) and self.__dict__ == o.__dict__


class Frame:
    """One snapshot: 16 u64 header fields + gap/margin histograms + level rows."""

    def __init__(self, gap, margin, levels, **hdr):
        for f in FRAME_FIELDS:
            setattr(self, f, int(hdr[f]))
        self.gap = gap        # {name: np.ndarray[gap_bins] u32}
        self.margin = margin  # {name: np.ndarray[margin_bins] u32}
        self.levels = levels  # np.ndarray[(n_rows, 8)] u32

    @property
    def var_inc(self):
        return struct.unpack('<d', struct.pack('<Q', self.var_inc_bits))[0]

    def equals(self, o):
        for f in FRAME_FIELDS:
            if getattr(self, f) != getattr(o, f):
                return False
        for name in GAP_HISTS:
            if not np.array_equal(self.gap[name], o.gap[name]):
                return False
        for name in MARGIN_HISTS:
            if not np.array_equal(self.margin[name], o.margin[name]):
                return False
        return np.array_equal(self.levels, o.levels)


# ---------------------------------------------------------------------------
# Binary parse / serialize
# ---------------------------------------------------------------------------

def parse_heapdist(path):
    """Parse a .heapdist file -> (FileHeader, [Frame], [warning strings]).

    A truncated final frame (writer killed mid-frame) is tolerated: the
    partial frame is dropped with a warning.
    """
    data = Path(path).read_bytes()
    if len(data) < FILE_HDR.size:
        raise ValueError(f'{path}: too short for file header ({len(data)} B)')
    (magic, version, flags, num_vars, onchip_levels, gap_bins,
     margin_bins, gap_step, gap_min) = FILE_HDR.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError(f'{path}: bad magic {magic!r} (want {MAGIC!r})')
    if version != 1:
        raise ValueError(f'{path}: unsupported version {version}')
    hdr = FileHeader(version, flags, num_vars, onchip_levels, gap_bins,
                     margin_bins, gap_step, gap_min)

    frames, warnings = [], []
    off = FILE_HDR.size
    gsz, msz = gap_bins * 4, margin_bins * 4
    while off < len(data):
        start = off
        if off + FRAME_HDR.size + ROWS_HDR.size > len(data):
            warnings.append(f'truncated final frame at byte {start} '
                            f'({len(data) - start} trailing bytes dropped)')
            break
        vals = FRAME_HDR.unpack_from(data, off)
        off += FRAME_HDR.size
        n_rows, _pad = ROWS_HDR.unpack_from(data, off)
        off += ROWS_HDR.size
        if n_rows > 1 << 20:
            warnings.append(f'implausible num_level_rows={n_rows} at byte '
                            f'{start}; treating rest of file as corrupt')
            break
        body = 6 * gsz + 4 * msz + n_rows * 32
        if off + body > len(data):
            warnings.append(f'truncated final frame at byte {start} '
                            f'({len(data) - start} trailing bytes dropped)')
            break
        gap = {}
        for name in GAP_HISTS:
            gap[name] = np.frombuffer(data, '<u4', gap_bins, off).copy()
            off += gsz
        margin = {}
        for name in MARGIN_HISTS:
            margin[name] = np.frombuffer(data, '<u4', margin_bins, off).copy()
            off += msz
        levels = np.frombuffer(data, '<u4', n_rows * 8, off).copy()
        levels = levels.reshape(n_rows, 8)
        off += n_rows * 32
        frames.append(Frame(gap, margin, levels,
                            **dict(zip(FRAME_FIELDS, vals))))
    return hdr, frames, warnings


def serialize_heapdist(hdr, frames):
    """Inverse of parse_heapdist (used by --selftest)."""
    out = bytearray()
    out += FILE_HDR.pack(MAGIC, hdr.version, hdr.flags, hdr.num_vars,
                         hdr.onchip_levels, hdr.gap_bins, hdr.margin_bins,
                         hdr.gap_step, hdr.gap_min)
    for fr in frames:
        out += FRAME_HDR.pack(*[int(getattr(fr, f)) for f in FRAME_FIELDS])
        out += ROWS_HDR.pack(len(fr.levels), 0)
        for name in GAP_HISTS:
            out += np.asarray(fr.gap[name], dtype='<u4').tobytes()
        for name in MARGIN_HISTS:
            out += np.asarray(fr.margin[name], dtype='<u4').tobytes()
        out += np.asarray(fr.levels, dtype='<u4').tobytes()
    return bytes(out)


# ---------------------------------------------------------------------------
# Derived quantities
# ---------------------------------------------------------------------------

def gap_value(hdr, b):
    """Representative gap for bin b (lower edge); ancient -> 504, zero -> 506."""
    return hdr.gap_min + b * hdr.gap_step


def gap_bin_of(hdr, g):
    return (g - hdr.gap_min) // hdr.gap_step


def agg_gap(frames, name):
    out = np.zeros(frames[0].gap[name].shape, dtype=np.int64) if frames else None
    for fr in frames:
        out += fr.gap[name]
    return out


def agg_margin(frames, name):
    out = np.zeros(frames[0].margin[name].shape, dtype=np.int64) if frames else None
    for fr in frames:
        out += fr.margin[name]
    return out


def stored_hist(fr):
    return fr.gap['onchip_stored'].astype(np.int64) + fr.gap['offchip_stored']


def tail_fraction(hdr, hist, g_threshold):
    """Fraction of mass with g >= threshold (ancient + zero count as colder)."""
    total = int(hist.sum())
    if total == 0:
        return float('nan')
    b0 = gap_bin_of(hdr, g_threshold)
    return float(hist[b0:].sum()) / total


def gap_percentile(hdr, hist, q):
    """q-th percentile of the gap distribution (bin lower edges)."""
    total = int(hist.sum())
    if total == 0:
        return float('nan')
    target = q / 100.0 * total
    c = 0
    for b, cnt in enumerate(hist):
        c += int(cnt)
        if c >= target:
            return float(gap_value(hdr, b))
    return float(gap_value(hdr, len(hist) - 1))


def gap_max(hdr, hist):
    nz = np.nonzero(hist)[0]
    return float(gap_value(hdr, int(nz[-1]))) if len(nz) else float('nan')


def margin_percentile(hdr, hist, q):
    """q-th percentile of m over decided finite-margin compares (bins 1..64);
    ties (bin 0) and zero-compares (bin 65) excluded."""
    zero_bin = hdr.margin_bins - 3
    core = hist[1:zero_bin]
    total = int(core.sum())
    if total == 0:
        return float('nan')
    target = q / 100.0 * total
    c = 0
    for m, cnt in enumerate(core):
        c += int(cnt)
        if c >= target:
            return float(m)
    return float(len(core) - 1)


def compute_summary(hdr, frames, case):
    """Summary scalars for one instance (the decision numbers of the plan)."""
    s = {'case': case, 'frames': len(frames)}
    if not frames:
        return s
    last = frames[-1]
    s['conflicts'] = last.conflicts
    s['heap_size_final'] = last.heap_size
    s['live_final'] = last.live_count
    s['rescales'] = last.rescales_total

    stored = agg_gap(frames, 'onchip_stored') + agg_gap(frames, 'offchip_stored')
    live = agg_gap(frames, 'live')
    pop = agg_gap(frames, 'pop')
    for x in (16, 32, 64):
        s[f'stored_tail_ge{x}'] = tail_fraction(hdr, stored, x)
        s[f'live_tail_ge{x}'] = tail_fraction(hdr, live, x)
    s['pop_gap_p50'] = gap_percentile(hdr, pop, 50)
    s['pop_gap_p99'] = gap_percentile(hdr, pop, 99)
    s['pop_gap_max'] = gap_max(hdr, pop)
    for name in MARGIN_HISTS:
        s[f'margin_p99_{name}'] = margin_percentile(hdr, agg_margin(frames, name), 99)

    conf = max(last.conflicts, 1)
    b_ins = sum(f.boundary_inserts for f in frames)
    b_sift = sum(f.boundary_sifts for f in frames)
    ins_acc = sum(f.inserts_accepted for f in frames)
    pops = sum(f.live_pops + f.stale_pops + f.purge_pops for f in frames)
    s['boundary_ins_per_kconf'] = 1000.0 * b_ins / conf
    s['boundary_sift_per_kconf'] = 1000.0 * b_sift / conf
    s['boundary_ins_frac_of_inserts'] = b_ins / ins_acc if ins_acc else float('nan')
    s['stale_pop_frac'] = (sum(f.stale_pops for f in frames) / pops
                           if pops else float('nan'))
    return s


SUMMARY_FIELDS = [
    'case', 'frames', 'conflicts', 'heap_size_final', 'live_final', 'rescales',
    'stored_tail_ge16', 'stored_tail_ge32', 'stored_tail_ge64',
    'live_tail_ge16', 'live_tail_ge32', 'live_tail_ge64',
    'pop_gap_p50', 'pop_gap_p99', 'pop_gap_max',
    'margin_p99_top4', 'margin_p99_onchip_rest', 'margin_p99_olc',
    'margin_p99_pop_margin',
    'boundary_ins_per_kconf', 'boundary_sift_per_kconf',
    'boundary_ins_frac_of_inserts', 'stale_pop_frac',
]


def write_summary_csv(records, out_csv):
    with open(out_csv, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=SUMMARY_FIELDS, extrasaction='ignore')
        w.writeheader()
        for r in sorted(records, key=lambda x: x['case']):
            row = {k: (round(v, 6) if isinstance(v, float) else v)
                   for k, v in r.items()}
            w.writerow(row)


# ---------------------------------------------------------------------------
# Invariant checks (--check)
# ---------------------------------------------------------------------------

def check_invariants(hdr, frames):
    """Return a list of violation strings (empty = clean).

    Histogram sums are compared to header counts within an in-flight slack:
    pipeline transients at snapshot time make totals approximate by design
    (plan: "this noise is acceptable and documented").
    """
    issues = []
    if not frames:
        issues.append('no complete frames in file')
        return issues

    def slack(ref):
        return max(64, int(0.02 * ref))

    prev = None
    for i, fr in enumerate(frames):
        tag = f'frame {i} (conflicts={fr.conflicts})'
        if prev is not None:
            if fr.conflicts <= prev.conflicts:
                issues.append(f'{tag}: conflicts not increasing '
                              f'({prev.conflicts} -> {fr.conflicts})')
            for f in ('decisions', 'cycle', 'rescales_total'):
                if getattr(fr, f) < getattr(prev, f):
                    issues.append(f'{tag}: {f} decreased '
                                  f'({getattr(prev, f)} -> {getattr(fr, f)})')
        prev = fr

        st_total = int(stored_hist(fr).sum())
        if abs(st_total - fr.heap_size) > slack(fr.heap_size):
            issues.append(f'{tag}: stored hist sum {st_total} vs '
                          f'heap_size {fr.heap_size}')
        if abs((fr.live_count + fr.stale_count) - fr.heap_size) > slack(fr.heap_size):
            issues.append(f'{tag}: live+stale {fr.live_count + fr.stale_count} '
                          f'vs heap_size {fr.heap_size}')
        lv_sum = int(fr.gap['live'].sum())
        if lv_sum > fr.live_count or fr.live_count - lv_sum > slack(fr.live_count):
            issues.append(f'{tag}: live hist sum {lv_sum} vs '
                          f'live_count {fr.live_count}')
        for hist, ref, what in (('pop', fr.live_pops, 'live_pops'),
                                ('insert_boundary', fr.boundary_inserts, 'boundary_inserts'),
                                ('sift_boundary', fr.boundary_sifts, 'boundary_sifts')):
            hs = int(fr.gap[hist].sum())
            if abs(hs - ref) > slack(ref):
                issues.append(f'{tag}: {hist} hist sum {hs} vs {what} {ref}')
        pm = int(fr.margin['pop_margin'].sum())
        if pm > fr.live_pops + slack(fr.live_pops):
            issues.append(f'{tag}: pop_margin samples {pm} > live_pops '
                          f'{fr.live_pops}')
        for name in MARGIN_HISTS:
            if fr.margin[name][hdr.margin_bins - 2:].any():
                issues.append(f'{tag}: reserved margin bins nonzero in {name}')

        # Level rows vs aggregate.
        rows = fr.levels
        if len(rows):
            if len(set(int(r[0]) for r in rows)) != len(rows):
                issues.append(f'{tag}: duplicate level in level rows')
            for r in rows:
                lvl, cnt, live, zero, b16, b32, b64, b128 = (int(v) for v in r)
                if max(live, zero, b16) > cnt:
                    issues.append(f'{tag}: level {lvl} row exceeds count '
                                  f'(count={cnt} live={live} zero={zero} '
                                  f'below16={b16})')
                if not (b16 >= b32 >= b64 >= b128):
                    issues.append(f'{tag}: level {lvl} belowX not monotone '
                                  f'({b16},{b32},{b64},{b128})')
            row_total = int(rows[:, 1].sum())
            if abs(row_total - st_total) > slack(st_total):
                issues.append(f'{tag}: level-row count sum {row_total} vs '
                              f'stored hist total {st_total}')
            row_live = int(rows[:, 2].sum())
            if abs(row_live - fr.live_count) > slack(fr.live_count):
                issues.append(f'{tag}: level-row live sum {row_live} vs '
                              f'live_count {fr.live_count}')
    return issues


# ---------------------------------------------------------------------------
# Plot helpers
# ---------------------------------------------------------------------------

def prettify(case):
    """Shorten an instance filename for plotting (drop md5 prefix + extension)."""
    name = case
    if len(name) > 33 and name[32] == '-' and all(
            c in '0123456789abcdef' for c in name[:32].lower()):
        name = name[33:]
    for ext in ('.cnf', '.dimacs', '.txt'):
        if name.endswith(ext):
            name = name[:-len(ext)]
            break
    return name if len(name) <= 40 else name[:37] + '...'


def gap_axis_ticks(hdr):
    """(tick_positions, labels) for a bin-index gap axis, incl. anc/zero."""
    ticks, labels = [], []
    for g in (0, 16, 32, 64, 128, 256, 384):
        b = gap_bin_of(hdr, g)
        if 0 <= b < hdr.gap_bins - 2:
            ticks.append(b)
            labels.append(str(g))
    # Single combined tick for the two adjacent special bins (labels collide
    # one bin apart): ancient (gap_bins-2) and zero (gap_bins-1).
    ticks.append(hdr.gap_bins - 1.5)
    labels.append('anc/zero')
    return ticks, labels


def conflict_edges(conflicts):
    """Bin edges around per-frame conflict counts for pcolormesh."""
    c = np.asarray(conflicts, dtype=float)
    if len(c) == 1:
        w = max(abs(c[0]) * 0.05, 1.0)
        return np.array([c[0] - w, c[0] + w])
    mid = (c[1:] + c[:-1]) / 2
    return np.concatenate([[c[0] - (mid[0] - c[0])], mid,
                           [c[-1] + (c[-1] - mid[-1])]])


def plot_heatmaps(hdr, frames, case, out_pdf):
    """Gap-bin occupancy over time: live / off-chip stored / pop (log color)."""
    series = [('live', 'Live (in-heap) activities'),
              ('offchip_stored', f'Off-chip stored copies (levels >= '
                                 f'{hdr.onchip_levels})'),
              ('pop', 'Live pops per interval')]
    edges = conflict_edges([f.conflicts for f in frames])
    yedges = np.arange(hdr.gap_bins + 1) - 0.5
    ticks, labels = gap_axis_ticks(hdr)
    fig, axes = plt.subplots(1, 3, figsize=(16, 6), sharey=True)
    for ax, (name, title) in zip(axes, series):
        m = np.stack([fr.gap[name] for fr in frames], axis=1).astype(float)
        if m.max() > 0:
            pm = ax.pcolormesh(edges, yedges, np.ma.masked_equal(m, 0),
                               cmap='viridis',
                               norm=LogNorm(vmin=1, vmax=max(m.max(), 2)))
            fig.colorbar(pm, ax=ax, label='count (log)')
        else:
            ax.annotate('no data', xy=(0.5, 0.5), xycoords='axes fraction',
                        ha='center', va='center', fontsize=12, color='gray')
        for b in (hdr.gap_bins - 2.5, hdr.gap_bins - 1.5):
            ax.axhline(b, color='white', lw=0.6, alpha=0.6)
        ax.set_title(title, fontsize=10)
        ax.set_xlabel('conflicts')
        ax.set_yticks(ticks)
        ax.set_yticklabels(labels)
    axes[0].set_ylabel('exponent gap g = ilogb(var_inc) - ilogb(act)\n'
                       '(anc = ancient, zero = act == 0)')
    fig.suptitle(f'{prettify(case)} — activity gap distributions over time',
                 fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(out_pdf)
    plt.close(fig)


def _overlay_panel(ax, hdr, stored, live, pop, title):
    x = np.arange(hdr.gap_bins)

    def frac(h):
        t = h.sum()
        y = h / t if t else np.zeros_like(h, dtype=float)
        return np.where(y > 0, y, np.nan)

    ax.step(x, frac(stored), where='mid', color=C_STORED, lw=2,
            label='stored content (on+off-chip)')
    ax.step(x, frac(live), where='mid', color=C_LIVE, lw=1.5, ls='--',
            label='live set')
    ax.step(x, frac(pop), where='mid', color=C_POP, lw=2,
            label='pop mass')
    ax.set_yscale('log')
    ticks, labels = gap_axis_ticks(hdr)
    ax.set_xticks(ticks)
    ax.set_xticklabels(labels)
    ax.set_xlabel('exponent gap g')
    ax.set_ylabel('fraction of mass (log)')
    ax.set_title(title, fontsize=10)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8)


def plot_occupancy_vs_pop(hdr, frames, case, out_pdf, final_window=1):
    """Where the stored keys sit vs which gaps actually get popped.

    Separation between the two = the mass lazy ops can safely bury."""
    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    _overlay_panel(axes[0], hdr,
                   agg_gap(frames, 'onchip_stored') + agg_gap(frames, 'offchip_stored'),
                   agg_gap(frames, 'live'), agg_gap(frames, 'pop'),
                   f'(a) aggregate over all {len(frames)} frames')
    w = frames[-final_window:]
    _overlay_panel(axes[1], hdr,
                   agg_gap(w, 'onchip_stored') + agg_gap(w, 'offchip_stored'),
                   agg_gap(w, 'live'), agg_gap(w, 'pop'),
                   f'(b) final window (last {len(w)} frame(s))')
    fig.suptitle(f'{prettify(case)} — stored occupancy vs pop mass by gap',
                 fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_pdf)
    plt.close(fig)


def plot_cold_fraction(hdr, frames, case, out_pdf):
    """Per-level cold fraction (belowX / count) over time.

    Solid = on-chip levels (< onchip_levels), dashed = off-chip."""
    conflicts = [f.conflicts for f in frames]
    all_levels = sorted({int(r[0]) for fr in frames for r in fr.levels})
    fig, axes = plt.subplots(2, 2, figsize=(13, 9), sharex=True, sharey=True)
    xs = [(16, 4), (32, 5), (64, 6), (128, 7)]
    cmap = plt.get_cmap('viridis')
    max_lvl = max(all_levels) if all_levels else 1
    for ax, (x_thr, col) in zip(axes.flat, xs):
        for lvl in all_levels:
            ys = np.full(len(frames), np.nan)
            for i, fr in enumerate(frames):
                for r in fr.levels:
                    if int(r[0]) == lvl and int(r[1]) > 0:
                        ys[i] = int(r[col]) / int(r[1])
            color = cmap(lvl / max(max_lvl, 1))
            ls = '-' if lvl < hdr.onchip_levels else '--'
            ax.plot(conflicts, ys, color=color, ls=ls, lw=1.5)
        ax.set_title(f'fraction with g >= {x_thr}', fontsize=10)
        ax.grid(alpha=0.3)
        ax.set_ylim(-0.02, 1.02)
    for ax in axes[1]:
        ax.set_xlabel('conflicts')
    for ax in axes[:, 0]:
        ax.set_ylabel('cold fraction (belowX / count)')
    if all_levels:
        sm = plt.cm.ScalarMappable(cmap=cmap,
                                   norm=plt.Normalize(0, max(max_lvl, 1)))
        sm.set_array([])
        fig.colorbar(sm, ax=axes, label='heap level '
                     f'(solid < {hdr.onchip_levels} on-chip, dashed off-chip)')
    fig.suptitle(f'{prettify(case)} — per-level cold fraction over time',
                 fontsize=13)
    fig.savefig(out_pdf)
    plt.close(fig)


def plot_margins(hdr, frames, case, out_pdf):
    """Compare-margin histogram per class + cumulative decided-within-m."""
    zero_bin = hdr.margin_bins - 3
    fig, axes = plt.subplots(2, len(MARGIN_HISTS), figsize=(16, 8))
    tick_bins = [0, 1, 9, 17, 33, 49, zero_bin]
    tick_lbls = ['tie', '0', '8', '16', '32', '48', 'vs0']
    for j, name in enumerate(MARGIN_HISTS):
        h = agg_margin(frames, name).astype(float)
        color = CLASS_COLORS[name]
        axT, axB = axes[0, j], axes[1, j]
        total = h[:zero_bin + 1].sum()
        if total > 0:
            axT.bar(np.arange(zero_bin + 1), h[:zero_bin + 1],
                    width=0.9, color=color)
            axT.set_yscale('log')
            # Cumulative: decided within m bits = zero-compares + margins <= m.
            cum = [(h[zero_bin] + h[1:m + 2].sum()) / total for m in range(64)]
            axB.plot(range(64), cum, color=color, lw=2)
            p99 = margin_percentile(hdr, h.astype(np.int64), 99)
            if not math.isnan(p99):
                axB.axvline(p99, color='red', ls='--', lw=1)
                axB.annotate(f'p99={p99:.0f}', xy=(p99, 0.5),
                             xytext=(4, 0), textcoords='offset points',
                             color='red', fontsize=8)
        else:
            axT.annotate('no data', xy=(0.5, 0.5), xycoords='axes fraction',
                         ha='center', va='center', color='gray')
        axT.set_title(MARGIN_LABELS[name], fontsize=10)
        axT.set_xticks(tick_bins)
        axT.set_xticklabels(tick_lbls, fontsize=8)
        axT.set_xlabel('margin m (bits below leading bit)')
        axB.set_xlabel('m (bits)')
        axB.set_ylim(0, 1.05)
        axB.grid(alpha=0.3)
    axes[0, 0].set_ylabel('compares (log)')
    axes[1, 0].set_ylabel('fraction decided within m bits\n'
                          '(ties never decided; vs-zero decided at m=0)')
    fig.suptitle(f'{prettify(case)} — compare margins by class', fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(out_pdf)
    plt.close(fig)


def animate_gif(hdr, frames, case, out_gif, fps=4):
    """GIF of the per-frame stored-vs-pop gap distributions."""
    x = np.arange(hdr.gap_bins)
    ymax = 1.0
    for fr in frames:
        ymax = max(ymax, float(stored_hist(fr).max()),
                   float(fr.gap['pop'].max()))
    fig, ax = plt.subplots(figsize=(9, 5))
    ln_s, = ax.plot([], [], color=C_STORED, lw=2, drawstyle='steps-mid',
                    label='stored content')
    ln_p, = ax.plot([], [], color=C_POP, lw=2, drawstyle='steps-mid',
                    label='pops this interval')
    ax.set_yscale('log')
    ax.set_xlim(-1, hdr.gap_bins)
    ax.set_ylim(0.5, ymax * 2)
    ticks, labels = gap_axis_ticks(hdr)
    ax.set_xticks(ticks)
    ax.set_xticklabels(labels)
    ax.set_xlabel('exponent gap g')
    ax.set_ylabel('count (log)')
    ax.grid(alpha=0.3)
    ax.legend(loc='upper right', fontsize=9)
    title = ax.set_title('')

    def update(i):
        fr = frames[i]
        for ln, h in ((ln_s, stored_hist(fr)), (ln_p, fr.gap['pop'])):
            y = h.astype(float)
            ln.set_data(x, np.where(y > 0, y, np.nan))
        title.set_text(f'{prettify(case)} — frame {i}: '
                       f'conflicts={fr.conflicts:,} heap={fr.heap_size:,}')
        return ln_s, ln_p, title

    anim = FuncAnimation(fig, update, frames=len(frames), blit=False)
    anim.save(str(out_gif), writer=PillowWriter(fps=fps))
    plt.close(fig)


# ---------------------------------------------------------------------------
# Sweep mode
# ---------------------------------------------------------------------------

def discover_heapdist(run_folder):
    """[(case, path)] across seed* subdirs (or flat); duplicate case names
    across seeds are disambiguated with @<seed_dir>."""
    run_folder = Path(run_folder)
    seed_dirs = sorted(d for d in run_folder.glob('seed*') if d.is_dir())
    search = seed_dirs if seed_dirs else [run_folder]
    found = []
    for d in search:
        for f in sorted(d.glob('*.heapdist')):
            case = f.name
            for suf in ('.stats.csv.heapdist', '.heapdist'):
                if case.endswith(suf):
                    case = case[:-len(suf)]
                    break
            found.append((case, f))
    names = defaultdict(int)
    for case, _ in found:
        names[case] += 1
    return [(case if names[case] == 1 else f'{case}@{f.parent.name}', f)
            for case, f in found]


def _box_strip(ax, groups, labels, colors, ylabel, title):
    """race_stats-style boxplot + deterministic jittered strip."""
    rng = random.Random(0)
    clean = [[v for v in g if not (isinstance(v, float) and math.isnan(v))]
             for g in groups]
    bp = ax.boxplot([g if g else [0] for g in clean], patch_artist=True,
                    showfliers=False)
    for patch, c in zip(bp['boxes'], colors):
        patch.set_facecolor(c)
        patch.set_alpha(0.45)
    for i, g in enumerate(clean):
        xs = [i + 1 + rng.uniform(-0.16, 0.16) for _ in g]
        ax.scatter(xs, g, s=14, color=colors[i], edgecolor='k',
                   linewidth=0.3, alpha=0.7, zorder=3)
    ax.set_xticks(range(1, len(labels) + 1))
    ax.set_xticklabels(labels, fontsize=9)
    ax.set_ylabel(ylabel)
    ax.set_title(title, fontsize=10)
    ax.grid(axis='y', alpha=0.3)


def plot_sweep_box(records, out_pdf):
    """Cross-instance distributions of the decision scalars."""
    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    _box_strip(axes[0, 0],
               [[r.get(f'stored_tail_ge{x}', float('nan')) for r in records]
                for x in (16, 32, 64)],
               ['g>=16', 'g>=32', 'g>=64'],
               [C_STORED] * 3,
               'fraction of stored copies',
               '(a) Stored-content cold-tail fraction')
    _box_strip(axes[0, 1],
               [[r.get(k, float('nan')) for r in records]
                for k in ('pop_gap_p50', 'pop_gap_p99', 'pop_gap_max')],
               ['p50', 'p99', 'max'],
               [C_POP] * 3,
               'gap g at percentile (504=ancient, 506=zero)',
               '(b) Pop-mass gap percentiles')
    _box_strip(axes[1, 0],
               [[r.get(f'margin_p99_{name}', float('nan')) for r in records]
                for name in MARGIN_HISTS],
               ['top4', 'onchip\nrest', 'OLC', 'pop\nmargin'],
               [CLASS_COLORS[name] for name in MARGIN_HISTS],
               'margin p99 (bits)',
               '(c) Compare-margin p99 by class')
    _box_strip(axes[1, 1],
               [[r.get(k, float('nan')) for r in records]
                for k in ('boundary_ins_per_kconf', 'boundary_sift_per_kconf')],
               ['inserts', 'sifts'],
               ['#4C72B0', '#DD8452'],
               'boundary crossings / 1000 conflicts',
               '(d) On/off-chip boundary flow rates')
    fig.suptitle('Heap activity distribution — cross-instance summary '
                 f'({len(records)} instances)', fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(out_pdf)
    plt.close(fig)


# ---------------------------------------------------------------------------
# Drivers
# ---------------------------------------------------------------------------

def run_instance(path, out_dir, animate=False, fps=4, final_window=1):
    """Per-instance pipeline: parse + all plots + summary. Returns outputs."""
    hdr, frames, warnings = parse_heapdist(path)
    for w in warnings:
        print(f'WARNING: {path}: {w}')
    if not frames:
        print(f'{path}: no complete frames — nothing to plot')
        return []
    case = Path(path).name
    for suf in ('.stats.csv.heapdist', '.heapdist'):
        if case.endswith(suf):
            case = case[:-len(suf)]
            break
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    outputs = []

    for fname, fn in ((f'{case}_heapdist_heatmaps.pdf', plot_heatmaps),
                      (f'{case}_heapdist_occ_vs_pop.pdf',
                       lambda h, f, c, p: plot_occupancy_vs_pop(
                           h, f, c, p, final_window=final_window)),
                      (f'{case}_heapdist_cold_fraction.pdf', plot_cold_fraction),
                      (f'{case}_heapdist_margins.pdf', plot_margins)):
        p = out_dir / fname
        fn(hdr, frames, case, p)
        outputs.append(p)
        print(f'Wrote {p}')
    if animate:
        p = out_dir / f'{case}_heapdist.gif'
        animate_gif(hdr, frames, case, p, fps=fps)
        outputs.append(p)
        print(f'Wrote {p}')

    summary = compute_summary(hdr, frames, case)
    p = out_dir / f'{case}_heapdist_summary.csv'
    write_summary_csv([summary], p)
    outputs.append(p)
    print(f'Wrote {p}')

    print(f'\n=== {case} summary ===')
    print(f"  frames={summary['frames']}  conflicts={summary['conflicts']:,}"
          f"  final heap={summary['heap_size_final']:,}"
          f" (live {summary['live_final']:,})")
    print(f"  stored tail fraction: g>=16 {summary['stored_tail_ge16']:.4f}"
          f"  g>=32 {summary['stored_tail_ge32']:.4f}"
          f"  g>=64 {summary['stored_tail_ge64']:.4f}")
    print(f"  live   tail fraction: g>=16 {summary['live_tail_ge16']:.4f}"
          f"  g>=32 {summary['live_tail_ge32']:.4f}"
          f"  g>=64 {summary['live_tail_ge64']:.4f}")
    print(f"  pop gap: p50={summary['pop_gap_p50']:.0f}"
          f"  p99={summary['pop_gap_p99']:.0f}"
          f"  max={summary['pop_gap_max']:.0f}"
          f"  (504=ancient, 506=zero)")
    print('  margin p99 (bits): ' + '  '.join(
        f"{name}={summary[f'margin_p99_{name}']:.0f}"
        if not math.isnan(summary[f'margin_p99_{name}']) else f'{name}=n/a'
        for name in MARGIN_HISTS))
    print(f"  boundary flow: {summary['boundary_ins_per_kconf']:.2f} ins/kconf"
          f"  {summary['boundary_sift_per_kconf']:.2f} sift/kconf"
          f"  ({100 * summary['boundary_ins_frac_of_inserts']:.2f}% of inserts)"
          if not math.isnan(summary['boundary_ins_frac_of_inserts']) else
          f"  boundary flow: {summary['boundary_ins_per_kconf']:.2f} ins/kconf"
          f"  {summary['boundary_sift_per_kconf']:.2f} sift/kconf")
    return outputs


def run_sweep(run_folder, out_dir, csv_path=None):
    """Sweep pipeline: per-instance scalars CSV + cross-instance boxplots."""
    files = discover_heapdist(run_folder)
    if not files:
        print(f'No *.heapdist found under {run_folder}')
        sys.exit(1)
    records = []
    for case, path in files:
        try:
            hdr, frames, warnings = parse_heapdist(path)
        except (ValueError, OSError) as e:
            print(f'SKIP {path}: {e}')
            continue
        for w in warnings:
            print(f'WARNING: {path}: {w}')
        if not frames:
            print(f'SKIP {path}: no complete frames')
            continue
        rec = compute_summary(hdr, frames, case)
        rec['check_issues'] = len(check_invariants(hdr, frames))
        records.append(rec)
    print(f'Found {len(files)} heapdist file(s); {len(records)} parsed')
    if not records:
        sys.exit(1)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_csv = Path(csv_path) if csv_path else out_dir / 'heapdist_summary.csv'
    write_summary_csv(records, out_csv)
    print(f'Wrote {out_csv}')
    out_pdf = out_dir / 'heapdist_sweep_box.pdf'
    plot_sweep_box(records, out_pdf)
    print(f'Wrote {out_pdf}')
    bad = [r['case'] for r in records if r['check_issues']]
    if bad:
        print(f'NOTE: {len(bad)} instance(s) had --check issues: '
              + ', '.join(bad[:5]) + ('...' if len(bad) > 5 else ''))
    return records, [out_csv, out_pdf]


# ---------------------------------------------------------------------------
# Selftest: synthetic writer + round-trip + every plot path
# ---------------------------------------------------------------------------

def make_synthetic_frames(hdr, n_frames=6, seed=0):
    """Deterministic synthetic frames that satisfy the --check invariants and
    exercise the edge cases: zero bin, ancient bin, empty level rows."""
    rng = np.random.default_rng(seed)
    gb, mb = hdr.gap_bins, hdr.margin_bins
    zero_b, anc_b = gb - 1, gb - 2
    mzero_b = mb - 3
    frames = []
    conflicts = 0
    for k in range(n_frames):
        conflicts += 10000 + int(rng.integers(0, 500))
        base = dict(conflicts=conflicts, decisions=conflicts * 3 + k,
                    cycle=conflicts * 1200 + k, rescales_total=k // 2,
                    var_inc_bits=struct.unpack(
                        '<Q', struct.pack('<d', 1.0 * (1.5 ** k)))[0])
        if k == 2:
            # Edge case: empty heap, zero level rows.
            frames.append(Frame(
                {n: np.zeros(gb, dtype=np.uint32) for n in GAP_HISTS},
                {n: np.zeros(mb, dtype=np.uint32) for n in MARGIN_HISTS},
                np.zeros((0, 8), dtype=np.uint32),
                heap_size=0, live_count=0, stale_count=0, live_pops=0,
                stale_pops=0, purge_pops=0, inserts_accepted=0,
                insert_skips=0, boundary_inserts=0, boundary_sifts=0,
                cmp_vs_empty=int(rng.integers(0, 50)), **base))
            continue

        n_levels = hdr.onchip_levels + 2 + (k % 2)
        mat = np.zeros((n_levels, gb), dtype=np.int64)
        for lvl in range(n_levels):
            cap = min(2 ** lvl, 200)
            n = int(rng.integers(cap // 2 + 1, cap + 1))
            bins = np.clip(rng.geometric(0.3, size=n) + lvl * 3, 0, 255)
            np.add.at(mat[lvl], bins, 1)
        # Edge cases: ancient + zero bins populated (deepest level; and one
        # zero-act copy on-chip in frame 1).
        mat[n_levels - 1, anc_b] += 5
        mat[n_levels - 1, zero_b] += 7
        if k == 1:
            mat[1, zero_b] += 2

        def cold(row, g_thr):
            return int(row[gap_bin_of(hdr, g_thr):].sum())

        rows, live_total = [], 0
        for lvl in range(n_levels):
            cnt = int(mat[lvl].sum())
            live = int(rng.integers(0, cnt + 1))
            live_total += live
            rows.append([lvl, cnt, live, int(mat[lvl, zero_b]),
                         cold(mat[lvl], 16), cold(mat[lvl], 32),
                         cold(mat[lvl], 64), cold(mat[lvl], 128)])
        levels = np.array(rows, dtype=np.uint32)
        heap_size = int(mat.sum())

        gap = {n: np.zeros(gb, dtype=np.uint32) for n in GAP_HISTS}
        gap['onchip_stored'] = mat[:hdr.onchip_levels].sum(0).astype(np.uint32)
        gap['offchip_stored'] = mat[hdr.onchip_levels:].sum(0).astype(np.uint32)

        # Live hist: live_count minus a small in-flight delta, with mass in
        # the zero + ancient bins too.
        delta = min(3, live_total)
        lh = np.zeros(gb, dtype=np.int64)
        lsum = live_total - delta
        if lsum > 0:
            bins = np.clip(rng.geometric(0.25, size=lsum), 0, 255)
            np.add.at(lh, bins, 1)
            hot = int(lh.argmax())
            for edge in (zero_b, anc_b):
                if lh[hot] > 1:
                    lh[hot] -= 1
                    lh[edge] += 1
        gap['live'] = lh.astype(np.uint32)

        live_pops = int(rng.integers(50, 200))
        ph = np.zeros(gb, dtype=np.int64)
        bins = np.clip(rng.geometric(0.5, size=live_pops - 1), 0, 12)
        np.add.at(ph, bins, 1)
        ph[anc_b] += 1  # a rare ancient pop
        gap['pop'] = ph.astype(np.uint32)

        boundary = {}
        for name, key in (('insert_boundary', 'boundary_inserts'),
                          ('sift_boundary', 'boundary_sifts')):
            n = int(rng.integers(20, 80))
            bh = np.zeros(gb, dtype=np.int64)
            bins = np.clip(rng.geometric(0.2, size=n) + 2, 0, 255)
            np.add.at(bh, bins, 1)
            gap[name] = bh.astype(np.uint32)
            boundary[key] = n

        margin = {}
        for name in MARGIN_HISTS:
            total = (live_pops - 2 if name == 'pop_margin'
                     else int(rng.integers(100, 400)))
            mh = np.zeros(mb, dtype=np.int64)
            ties = min(int(rng.integers(0, 10)), total)
            vs0 = min(int(rng.integers(0, 15)), total - ties)
            rest = total - ties - vs0
            mh[0] = ties
            mh[mzero_b] = vs0
            if rest > 0:
                bins = np.clip(rng.geometric(0.25, size=rest), 1, mzero_b - 1)
                np.add.at(mh, bins, 1)
            margin[name] = mh.astype(np.uint32)

        frames.append(Frame(
            gap, margin, levels,
            heap_size=heap_size, live_count=live_total,
            stale_count=heap_size - live_total, live_pops=live_pops,
            stale_pops=int(rng.integers(0, 40)),
            purge_pops=int(rng.integers(0, 10)),
            inserts_accepted=int(rng.integers(500, 2000)),
            insert_skips=int(rng.integers(0, 100)),
            cmp_vs_empty=int(rng.integers(0, 300)),
            **boundary, **base))
    return frames


def run_selftest(out_dir=None):
    out = Path(out_dir) if out_dir else Path(
        tempfile.mkdtemp(prefix='plot_heap_dist_selftest_'))
    out.mkdir(parents=True, exist_ok=True)
    print(f'selftest dir: {out}')
    hdr = FileHeader(version=1, flags=0, num_vars=4000, onchip_levels=4,
                     gap_bins=258, margin_bins=68, gap_step=2, gap_min=-8)
    frames = make_synthetic_frames(hdr, n_frames=6, seed=0)

    # 1. Round trip: serialize -> parse -> deep equality.
    p_full = out / 'caseA.stats.csv.heapdist'
    blob = serialize_heapdist(hdr, frames)
    p_full.write_bytes(blob)
    hdr2, frames2, warns = parse_heapdist(p_full)
    assert not warns, f'unexpected warnings: {warns}'
    assert hdr2 == hdr, 'file header round-trip mismatch'
    assert len(frames2) == len(frames)
    for a, b in zip(frames, frames2):
        assert a.equals(b), 'frame round-trip mismatch'
    assert frames2[2].heap_size == 0 and len(frames2[2].levels) == 0, \
        'empty-level-rows edge frame lost'
    assert frames2[3].gap['offchip_stored'][hdr.gap_bins - 2] > 0, 'no ancient mass'
    assert frames2[3].gap['offchip_stored'][hdr.gap_bins - 1] > 0, 'no zero mass'
    assert abs(frames2[3].var_inc - 1.5 ** 3) < 1e-12, 'var_inc f64 round-trip'
    print('PASS round-trip (6 frames, incl. zero/ancient bins + empty level rows)')

    # 2. Truncated final frame: tolerated with a warning, other frames intact.
    p_trunc = out / 'caseB.stats.csv.heapdist'
    p_trunc.write_bytes(blob[:-1000])
    hdr3, frames3, warns3 = parse_heapdist(p_trunc)
    assert len(frames3) == len(frames) - 1, 'truncated frame not dropped'
    assert any('truncated' in w for w in warns3), 'no truncation warning'
    for a, b in zip(frames[:-1], frames3):
        assert a.equals(b)
    print('PASS truncated-final-frame tolerance (warning emitted)')

    # 3. Bad magic rejected.
    p_bad = out / 'bad.heapdist'
    p_bad.write_bytes(b'XXXX' + blob[4:])
    try:
        parse_heapdist(p_bad)
        raise AssertionError('bad magic accepted')
    except ValueError:
        pass
    p_bad.unlink()
    print('PASS bad-magic rejection')

    # 4. Invariant checks clean on the synthetic frames; violations detected.
    issues = check_invariants(hdr, frames2)
    assert not issues, f'--check issues on synthetic data: {issues}'
    import copy
    broken = copy.deepcopy(frames2)
    broken[1].heap_size += 10000
    assert check_invariants(hdr, broken), 'heap_size violation not detected'
    print('PASS --check invariants (clean on good data, catches violations)')

    # 5. Every per-instance plot path (incl. --animate) on the full file.
    outputs = run_instance(p_full, out, animate=True, fps=8, final_window=2)
    for p in outputs:
        assert p.exists() and p.stat().st_size > 0, f'missing output {p}'
    assert any(p.suffix == '.gif' for p in outputs), 'no GIF produced'
    print(f'PASS per-instance plots + GIF ({len(outputs)} outputs)')

    # 6. Sweep mode over a synthetic run folder (incl. the truncated file).
    rundir = out / 'sweeprun' / 'seed0'
    rundir.mkdir(parents=True, exist_ok=True)
    (rundir / 'caseA.stats.csv.heapdist').write_bytes(blob)
    (rundir / 'caseB.stats.csv.heapdist').write_bytes(blob[:-1000])
    records, sweep_outputs = run_sweep(out / 'sweeprun', out / 'sweeprun')
    assert len(records) == 2, f'sweep parsed {len(records)} != 2 instances'
    for p in sweep_outputs:
        assert p.exists() and p.stat().st_size > 0, f'missing output {p}'
    print('PASS sweep aggregate (CSV + boxplots)')

    print('\nSELFTEST PASSED')
    return 0


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description='Parse & plot PipelinedHeap activity-distribution '
                    'profiles (.heapdist, spec: plans/pheap-act-dist.md)')
    ap.add_argument('path', nargs='?',
                    help='.heapdist file (per-instance) or run folder (sweep)')
    ap.add_argument('--out-dir', default=None,
                    help='Output directory (default: alongside the input)')
    ap.add_argument('--csv', default=None,
                    help='Sweep mode: path for the per-instance summary CSV')
    ap.add_argument('--animate', action='store_true',
                    help='Also render a per-frame GIF (per-instance mode)')
    ap.add_argument('--fps', type=int, default=4, help='GIF frames per second')
    ap.add_argument('--final-window', type=int, default=1,
                    help='Frames in the "final window" overlay panel')
    ap.add_argument('--check', action='store_true',
                    help='Run invariant checks only (exit 1 on violation)')
    ap.add_argument('--selftest', action='store_true',
                    help='Synthetic round-trip + exercise every plot path')
    args = ap.parse_args()

    if args.selftest:
        sys.exit(run_selftest(args.out_dir))
    if not args.path:
        ap.error('path is required (or use --selftest)')
    path = Path(args.path)
    if not path.exists():
        print(f'Error: {path} not found')
        sys.exit(1)

    if path.is_dir():
        run_sweep(path, Path(args.out_dir) if args.out_dir else path, args.csv)
        return

    if args.check:
        hdr, frames, warnings = parse_heapdist(path)
        for w in warnings:
            print(f'WARNING: {w}')
        issues = check_invariants(hdr, frames)
        for msg in issues:
            print(f'CHECK FAIL: {msg}')
        if issues:
            print(f'{path}: {len(issues)} invariant violation(s)')
            sys.exit(1)
        print(f'{path}: CHECK PASSED ({len(frames)} frames)')
        return

    run_instance(path, Path(args.out_dir) if args.out_dir else path.parent,
                 animate=args.animate, fps=args.fps,
                 final_window=args.final_window)


if __name__ == '__main__':
    main()
