#include<bits/stdc++.h>
#include<assert.h>
#include "phase.hpp"

template <typename T>
struct DoubleBufferRegister {
    T bank[2];       // reg
    bool phase;      // reg
    bool driven;     // wire
    
    void reset() {
        bank[0]=T(); 
        bank[1]=T(); 
        phase=false;
    }
    
    void prepare(){
        assert(global_phase == Phase::Prepare)
        bank[~phase] = T();
        driven = 0;
    }

    void commit() { 
        assert(global_phase == Phase::Commit)
        phase = !phase; 
        if(!driven){
            bank[phase] = bank[~phase]
            // warning: not driven
        }
    }

    const T& get() const { 
        return bank[phase]; 
    }      // 只读 current

    void set(const T& x) { 
        assert(global_phase == Phase::Comb)
        assert(driven == 0)
        driven = 1;
        bank[!phase] = x; 
    }        // 写 next

    T& nxt() {
        assert(driven == 1)
        return bank[~phase]; 
    }

    
};

template <typename T>
using Register = DoubleBufferRegister;