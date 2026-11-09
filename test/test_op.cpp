#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "src/core/addr.hpp"
#include "src/core/idvol.hpp"
#include "src/core/mop.hpp"
#include "src/core/op.hpp"
#include "src/core/types.hpp"
#include "src/core/uop.hpp"

// Unit tests for the single-cycle OP stage: `OPQueue::issue` turns one OP into
// program-ordered mOPs, and `OPQueue::release` consumes canonical events to
// retire the OP and maintain the resting index (`order_id -> addr`).
//
// The op decoder runs against the real id-vol book (`meta` + level sums); there
// is no cross-cycle pending state in phase 1.
//
// `tail` / `issued_tail` / `head` are double-buffered registers, so an OP
// allocated this tick becomes visible to the issue logic on the next tick: the
// harness therefore drives one `alloc` tick followed by one `issue` tick.

namespace {

CanonicalAddr_t A(Pos_t pos, Price_t price, Side_t side, Sym_t sym = 0) {
    CanonicalAddrStructured s{sym, side, price, pos};
    REQUIRE(CanonicalAddrStructuredValid(s));
    return CanonicalAddrPack(s);
}

struct OPHarness {
    IdVolMap idvol;
    OPQueue q;

    OPHarness() {
        idvol.reset();
        q.reset();
    }

    // One tick that allocates OPs (write to the ROB; pointers commit at edge).
    void alloc(std::vector<OP> ops) {
        global_phase = Phase::Prepare;
        q.prepare();
        global_phase = Phase::Comb;
        q.alloc(ops.size(), ops);
        global_phase = Phase::Commit;
        q.commit();
    }

    // One tick that issues the oldest OP. `alloc(0)` keeps the alloc port driven.
    std::vector<Mop> issue() {
        std::vector<CanonicalEvent> no_events;
        std::vector<OP> no_release;
        return issue_impl(no_events, no_release);
    }

    std::vector<Mop> issue_and_release(
        std::vector<CanonicalEvent>& events,
        std::vector<OP>& released) {
        return issue_impl(events, released);
    }

