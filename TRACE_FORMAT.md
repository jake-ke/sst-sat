# SST SAT Solver Binary Trace Format

Version: **1.0**

This document specifies the on-disk format of the binary memory-access traces
produced by the SST SAT solver simulator when the `trace_file` parameter is
set. It is the canonical reference for downstream analysis tools. Any
additive format change bumps the `version=` line and adds a Changelog entry.

## 1. Overview

A trace captures, for a single `(CNF, random_seed)` simulation run:

- **Every simulated memory request** issued by any data-structure subsystem
  (clauses, watches, variables, heap, var-activity), with address, size,
  read/write direction, and a data-structure tag. Cache-line-sized chunks of
  burst reads are emitted as individual events, matching what the memHierarchy
  actually sees.
- **Algorithm events** interleaved in causal order: `DECIDE`, `ENQUEUE`,
  `CONFLICT`, `LEARN`, `BACKTRACK`, `RESTART`, `REDUCE`.
- **Static context changes**: `PHASE` (solver FSM state), `LEVEL` (current
  decision level), `TICK` (simulated cycle). These are emitted only on change;
  memory events inherit the most recent values.

Target scale is billions of events per run. The format is designed around
~2–4 bytes per memory event with varint encoding.

What the trace does **not** capture:

- Worker-id / parallel coroutine boundaries.
- Flags distinguishing store-queue-forwarded reads, burst chunks, speculative
  accesses. (Additive `v1.x` extensions are reserved for these.)

## 2. File layout

```
+-----------------------------------------------------+
| 8-byte magic:  'S','S','T','S','A','T',0,0          |
| Text header lines:                                  |
|   version=1.0\n                                     |
|   cnf=<absolute path>\n                             |
|   seed=<u64>\n                                      |
|   num_vars=<u32>\n                                  |
|   num_clauses=<u32>\n                               |
|   ---\n             <- marker: binary stream starts |
| Binary event stream:                                |
|   ...records until a FINISH record...               |
+-----------------------------------------------------+
```

Readers must accept **extra unknown header keys** for forward compatibility
and stop header parsing at the literal `---\n` line.

## 3. Encoding primitives

### 3.1 Unsigned varint (LEB128)

Little-endian, 7 bits per byte. Continuation bit (0x80) set on all bytes
except the last. Max 10 bytes for u64.

```
encode(v):
    while v >= 0x80:
        emit((v & 0x7f) | 0x80)
        v >>= 7
    emit(v)

decode():
    result = 0; shift = 0
    while True:
        b = read_byte()
        result |= (b & 0x7f) << shift
        if (b & 0x80) == 0: return result
        shift += 7
```

Worked example: `300` → `0xAC, 0x02`.

### 3.2 Signed varint (zigzag)

```
zigzag_encode(n) = (n << 1) ^ (n >> 63)   # signed shift
zigzag_decode(u) = (u >> 1) ^ -(u & 1)
```

Worked examples: `0 → 0x00`, `-1 → 0x01`, `+1 → 0x02`, `-2 → 0x03`.

## 4. Event stream

Each record starts with a **1-byte tag**. The reader dispatches on the tag to
decode the body.

| Tag    | Name       | Frequency | Body                                                                        |
| ------ | ---------- | --------- | --------------------------------------------------------------------------- |
| `0x00` | TICK       | rare      | uvarint `cycle_delta` (from previous `TICK`)                                |
| `0x01` | PHASE      | rare      | u8 `phase`                                                                  |
| `0x02` | LEVEL      | rare      | uvarint `level` (absolute)                                                  |
| `0x10` | MEM_READ   | hot       | packed byte + svarint addr delta + optional size                            |
| `0x11` | MEM_WRITE  | hot       | same                                                                        |
| `0x20` | DECIDE     | rare      | uvarint `var`, u8 `sign`, uvarint `new_level`                               |
| `0x21` | ENQUEUE    | medium    | uvarint `var`, u8 `sign`, svarint `reason_cref` (`-1` = decision)           |
| `0x22` | CONFLICT   | rare      | uvarint `cref`                                                              |
| `0x23` | LEARN      | rare      | uvarint `lbd`, uvarint `clause_size`, uvarint `bt_level`, uvarint `new_cref`|
| `0x24` | BACKTRACK  | rare      | uvarint `from_level`, uvarint `to_level`                                    |
| `0x25` | RESTART    | rare      | uvarint `restart_idx`                                                       |
| `0x26` | REDUCE     | rare      | uvarint `removed`, uvarint `kept`                                           |
| `0x7f` | FINISH     | once      | u64 LE `total_cycles`, u64 LE `events_written`, u32 LE `crc32`              |

Unknown tags abort the reader. The `version=` header field lets a reader
detect incompatible streams early.

### 4.1 Memory event byte layout

