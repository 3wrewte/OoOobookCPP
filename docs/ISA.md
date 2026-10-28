# Matcher Core ISA

Encoding of the three operation levels and the address/cache layout for the
`exchange-matcher-core` pipeline. This is the software analogue of an HDL ISA
spec; the same formats are intended to translate directly to SystemVerilog.

The pipeline:

```
OP ROB (vector planner)  ->  mOP ROB (vector planner)  ->  uOP ROB (per-pool OoO)
       |                          |                            |
       | emit mOPs (batch)        | emit uOPs (batch)          | exec (read-only)
       v                          v                            v
  OP decode                   mOP decode                   id-vol cache (RAM)
  virtual level-sum           virtual line-sum                |
  + mOP pending delta         + uOP pending delta             | apply_mutation (commit)
       |                          |                            v
       v                          v                   RAM + line-sum + level-sum + meta
  commit (in OP order)        commit (in mOP order)
```

- OP and mOP ROBs use **vector planners**: each tick the planner scans
  multiple entries in age order, decoding each into child operations.  An OP
  emits up to `OP_VECTOR_SLOT_WIDTH` (= 3) mOPs per tick; an mOP emits up to
  `MOP_VECTOR_WIDTH` (= 8) uOPs per tick.  Partially-planned entries store a
  continuation and resume on a later tick.
- uOP ROB issues **out of order** from three physical pools (Insert / Trade /
  Cancel) with configurable per-pool issue widths. Same-line TakeLine
  forwarding allows up to `SAME_LINE_TAKE_DEPTH` (= 4) TakeLine uOPs on one
  id-vol line to execute in the same tick via store-to-load bypass.
  CancelSlot still uses strict same-line blocking. InsertSlot has no hazard
  check but sets a same-line gap that blocks younger TakeLines until it
  commits.
- uOP execution is split into **exec** (read-only, computes result + mutation)
  and **apply_mutation** (writes canonical RAM + caches at commit time).
  Writeback forwards the result to the parent mOP independently of canonical
  commit, so the decoder can continue planning without waiting for commit.
- All three ROBs commit in order.  uOP commit is OP-ordered but may advance
  past the architectural head into a **canonical commit frontier** — a safe
  window of fully-materialized OPs whose mOP and uOP streams are closed.
  mOP and OP commit in age order.
- Monotonic sequence numbers (`op_seq`, `mop_seq`, `uop_seq`) are assigned at
  allocation and used for commit ordering and result routing.

---

## 1. Address layout

### 1.1 Canonical order address (64 bits)

```
 MSB                                                                  LSB
 +-------+-----------+------------+-----+------------+----------+
 | sym   | pos[27:4] | price[27:4]| B/S | price[3:0] | pos[3:0] |
 | 7 b   | 24 b      | 24 b       | 1 b | 4 b        | 4 b      |
 +-------+-----------+------------+-----+------------+----------+
   sym=0 for now (reserved for multi-symbol routing)
```

Placement is chosen so the id-vol cache decomposes into contiguous slices. The
side bit lives inside the index, so Buy and Sell occupy disjoint index ranges
within one shared cache array (two side-caches in one structure).

- `pos`   = FIFO/time position of an order within its price level (28 bits).
- `price` = price level, non-negative (28 bits).

### 1.2 id-vol cache (direct-mapped; one slot = one order)

```
 tag = bits[63:9]  = { sym, pos[27:4], price[27:4] }            55 bits
 index = bits[8:4] = { B/S, price[3:0] }                         5 bits  -> 32 lines
 offset = bits[3:0]= pos[3:0]                                     4 bits  -> 16 slots/line
```

One cache line = 16 consecutive FIFO positions at one `(sym, price, side,
pos[27:4])`. Slot payload:

```
 Slot { order_id: u32, volume: u32 }      // hole = {0,0}
```

Holes (never-written or cancel-cleared slots) are left in place; the line is
never compacted. RAM is line-keyed (key = packed address with offset cleared)
and is the canonical truth; the cache is a direct-mapped write-through window
over it.

**B4 note**: in the current execute/commit split, `exec_*` functions read
canonical RAM directly (bypassing the cache window), and `apply_mutation`
merges results back.  The direct-mapped window is therefore exercised only on
the apply path, not on the exec path.  A future set-associative + prefetch
design would make the exec read path use the cache window.

### 1.3 line-sum cache (direct-mapped; one slot = one id-vol line's total)

```
 tag    = { sym, price[27:4], pos[27:8] }                       51 bits
 index  = { B/S, price[3:0] }                                     5 bits -> 32 lines
 offset = pos[7:4]                                                4 bits -> 16 slots/line
```

