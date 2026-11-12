#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/bookkeeping/checker.hpp"
#include "src/core/events.hpp"
#include "src/core/instructions.hpp"
#include "src/core/types.hpp"

// ---------------------------------------------------------------------------
// Checker unit tests.
//
// The checker is the pre-trade risk + settlement ledger:
//   - check()  validates and, on accept, opens a reservation record and locks
//              cash (buy) or base (sell).
//   - update() settles trades (both sides), then consumes input replies to
//              release the remainder (market discard / cancel / full fill) or
//              keep a resting limit.
//
// Order records are keyed by (account, client_order_id); the same client id in
// two accounts must not collide.
// ---------------------------------------------------------------------------

namespace {

constexpr Price_t P = 100;

OPcontext ctx(OPKind kind, Seq seq, AccountId_t account, ClientOrderId_t client,
              Side_t side, Price_t price, Qty_t qty, uint32_t contract = 1) {
    OPcontext c{};
    c.kind          = kind;
    c.Seq           = seq;
    c.ContractId    = contract;
    c.AccountId     = account;
    c.ClientOrderId = client;
    c.Side          = side;
    c.Price         = price;
    c.qty           = qty;
    c.reject_reason = OPIssue::Default;
    c.reject_detail = 0;
    return c;
}

AccountState account(uint64_t cash, uint64_t base,
                     std::optional<uint64_t> position_limit = std::nullopt) {
    AccountState s;
    s.cash           = cash;
    s.cash_locked    = 0;
    s.base           = base;
    s.base_locked    = 0;
    s.position_limit = position_limit;
    return s;
}

TradeEvent trade(Seq seq, uint64_t idx,
                 AccountId_t ta, ClientOrderId_t tc,
                 AccountId_t ma, ClientOrderId_t mc,
                 Side_t taker_side, Price_t price, Qty_t qty,
                 bool maker_cleared) {
    TradeEvent e{};
    e.seq                   = seq;
    e.trade_idx             = idx;
    e.taker_account_id      = ta;
    e.taker_client_order_id = tc;
    e.maker_account_id      = ma;
    e.maker_client_order_id = mc;
    e.taker_side            = taker_side;
    e.price                 = price;
    e.qty                   = qty;
    e.maker_cleared         = maker_cleared;
    return e;
}

InputReplyEvent reply(Seq seq, AccountId_t account, ClientOrderId_t client,
                      OPKind kind, OPIssue reason, Qty_t filled, Qty_t rested,
                      Qty_t discarded, bool cancel_found = false,
                      Qty_t cancel_qty = 0) {
    InputReplyEvent e{};
    e.seq             = seq;
    e.contract_id     = 1;
    e.account_id      = account;
    e.client_order_id = client;
    e.kind            = kind;
    e.reject_reason   = reason;
    e.reject_detail   = 0;
    e.filled_qty      = filled;
    e.rested_qty      = rested;
    e.discarded_qty   = discarded;
    e.cancel_found    = cancel_found;
    e.cancel_qty      = cancel_qty;
    return e;
}

std::unordered_map<AccountId_t, AccountState> book(std::initializer_list<
    std::pair<AccountId_t, AccountState>> entries) {
    std::unordered_map<AccountId_t, AccountState> m;
    for (const auto& [id, state] : entries) m[id] = state;
    return m;
}

} // namespace

TEST_CASE("checker rejects an unknown account without touching state") {
    Checker c;
    c.reset();

    const OPcontext out = c.check(ctx(OPKind::Limit, 1, 7, 1, Side_t::Buy, P, 5));

    CHECK(out.kind == OPKind::NOOP);
    CHECK(out.reject_reason == OPIssue::InvalidAccount);
    CHECK(c.states.empty());
    CHECK(c.open.empty());
}

TEST_CASE("buy limit reserves cash and rejects when free cash is insufficient") {
    Checker c;
    auto init = book({{1, account(1000, 0)}});
    c.reset(init, 0.0);

    const OPcontext ok = c.check(ctx(OPKind::Limit, 1, 1, 1, Side_t::Buy, P, 5));
    CHECK(ok.kind == OPKind::Limit);
    CHECK(c.states.at(1).cash_locked == 500);
    REQUIRE(c.open.contains({1, 1}));
    CHECK(c.open.at({1, 1}).side == Side_t::Buy);
    CHECK(c.open.at({1, 1}).price == P);
    CHECK(c.open.at({1, 1}).account == 1);

    // free cash = 1000 - 500 = 500 < 600
    const OPcontext no = c.check(ctx(OPKind::Limit, 2, 1, 2, Side_t::Buy, P, 6));
    CHECK(no.kind == OPKind::NOOP);
    CHECK(no.reject_reason == OPIssue::InsufficientFunds);
    CHECK(c.states.at(1).cash_locked == 500);
    CHECK_FALSE(c.open.contains({1, 2}));
}

