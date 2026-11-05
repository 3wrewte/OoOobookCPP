#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "src/core/addr.hpp"
#include "src/core/idvol.hpp"
#include "src/core/mop.hpp"
#include "src/core/types.hpp"
#include "src/core/uop.hpp"

// Unit tests for the single-cycle mOP stage (`MopQueue::issue`): it turns a
// batch of mOPs into program-ordered uOPs. The stage does not execute anything
// itself; it only emits uOPs (TakeLine / InsertSlot / CancelSlot / End) with the
// addresses and budgets the uOP stage will consume.
//
// The id-vol book is the *real* one (there is no cross-cycle pending state in
// phase 1), so `tail` is exact and the line-sum rows are canonical.

namespace {

CanonicalAddr_t A(Pos_t pos, Price_t price, Side_t side, Sym_t sym = 0) {
    CanonicalAddrStructured s{sym, side, price, pos};
    REQUIRE(CanonicalAddrStructuredValid(s));
    return CanonicalAddrPack(s);
}

LevelAddr_t L(Price_t price, Side_t side, Sym_t sym = 0) {
    return LevelAddrPack(LevelAddrStructured{sym, side, price});
}

struct MopHarness {
    IdVolMap idvol;
    MopQueue q;

    MopHarness() {
        idvol.reset();
        q.reset();
    }

    std::vector<Uop> issue(std::vector<Mop> mops) {
        global_phase = Phase::Prepare;
        q.prepare();
        global_phase = Phase::Comb;
        q.alloc(mops.size(), mops);
        auto out = q.issue(idvol.line_sum, idvol);
        global_phase = Phase::Commit;
        q.commit();
        return out.second;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// TakeLevel
// ---------------------------------------------------------------------------

TEST_CASE("take level emits one take line for a single line") {
    MopHarness h;
    const Price_t p = 100;

    h.idvol.insert_slot(A(0, p, Side_t::Sell), 1, 5);
    h.idvol.insert_slot(A(1, p, Side_t::Sell), 2, 3);
    h.idvol.insert_slot(A(2, p, Side_t::Sell), 3, 4);

    const std::vector<Uop> uops =
        h.issue({Mop{MopKind::TakeLevel, 99, 0, true, A(0, p, Side_t::Sell), 12}});

    REQUIRE(uops.size() == 1);
    CHECK(uops[0].kind == UopKind::TakeLine);
    CHECK(uops[0].id == 99);
    CHECK(uops[0].op_seq == 0);
    CHECK(uops[0].op_end);
    CHECK(uops[0].addr == A(0, p, Side_t::Sell));
    CHECK(uops[0].imm == 12);

    // Decode only: the book is untouched.
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Sell)) == 12);
}

TEST_CASE("take level emits one take line per non-empty id-vol line") {
    MopHarness h;
    const Price_t p = 0x20;

    for (Pos_t pos = 0; pos < 20; ++pos) {
        h.idvol.insert_slot(A(pos, p, Side_t::Sell), pos + 1, 1);
    }

    const std::vector<Uop> uops =
        h.issue({Mop{MopKind::TakeLevel, 7, 3, true, A(0, p, Side_t::Sell), 20}});

    REQUIRE(uops.size() == 2);
    CHECK(uops[0].kind == UopKind::TakeLine);
    CHECK(uops[0].addr == A(0, p, Side_t::Sell));
    CHECK(uops[0].imm == 16);
    CHECK_FALSE(uops[0].op_end);

    CHECK(uops[1].kind == UopKind::TakeLine);
    CHECK(uops[1].addr == A(16, p, Side_t::Sell));
    CHECK(uops[1].imm == 4);
    CHECK(uops[1].op_end); // only the last emitted uOP of the mOP carries op_end

    for (const Uop& u : uops) {
        CHECK(u.id == 7);
        CHECK(u.op_seq == 3);
    }
}

TEST_CASE("take level scans past the 16-line group boundary") {
    MopHarness h;
    const Price_t p = 0x30;

    // 257 orders: lines 0..15 are full (16 each), line 16 has one.
    for (Pos_t pos = 0; pos < 257; ++pos) {
        h.idvol.insert_slot(A(pos, p, Side_t::Sell), pos + 1, 1);
    }

    const std::vector<Uop> uops =
        h.issue({Mop{MopKind::TakeLevel, 5, 0, true, A(0, p, Side_t::Sell), 257}});

    REQUIRE(uops.size() == 17);
    CHECK(uops[0].addr == A(0, p, Side_t::Sell));
    CHECK(uops[0].imm == 16);
    CHECK(uops[15].addr == A(15u * 16u, p, Side_t::Sell));
    CHECK(uops[15].imm == 16);
    CHECK_FALSE(uops[15].op_end);

    // First line of group 1.
    CHECK(uops[16].addr == A(256, p, Side_t::Sell));
    CHECK(uops[16].imm == 1);
    CHECK(uops[16].op_end);
}

