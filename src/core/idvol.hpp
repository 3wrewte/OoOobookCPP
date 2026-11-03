#pragma once
#include <array>
#include "types.hpp"
#include "addr.hpp"
#include "sumcache.hpp"
#include "pricemeta.hpp"

//struct Slot { OrderID_t order_id; Qty_t volume; };   // 洞 = {0,0}
//struct Line { std::array<Slot, 16> slots; };
using IDLine   = std::array<OrderID_t, 16>;
using VolLine  = std::array<Qty_t    , 16>;
struct Line{
    IDLine  IDs ;
    VolLine Vols;
};

// struct TakeResult { //not needed, uOP already know it when issue(when uOP ROB reads IDVol)
//     std::array<OrderID_t,16> traded_ids;     // 每个 offset 碰到的 maker id，0=没吃
//     std::array<Qty_t,16>      traded_vols;   // 每个 offset 吃掉的量
//     Qty_t    consumed;                        // 本次总共吃掉
//     Qty_t    remaining_budget;                // 0 = 预算满足
//     QtySum_t line_remaining;                  // 吃完后这条 line 还剩多少
// };
struct CancelResult {
    bool found;
    OrderID_t order_id;
    Qty_t volume_removed;
};

// enum class UopApplyKind { Take, Insert, Cancel }; // should be in uOP
// struct UopMutation {
//     CanonicalAddr_t addr;
//     IDVol_key_t     line_key;
//     IDVol_index_t   index;   // 窗口命中用，golden 暂不用
//     IDVol_tag_t     tag;     // 同上
//     Line old_line, new_line;
//     QtyDelta_t tail_delta;   // 0 或 +1（仅 insert）
// };
//接口：golden 直接改版（M1 现在写这套）
struct IdVolMap { // also not RTL yet
    std::unordered_map<IDVol_key_t, Line > ram ;
    LineSumMap  line_sum;
    LevelSumMap level_sum;
    LevelSumMap tail;                             // use LevelSumMap to replace
    BookMeta    meta;

    void reset();

    // read through, does not fill cache line(since it does not exist yet)
    Qty_t    slot_volume      (CanonicalAddr_t addr);   // 单槽，从 ram 读
    Line     read_line        (CanonicalAddr_t addr);   // 整 line 快照
    QtySum_t line_volume      (CanonicalAddr_t addr);   // = read_line 求和
    QtySum_t line_sum_ram     (CanonicalAddr_t addr);   // line_sum.read_single_through
    QtySum_t level_sum_ram    (CanonicalAddr_t addr);   // level_sum.read_single_through
    QtySum_t tail_ram         (CanonicalAddr_t addr);   // tail.read_single_through
    std::array<QtySum_t,16> read_line_sum_row (CanonicalAddr_t addr);  // line_sum.read_row_through
    std::array<QtySum_t,16> read_level_sum_row(CanonicalAddr_t addr);  // level_sum.read_row_through
    std::optional<Pos_t> meta_head_line(CanonicalAddr_t addr);         // meta.get_meta_head_line
    void set_meta_head_line(CanonicalAddr_t addr, Pos_t);              // meta.set_meta_head_line

    // write-immediate
    void         take_line   (CanonicalAddr_t addr, VolLine delta_vec, QtySum_t delta_sum);// delta_vec and delta_sum is negtive delta, that is, the consumed delta
    void         insert_slot (CanonicalAddr_t addr, OrderID_t id, Qty_t vol);
    CancelResult cancel_slot (CanonicalAddr_t addr, OrderID_t id);
    CancelResult cancel_slot_with_volume(CanonicalAddr_t addr, OrderID_t id, Qty_t vol);
};

inline void IdVolMap::reset()
{
    line_sum.reset();
    level_sum.reset();
    tail.reset();
    meta.reset();
    ram.clear();
}

inline Qty_t IdVolMap::slot_volume(CanonicalAddr_t addr){
    const IDVolCacheView view = IDVolCacheIndexing(addr);
    const IDVol_key_t key = get_IDVol_key(view);
    if(ram.find(key) == ram.end())return 0;
    return ram[key].Vols[view.offset];
}