- One slot stores the total resting volume of one id-vol line (selected by
  `pos[27:4] = pos[27:8] ++ pos[7:4]`).
- One cache line holds 16 such sums (varying `pos[7:4]`, fixed `pos[27:8]`),
  i.e. a 256-order block at one price/side.
- Read-only mirror; re-synced on every canonical id-vol slot mutation
  (delta-updated at `apply_mutation` time).
- `read_row_ram` reads an entire 16-entry row in one RAM access (used by the
  mOP vector planner for batch TakeLine planning).

### 1.4 level-sum cache (direct-mapped; one slot = one price level's total)

```
 tag    = { sym, price[27:8] }                                  27 bits
 index  = { B/S, price[7:4] }                                     5 bits -> 32 lines
 offset = price[3:0]                                              4 bits -> 16 slots/line
```

- One slot stores the total resting volume at one `(sym, price, side)`.
- One cache line holds 16 consecutive prices' totals (varying `price[3:0]`).
- Read-only mirror; re-synced on every canonical id-vol slot mutation.
- `read_row_ram` reads an entire 16-price row in one RAM access (used by the
  OP vector planner for batch TakeLevel planning).

### 1.5 Virtual (pending-delta) sum overlays

The decoders plan against a **virtual book** = canonical book + pending deltas
from not-yet-committed child operations.  This lets the vector planner continue
emitting child operations without waiting for earlier children to commit.

Two layers of pending delta exist:

```
uOP pending deltas  (in UopRob):
  pending_line_deltas : HashMap<line_key, [i64; 16]>
  pending_level_deltas: HashMap<level_key, [i64; 16]>

  Applied at uOP allocate time (planned_delta per uOP kind).
  Removed at canonical commit time.

mOP pending deltas  (in MopStage):
  per-slot planned_delta - delegated_delta  (net uncommitted delta per mOP)

  Applied at mOP push time (from the OP vector planner).
  Removed at mOP commit time.
```

Consumed via:

- `virtual_line_sum_row(addr)` = canonical line_sum row + uOP pending line deltas.
- `virtual_level_sum_row(addr)` = canonical level_sum row + uOP pending level deltas.
- `pending_net_delta(level_key)` = net mOP planned delta at a level.
- `best_pending_buy_rest_price()` / `best_pending_sell_rest_price()` = best
  pending rest price from mOP slots.
- `best_possible_opponent(...)` = min/max of canonical best + uOP pending +
  mOP pending, filtered by limit.

### 1.6 Price metadata backing RAM + price-group summary

`PriceMetaRAM` is the complete per-price backing store for hot metadata:

```text
PriceMeta[(sym, side, price)] {
    level_sum : u64,
    head_line : u32,
    head_valid: bool,
    tail_pos  : u32,
    active    : bool,
}
```

It coexists with the single-cycle L0 caches (`level_sum`, `head`, `tail`). The
L0 caches are small and direct-mapped; `PriceMetaRAM` is complete for every
price level that has ever carried resting interest, so cache misses can refill
without a tree lookup.

Active-price walks use FPGA-friendly summaries instead of a red-black tree:

```text
price[3:0]    -> bit inside one 16-price row
price[9:4]    -> row bit inside one 64-row block
price[15:10]  -> block bit inside one 64-block super-block
price[27:16]  -> super-block index
```

`next_ask_after` / `next_bid_below` first consult these masks, then priority
encode the row mask.

---

## 2. OP (one inbound order)

Parent unit of work. One OP per inbound New/Cancel. Each OP carries a context
(`OpContext`) with exchange-level fields (seq, account/client/contract ids)
and an `op_seq` assigned at ROB allocation.

```
OP::New {
    order_id   : u32,       // internal order id; also stored in the resting slot
    side       : Side,      // Buy | Sell
    price      : u32,       // price level (28 bits used); ignored when market
    qty        : u32,       // requested quantity
    is_market  : bool,      // true = market order
}
OP::Cancel {
    account_id       : AccountId,
    client_order_id  : ClientOrderId,
    // resolved at decode time: (account_id, client_order_id) -> order_id -> addr
}
```

### 2.1 Market vs limit

A market order sweeps the entire opposite side regardless of price. It is
implemented by substituting an extreme limit inside the OP decoder:

```
limit = is_market
      ? (side == Buy ? u32::MAX : 0)
      : price;
```

When the walk terminates (no more opponent prices), a limit order transitions
to rest; a market order finishes immediately and discards any unfilled
remainder.

### 2.2 OP vector planner

The OP vector planner scans OP slots in age order each tick. For each
non-`emit_done` OP it calls `plan_lane`, which classifies the OP and produces
up to `OP_VECTOR_SLOT_WIDTH` (= 3) mOPs:

