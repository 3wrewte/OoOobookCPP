#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "src/core/addr.hpp"
#include "src/core/idvol.hpp"
#include "src/core/types.hpp"
#include "src/core/uop.hpp"

// Unit tests for the single-cycle, in-order uOP stage
// (`UopQueue::exec_n_commit`). The stage is a golden reference: it consumes a
// program-ordered batch of uOPs, executes each against `IdVolMap` in order, and
// emits one `CanonicalEvent` per uOP. These tests pin down:
//   * `TakeLineCalc` FIFO / hole / partial semantics,
//   * take / insert / cancel side effects on the id-vol book,
//   * program-order visibility (an insert queued after a take is not seen by it),
//   * `op_end` / `End` completion markers,
//   * the per-event BBO snapshot.

namespace {

using VolLine = std::array<Qty_t, 16>;

CanonicalAddr_t A(Pos_t pos, Price_t price, Side_t side, Sym_t sym = 0) {
    CanonicalAddrStructured s{sym, side, price, pos};
    REQUIRE(CanonicalAddrStructuredValid(s));
    return CanonicalAddrPack(s);
}

Line make_line(std::initializer_list<std::pair<size_t, std::pair<OrderID_t, Qty_t>>> items) {
    Line line{};
    for (const auto& [offset, id_vol] : items) {
        REQUIRE(offset < 16);
        line.IDs[offset]  = id_vol.first;
        line.Vols[offset] = id_vol.second;
    }
    return line;
}

QtySum_t sum_vols(const VolLine& vols) {
    QtySum_t total = 0;
    for (Qty_t v : vols) total += v;
    return total;
}

Uop take_uop(OpSeq op_seq, CanonicalAddr_t addr, Qty_t budget,
             bool op_end = false, OrderID_t id = 0) {
    return Uop{
        .kind   = UopKind::TakeLine,
        .id     = id,
        .op_seq = op_seq,
        .op_end = op_end,
        .addr   = addr,
        .imm    = budget,
    };
}

Uop insert_uop(OpSeq op_seq, CanonicalAddr_t addr, OrderID_t id, Qty_t volume,
               bool op_end = false) {
    return Uop{
        .kind   = UopKind::InsertSlot,
        .id     = id,
        .op_seq = op_seq,
        .op_end = op_end,
        .addr   = addr,
        .imm    = volume,
    };
}

Uop cancel_uop(OpSeq op_seq, CanonicalAddr_t addr, OrderID_t target,
               bool op_end = false) {
    return Uop{
        .kind   = UopKind::CancelSlot,
        .id     = target,
        .op_seq = op_seq,
        .op_end = op_end,
        .addr   = addr,
        .imm    = target,
    };
}

Uop end_uop(OpSeq op_seq, bool op_end = true) {
    return Uop{
        .kind   = UopKind::End,
        .id     = 0,
        .op_seq = op_seq,
        .op_end = op_end,
        .addr   = 0,
        .imm    = 0,
    };
}

// Drives one tick through the three-phase protocol. The id-vol book persists
// across ticks; the uOP queue is per-tick scratch and is drained by
// `exec_n_commit`.
struct Harness {
    IdVolMap idvol;
    UopQueue queue;

    Harness() {
        idvol.reset();
        queue.reset();
    }

    std::vector<CanonicalEvent> run(std::vector<Uop> uops) {
        global_phase = Phase::Prepare;
        queue.prepare();
        global_phase = Phase::Comb;
        queue.alloc(uops.size(), uops);
        std::vector<CanonicalEvent> events = queue.exec_n_commit(idvol);
        global_phase = Phase::Commit;
        queue.commit();
        global_phase = Phase::Prepare;
        return events;
    }
};

const TakeLineResult& take_result(const CanonicalEvent& ev) {
    REQUIRE(std::holds_alternative<TakeLineResult>(ev.payload));
    return std::get<TakeLineResult>(ev.payload);
}

const CancelResult& cancel_result(const CanonicalEvent& ev) {
    REQUIRE(std::holds_alternative<CancelResult>(ev.payload));
    return std::get<CancelResult>(ev.payload);
}

} // namespace

// ---------------------------------------------------------------------------
// TakeLineCalc: pure FIFO consume arithmetic
// ---------------------------------------------------------------------------