inline void IdVolMap::take_line(CanonicalAddr_t addr, VolLine delta_vec, QtySum_t delta_sum)
{
    const CanonicalAddrStructured addr_structured = CanonicalAddrUnpack(addr);
    const IDVolCacheView view = IDVolCacheIndexing(addr);
    const IDVol_key_t key = get_IDVol_key(view);
    assert(ram.find(key) != ram.end()); // empty line cannot be take
    const Line old_line = ram[key];
    Line new_line = Line();
    bool non_empty = false;
    new_line.IDs = old_line.IDs;
    for(size_t i = 0; i < 16; i++){
        new_line.Vols[i] = old_line.Vols[i] - delta_vec[i];
        non_empty |= (new_line.Vols[i] != 0);
    }
    if(non_empty)ram[key] = new_line;
    else ram.erase(key);

    const QtySum_t old_line_sum = line_sum.read_single_through(LineSumCacheIndexing(addr_structured)); //actually can be non-read-through
    const QtySum_t old_level_sum = level_sum.read_single_through(LevelSumCacheIndexing(addr_structured));
    line_sum.apply_delta(LineSumCacheIndexing(addr_structured), -QtyDelta_t(delta_sum));
    level_sum.apply_delta(LevelSumCacheIndexing(addr_structured), -QtyDelta_t(delta_sum));

    const QtySum_t new_line_sum = old_line_sum - QtyDelta_t(delta_sum); //actually can be non-read-through
    const QtySum_t new_level_sum = old_level_sum - QtyDelta_t(delta_sum);
    meta.sync_level_after_write(addr_structured.side, addr_structured.price, key, new_level_sum, new_line_sum, std::nullopt);
}

inline void IdVolMap::insert_slot(CanonicalAddr_t addr, OrderID_t id, Qty_t vol)
{
    const CanonicalAddrStructured addr_structured = CanonicalAddrUnpack(addr);
    const IDVolCacheView view = IDVolCacheIndexing(addr);
    const IDVol_key_t key = get_IDVol_key(view);
    const IDVol_offset_t offset = view.offset;
    const Pos_t old_tail = tail.read_single_through(LevelSumCacheIndexing(addr_structured));
    assert(old_tail == addr_structured.pos);
    Line & line = ram[key];//if not exist, map will insert one empty line.
    line.IDs[offset] = id;
    line.Vols[offset] = vol;

    const QtySum_t old_line_sum = line_sum.read_single_through(LineSumCacheIndexing(addr_structured)); //actually can be non-read-through
    const QtySum_t old_level_sum = level_sum.read_single_through(LevelSumCacheIndexing(addr_structured));
    line_sum.apply_delta(LineSumCacheIndexing(addr_structured), vol);
    level_sum.apply_delta(LevelSumCacheIndexing(addr_structured), vol);

    const QtySum_t new_line_sum = old_line_sum + vol;
    const QtySum_t new_level_sum = old_level_sum + vol;
    meta.sync_level_after_write(addr_structured.side, addr_structured.price, key, new_level_sum, new_line_sum, old_tail + 1);

    tail.apply_delta(LevelSumCacheIndexing(addr_structured), 1);
}

inline CancelResult IdVolMap::cancel_slot_with_volume(CanonicalAddr_t addr, OrderID_t id, Qty_t vol)
{
    const CanonicalAddrStructured addr_structured = CanonicalAddrUnpack(addr);
    const IDVolCacheView view = IDVolCacheIndexing(addr);
    const IDVol_key_t key = get_IDVol_key(view);
    const IDVol_offset_t offset = view.offset;
    if(ram.find(key) == ram.end()) // not find error, either wrong instruction or the line is already cleaned up
        return CancelResult{
            .found = false,
            .order_id = 0,
            .volume_removed = 0
        };

    Line line = ram[key];
    if(id != line.IDs[offset]) // id not match error
        return CancelResult{
            .found = false,
            .order_id = 0,
            .volume_removed = 0
        };
    const Qty_t canceled_vol = std::min(vol, line.Vols[offset]);
    line.Vols[offset] -= canceled_vol;
    bool non_empty = false;
    for(size_t i = 0; i < 16; i++){
        non_empty |= (line.Vols[i] != 0);
    }
    if(non_empty)ram[key] = line;
    else ram.erase(key);

    const QtySum_t old_line_sum = line_sum.read_single_through(LineSumCacheIndexing(addr_structured)); //actually can be non-read-through
    const QtySum_t old_level_sum = level_sum.read_single_through(LevelSumCacheIndexing(addr_structured));
    line_sum.apply_delta(LineSumCacheIndexing(addr_structured), -QtyDelta_t(canceled_vol));
    level_sum.apply_delta(LevelSumCacheIndexing(addr_structured), -QtyDelta_t(canceled_vol));

    const QtySum_t new_line_sum = old_line_sum - QtyDelta_t(canceled_vol);
    const QtySum_t new_level_sum = old_level_sum - QtyDelta_t(canceled_vol);
    meta.sync_level_after_write(addr_structured.side, addr_structured.price, key, new_level_sum, new_line_sum, std::nullopt);

    return CancelResult{
        .found = bool(canceled_vol),
        .order_id = id,
        .volume_removed = canceled_vol
    };
}

inline CancelResult IdVolMap::cancel_slot(CanonicalAddr_t addr, OrderID_t id)
{
    return cancel_slot_with_volume(addr, id, Qty_max);
}