A `MEM_READ` / `MEM_WRITE` record is:

```
byte 0:  tag            0x10 = read, 0x11 = write
byte 1:  packed byte    bits 7..7 reserved (0), bits 6..4 = size_class, bits 3..0 = ds_id
bytes 2..: svarint addr_delta     (zigzag varint of addr - last_addr_per_ds[ds])
[optional]: uvarint size          (only if size_class == 7)
```

- **size_class** (3 bits):

  | value | size (bytes) |
  | ----- | ------------ |
  | 0     | 1            |
  | 1     | 2            |
  | 2     | 4            |
  | 3     | 8            |
  | 4     | 16           |
  | 5     | 32           |
  | 6     | 64           |
  | 7     | explicit uvarint follows the address delta |

- **ds_id** (4 bits): index into the DS table (see §6).

Following the packed byte is a **zigzag varint address delta** relative to
`last_addr_per_ds[ds_id]`. The reader must maintain a 9-slot
`uint64_t last_addr[9]` array initialized to zero and update
`last_addr[ds_id] += svarint` on each memory event.

If `size_class == 7`, an unsigned varint with the explicit size follows.

**Example**: the first memory event in a trace is a 64-byte read from
`0x30000040` (DS_WATCHES, id = 3). `last_addr[3]` starts at 0 so the delta is
`+0x30000040`, zigzag-encoded as `0x60000080`. The encoded bytes are:

```
0x10                    # tag = MEM_READ
0x63                    # packed: size_class=6 (64B), ds_id=3
0x80 0x81 0x80 0x80 0x06 # svarint zigzag(0x30000040)
```

The next 64-byte read from `0x30000080` produces:

```
0x10
0x63
0x80 0x01              # svarint zigzag(+0x40) = 0x80
```

— 4 bytes total because the per-DS delta packs into 2 bytes.

## 5. Reader state

A streaming reader maintains four pieces of state:

```
current_phase : u8 = 0
current_level : u32 = 0
current_cycle : u64 = 0
last_addr     : u64[9] = {0}
```

Update rules:

- `TICK`:     `current_cycle += cycle_delta`
- `PHASE`:    `current_phase = phase`
- `LEVEL`:    `current_level = level`
- `MEM_*`:    `last_addr[ds_id] += zigzag_svarint`; yield
              `(current_cycle, current_phase, current_level, ds_id, addr=last_addr[ds_id], size, is_write)`
- all others: yield the decoded algorithm event

Memory events are decoded sequentially (per-DS delta), so random-access
decoding requires replaying from the last known address for the target DS.
Analysis tools typically stream linearly.

Mid-tick phase transitions: a phase change that happens mid-`clockTick`
materialises as a `PHASE` record at the *start* of the following tick.
Memory events emitted in the tail of the originating tick carry the
previous phase label. Sub-tick resolution is not supported in v1.

## 6. Enum reference

### 6.1 `DsId` (4 bits)

| id | name          | base address (default) | backing data structure               |
| -- | ------------- | ---------------------- | ------------------------------------ |
| 0  | heap          | `0x00000000`           | external VSIDS heap / pipelined heap |
| 1  | indices       | `0x10000000`           | heap-index array (classic heap only) |
| 2  | variables     | `0x20000000`           | `Variable` struct array              |
| 3  | watches       | `0x30000000`           | per-literal watch-list metadata      |
| 4  | watch_nodes   | `0x40000000`           | watcher-block linked list            |
| 5  | clauses_cmd   | `0x50000000`           | clause offset table                  |
| 6  | clauses       | `0x60000000`           | clause payload (literals + activity) |
| 7  | var_activity  | `0x70000000`           | VSIDS per-variable activity          |
| 8  | unknown       | —                      | addresses outside the above ranges   |

Base addresses are configurable via the SST Python driver; the v1.0 format
assumes the standard layout. If you mix traces from runs with different base
addresses, normalise to the DS id + per-DS offset.

### 6.2 `SolverState` → `PHASE` (u8)

Matches the `SolverState` enum in `src/satsolver.h`. **Regenerate this table
if the enum in `satsolver.h` changes.**

| id | name       |
| -- | ---------- |
| 0  | IDLE       |
| 1  | INIT       |
| 2  | STEP       |
| 3  | PROPAGATE  |
| 4  | DECIDE     |
| 5  | ANALYZE    |
| 6  | MINIMIZE   |
| 7  | BTLEVEL    |
| 8  | BACKTRACK  |
| 9  | REDUCE     |
| 10 | RESTART    |
| 11 | WAIT_HEAP  |
| 12 | DONE       |

Note: `STEP` is a transient response-routing state and is never emitted as a
`PHASE` value by the writer — `saved_state` is emitted instead.

## 7. Invariants

A valid v1.0 trace satisfies:

1. Magic bytes are `SSTSAT\0\0`.
2. Header is terminated by a `---\n` line.
3. Exactly one `FINISH` record appears, and it is the last record in the
   file.
4. `FINISH.crc32` equals the CRC-32 (IEEE 802.3, reflected, initial
   `0xFFFFFFFF`, final XOR `0xFFFFFFFF`) computed over every byte of the
   event stream *up to but not including* the FINISH tag.
5. `TICK` cycles are monotonic non-decreasing (cycle deltas are unsigned).
6. `LEVEL` decreases only after a preceding `BACKTRACK` or `RESTART`.
7. `DECIDE` increments `LEVEL` by exactly 1.
8. `#DECIDE`, `#CONFLICT`, `#LEARN`, `#RESTART`, `#REDUCE` in the trace
   match the corresponding counters in `stats.csv` produced by the same
   run.

## 8. Reference reader pseudocode

```python
def read_trace(path):
    with open(path, 'rb') as f:
        magic = f.read(8)
        assert magic == b'SSTSAT\0\0'
        header = {}
        line = b''
        while True:
            line = f.readline()
            if line == b'---\n':
                break
            k, _, v = line.decode().strip().partition('=')
            header[k] = v

        cur_phase = 0
        cur_level = 0
        cur_cycle = 0
        last_addr = [0]*9

        while True:
            tag = f.read(1)
            if not tag: break
            t = tag[0]
            if t == 0x00:
                cur_cycle += uvarint(f)
            elif t == 0x01:
                cur_phase = f.read(1)[0]
            elif t == 0x02:
                cur_level = uvarint(f)
            elif t in (0x10, 0x11):
                packed = f.read(1)[0]
                size_class = (packed >> 4) & 0x7
                ds = packed & 0xF
                last_addr[ds] += svarint(f)
                addr = last_addr[ds]
                size = [1,2,4,8,16,32,64,None][size_class]
                if size is None:
                    size = uvarint(f)
                yield ('mem', cur_cycle, cur_phase, cur_level, ds, addr, size, t == 0x11)
            elif t == 0x20:
                var = uvarint(f); sign = f.read(1)[0]; lvl = uvarint(f)
                yield ('decide', var, sign, lvl)
            elif t == 0x21:
                var = uvarint(f); sign = f.read(1)[0]; reason = svarint(f)
                yield ('enqueue', var, sign, reason)
            elif t == 0x22:
                yield ('conflict', uvarint(f))
            elif t == 0x23:
                lbd = uvarint(f); sz = uvarint(f); btl = uvarint(f); cref = uvarint(f)
                yield ('learn', lbd, sz, btl, cref)
            elif t == 0x24:
                yield ('backtrack', uvarint(f), uvarint(f))
            elif t == 0x25:
                yield ('restart', uvarint(f))
            elif t == 0x26:
                yield ('reduce', uvarint(f), uvarint(f))
            elif t == 0x7f:
                total_cycles = int.from_bytes(f.read(8), 'little')
                events = int.from_bytes(f.read(8), 'little')
                crc = int.from_bytes(f.read(4), 'little')
                yield ('finish', total_cycles, events, crc)
                return
            else:
                raise ValueError(f'unknown tag {t:#x}')
```

## 9. Analysis recipes

### 9.1 Representation 1 — access sequence (string-like)

Stream the trace and emit one symbol per memory event. The symbol alphabet
can be `(ds_id, size_class)` pairs for a 56-symbol alphabet, or
`(ds_id, offset bucket)` for finer locality. Use standard motif discovery,
suffix array, or compression-based complexity measures.

### 9.2 Representation 2 — wave sets

Segment the event stream at each `BACKTRACK` or `RESTART` record. Within
each segment ("wave"), collect the set of unique `(ds_id, cache-line-aligned
addr)` tuples — this is the working set of that wave. Use set-similarity
(Jaccard, frequent-itemset mining) across waves or across runs.

### 9.3 Representation 3 — co-access graph

Nodes: `(ds_id, addr)` tuples (or cache-line-aligned tuples to cap the
graph size). Edge weight between `u` and `v`: number of times `u` and `v`
appear within `W` events of each other. Apply PageRank / community
detection / transition entropy.

### 9.4 Representation 4 — phase-annotated channels

Split the event stream into N streams (one per `PHASE` value). Compute
per-phase histograms of DS access counts, cache-line reuse, and
phase-transition working-set differences. Transfer-entropy between phase
channels measures inter-phase sharing.

## 10. Cross-trace conventions

Recommended file name: `<cnf_stem>.<seed>.trace.bin`. Keep `stats.csv`
alongside each trace in the same directory. A database-level `config.txt`
captures the build knobs and base addresses that are constant across the
database.

## 11. Changelog

- **1.0** — initial format: text header + tagged binary event stream with
  per-DS address delta encoding. No worker-id, no flags.
