#include <string>

#include "engine_top.hpp"
#include "io/csvloader.hpp"
#include "io/csvrecorder.hpp"

struct top{
    engine_top engine;
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
    engine      .reset();
    instructions.reset();
    trades      .reset();
    ticks       .reset();
}
void top::prepare(){
    engine      .prepare();
    instructions.prepare();
    trades      .prepare();
    ticks       .prepare();
}
void top::commit (){
    engine      .commit();
    instructions.commit();
    trades      .commit();
    ticks       .commit();
}

void top::finish(){
    trades.finish();
    ticks.finish();
}
void top::comb(){
    std::vector<OPcontext> opcs = instructions.issue(engine.op_queue.ready_num());
    auto [trade_events, tick_events, reply_events] =
        engine.comb(opcs.size(), opcs);
    (void)reply_events;
    trades.record(trade_events);
    ticks.record(tick_events);
}

bool top::end_flag(){
    return (instructions.get_valid_num() == 0) && (engine.op_queue.ready_num() == OP_ROB_LENGTH - 1);
}