TEST_CASE("market buy reserves against its cap price") {
    Checker c;
    auto init = book({{1, account(1000, 0)}});
    c.reset(init, 0.0);

    // market cap: 50 * 10 = 500 (not Price_max)
    const OPcontext ok = c.check(ctx(OPKind::Market, 1, 1, 1, Side_t::Buy, 50, 10));
    CHECK(ok.kind == OPKind::Market);
    CHECK(c.states.at(1).cash_locked == 500);
    REQUIRE(c.open.contains({1, 1}));
    CHECK(c.open.at({1, 1}).price == 50);
}

TEST_CASE("sell reserves base and rejects when free base is insufficient") {
    Checker c;
    auto init = book({{1, account(0, 10)}});
    c.reset(init, 0.0);

    const OPcontext ok = c.check(ctx(OPKind::Limit, 1, 1, 1, Side_t::Sell, P, 5));
    CHECK(ok.kind == OPKind::Limit);
    CHECK(c.states.at(1).base_locked == 5);
    REQUIRE(c.open.contains({1, 1}));

    // free base = 10 - 5 = 5 < 6
    const OPcontext no = c.check(ctx(OPKind::Limit, 2, 1, 2, Side_t::Sell, P, 6));
    CHECK(no.kind == OPKind::NOOP);
    CHECK(no.reject_reason == OPIssue::InsufficientBase);
    CHECK(c.states.at(1).base_locked == 5);
    CHECK_FALSE(c.open.contains({1, 2}));
}

TEST_CASE("buy position limit is enforced against the current base") {
    Checker c;
    auto init = book({{1, account(1'000'000, 5, 10)}});
    c.reset(init, 0.0);

    const OPcontext no = c.check(ctx(OPKind::Limit, 1, 1, 1, Side_t::Buy, P, 6));
    CHECK(no.kind == OPKind::NOOP);
    CHECK(no.reject_reason == OPIssue::PositionLimit);
    CHECK(c.states.at(1).cash_locked == 0);
    CHECK(c.open.empty());

    const OPcontext ok = c.check(ctx(OPKind::Limit, 2, 1, 2, Side_t::Buy, P, 5));
    CHECK(ok.kind == OPKind::Limit);
    CHECK(c.states.at(1).cash_locked == 500);
}

TEST_CASE("repeated (account, client) id is rejected, same client id across accounts is fine") {
    Checker c;
    auto init = book({{1, account(10'000, 0)}, {2, account(10'000, 0)}});
    c.reset(init, 0.0);

    CHECK(c.check(ctx(OPKind::Limit, 1, 1, 7, Side_t::Buy, P, 1)).kind == OPKind::Limit);

    const OPcontext dup = c.check(ctx(OPKind::Limit, 2, 1, 7, Side_t::Buy, P, 1));
    CHECK(dup.kind == OPKind::NOOP);
    CHECK(dup.reject_reason == OPIssue::RepeatedID);

    // different account, same client id: distinct key, accepted
    CHECK(c.check(ctx(OPKind::Limit, 3, 2, 7, Side_t::Buy, P, 1)).kind == OPKind::Limit);
    CHECK(c.open.contains({1, 7}));
    CHECK(c.open.contains({2, 7}));
}

TEST_CASE("an out-of-range contract is rejected without reserving") {
    Checker c;
    auto init = book({{1, account(1000, 0)}});
    c.reset(init, 0.0);

    const OPcontext no = c.check(
        ctx(OPKind::Limit, 1, 1, 1, Side_t::Buy, P, 5, /*contract=*/200));
    CHECK(no.kind == OPKind::NOOP);
    CHECK(no.reject_reason == OPIssue::InvalidContract);
    CHECK(c.states.at(1).cash_locked == 0);
    CHECK(c.open.empty());
}

