#include <doctest/doctest.h>

#include <cassert>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

#include "src/bookkeeping/IDallocator.hpp"
#include "src/bookkeeping/router.hpp"
#include "src/core/events.hpp"

// Router unit tests.
//
// The router is the single consumer of canonical events: it resolves internal
// order ids through IDallocator::map_out and emits self-contained
// TradeEvent / TickEvent / InputReplyEvent records. These tests build the
// canonical event stream and the context table by hand, then check the three
// output streams plus the internal EndEvents / NewRetireEvents streams.

namespace {

OPcontext make_ctx(OPKind kind, Seq seq, uint64_t account, uint64_t client,
                   Side_t side, Price_t price, Qty_t qty) {
    OPcontext c{};
    c.kind          = kind;
    c.Seq           = seq;
    c.ContractId    = 1;
    c.AccountId     = account;
    c.ClientOrderId = client;
    c.Side          = side;
    c.Price         = price;
    c.qty           = qty;
    c.reject_reason = OPIssue::Default;
    c.reject_detail = 0;
    return c;
}

OP alloc(IDallocator& ids, OPcontext c) {
    return ids.allocate(c);
}

struct TradeSpec {
    OrderID_t maker;
    Qty_t     vol;
    bool      cleared;
};

CanonicalEvent take_ev(OrderID_t taker, OpSeq op_seq, bool op_end,
                       std::initializer_list<TradeSpec> trades,
                       std::optional<CanonicalBookSnapshot> snap = std::nullopt) {
    TakeLineResult tr{};
    size_t slot = 0;
    Qty_t consumed = 0;
    for (const TradeSpec& t : trades) {
        assert(slot < 16);
        tr.trade_id_vec[slot]       = t.maker;
        tr.trade_vol_vec[slot]      = t.vol;
        tr.trade_complete_vec[slot] = t.cleared;
        consumed += t.vol;
        ++slot;
    }
    tr.consumed = consumed;
    return CanonicalEvent{
        .kind = UopKind::TakeLine,
        .id = taker,
        .op_seq = op_seq,
        .op_end = op_end,
        .payload = tr,
        .snapshot = snap};
}

CanonicalEvent insert_ev(OrderID_t id, OpSeq op_seq, bool op_end,
                         CanonicalAddr_t addr, Qty_t qty,
                         std::optional<CanonicalBookSnapshot> snap = std::nullopt) {
    return CanonicalEvent{
        .kind = UopKind::InsertSlot,
        .id = id,
        .op_seq = op_seq,
        .op_end = op_end,
        .payload = InsertLineResult{.insert_addr = addr, .insert_qty = qty},
        .snapshot = snap};
}

CanonicalEvent cancel_ev(OrderID_t cmd, OpSeq op_seq, bool op_end,
                         OrderID_t target, bool found, Qty_t removed,
                         std::optional<CanonicalBookSnapshot> snap = std::nullopt) {
    return CanonicalEvent{
        .kind = UopKind::CancelSlot,
        .id = cmd,
        .op_seq = op_seq,
        .op_end = op_end,
        .payload = CancelResult{.found = found, .order_id = target,
                                .volume_removed = removed},
        .snapshot = snap};
}

CanonicalEvent end_ev(OrderID_t id, OpSeq op_seq, OPIssue issue,
                      CanonicalBookSnapshot snap) {
    return CanonicalEvent{
        .kind = UopKind::End,
        .id = id,
        .op_seq = op_seq,
        .op_end = true,
        .payload = issue,
        .snapshot = snap};
}

CanonicalBookSnapshot bbo(std::optional<Price_t> bid, QtySum_t bid_qty,
                          std::optional<Price_t> ask, QtySum_t ask_qty) {
    CanonicalBookSnapshot s{};
    s.bid_price = bid;
    s.bid_qty   = bid_qty;
    s.ask_price = ask;
    s.ask_qty   = ask_qty;
    return s;
}

struct Routed {
    std::vector<TradeEvent>       trades;
    std::vector<TickEvent>        ticks;
    std::vector<InputReplyEvent>  replies;
};

Routed route(router& r, const std::vector<CanonicalEvent>& events,
             const IDallocator& ids) {
    global_phase = Phase::Prepare;
    r.prepare();
    global_phase = Phase::Comb;
    auto [trades, ticks, replies] = r.RouteEvents(events, ids);
    global_phase = Phase::Commit;
    r.commit();
    return Routed{trades, ticks, replies};
}

bool has_retire(const std::vector<NewRetireEvent>& v, OrderID_t id) {
    for (const NewRetireEvent& e : v) {
        if (e.type == NewRetireEvent::Types::Retire && e.id == id) return true;
    }
    return false;
}

bool has_new(const std::vector<NewRetireEvent>& v, OrderID_t id) {
    for (const NewRetireEvent& e : v) {
        if (e.type == NewRetireEvent::Types::New && e.id == id) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Internal streams
// ---------------------------------------------------------------------------

TEST_CASE("EndEvents reports the op_seq of every op_end") {
    const CanonicalAddr_t addr = CanonicalAddrPack(
        CanonicalAddrStructured{0, Side_t::Buy, 100, 0});

    const std::vector<CanonicalEvent> events = {
        take_ev(1, 0, false, {{2, 3, true}}),
        insert_ev(1, 0, true, addr, 2, bbo(100, 2, std::nullopt, 0)),
    };
    CHECK(EndEvents(events) == std::vector<OpSeq>{0});
}

TEST_CASE("NewRetireEvents: insert->New, cleared maker/taker->Retire") {
    const CanonicalAddr_t addr = CanonicalAddrPack(
        CanonicalAddrStructured{0, Side_t::Buy, 100, 0});

    const std::vector<CanonicalEvent> events = {
        take_ev(1, 0, true, {{2, 5, true}}),
        insert_ev(3, 1, true, addr, 7),
    };
    const std::vector<NewRetireEvent> ev = NewRetireEvents(events);

    CHECK(has_retire(ev, 2)); // cleared maker
    CHECK(has_retire(ev, 1)); // taker reached op_end
    CHECK(has_new(ev, 3));    // resting order
}

TEST_CASE("NewRetireEvents: cancel retires the target and the command") {
    const std::vector<CanonicalEvent> events = {
        cancel_ev(3, 0, true, 2, true, 6),
    };
    const std::vector<NewRetireEvent> ev = NewRetireEvents(events);

    CHECK(has_retire(ev, 2)); // cancelled target
    CHECK(has_retire(ev, 3)); // cancel command itself (FIXME: currently missing)
}

TEST_CASE("NewRetireEvents: End retires its own op") {
    const std::vector<CanonicalEvent> events = {
        end_ev(4, 0, OPIssue::InsufficientFunds, bbo(std::nullopt, 0, std::nullopt, 0)),
    };
    const std::vector<NewRetireEvent> ev = NewRetireEvents(events);
    CHECK(has_retire(ev, 4));
}

// ---------------------------------------------------------------------------
// RouteEvents: single input
// ---------------------------------------------------------------------------

TEST_CASE("limit fully filled: trade + tick + reply") {
    IDallocator ids;
    ids.reset();
    const OP taker = alloc(ids, make_ctx(OPKind::Limit, 1, 10, 101,
                                           Side_t::Buy, 100, 5));
    const OP maker = alloc(ids, make_ctx(OPKind::Limit, 0, 20, 202,
                                           Side_t::Sell, 99, 5));
    REQUIRE(taker.id == 1);
    REQUIRE(maker.id == 2);

    router r;
    r.reset();
    const Routed out = route(r, {
        take_ev(taker.id, 0, true, {{maker.id, 5, true}},
                bbo(99, 0, std::nullopt, 0)),
    }, ids);

    REQUIRE(out.trades.size() == 1);
    CHECK(out.trades[0].seq == 1);
    CHECK(out.trades[0].trade_idx == 1);
    CHECK(out.trades[0].taker_client_order_id == 101);
    CHECK(out.trades[0].maker_client_order_id == 202);
    CHECK(out.trades[0].taker_side == Side_t::Buy);
    CHECK(out.trades[0].price == 99);
    CHECK(out.trades[0].qty == 5);
    CHECK(out.trades[0].maker_cleared);

    REQUIRE(out.ticks.size() == 1);
    CHECK(out.ticks[0].seq == 1);
    CHECK(out.ticks[0].bid_price == 99);
    CHECK(out.ticks[0].last_price == 99);
    CHECK(out.ticks[0].last_qty == 5);
    CHECK(out.ticks[0].trade_count == 1);
    CHECK(out.ticks[0].total_volume == 5);

    REQUIRE(out.replies.size() == 1);
    CHECK(out.replies[0].seq == 1);
    CHECK(out.replies[0].account_id == 10);
    CHECK(out.replies[0].client_order_id == 101);
    CHECK(out.replies[0].reject_reason == OPIssue::Default);
    CHECK(out.replies[0].filled_qty == 5);
    CHECK(out.replies[0].rested_qty == 0);
    CHECK(out.replies[0].discarded_qty == 0);
}

TEST_CASE("non-crossing limit rests: reply has rested qty") {
    IDallocator ids;
    ids.reset();
    const OP taker = alloc(ids, make_ctx(OPKind::Limit, 2, 10, 102,
                                           Side_t::Sell, 101, 7));
    const CanonicalAddr_t addr = CanonicalAddrPack(
        CanonicalAddrStructured{0, Side_t::Sell, 101, 0});

    router r;
    r.reset();
    const Routed out = route(r, {
        insert_ev(taker.id, 0, true, addr, 7, bbo(std::nullopt, 0, 101, 7)),
    }, ids);

    CHECK(out.trades.empty());
    REQUIRE(out.ticks.size() == 1);
    CHECK(out.ticks[0].seq == 2);
    CHECK(out.ticks[0].ask_price == 101);
    CHECK(out.ticks[0].trade_count == 0);
    CHECK(out.ticks[0].total_volume == 0);

    REQUIRE(out.replies.size() == 1);
    CHECK(out.replies[0].filled_qty == 0);
    CHECK(out.replies[0].rested_qty == 7);
    CHECK(out.replies[0].discarded_qty == 0);
}

TEST_CASE("limit partial fill then rest") {
    IDallocator ids;
    ids.reset();
    const OP taker = alloc(ids, make_ctx(OPKind::Limit, 3, 10, 103,
                                           Side_t::Buy, 100, 7));
    const OP maker = alloc(ids, make_ctx(OPKind::Limit, 0, 20, 203,
                                           Side_t::Sell, 98, 3));
    const CanonicalAddr_t addr = CanonicalAddrPack(
        CanonicalAddrStructured{0, Side_t::Buy, 100, 0});

    router r;
    r.reset();
    const Routed out = route(r, {
        take_ev(taker.id, 0, false, {{maker.id, 3, true}}),
        insert_ev(taker.id, 0, true, addr, 4, bbo(100, 4, std::nullopt, 0)),
    }, ids);

    REQUIRE(out.trades.size() == 1);
    CHECK(out.trades[0].qty == 3);
    CHECK(out.trades[0].price == 98);

    REQUIRE(out.replies.size() == 1);
    CHECK(out.replies[0].filled_qty == 3);
    CHECK(out.replies[0].rested_qty == 4);
    CHECK(out.replies[0].discarded_qty == 0);
}

TEST_CASE("market partial fill reports discarded remainder") {
    IDallocator ids;
    ids.reset();
    const OP taker = alloc(ids, make_ctx(OPKind::Market, 4, 10, 104,
                                           Side_t::Buy, 100, 10));
    const OP maker = alloc(ids, make_ctx(OPKind::Limit, 0, 20, 204,
                                           Side_t::Sell, 97, 4));

    router r;
    r.reset();
    const Routed out = route(r, {
        take_ev(taker.id, 0, true, {{maker.id, 4, true}},
                bbo(std::nullopt, 0, 97, 0)),
    }, ids);

    REQUIRE(out.trades.size() == 1);
    CHECK(out.trades[0].qty == 4);
    CHECK(out.trades[0].price == 97);

    REQUIRE(out.replies.size() == 1);
    CHECK(out.replies[0].filled_qty == 4);
    CHECK(out.replies[0].rested_qty == 0);
    CHECK(out.replies[0].discarded_qty == 6);
    CHECK(out.replies[0].reject_reason == OPIssue::Default);
}

TEST_CASE("market with no fill reports the whole qty as discarded") {
    IDallocator ids;
    ids.reset();
    const OP taker = alloc(ids, make_ctx(OPKind::Market, 5, 10, 105,
                                           Side_t::Buy, 100, 10));

    router r;
    r.reset();
    const Routed out = route(r, {
        end_ev(taker.id, 0, OPIssue::Default, bbo(std::nullopt, 0, std::nullopt, 0)),
    }, ids);

    CHECK(out.trades.empty());
    REQUIRE(out.ticks.size() == 1);
    CHECK(out.ticks[0].seq == 5);
    REQUIRE(out.replies.size() == 1);
    CHECK(out.replies[0].filled_qty == 0);
    CHECK(out.replies[0].rested_qty == 0);
    CHECK(out.replies[0].discarded_qty == 10);
}

TEST_CASE("rejected limit reports zero rested/discarded") {
    IDallocator ids;
    ids.reset();
    const OP taker = alloc(ids, make_ctx(OPKind::Limit, 6, 10, 106,
                                           Side_t::Buy, 100, 10));

    router r;
    r.reset();
    const Routed out = route(r, {
        end_ev(taker.id, 0, OPIssue::InsufficientFunds,
               bbo(std::nullopt, 0, std::nullopt, 0)),
    }, ids);

    REQUIRE(out.replies.size() == 1);
    CHECK(out.replies[0].reject_reason == OPIssue::InsufficientFunds);
    CHECK(out.replies[0].filled_qty == 0);
    CHECK(out.replies[0].rested_qty == 0);    // FIXME: current router reports qty
    CHECK(out.replies[0].discarded_qty == 0);
}

TEST_CASE("cancel found: reply reports target removal") {
    IDallocator ids;
    ids.reset();
    const OP target = alloc(ids, make_ctx(OPKind::Limit, 0, 10, 121,
                                            Side_t::Sell, 105, 6));
    const OP cmd = alloc(ids, make_ctx(OPKind::Cancel, 7, 10, 121,
                                         Side_t::Buy, 0, 0));
    REQUIRE(cmd.imm0 == target.id);

    router r;
    r.reset();
    const Routed out = route(r, {
        cancel_ev(cmd.id, 0, true, target.id, true, 6,
                  bbo(std::nullopt, 0, 105, 0)),
    }, ids);

    CHECK(out.trades.empty());
    REQUIRE(out.replies.size() == 1);
    CHECK(out.replies[0].seq == 7);
    CHECK(out.replies[0].client_order_id == 121);
    CHECK(out.replies[0].cancel_found);
    CHECK(out.replies[0].cancel_qty == 6);
    CHECK(out.replies[0].filled_qty == 0);
    CHECK(out.replies[0].rested_qty == 0);
    CHECK(out.replies[0].discarded_qty == 0);
}

TEST_CASE("cancel of an unknown target is a no-op End") {
    IDallocator ids;
    ids.reset();
    const OP cmd = alloc(ids, make_ctx(OPKind::Cancel, 8, 10, 199,
                                         Side_t::Buy, 0, 0));

    router r;
    r.reset();
    const Routed out = route(r, {
        end_ev(cmd.id, 0, OPIssue::UnknownCancelTarget,
               bbo(std::nullopt, 0, std::nullopt, 0)),
    }, ids);

    REQUIRE(out.replies.size() == 1);
    CHECK(out.replies[0].seq == 8);
    CHECK(out.replies[0].client_order_id == 199);
    CHECK(out.replies[0].reject_reason == OPIssue::UnknownCancelTarget);
    CHECK(out.replies[0].cancel_found == false);
    CHECK(out.replies[0].filled_qty == 0);
    CHECK(out.replies[0].rested_qty == 0);
    CHECK(out.replies[0].discarded_qty == 0);
}

// ---------------------------------------------------------------------------
// RouteEvents: multiple OPs / cross-tick counters
// ---------------------------------------------------------------------------

TEST_CASE("two op_ends in one batch get their own fills and cumulative ticks") {
    IDallocator ids;
    ids.reset();
    const OP t1 = alloc(ids, make_ctx(OPKind::Limit, 10, 10, 110,
                                        Side_t::Buy, 100, 5));
    const OP m1 = alloc(ids, make_ctx(OPKind::Limit, 0, 20, 210,
                                        Side_t::Sell, 99, 5));
    const OP t2 = alloc(ids, make_ctx(OPKind::Limit, 11, 11, 111,
                                        Side_t::Sell, 101, 3));
    const OP m2 = alloc(ids, make_ctx(OPKind::Limit, 0, 21, 211,
                                        Side_t::Buy, 102, 3));

    router r;
    r.reset();
    const Routed out = route(r, {
        take_ev(t1.id, 0, true, {{m1.id, 5, true}}, bbo(99, 0, std::nullopt, 0)),
        take_ev(t2.id, 1, true, {{m2.id, 3, true}}, bbo(std::nullopt, 0, 102, 0)),
    }, ids);

    REQUIRE(out.trades.size() == 2);
    CHECK(out.trades[0].trade_idx == 1);
    CHECK(out.trades[1].trade_idx == 2);

    REQUIRE(out.ticks.size() == 2);
    CHECK(out.ticks[0].seq == 10);
    CHECK(out.ticks[0].trade_count == 1);
    CHECK(out.ticks[0].total_volume == 5);
    CHECK(out.ticks[1].seq == 11);
    CHECK(out.ticks[1].trade_count == 2);
    CHECK(out.ticks[1].total_volume == 8);

    REQUIRE(out.replies.size() == 2);
    CHECK(out.replies[0].filled_qty == 5);
    CHECK(out.replies[1].filled_qty == 3);
}

TEST_CASE("trade counters persist across ticks") {
    IDallocator ids;
    ids.reset();
    const OP t1 = alloc(ids, make_ctx(OPKind::Limit, 12, 10, 112,
                                        Side_t::Buy, 100, 4));
    const OP m1 = alloc(ids, make_ctx(OPKind::Limit, 0, 20, 212,
                                        Side_t::Sell, 98, 4));
    const OP t2 = alloc(ids, make_ctx(OPKind::Limit, 13, 10, 113,
                                        Side_t::Sell, 103, 2));
    const CanonicalAddr_t addr = CanonicalAddrPack(
        CanonicalAddrStructured{0, Side_t::Sell, 103, 0});

    router r;
    r.reset();

    const Routed first = route(r, {
        take_ev(t1.id, 0, true, {{m1.id, 4, true}}, bbo(98, 0, std::nullopt, 0)),
    }, ids);
    REQUIRE(first.ticks.size() == 1);
    CHECK(first.ticks[0].trade_count == 1);
    CHECK(first.ticks[0].total_volume == 4);

    const Routed second = route(r, {
        insert_ev(t2.id, 1, true, addr, 2, bbo(std::nullopt, 0, 103, 2)),
    }, ids);
    REQUIRE(second.ticks.size() == 1);
    CHECK(second.ticks[0].seq == 13);
    CHECK(second.ticks[0].trade_count == 1);   // unchanged
    CHECK(second.ticks[0].total_volume == 4);  // unchanged
    CHECK(second.ticks[0].last_price == 98);   // carried over
    CHECK(second.ticks[0].last_qty == 4);
}