  private:
    std::vector<Mop> issue_impl(std::vector<CanonicalEvent>& events,
                                std::vector<OP>& released) {
        std::vector<OP> no_ops;
        global_phase = Phase::Prepare;
        q.prepare();
        global_phase = Phase::Comb;
        q.alloc(0, no_ops);
        auto out = q.issue(idvol.level_sum, idvol);
        released = q.release(events);
        global_phase = Phase::Commit;
        q.commit();
        return out.second;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Limit / Market
// ---------------------------------------------------------------------------

TEST_CASE("non-crossing limit buy rests") {
    OPHarness h;
    h.alloc({OP{7, OPKind::Limit, 0, Side_t::Buy, 100, 5}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 1);
    CHECK(mops[0].kind == MopKind::InsertLevel);
    CHECK(mops[0].id == 7);
    CHECK(mops[0].op_seq == 0);
    CHECK(mops[0].op_end);
    CHECK(mops[0].addr == A(0, 100, Side_t::Buy));
    CHECK(mops[0].imm == 5);
}

TEST_CASE("non-crossing limit sell rests") {
    OPHarness h;
    h.alloc({OP{8, OPKind::Limit, 0, Side_t::Sell, 200, 4}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 1);
    CHECK(mops[0].kind == MopKind::InsertLevel);
    CHECK(mops[0].addr == A(0, 200, Side_t::Sell));
    CHECK(mops[0].imm == 4);
    CHECK(mops[0].op_end);
}

TEST_CASE("crossing buy takes asks in ascending price order") {
    OPHarness h;
    h.idvol.insert_slot(A(0, 101, Side_t::Sell), 1, 3);
    h.idvol.insert_slot(A(0, 102, Side_t::Sell), 2, 4);

    h.alloc({OP{9, OPKind::Limit, 0, Side_t::Buy, 102, 5}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 2);
    CHECK(mops[0].kind == MopKind::TakeLevel);
    CHECK(mops[0].addr == A(0, 101, Side_t::Sell));
    CHECK(mops[0].imm == 3);
    CHECK_FALSE(mops[0].op_end);

    CHECK(mops[1].kind == MopKind::TakeLevel);
    CHECK(mops[1].addr == A(0, 102, Side_t::Sell));
    CHECK(mops[1].imm == 2);
    CHECK(mops[1].op_end);
}

TEST_CASE("crossing sell takes bids in descending price order") {
    OPHarness h;
    h.idvol.insert_slot(A(0, 99, Side_t::Buy), 1, 3);
    h.idvol.insert_slot(A(0, 98, Side_t::Buy), 2, 4);

    h.alloc({OP{10, OPKind::Limit, 0, Side_t::Sell, 98, 5}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 2);
    CHECK(mops[0].kind == MopKind::TakeLevel);
    CHECK(mops[0].addr == A(0, 99, Side_t::Buy));
    CHECK(mops[0].imm == 3);
    CHECK_FALSE(mops[0].op_end);

    CHECK(mops[1].kind == MopKind::TakeLevel);
    CHECK(mops[1].addr == A(0, 98, Side_t::Buy));
    CHECK(mops[1].imm == 2);
    CHECK(mops[1].op_end);
}

TEST_CASE("crossing buy rests the remainder at its limit") {
    OPHarness h;
    h.idvol.insert_slot(A(0, 101, Side_t::Sell), 1, 2);

    h.alloc({OP{11, OPKind::Limit, 0, Side_t::Buy, 101, 5}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 2);
    CHECK(mops[0].kind == MopKind::TakeLevel);
    CHECK(mops[0].addr == A(0, 101, Side_t::Sell));
    CHECK(mops[0].imm == 2);
    CHECK_FALSE(mops[0].op_end);

    CHECK(mops[1].kind == MopKind::InsertLevel);
    CHECK(mops[1].addr == A(0, 101, Side_t::Buy));
    CHECK(mops[1].imm == 3);
    CHECK(mops[1].op_end);
}

TEST_CASE("market buy discards its remainder without resting") {
    OPHarness h;
    h.idvol.insert_slot(A(0, 101, Side_t::Sell), 1, 2);

    h.alloc({OP{12, OPKind::Market, 0, Side_t::Buy, 0, 5}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 1);
    CHECK(mops[0].kind == MopKind::TakeLevel);
    CHECK(mops[0].addr == A(0, 101, Side_t::Sell));
    CHECK(mops[0].imm == 2);
    CHECK(mops[0].op_end); // last emitted mOP is forced to op_end
}

TEST_CASE("market buy on an empty book emits only an end marker") {
    OPHarness h;
    h.alloc({OP{13, OPKind::Market, 0, Side_t::Buy, 0, 5}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 1);
    CHECK(mops[0].kind == MopKind::End);
    CHECK(mops[0].op_end);
}

TEST_CASE("market sell on an empty book emits only an end marker") {
    OPHarness h;
    h.alloc({OP{14, OPKind::Market, 0, Side_t::Sell, 0, 5}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 1);
    CHECK(mops[0].kind == MopKind::End);
    CHECK(mops[0].op_end);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST_CASE("cancel resolves a resting order to its address") {
    OPHarness h;
    const CanonicalAddr_t target = A(3, 100, Side_t::Buy);
    h.q.resting_index[42] = target;

    h.alloc({OP{0, OPKind::Cancel, 0, Side_t::Buy, 42, 0}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 1);
    CHECK(mops[0].kind == MopKind::CancelLevel);
    CHECK(mops[0].addr == target);
    CHECK(mops[0].imm == 42);
    CHECK(mops[0].op_end);
}

TEST_CASE("cancel of an unknown target is a no-op end marker") {
    OPHarness h;
    h.alloc({OP{0, OPKind::Cancel, 0, Side_t::Buy, 999, 0}});
    const std::vector<Mop> mops = h.issue();

    REQUIRE(mops.size() == 1);
    CHECK(mops[0].kind == MopKind::End);
    CHECK(mops[0].op_end);
}

// ---------------------------------------------------------------------------
// op_seq allocation and release
// ---------------------------------------------------------------------------

TEST_CASE("op_seq is the ring slot assigned at alloc, across ticks") {
    OPHarness h;

    h.alloc({OP{7, OPKind::Limit, 0, Side_t::Buy, 100, 5}});
    const std::vector<Mop> m1 = h.issue();
    REQUIRE(m1.size() == 1);
    CHECK(m1[0].op_seq == 0);

    h.alloc({OP{8, OPKind::Limit, 0, Side_t::Buy, 101, 5}});
    const std::vector<Mop> m2 = h.issue();
    REQUIRE(m2.size() == 1);
    CHECK(m2[0].op_seq == 1);
}

TEST_CASE("release retires the head OP and backfills the resting index") {
    OPHarness h;

    h.alloc({OP{7, OPKind::Limit, 0, Side_t::Buy, 100, 5}});

    const CanonicalAddr_t addr = A(0, 100, Side_t::Buy);
    std::vector<CanonicalEvent> events;
    events.push_back(CanonicalEvent{
        .kind = UopKind::InsertSlot,
        .id = 7,
        .op_seq = 0,
        .op_end = true,
        .payload = addr,
        .snapshot = std::nullopt,
    });
    std::vector<OP> released;
    const std::vector<Mop> mops = h.issue_and_release(events, released);

    REQUIRE(mops.size() == 1);
    REQUIRE(released.size() == 1);
    CHECK(released[0].id == 7);
    REQUIRE(h.q.resting_index.count(7) == 1);
    CHECK(h.q.resting_index.at(7) == addr);

    // The next OP is a cancel of order 7; it resolves through the index.
    h.alloc({OP{0, OPKind::Cancel, 0, Side_t::Buy, 7, 0}});
    const std::vector<Mop> cancels = h.issue();

    REQUIRE(cancels.size() == 1);
    CHECK(cancels[0].kind == MopKind::CancelLevel);
    CHECK(cancels[0].addr == addr);
    CHECK(cancels[0].imm == 7);
    CHECK(cancels[0].op_seq == 1);
}