TEST_CASE("TakeLineCalc consumes FIFO and skips holes") {
    const Line line = make_line({{0, {10, 5}}, {2, {20, 4}}});

    const TakeLineResult r = TakeLineCalc(line, 20);

    CHECK(r.consumed == 9);
    CHECK(r.line_remaining == 0);
    CHECK(r.trade_id_vec[0] == 10);
    CHECK(r.trade_vol_vec[0] == 5);
    CHECK(r.trade_id_vec[1] == 0); // hole: untouched
    CHECK(r.trade_vol_vec[1] == 0);
    CHECK(r.trade_id_vec[2] == 20);
    CHECK(r.trade_vol_vec[2] == 4);
    for (size_t i = 3; i < 16; ++i) {
        CHECK(r.trade_id_vec[i] == 0);
        CHECK(r.trade_vol_vec[i] == 0);
    }
    CHECK(sum_vols(r.trade_vol_vec) == r.consumed);
}

TEST_CASE("TakeLineCalc leaves the partial remainder on the last slot") {
    const Line line = make_line({{0, {1, 5}}, {1, {2, 4}}});

    const TakeLineResult r = TakeLineCalc(line, 7);

    CHECK(r.consumed == 7);
    CHECK(r.trade_id_vec[0] == 1);
    CHECK(r.trade_vol_vec[0] == 5);
    CHECK(r.trade_id_vec[1] == 2);
    CHECK(r.trade_vol_vec[1] == 2);
    CHECK(r.line_remaining == 2);
    CHECK(sum_vols(r.trade_vol_vec) == r.consumed);
}

TEST_CASE("TakeLineCalc budget below the first resting volume") {
    const Line line = make_line({{0, {7, 5}}});

    const TakeLineResult r = TakeLineCalc(line, 2);

    CHECK(r.consumed == 2);
    CHECK(r.trade_id_vec[0] == 7);
    CHECK(r.trade_vol_vec[0] == 2);
    CHECK(r.line_remaining == 3);
}

TEST_CASE("TakeLineCalc on an empty line consumes nothing") {
    const Line line{};

    const TakeLineResult r = TakeLineCalc(line, 5);

    CHECK(r.consumed == 0);
    CHECK(r.line_remaining == 0);
    CHECK(sum_vols(r.trade_vol_vec) == 0);
}

// ---------------------------------------------------------------------------
// InsertSlot
// ---------------------------------------------------------------------------

TEST_CASE("insert becomes visible and updates the BBO snapshot") {
    Harness h;
    const Price_t p = 100;

    const std::vector<CanonicalEvent> ev =
        h.run({insert_uop(0, A(0, p, Side_t::Buy), 42, 5, true)});

    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == UopKind::InsertSlot);
    CHECK(ev[0].id == 42);
    CHECK(ev[0].op_seq == 0);
    CHECK(ev[0].op_end);
    CHECK(std::holds_alternative<CanonicalAddr_t>(ev[0].payload));
    CHECK(std::get<CanonicalAddr_t>(ev[0].payload) == A(0, p, Side_t::Buy));

    CHECK(h.idvol.slot_volume(A(0, p, Side_t::Buy)) == 5);
    CHECK(h.idvol.line_sum_ram(A(0, p, Side_t::Buy)) == 5);
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Buy)) == 5);
    CHECK(h.idvol.tail_ram(A(0, p, Side_t::Buy)) == 1);

    REQUIRE(ev[0].snapshot.has_value());
    REQUIRE(ev[0].snapshot->bid_price.has_value());
    CHECK(*ev[0].snapshot->bid_price == p);
    CHECK(ev[0].snapshot->bid_qty == 5);
    CHECK_FALSE(ev[0].snapshot->ask_price.has_value());
    CHECK(ev[0].snapshot->ask_qty == 0);
}

TEST_CASE("two inserts at one level get consecutive FIFO positions") {
    Harness h;
    const Price_t p = 0x20;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Sell), 11, 3),
        insert_uop(0, A(1, p, Side_t::Sell), 12, 4),
    });

    REQUIRE(ev.size() == 2);
    CHECK(h.idvol.slot_volume(A(0, p, Side_t::Sell)) == 3);
    CHECK(h.idvol.slot_volume(A(1, p, Side_t::Sell)) == 4);
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Sell)) == 7);
    CHECK(h.idvol.tail_ram(A(0, p, Side_t::Sell)) == 2);
}

