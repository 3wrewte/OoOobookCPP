#ifndef WIRE_H
#define WIRE_H

#include<assert.h>
#include "phase.hpp"

template<typename T>
struct wire{
    T val; //template wire
    bool driven;

    void reset(){
        val = T();
        driven = 0;
    }
    void prepare(){
        assert(global_phase == Phase::Prepare);
        driven = 0;
        val = T();
    }
    void commit(){
        assert(global_phase == Phase::Commit);
        assert(driven == 1);
    }

    void set(const T &x){
        assert(global_phase == Phase::Comb);
        assert(driven==0);
        driven = 1;
        val = x;
    }
    const T& get() const {
        assert(driven == 1);
        return val;
    }
};

#endif