#ifndef RAM_H
#define RAM_H
#include<array>
#include<cstddef>
#include<assert.h>
#include "phase.hpp"


template <typename T, size_t P, size_t D>
struct RAM {
    std::array<T, D> bank;      // reg
    std::array<T, P> pending_data;             // wire
    std::array<size_t, P> pending_addr;                      // wire
    std::array<bool, P>   pending_enable;                     // wire：必须有，否则每 tick 误写 bank[0]
    std::array<bool, P> driven; // wire
    void reset() { 
        bank.fill(T()); 
        pending_enable.fill(false);
        pending_data.fill(T());
        pending_addr.fill(0);
        driven.fill(false);
    }
    void prepare(){
        assert(global_phase == Phase::Prepare);
        pending_enable.fill(false); 
        pending_data.fill(T());
        pending_addr.fill(0);
        driven.fill(false);

    }

    const T& read(size_t& addr) const {
        assert(addr < D) ;
        return bank[addr]; 
    }  // 旧值

    template<size_t port>
    void write(size_t& addr, const T& din, bool enable) {
        static_assert(port < P);
        assert(global_phase == Phase::Comb) ;
        assert(addr < D);
        assert(driven[port]  == false);
        pending_addr[port]=addr; 
        pending_data[port]=din; 
        pending_enable[port]=enable;
        driven[port] = true;
    }
    
    // Transparent read: sees a write staged on the same tick (lowest port wins
    // on a same-address conflict; callers should merge same-line writes first).
    const T& read_through(size_t& addr) const {
        assert(addr < D) ;
        for (size_t i = 0; i < P; i++){
            if (pending_addr[i] == addr && pending_enable[i]) return pending_data[i]; 
        }
        return bank[addr];
    }
    void commit() { 
        assert(global_phase == Phase::Commit);
        for (size_t i = 0; i < P; i++){
            if (pending_enable[i]) bank[pending_addr[i]] = pending_data[i]; 
        }
    }
};


/*
template <typename T, size_t P, size_t D>
struct SinglePortRAM {}; // to be complete if needed
*/
#endif
