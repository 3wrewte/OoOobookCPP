#include <doctest/doctest.h>

#include <cstdint>

#include "src/bookkeeping/IDallocator.hpp"
#include "src/core/instructions.hpp"

// The ID allocator is the external "checker" boundary: it assigns internal op
// ids, resolves cancel targets through (account, client) -> order_id, and turns
// invalid input into a NOOP carrying an OPIssue on the imm0 channel. The core
// only forwards that issue through its END event.

namespace {

OPcontext limit_ctx(uint64_t account, uint64_t client, uint32_t contract,
                    Side_t side, uint32_t price, uint32_t qty) {
    OPcontext c{};
    c.kind        = OPKind::Limit;
    c.Seq         = client;
    c.ContractId  = contract;
    c.AccountId   = account;
    c.ClientOrderId = client;
    c.Side        = side;
    c.Price       = price;
    c.qty         = qty;
    return c;
}

OPcontext cancel_ctx(uint64_t account, uint64_t client) {
    OPcontext c{};
    c.kind        = OPKind::Cancel;
    c.Seq         = client;
    c.ContractId  = 1;
    c.AccountId   = account;
    c.ClientOrderId = client;
    c.Side        = Side_t::Buy;
    return c;
}

} // namespace

TEST_CASE("IDallocator accepts a valid limit and resolves its cancel") {
    IDallocator a;
    a.reset();

    OPcontext lc = limit_ctx(100, 7, 1, Side_t::Buy, 10'000, 5);
    const OP new_op = a.allocate(lc);
    CHECK(new_op.kind == OPKind::Limit);
    CHECK(new_op.id == 1);
    CHECK(new_op.imm0 == 10'000);
    CHECK(new_op.imm1 == 5);
    CHECK(new_op.sym == 1);

    OPcontext cc = cancel_ctx(100, 7);
    const OP cancel_op = a.allocate(cc);
    CHECK(cancel_op.kind == OPKind::Cancel);
    CHECK(cancel_op.imm0 == 1); // internal order id of the resting order
}

TEST_CASE("IDallocator rejects an unknown cancel target") {
    IDallocator a;
    a.reset();

    OPcontext cc = cancel_ctx(999, 999);
    const OP op = a.allocate(cc);
    CHECK(op.kind == OPKind::NOOP);
    CHECK(op.imm0 == uint32_t(OPIssue::UnknownCancelTarget));
}

TEST_CASE("IDallocator rejects an out-of-range contract") {
    IDallocator a;
    a.reset();

    OPcontext lc = limit_ctx(1, 1, 200, Side_t::Sell, 100, 1);
    const OP op = a.allocate(lc);
    CHECK(op.kind == OPKind::NOOP);
    CHECK(op.imm0 == uint32_t(OPIssue::InvalidContract));
}

TEST_CASE("IDallocator rejects an out-of-range price") {
    IDallocator a;
    a.reset();

    OPcontext lc = limit_ctx(1, 1, 1, Side_t::Buy, uint32_t(Price_max) + 1, 1);
    const OP op = a.allocate(lc);
    CHECK(op.kind == OPKind::NOOP);
    CHECK(op.imm0 == uint32_t(OPIssue::InvalidPrice));
}

TEST_CASE("IDallocator forwards an already rejected NOOP reason") {
    IDallocator a;
    a.reset();

    OPcontext rc{};
    rc.kind          = OPKind::NOOP;
    rc.Seq           = 3;
    rc.ContractId    = 1;
    rc.AccountId     = 1;
    rc.ClientOrderId = 3;
    rc.reject_reason = OPIssue::InsufficientFunds;
    rc.reject_detail = 42;

    const OP op = a.allocate(rc);
    CHECK(op.kind == OPKind::NOOP);
    CHECK(op.imm0 == uint32_t(OPIssue::InsufficientFunds));

    const auto ctx = a.get(op.id);
    REQUIRE(ctx.has_value());
    CHECK(ctx->reject_reason == OPIssue::InsufficientFunds);
    CHECK(ctx->reject_detail == 42);
}
