#ifndef REGISTER_H
#define REGISTER_H
#include<array>
#include<assert.h>
#include "phase.hpp"

// Double-buffer register (flop). Retain semantics: an un-driven register keeps
// its value across a tick, exactly like `always_ff @(posedge)`.
//
//   prepare(): next = current  (default = retain), clear driven
//   comb:      set(x)  -> next = x            (full overwrite)
//              nxt()   -> mutable ref to next (read-modify-write; next already
//                         holds a copy of current from prepare)
//   commit():  phase = !phase
template <typename T>
struct DoubleBufferRegister {
    T bank[2];       // reg
    bool phase;      // reg
    bool driven;     // wire

    void reset() {
        bank[0]=T(); 
        bank[1]=T(); 
        phase=false;
        driven = false;
    }
    
    void prepare(){
        assert(global_phase == Phase::Prepare);
        bank[!phase] = bank[phase]; // default next = retain current
        driven = false;
    }

    void commit() { 
        assert(global_phase == Phase::Commit);
        phase = !phase;
        // if (!driven) -> retained; emit a warning here in debug builds if you
        // want to catch a possibly-missing driver.
    }

    const T& get() const { 
        return bank[phase]; 
    }      // 只读 current

    void set(const T& x) { 
        assert(global_phase == Phase::Comb);
        assert(driven == 0);
        driven = 1;
        bank[!phase] = x; 
    }        // 写 next

    // Read-modify-write: returns a sratic reference to next (already a copy of
    // current from prepare). read-only and only available after driven
    const T& nxt() {
        assert(driven == 1);
        return bank[!phase]; 
    }

    
};


/*
template <typename T>
struct PendingChangeRegister {};
*/

#endif