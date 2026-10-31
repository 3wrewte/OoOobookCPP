#include <doctest/doctest.h>
#include "src/core/wire.hpp"
#include "src/core/registers.hpp"
#include "src/core/RAM.hpp"
#include "src/core/types.hpp"

// A small composed module demonstrating the three-phase protocol
// (prepare -> comb -> commit) with a register, a single-port RAM, and a wire.
struct PipelineSlice {
    DoubleBufferRegister<uint32_t> index;
    RAM<uint32_t, 1, 16> ram;
    wire<uint32_t> nxt;

    void reset()   { index.reset(); ram.reset(); nxt.reset(); }
    void prepare() { index.prepare(); ram.prepare(); nxt.prepare(); }
    void commit()  { index.commit(); ram.commit(); nxt.commit(); }

    // comb: address = index*3+1 (low nibble), then read RAM at that address.
    uint32_t comb1() {
        uint32_t tmp = index.get() * 3 + 1;
        nxt.set(tmp & 0b1111u);
        return ram.read(nxt.get());
    }
    // comb: write RAM + advance the index register.
    void comb2(uint32_t din) {
        ram.write<0>(nxt.get(), din, true);
        index.set(nxt.get());
    }
};

TEST_CASE("composed module advances index and writes RAM each tick") {
    PipelineSlice u;
    u.reset();
    uint32_t expect_index = 0;
    for (uint32_t i = 0; i < 32; i++) {
        global_phase = Phase::Prepare;
        u.prepare();
        global_phase = Phase::Comb;
        (void)u.comb1();
        uint32_t din = (i + 1) * 7;
        u.comb2(din);
        global_phase = Phase::Commit;
        u.commit();

        expect_index = (expect_index * 3 + 1) & 0b1111u;
        CHECK(u.index.get() == expect_index);
        CHECK(u.ram.read(expect_index) == din);
    }
}

TEST_CASE("register publishes a set value") {
    DoubleBufferRegister<uint32_t> r;
    r.reset();
    global_phase = Phase::Prepare; r.prepare();
    global_phase = Phase::Comb;   r.set(42);
    global_phase = Phase::Commit; r.commit();
    CHECK(r.get() == 42);
}

TEST_CASE("register retains when not driven") {
    DoubleBufferRegister<uint32_t> r;
    r.reset();
    global_phase = Phase::Prepare; r.prepare();
    global_phase = Phase::Comb;   r.set(42);
    global_phase = Phase::Commit; r.commit();
    CHECK(r.get() == 42);

    global_phase = Phase::Prepare; r.prepare();
    global_phase = Phase::Comb;   /* not driven */
    global_phase = Phase::Commit; r.commit();
    CHECK(r.get() == 42);
}

TEST_CASE("register nxt() reads the pending next value after set") {
    DoubleBufferRegister<uint32_t> r;
    r.reset();
    global_phase = Phase::Prepare; r.prepare();
    global_phase = Phase::Comb;   r.set(10);
    CHECK(r.nxt() == 10);          // pending next, not yet committed
    global_phase = Phase::Commit; r.commit();
    CHECK(r.get() == 10);
}

TEST_CASE("ram write commits and reads back") {
    RAM<uint32_t, 1, 16> ram;
    ram.reset();
    global_phase = Phase::Prepare; ram.prepare();
    global_phase = Phase::Comb;   ram.write<0>(3, 99, true);
    global_phase = Phase::Commit; ram.commit();
    CHECK(ram.read(3) == 99);
}

TEST_CASE("ram read_through sees a staged write") {
    RAM<uint32_t, 1, 16> ram;
    ram.reset();
    global_phase = Phase::Prepare; ram.prepare();
    global_phase = Phase::Comb;   ram.write<0>(5, 77, true);
    CHECK(ram.read(5) == 0);           // synchronous read: old value
    CHECK(ram.read_through(5) == 77);  // transparent read: staged write
    global_phase = Phase::Commit; ram.commit();
    CHECK(ram.read(5) == 77);
}

TEST_CASE("wire set then get") {
    wire<uint32_t> w;
    w.reset();
    global_phase = Phase::Prepare; w.prepare();
    global_phase = Phase::Comb;   w.set(123);
    CHECK(w.get() == 123);
    global_phase = Phase::Commit; w.commit(); // asserts driven == 1
}