TEST_CASE("invalid price and zero qty are rejected without locking") {
    Checker c;
    auto init = book({{1, account(10'000, 0)}});
    c.reset(init, 0.0);

    const OPcontext bad_price = c.check(
        ctx(OPKind::Limit, 1, 1, 1, Side_t::Buy, P + Price_max, 1));
    CHECK(bad_price.kind == OPKind::NOOP);
    CHECK(bad_price.reject_reason == OPIssue::InvalidPrice);

    const OPcontext bad_qty = c.check(ctx(OPKind::Limit, 2, 1, 2, Side_t::Buy, P, 0));
    CHECK(bad_qty.kind == OPKind::NOOP);
    CHECK(bad_qty.reject_reason == OPIssue::InvalidQty);

    CHECK(c.states.at(1).cash_locked == 0);
    CHECK(c.open.empty());
}

TEST_CASE("taker buy vs maker sell: settle cash/base, release locks, clear maker") {
    Checker c;
    auto init = book({{1, account(1000, 0)}, {2, account(0, 10)}});
    c.reset(init, 0.0);

    REQUIRE(c.check(ctx(OPKind::Limit, 1, 1, 1, Side_t::Buy, P, 5)).kind == OPKind::Limit);
    REQUIRE(c.check(ctx(OPKind::Limit, 2, 2, 1, Side_t::Sell, P, 5)).kind == OPKind::Limit);

    const std::vector<TradeEvent> trades = {
        trade(1, 1, /*taker*/ 1, 1, /*maker*/ 2, 1, Side_t::Buy, P, 5, true)};
    c.update(trades, {});

    // buyer (account 1)
    CHECK(c.states.at(1).cash.has_value());
    CHECK(c.states.at(1).cash.value() == 500);
    CHECK(c.states.at(1).base.has_value());
    CHECK(c.states.at(1).base.value() == 5);
    CHECK(c.states.at(1).cash_locked == 0);

    // seller (account 2)
    CHECK(c.states.at(2).cash.value() == 500);
    CHECK(c.states.at(2).base.value() == 5);
    CHECK(c.states.at(2).base_locked == 0);

    // maker fully cleared and removed; taker record remains until its reply
    CHECK_FALSE(c.open.contains({2, 1}));
    CHECK(c.open.contains({1, 1}));

    const std::vector<InputReplyEvent> replies = {
        reply(1, 1, 1, OPKind::Limit, OPIssue::Default, 5, 0, 0)};
    c.update({}, replies);

    CHECK(c.open.empty());
}

TEST_CASE("market remainder is discarded and its reservation released") {
    Checker c;
    auto init = book({{1, account(1000, 0)}, {2, account(0, 10)}});
    c.reset(init, 0.0);

    // market buy cap 100, qty 10 -> reserves 1000
    REQUIRE(c.check(ctx(OPKind::Market, 1, 1, 1, Side_t::Buy, P, 10)).kind == OPKind::Market);
    REQUIRE(c.check(ctx(OPKind::Limit, 2, 2, 1, Side_t::Sell, P, 4)).kind == OPKind::Limit);

    const std::vector<TradeEvent> trades = {
        trade(1, 1, 1, 1, 2, 1, Side_t::Buy, P, 4, true)};
    c.update(trades, {});

    CHECK(c.states.at(1).cash.value() == 600);
    CHECK(c.states.at(1).base.value() == 4);
    CHECK(c.states.at(1).cash_locked == 600); // 6 still reserved for the remainder

    const std::vector<InputReplyEvent> replies = {
        reply(1, 1, 1, OPKind::Market, OPIssue::Default, 4, 0, 6)};
    c.update({}, replies);

    CHECK(c.states.at(1).cash_locked == 0);
    CHECK_FALSE(c.open.contains({1, 1}));
}

TEST_CASE("limit rests, then is filled later as a maker") {
    Checker c;
    // account 1: original buyer that will rest
    // account 3: first taker (seller of 3)
    // account 4: second taker (seller of 7)
    auto init = book({{1, account(1000, 0)},
                      {3, account(0, 10)},
                      {4, account(0, 10)}});
    c.reset(init, 0.0);

    REQUIRE(c.check(ctx(OPKind::Limit, 1, 1, 1, Side_t::Buy, P, 10)).kind == OPKind::Limit);
    REQUIRE(c.check(ctx(OPKind::Limit, 2, 3, 1, Side_t::Sell, P, 3)).kind == OPKind::Limit);

    // partial fill of 3: buyer 1 pays 300, gets 3 base; lock drops by 3*100
    c.update({trade(1, 1, 3, 1, 1, 1, Side_t::Sell, P, 3, false)}, {});
    CHECK(c.states.at(1).cash.value() == 700);
    CHECK(c.states.at(1).base.value() == 3);
    CHECK(c.states.at(1).cash_locked == 700);

    // taker 3 fully filled
    c.update({}, {reply(2, 3, 1, OPKind::Limit, OPIssue::Default, 3, 0, 0)});
    CHECK_FALSE(c.open.contains({3, 1}));

    // limit 1 rests the remaining 7
    c.update({}, {reply(1, 1, 1, OPKind::Limit, OPIssue::Default, 3, 7, 0)});
    CHECK(c.open.contains({1, 1}));
    CHECK(c.states.at(1).cash_locked == 700);

    // a later taker (account 4) sells 7 into the resting bid
    REQUIRE(c.check(ctx(OPKind::Limit, 3, 4, 1, Side_t::Sell, P, 7)).kind == OPKind::Limit);
    c.update({trade(3, 2, 4, 1, 1, 1, Side_t::Sell, P, 7, true)}, {});
    c.update({}, {reply(3, 4, 1, OPKind::Limit, OPIssue::Default, 7, 0, 0)});

    CHECK(c.states.at(1).cash.value() == 0);
    CHECK(c.states.at(1).base.value() == 10);
    CHECK(c.states.at(1).cash_locked == 0);
    CHECK(c.states.at(4).cash.value() == 700);
    CHECK(c.states.at(4).base.value() == 3);
    CHECK(c.states.at(4).base_locked == 0);
    CHECK(c.open.empty());
}

TEST_CASE("cancel releases the target's remaining reservation") {
    Checker c;
    auto init = book({{1, account(1000, 0)}});
    c.reset(init, 0.0);

    REQUIRE(c.check(ctx(OPKind::Limit, 1, 1, 1, Side_t::Buy, P, 10)).kind == OPKind::Limit);
    CHECK(c.states.at(1).cash_locked == 1000);

    const std::vector<InputReplyEvent> replies = {
        reply(2, 1, 1, OPKind::Cancel, OPIssue::Default, 0, 0, 0, true, 10)};
    c.update({}, replies);

    CHECK(c.states.at(1).cash_locked == 0);
    CHECK(c.open.empty());
}

TEST_CASE("a rejected reply (kind NOOP) is ignored by the ledger") {
    Checker c;
    auto init = book({{1, account(1000, 0)}});
    c.reset(init, 0.0);

    REQUIRE(c.check(ctx(OPKind::Limit, 1, 1, 1, Side_t::Buy, P, 5)).kind == OPKind::Limit);
    const auto locked = c.states.at(1).cash_locked;

    const std::vector<InputReplyEvent> replies = {
        reply(2, 1, 2, OPKind::NOOP, OPIssue::InvalidPrice, 0, 0, 0)};
    c.update({}, replies);

    CHECK(c.states.at(1).cash_locked == locked);
    CHECK(c.open.contains({1, 1}));
}

TEST_CASE("partial maker fill keeps its reservation until fully cleared") {
    Checker c;
    auto init = book({{1, account(1000, 0)}, {2, account(0, 10)}, {3, account(1000, 0)}});
    c.reset(init, 0.0);

    REQUIRE(c.check(ctx(OPKind::Limit, 1, 2, 1, Side_t::Sell, P, 5)).kind == OPKind::Limit);
    REQUIRE(c.check(ctx(OPKind::Limit, 2, 1, 1, Side_t::Buy, P, 2)).kind == OPKind::Limit);

    c.update({trade(1, 1, 1, 1, 2, 1, Side_t::Buy, P, 2, false)}, {});
    CHECK(c.open.contains({2, 1}));
    CHECK(c.states.at(2).base.value() == 8);
    CHECK(c.states.at(2).base_locked == 3); // 5 reserved - 2 filled

    c.update({}, {reply(2, 1, 1, OPKind::Limit, OPIssue::Default, 2, 0, 0)});
    CHECK_FALSE(c.open.contains({1, 1}));

    // remaining 3 of the maker are taken later
    REQUIRE(c.check(ctx(OPKind::Limit, 3, 3, 1, Side_t::Buy, P, 3)).kind == OPKind::Limit);
    c.update({trade(3, 2, 3, 1, 2, 1, Side_t::Buy, P, 3, true)}, {});
    c.update({}, {reply(3, 3, 1, OPKind::Limit, OPIssue::Default, 3, 0, 0)});

    CHECK_FALSE(c.open.contains({2, 1}));
    CHECK(c.states.at(2).base.value() == 5);
    CHECK(c.states.at(2).base_locked == 0);
}