// ---------------------------------------------------------------------------
// TakeLine
// ---------------------------------------------------------------------------

TEST_CASE("take consumes FIFO and reports maker ids and volumes") {
    Harness h;
    const Price_t p = 0x10;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Sell), 11, 5),
        insert_uop(0, A(1, p, Side_t::Sell), 12, 3),
        insert_uop(0, A(2, p, Side_t::Sell), 13, 4),
        take_uop(1, A(0, p, Side_t::Sell), 7),
    });

    REQUIRE(ev.size() == 4);
    CHECK(ev[3].kind == UopKind::TakeLine);
    const TakeLineResult& r = take_result(ev[3]);
    CHECK(r.consumed == 7);
    CHECK(r.line_remaining == 5);
    CHECK(r.trade_id_vec[0] == 11);
    CHECK(r.trade_vol_vec[0] == 5);
    CHECK(r.trade_id_vec[1] == 12);
    CHECK(r.trade_vol_vec[1] == 2);
    CHECK(r.trade_id_vec[2] == 0);
    CHECK(r.trade_vol_vec[2] == 0);
    CHECK(sum_vols(r.trade_vol_vec) == r.consumed);

    CHECK(h.idvol.slot_volume(A(0, p, Side_t::Sell)) == 0);
    CHECK(h.idvol.slot_volume(A(1, p, Side_t::Sell)) == 1);
    CHECK(h.idvol.slot_volume(A(2, p, Side_t::Sell)) == 4);
    CHECK(h.idvol.line_sum_ram(A(0, p, Side_t::Sell)) == 5);
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Sell)) == 5);
}

TEST_CASE("two takes on the same line apply in program order") {
    Harness h;
    const Price_t p = 0x20;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Buy), 100, 5),
        insert_uop(0, A(1, p, Side_t::Buy), 200, 5),
        take_uop(1, A(0, p, Side_t::Buy), 5),
        take_uop(2, A(0, p, Side_t::Buy), 5),
    });

    REQUIRE(ev.size() == 4);
    const TakeLineResult& t1 = take_result(ev[2]);
    const TakeLineResult& t2 = take_result(ev[3]);
    CHECK(t1.consumed == 5);
    CHECK(t1.trade_id_vec[0] == 100);
    CHECK(t1.trade_vol_vec[0] == 5);
    // The second take sees the hole left by the first and drains the next slot.
    CHECK(t2.consumed == 5);
    CHECK(t2.trade_id_vec[0] == 0);
    CHECK(t2.trade_vol_vec[0] == 0);
    CHECK(t2.trade_id_vec[1] == 200);
    CHECK(t2.trade_vol_vec[1] == 5);
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Buy)) == 0);
}

TEST_CASE("a take does not see an insert queued after it") {
    Harness h;
    const Price_t p = 0x30;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Sell), 1, 5),
        take_uop(1, A(0, p, Side_t::Sell), 10), // consumes only order 1
        insert_uop(2, A(1, p, Side_t::Sell), 2, 5),
        take_uop(3, A(0, p, Side_t::Sell), 10), // consumes only order 2
    });

    REQUIRE(ev.size() == 4);
    const TakeLineResult& t1 = take_result(ev[1]);
    const TakeLineResult& t2 = take_result(ev[3]);
    CHECK(t1.consumed == 5);
    CHECK(t1.trade_id_vec[0] == 1);
    CHECK(t1.trade_vol_vec[0] == 5);
    CHECK(t2.consumed == 5);
    CHECK(t2.trade_id_vec[0] == 0);
    CHECK(t2.trade_id_vec[1] == 2);
    CHECK(t2.trade_vol_vec[1] == 5);
}