| OP input                          | mOPs emitted                                                                     |
|-----------------------------------|----------------------------------------------------------------------------------|
| `New` limit, crossing             | one or more `TakeLevel`(opponent, ascending price, up to limit); if remainder > 0 after last crossing price, one `RestInsert`(own side, own price, remainder) |
| `New` limit, non-crossing         | one `RestInsert`(own side, own price, qty)                                       |
| `New` market, opponent non-empty  | one or more `TakeLevel`(opponent, ascending price, **no upper bound**); remainder discarded, **no** `RestInsert` |
| `New` market, opponent empty      | zero mOPs; OP completes immediately (`filled=0`, no rest)                        |
| `Cancel`                          | one `CancelLevel`(resolved addr, order_id, volume)                               |

Crossing detection uses `best_possible_opponent` = canonical best + uOP pending
+ mOP pending, so a younger OP sees the virtual book including older OPs'
not-yet-committed effects. Within one vector pass, earlier lanes' deltas are
visible to later lanes via a local overlay.

If a crossing OP's sweep exceeds the lane budget, a `Continuation` is stored
and the OP resumes on a later tick once its emitted mOPs have committed
(ensuring virtual == canonical at resume time).

OP submit is fenced on uOP canonical commit via the **canonical commit
frontier**: `canonical_commit_frontier(mop_stage)` returns the inclusive
op_seq of the youngest OP whose mOP/uOP streams are fully materialised. uOP
commit may advance through this window (in strict `uop_seq` order). BBO
snapshots are captured per-OP at the last uOP's commit so per-OP tick output
remains correct.

---

## 3. mOP (price-level operation)

Parent = OP. Pushed into the mOP ROB by the OP vector planner.

```
mOP::TakeLevel {
    parent_op  : OpId,
    side       : Side,        // opposite side being consumed
    price      : u32,         // single price level
    qty_budget : u32,         // how much this mOP may consume at this price
}
mOP::RestInsert {
    parent_op : OpId,
    order_id  : u32,
    side      : Side,         // own side
    price     : u32,          // own price
    qty       : u32,          // full remaining qty to rest
}
mOP::CancelLevel {
    parent_op : OpId,
    order_id  : u32,
    addr      : OrderAddr,    // resolved by OP decode via the order_id -> addr index
    volume    : u32,          // known at decode time (from resting index)
}
```

### 3.1 mOP vector planner

The mOP vector planner scans mOP slots in age order each tick. For each
non-`emit_done` mOP it calls `plan_mop_lane`, producing up to
`MOP_VECTOR_WIDTH` (= 8) uOPs:

| mOP input     | uOPs emitted                                                              |
|---------------|---------------------------------------------------------------------------|
| `TakeLevel`   | one `TakeLine` per non-empty id-vol line at `price` (ascending `pos[27:4]`), each carrying the remaining `qty_budget`; batch emission uses `virtual_line_sum_row` to skip empty lines in one read |
| `RestInsert`  | exactly one `InsertSlot` at `effective_tail(level)` (canonical tail + pending inserts at this level) |
| `CancelLevel` | one `CancelSlot` at `addr` (carrying known order_id + volume)             |

Partially-planned TakeLevel mOPs store a `MopContinuation { planned_remaining, next_line }`
and resume on a later tick.

Per-slot take accumulation: each `MopSlot` carries its own `take_consumed` and
`take_trades`, replacing the earlier global decoder state. This allows
multiple TakeLevel mOPs to accumulate results independently.

---

## 4. uOP (line / slot operation)

Parent = mOP. Issued out of order from three physical pools. **Each uOP
executes in two phases**:

1. **Exec** (at issue time): read-only against canonical RAM, computes a
   fixed-size result bundle + a `UopMutation` (old/new line snapshot). Stored
   in the ROB entry; canonical book is untouched.
2. **Apply** (at commit time): `apply_mutation` merges the mutation into
   canonical RAM + cache window + sum caches + tail + meta.

```
uOP::TakeLine {
    parent_mop : MopId,
    op_seq     : OpId,
    addr       : OrderAddr,   // any address inside the target line (offset ignored)
    budget     : u32,
}
uOP::InsertSlot {
    parent_mop : MopId,
    op_seq     : OpId,
    addr       : OrderAddr,   // full address including offset
    order_id   : u32,
    volume     : u32,
}
uOP::CancelSlot {
    parent_mop : MopId,
    op_seq     : OpId,
    addr       : OrderAddr,   // full address including offset
    order_id   : u32,
    volume     : u32,         // known from resting index (no read-modify-write)
}
```

### 4.1 Result bundles (returned via writeback to the parent mOP)

