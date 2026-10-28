#include "src/core/wire.hpp"
#include "src/core/registers.hpp"
#include "src/core/RAM.hpp"
#include "src/core/types.hpp"
#include<random>
#include<iostream>

struct myMudule{
    DoubleBufferRegister<uint32_t> index;
    RAM<uint32_t, 1, 16> ram;
    wire<uint32_t> nxt;
    void reset(){
        index.reset();
        ram  .reset();
        nxt  .reset();
    }
    void prepare(){
        index.prepare();
        ram  .prepare();
        nxt  .prepare();
    }
    void commit(){
        index.commit();
        ram  .commit();
        nxt  .commit();
    }
    
    uint32_t comb1(){
        int tmp = index.get() * 3 + 1;
        nxt.set(tmp & 0b1111);
        return ram.read(nxt.get());
    }
    void comb2(const uint32_t &x){
        ram.write(0, nxt.get(), x, 1);
        index.set(nxt.get());
    }
};

void cat(myMudule m){
    std::cout << "index:" <<m.index.bank[0] << " " << m.index.bank[1] << " " << m.index.phase << " nxt: " << m.nxt.val << " RAM: " ;
    for(int i = 0; i < 16; i++){
        std::cout << m.ram.bank[i] << " ";
    }
    std::cout << std::endl;
}

int main(){
    srand(time(0));
    myMudule u0;
    u0.reset();
    for (int i = 0; i < 32; i++){
        std::cout << "begining i = " << i << std::endl;
        global_phase = Phase::Prepare;
        u0.prepare();
        cat(u0);

        global_phase = Phase::Comb;
        u0.comb1();
        cat(u0);
        uint32_t din = rand();
        u0.comb2(din);
        cat(u0);

        global_phase = Phase::Commit;
        u0.commit();
        cat(u0);
    }
    return 0;
}