TEST_CASE("a take targets exactly one id-vol line") {
    Harness h;
    const Price_t p = 0x90;

    std::vector<Uop> uops;
    for (Pos_t pos = 0; pos < 16; ++pos) {
        uops.push_back(insert_uop(0, A(pos, p, Side_t::Sell), 100 + pos, 1));
    }
    uops.push_back(insert_uop(0, A(16, p, Side_t::Sell), 200, 7)); // second line
    uops.push_back(take_uop(1, A(0, p, Side_t::Sell), 20, true));

    const std::vector<CanonicalEvent> ev = h.run(uops);

    REQUIRE(ev.size() == 18);
    const TakeLineResult& r = take_result(ev.back());
    CHECK(r.consumed == 16);
    CHECK(r.line_remaining == 0);
    CHECK(h.idvol.line_sum_ram(A(0, p, Side_t::Sell)) == 0);
    CHECK(h.idvol.line_sum_ram(A(16, p, Side_t::Sell)) == 7);
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Sell)) == 7);
}

TEST_CASE("a take on an absent line is a no-op") {
    Harness h;
    const Price_t p = 0x123;

    const std::vector<CanonicalEvent> ev =
        h.run({take_uop(0, A(0, p, Side_t::Sell), 5, true)});

    REQUIRE(ev.size() == 1);
    const TakeLineResult& r = take_result(ev[0]);
    CHECK(r.consumed == 0);
    CHECK(sum_vols(r.trade_vol_vec) == 0);
    CHECK(h.idvol.ram.empty());
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Sell)) == 0);
    REQUIRE(ev[0].snapshot.has_value());
    CHECK_FALSE(ev[0].snapshot->ask_price.has_value());
}

// ---------------------------------------------------------------------------
// CancelSlot
// ---------------------------------------------------------------------------

TEST_CASE("cancel clears the target slot and reports the removed volume") {
    Harness h;
    const Price_t p = 0x40;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Sell), 42, 9),
        cancel_uop(1, A(0, p, Side_t::Sell), 42, true),
    });

    REQUIRE(ev.size() == 2);
    CHECK(ev[1].kind == UopKind::CancelSlot);
    const CancelResult& c = cancel_result(ev[1]);
    CHECK(c.found);
    CHECK(c.order_id == 42);
    CHECK(c.volume_removed == 9);

    CHECK(h.idvol.slot_volume(A(0, p, Side_t::Sell)) == 0);
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Sell)) == 0);
    REQUIRE(ev[1].snapshot.has_value());
    CHECK_FALSE(ev[1].snapshot->ask_price.has_value());
}

TEST_CASE("cancel with a mismatched order id is a no-op") {
    Harness h;
    const Price_t p = 0x50;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Buy), 42, 9),
        cancel_uop(1, A(0, p, Side_t::Buy), 43, true),
    });

    REQUIRE(ev.size() == 2);
    const CancelResult& c = cancel_result(ev[1]);
    CHECK_FALSE(c.found);
    CHECK(c.volume_removed == 0);

    CHECK(h.idvol.slot_volume(A(0, p, Side_t::Buy)) == 9);
    REQUIRE(ev[1].snapshot.has_value());
    REQUIRE(ev[1].snapshot->bid_price.has_value());
    CHECK(*ev[1].snapshot->bid_price == p);
    CHECK(ev[1].snapshot->bid_qty == 9);
}

TEST_CASE("take skips a hole created by a cancel") {
    Harness h;
    const Price_t p = 0x60;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Sell), 1, 5),
        insert_uop(0, A(1, p, Side_t::Sell), 2, 5),
        insert_uop(0, A(2, p, Side_t::Sell), 3, 5),
        cancel_uop(1, A(1, p, Side_t::Sell), 2),
        take_uop(2, A(0, p, Side_t::Sell), 20),
    });

    REQUIRE(ev.size() == 5);
    const TakeLineResult& r = take_result(ev[4]);
    CHECK(r.consumed == 10);
    CHECK(r.trade_id_vec[0] == 1);
    CHECK(r.trade_vol_vec[0] == 5);
    CHECK(r.trade_id_vec[1] == 0); // cancelled hole
    CHECK(r.trade_vol_vec[1] == 0);
    CHECK(r.trade_id_vec[2] == 3);
    CHECK(r.trade_vol_vec[2] == 5);
    CHECK(h.idvol.ram.empty());
}

// ---------------------------------------------------------------------------
// End marker and event ordering
// ---------------------------------------------------------------------------