```
TakeLineResult {
    traded_ids   : [u32; 16],   // maker order id touched at each offset (0 = none)
    traded_vols  : [u32; 16],   // volume taken from each offset
    consumed     : u32,         // total volume consumed
    remaining_budget : u32,     // 0 == budget fully satisfied
    line_remaining   : u64,     // volume still resting in this line after the take
}
InsertSlotResult { /* none beyond completion */ }
CancelSlotResult {
    found          : bool,
    order_id       : u32,
    volume_removed : u32,
}
```

### 4.2 uOP ROB structure

Three physical pools, one logical in-order pipeline:

| Pool   | Size | Contents        | Hazard check          |
|--------|------|-----------------|-----------------------|
| Insert | 16   | `InsertSlot`    | none (writes known slots); sets same-line gap blocking younger TakeLines |
| Trade  | 16   | `TakeLine`      | same-line forwarding up to depth 4; InsertSlot gap blocks until insert commits |
| Cancel | 256  | `CancelSlot`    | strict same-line (any older same-line entry blocks) |

Allocation assigns a global `uop_seq` (monotonic across all pools). Per-pool
issue widths are configurable (`IssueWidths { insert, trade, cancel }`).

---

## 5. ROB and execution model

| ROB     | Alloc       | Issue                         | Execute                      | Writeback         | Commit (retire)               |
|---------|-------------|-------------------------------|------------------------------|-------------------|-------------------------------|
| OP ROB  | in order    | vector planner (multi-OP)     | OP decode (batch mOP emit)   | —                 | in order (multi-submit)       |
| mOP ROB | in order    | vector planner (multi-mOP)    | mOP decode (batch uOP emit)  | —                 | in order (multi-submit)       |
| uOP ROB | per-pool    | **OoO** (per-pool multi-issue + same-line forwarding)| exec (read-only) + forward | forwards result   | frontier-ordered (multi-commit) |

### 5.1 Per-tick pipeline

```
1. uOP writeback   — forward up to W executed results to mOP stage
2. uOP commit      — apply up to W mutations to canonical (frontier-ordered)
3. uOP issue       — exec up to IssueWidths picks per pool (with forwarding)
4. mOP route       — process all uOP writebacks
5. mOP submit      — retire completed mOPs (multi)
6. mOP vector plan — emit uOPs (batch, multi-mOP)
7. OP route        — process all mOP commits
8. OP submit       — retire completed OPs (multi, fenced on commit frontier)
9. OP vector plan  — emit mOPs (batch, multi-OP)
```

W = writeback/commit width = sum of issue widths (insert + trade + cancel).

### 5.2 Completion propagation

- uOP writeback -> result forwarded to parent mOP (no canonical change).
- uOP commit -> applies mutation to canonical RAM + line-sum + level-sum +
  tail + meta.  Commit may advance through the canonical commit frontier
  (strict `uop_seq` order within the safe OP window).
- mOP becomes submittable when all its uOPs have writebacked.
- OP becomes submittable when all its mOPs have submitted AND the OP's last
  uOP has committed to canonical (checked via `has_pending_op(op_seq)`,
  or equivalently the BBO snapshot has been captured).
- On OP submit the matcher emits the final tick/order-done event using the
  per-OP BBO snapshot taken at the last uOP's commit.

### 5.3 Canonical commit frontier

The frontier is the inclusive `op_seq` of the youngest OP whose mOP/uOP
streams are fully closed:

```
for each OP in age order:
    frontier = op_seq
    closed = op.emit_done
          && mop_stage.op_uop_stream_closed(op_seq, remaining_mops)
    if !closed: break
```

uOP commit may target any `op_seq <= frontier` (in strict `uop_seq` order).
This decouples commit throughput from the OP submit path: younger OPs whose
uOPs have executed and written back can commit their mutations to canonical
even while older OPs are still being decoded, as long as the older OPs'
uOP streams are closed (no future uOPs will appear).

### 5.4 Same-line TakeLine forwarding

Up to `SAME_LINE_TAKE_DEPTH` (= 4) TakeLine uOPs on the same id-vol line may
be issued in a single tick. Each younger TakeLine's exec reads from the
oldest pending same-line mutation's `new_line` instead of canonical RAM:

```
forward_map: line_key -> (uop_seq, post-exec Line)
  built from issued Trade/Cancel entries before the issue loop
  updated after each TakeLine/CancelSlot exec in the issue loop

TakeLine exec:
  if forward_map has line_key:
    exec_take_line_from(addr, budget, forwarded_line)
  else:
    exec_take_line(addr, budget)  // read canonical
```

InsertSlot entries set a same-line **gap** that blocks younger TakeLines
until the insert commits (TakeLine cannot forward from InsertSlot because
inserts write the tail and forwarding only covers Take/Cancel mutations).