TEST_CASE("take level falls back to line 0 when the head hint is invalid") {
    MopHarness h;
    const Price_t p = 0x40;

    for (Pos_t pos = 0; pos < 16; ++pos) {
        h.idvol.insert_slot(A(pos, p, Side_t::Sell), pos + 1, 1);
    }
    h.idvol.insert_slot(A(16, p, Side_t::Sell), 100, 1);

    // Drain line 0, invalidating the head hint; line 1 still holds volume.
    VolLine all{};
    for (size_t i = 0; i < 16; ++i) all[i] = 1;
    h.idvol.take_line(A(0, p, Side_t::Sell), all, 16);
    CHECK(h.idvol.meta_head_line(L(p, Side_t::Sell)) == std::nullopt);
    CHECK(h.idvol.level_sum_ram(A(0, p, Side_t::Sell)) == 1);

    const std::vector<Uop> uops =
        h.issue({Mop{MopKind::TakeLevel, 1, 0, true, A(0, p, Side_t::Sell), 1}});

    REQUIRE(uops.size() == 1);
    CHECK(uops[0].addr == A(16, p, Side_t::Sell));
    CHECK(uops[0].imm == 1);
    CHECK(uops[0].op_end);
}

// ---------------------------------------------------------------------------
// InsertLevel
// ---------------------------------------------------------------------------

TEST_CASE("insert level places the slot at the committed tail") {
    MopHarness h;
    const Price_t p = 0x50;

    for (Pos_t pos = 0; pos < 5; ++pos) {
        h.idvol.insert_slot(A(pos, p, Side_t::Buy), pos + 1, 1);
    }

    const std::vector<Uop> uops =
        h.issue({Mop{MopKind::InsertLevel, 42, 2, true, A(0, p, Side_t::Buy), 10}});

    REQUIRE(uops.size() == 1);
    CHECK(uops[0].kind == UopKind::InsertSlot);
    CHECK(uops[0].id == 42);
    CHECK(uops[0].op_seq == 2);
    CHECK(uops[0].op_end);
    CHECK(uops[0].addr == A(5, p, Side_t::Buy));
    CHECK(uops[0].imm == 10);

    // Decode only: tail does not move.
    CHECK(h.idvol.tail_ram(A(0, p, Side_t::Buy)) == 5);
}

// ---------------------------------------------------------------------------
// CancelLevel / End
// ---------------------------------------------------------------------------

TEST_CASE("cancel level forwards the target address and id") {
    MopHarness h;

    const CanonicalAddr_t target = A(3, 0x60, Side_t::Sell);
    const std::vector<Uop> uops =
        h.issue({Mop{MopKind::CancelLevel, 0, 1, true, target, 77}});

    REQUIRE(uops.size() == 1);
    CHECK(uops[0].kind == UopKind::CancelSlot);
    CHECK(uops[0].id == 0);
    CHECK(uops[0].op_seq == 1);
    CHECK(uops[0].op_end);
    CHECK(uops[0].addr == target);
    CHECK(uops[0].imm == 77);
}

TEST_CASE("end marker becomes an end uOP") {
    MopHarness h;

    const std::vector<Uop> uops =
        h.issue({Mop{MopKind::End, 0, 4, true, 0, 0}});

    REQUIRE(uops.size() == 1);
    CHECK(uops[0].kind == UopKind::End);
    CHECK(uops[0].op_seq == 4);
    CHECK(uops[0].op_end);
}

TEST_CASE("multiple mOPs preserve program order") {
    MopHarness h;
    const Price_t p = 0x70;

    h.idvol.insert_slot(A(0, p, Side_t::Sell), 1, 2);

    const std::vector<Uop> uops = h.issue({
        Mop{MopKind::TakeLevel, 10, 0, false, A(0, p, Side_t::Sell), 2},
        Mop{MopKind::InsertLevel, 11, 0, true, A(0, p, Side_t::Buy), 3},
    });

    REQUIRE(uops.size() == 2);
    CHECK(uops[0].kind == UopKind::TakeLine);
    CHECK_FALSE(uops[0].op_end); // the first mOP was not marked op_end
    CHECK(uops[1].kind == UopKind::InsertSlot);
    CHECK(uops[1].imm == 3);
    CHECK(uops[1].op_end);
}
