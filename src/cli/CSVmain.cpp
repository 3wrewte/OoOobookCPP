#include "core/top.hpp"
#include "bookkeeping/IDallocator.hpp"
#include "io/csvloader.hpp"
#include "io/csvrecorder.hpp"

struct top{
    core_top core;
    IDallocator id_allocator;
    InstructionsFromCSV instructions;//("build/orders.csv");
    TradeStreamToCSV    trades      ;//("build/trades.csv");
    TickStreamToCSV     ticks       ;//("build/ticks.csv");


    top();

    void reset  ();
    void prepare();
    void commit ();

    void comb   ();
    bool end_flag();//instructions empty && OP ROB empty
};


int main(int argc, char** argv){
    top top_u;
    top_u.reset();
    while(true){
        global_phase = Phase::Prepare;
        top_u.prepare();
        global_phase = Phase::Comb;
        if(top_u.end_flag())break;
        top_u.comb();
        global_phase = Phase::Commit;
        top_u.commit();
    }
    return 0;
}

top::top(){
    instructions = InstructionsFromCSV("build/orders.csv");
    trades       = TradeStreamToCSV   ("build/trades.csv");
    ticks        = TickStreamToCSV    ("build/ticks.csv" );
}

void top::reset  (){
    core        .reset();
    id_allocator.reset();
    instructions.reset();
    trades      .reset();
    ticks       .reset();
}
void top::prepare(){
    core        .prepare();
    id_allocator.prepare();
    instructions.prepare();
    trades      .prepare();
    ticks       .prepare();
}
void top::commit (){
    core        .commit();
    id_allocator.commit();
    instructions.commit();
    trades      .commit();
    ticks       .commit();
}

void top::comb(){
    const std::vector<OPcontext> opcs = instructions.issue(core.op_queue.ready_num());
    std::vector<OP> ops;
    for(auto i : opcs){
        ops.push_back(id_allocator.allocate(i));
    }
    auto [event_num, events] = core.comb(instructions.get_valid_num(), ops);
    auto lookup = [&](OrderID_t id){ return id_allocator.get(id); };
    trades.record(event_num, events, lookup);
    ticks.record(event_num, events, lookup);
}

bool top::end_flag(){
    return (instructions.get_valid_num() == 0) && (core.op_queue.ready_num() == OP_ROB_LENGTH - 1);
}
