#include <string>

#include "core/top.hpp"
#include "bookkeeping/IDallocator.hpp"
#include "io/csvloader.hpp"
#include "io/csvrecorder.hpp"

struct top{
    core_top core;
    IDallocator id_allocator;
    InstructionsFromCSV instructions;
    TradeStreamToCSV    trades      ;
    TickStreamToCSV     ticks       ;


    top(std::string orders_path, std::string trades_path, std::string ticks_path);

    void reset  ();
    void prepare();
    void commit ();
    void finish ();

    void comb   ();
    bool end_flag();//instructions empty && OP ROB empty
};


int main(int argc, char** argv){
    std::string orders = "build/orders.csv";
    std::string trades = "build/trades.csv";
    std::string ticks  = "build/ticks.csv";
    if (argc >= 2) orders = argv[1];
    if (argc >= 3) trades = argv[2];
    if (argc >= 4) ticks  = argv[3];

    top top_u(orders, trades, ticks);
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
    top_u.finish();
    return 0;
}

top::top(std::string orders_path, std::string trades_path, std::string ticks_path)
    : instructions(std::move(orders_path)),
      trades      (std::move(trades_path)),
      ticks       (std::move(ticks_path)) {
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

void top::finish(){
    trades.finish();
    ticks.finish();
}
void top::comb(){
    const std::vector<OPcontext> opcs = instructions.issue(core.op_queue.ready_num());
    std::vector<OP> ops;
    for(auto i : opcs){
        ops.push_back(id_allocator.allocate(i));
    }
    auto [event_num, events] = core.comb(ops.size(), ops);
    auto lookup = [&](OrderID_t id){ return id_allocator.get(id); };
    trades.record(event_num, events, lookup);
    ticks.record(event_num, events, lookup);
}

bool top::end_flag(){
    return (instructions.get_valid_num() == 0) && (core.op_queue.ready_num() == OP_ROB_LENGTH - 1);
}