TEST_CASE("End completes an OP without touching the book") {
    Harness h;
    const Price_t p = 0x70;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Buy), 7, 4),
        end_uop(0),
    });

    REQUIRE(ev.size() == 2);
    CHECK(ev[1].kind == UopKind::End);
    CHECK(ev[1].op_seq == 0);
    CHECK(ev[1].op_end);
    CHECK(std::holds_alternative<std::monostate>(ev[1].payload));

    CHECK(h.idvol.slot_volume(A(0, p, Side_t::Buy)) == 4);
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Buy)) == 4);
    REQUIRE(ev[1].snapshot.has_value());
    REQUIRE(ev[1].snapshot->bid_price.has_value());
    CHECK(*ev[1].snapshot->bid_price == p);
    CHECK(ev[1].snapshot->bid_qty == 4);
}

TEST_CASE("events keep uop order and op_end markers") {
    Harness h;
    const Price_t p = 0x80;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Sell), 10, 6, false),
        end_uop(0, true),
        take_uop(1, A(0, p, Side_t::Sell), 6, false),
        end_uop(1, true),
    });

    REQUIRE(ev.size() == 4);
    CHECK(ev[0].kind == UopKind::InsertSlot);
    CHECK(ev[0].op_seq == 0);
    CHECK_FALSE(ev[0].op_end);
    CHECK(ev[1].kind == UopKind::End);
    CHECK(ev[1].op_seq == 0);
    CHECK(ev[1].op_end);
    CHECK(ev[2].kind == UopKind::TakeLine);
    CHECK(ev[2].op_seq == 1);
    CHECK_FALSE(ev[2].op_end);
    CHECK(ev[3].kind == UopKind::End);
    CHECK(ev[3].op_seq == 1);
    CHECK(ev[3].op_end);
}

// ---------------------------------------------------------------------------
// BBO snapshot
// ---------------------------------------------------------------------------

TEST_CASE("snapshot follows the best bid as its level drains") {
    Harness h;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, 100, Side_t::Buy), 1, 5),
        insert_uop(0, A(0,  99, Side_t::Buy), 2, 3, true),
        take_uop(1, A(0, 100, Side_t::Buy), 5, true),
    });

    REQUIRE(ev.size() == 3);
    // After inserting 5@100 and 3@99 the best bid is still 100.
    REQUIRE(ev[1].snapshot.has_value());
    REQUIRE(ev[1].snapshot->bid_price.has_value());
    CHECK(*ev[1].snapshot->bid_price == 100);
    CHECK(ev[1].snapshot->bid_qty == 5);
    // Draining 100 moves the best bid to 99.
    REQUIRE(ev[2].snapshot.has_value());
    REQUIRE(ev[2].snapshot->bid_price.has_value());
    CHECK(*ev[2].snapshot->bid_price == 99);
    CHECK(ev[2].snapshot->bid_qty == 3);
}

TEST_CASE("snapshot exposes both sides independently") {
    Harness h;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, 100, Side_t::Buy),  1, 5),
        insert_uop(0, A(0, 105, Side_t::Sell), 2, 7, true),
    });

    REQUIRE(ev.size() == 2);
    REQUIRE(ev[1].snapshot.has_value());
    REQUIRE(ev[1].snapshot->bid_price.has_value());
    REQUIRE(ev[1].snapshot->ask_price.has_value());
    CHECK(*ev[1].snapshot->bid_price == 100);
    CHECK(ev[1].snapshot->bid_qty == 5);
    CHECK(*ev[1].snapshot->ask_price == 105);
    CHECK(ev[1].snapshot->ask_qty == 7);
}

TEST_CASE("partial take reduces the best-level snapshot quantity") {
    Harness h;
    const Price_t p = 0x110;

    const std::vector<CanonicalEvent> ev = h.run({
        insert_uop(0, A(0, p, Side_t::Sell), 1, 5),
        take_uop(1, A(0, p, Side_t::Sell), 2, true),
    });

    REQUIRE(ev.size() == 2);
    const TakeLineResult& r = take_result(ev[1]);
    CHECK(r.consumed == 2);
    CHECK(r.line_remaining == 3);
    REQUIRE(ev[1].snapshot.has_value());
    REQUIRE(ev[1].snapshot->ask_price.has_value());
    CHECK(*ev[1].snapshot->ask_price == p);
    CHECK(ev[1].snapshot->ask_qty == 3);
}
