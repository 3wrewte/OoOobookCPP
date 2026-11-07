#pragma once
#include <queue>
#include <string>
#include <vector>
#include "core/instructions.hpp"
#include "core/addr.hpp"
#include "core/types.hpp"
#include "core/registers.hpp"


struct InstructionsFromCSV{
    std::vector<OPcontext> instructions;
    std::string filename;
    Register<size_t> current;

    InstructionsFromCSV(std::string name){
        filename = name;
    }

    void reset();
    void prepare();
    void commit();

    size_t get_valid_num(){
        return instructions.size() - current.get();
    }

    std::vector<OPcontext> issue(size_t ready_num){
        const size_t num = std::min(get_valid_num(), ready_num); //handshake
        current.set(current.get() + num);
        std::vector<OPcontext> results;
        for(size_t i = 0; i < num; i++){
            results[i] = instructions[i + current.get()];
        }
        return results;
    }
};

inline void InstructionsFromCSV::reset(){
    current.reset();
    // read filename to instructions
}
inline void InstructionsFromCSV::prepare(){
    current.prepare();
}
inline void InstructionsFromCSV::commit(){
    current.commit();
}
