#include<cstdint>
#include<cstddef>
#include<optional>
#include<unordered_map>
#include<array>
#include<assert.h>
#include"types.hpp"
#include"phase.hpp"

struct PriceMeta {
    QtySum_t level_sum ;
    Pos_t    head_line ;//pos[27:4], Hint, NOT AUTHORIZED !!!
    bool     head_valid;
    Pos_t    tail_pos  ;
    bool     active    ;
};
struct SideMeta{
    std::unordered_map<Price_t, PriceMeta> PriceMetaRAM;
    std::unordered_map<Price_t, uint16_t > row_masks   ; // mask price[3:0] per row
    std::unordered_map<Price_t, uint64_t > block_masks ; // mask price[9:4] per block
    std::array        <uint64_t,4096     > super_masks ; // mask price[15:10] per superblock, index price[27:16] in total 4096
    size_t active_levels;
    std::optional<Price_t> best;

    void set_active(Price_t price, bool active){
        Price_t row_index   = price >> 4 ;
        Price_t block_index = price >> 10;
        Price_t super_index = price >> 18;
        Price_t row_offset  = price & 0x0f; // price[3:0]
        Price_t block_offset= (price >> 4 ) & 0x3f; // price[9:4]
        Price_t super_offset= (price >> 10) & 0x3f; // price[15:10]
        if (active){ //activate
            if(row_masks.find(row_index) == row_masks.end()){
                row_masks[row_index] = 0;
            }
            row_masks[row_index] |= 1 << row_offset;

            if(block_masks.find(block_index) == block_masks.end()){
                block_masks[block_index] = 0;
            }
            block_masks[block_index] |= 1 << block_offset;

            super_masks[super_index] |= 1 << super_offset;
        }
        else{ // deactivate
            if(row_masks.find(row_index) != row_masks.end()){ //if not find, then do nothing
                row_masks[row_index] &= ~(1 << row_offset);
                if (row_masks[row_index] == 0)row_masks.erase(row_masks.find(row_index));
            }

            if(block_masks.find(block_index) != block_masks.end()){ //if not find, then do nothing
                block_masks[block_index] &= ~(1 << block_offset);
                if (block_masks[block_index] == 0)block_masks.erase(block_masks.find(block_index));
            }

            super_masks[super_index] &= ~(1 << super_offset);
        }

    }

    void refresh_best_after_remove(Side_t side, Price_t price){
        if(best.has_value()){
            if(row_masks.find(best.value() >> 4) != row_masks.end()){
                if (row_masks[best.value() >> 4] & (1 << best.value() & 0x0f)){ // best is valid and still active
                    return;
                }
            }
        }
        // search for best
    }

    void refresh_best_after_insert(Side_t side, Price_t price){
        if(side == Side_t::Buy){
            best = std::min(price, best.value_or(Price_max));
        }
        else if(side == Side_t::Sell){
            best = std::max(price, best.value_or(Price_min));
        }
    }

    std::optional<Price_t> search_upper_active(Price_t start, Price_t end){
        assert(start <= end);
        Price_t start_row_index   = start >> 4 ;
        Price_t start_block_index = start >> 10;
        Price_t start_super_index = start >> 18;
        Price_t start_row_offset  = start & 0x0f; // price[3:0]
        Price_t start_block_offset= (start >> 4 ) & 0x3f; // price[9:4]
        Price_t start_super_offset= (start >> 10) & 0x3f; // price[15:10]

        Price_t end_row_index   = end >> 4 ;
        Price_t end_block_index = end >> 10;
        Price_t end_super_index = end >> 18;
        Price_t end_row_offset  = end & 0x0f; // price[3:0]
        Price_t end_block_offset= (end >> 4 ) & 0x3f; // price[9:4]
        Price_t end_super_offset= (end >> 10) & 0x3f; // price[15:10]

        Price_t res_row_index   ;
        Price_t res_block_index ;
        Price_t res_super_index ;
        Price_t res_row_offset  ; // price[3:0]
        Price_t res_block_offset; // price[9:4]
        Price_t res_super_offset; // price[15:10]

        if(super_masks[start_super_index] & ~((1 << start_super_offset) - 1)) { // 1 from offset to all upper offset
            res_super_index = start_super_index;
            res_super_offset ;
            goto super_done;
        }
        for(Price_t i = start_super_index + 1; i < end_super_index; i++ ){
            if(super_masks[i]){
                res_super_index = i;
                res_super_offset ;
                goto super_done;
            }
        }
        if(super_masks[end_super_index] & ((1 << end_super_offset) - 1)) { // 1 from offset 0 to end offset
            res_super_index = end_super_index;
            res_super_offset ;
            goto super_done;
        }
        return std::nullopt;
    super_done:

    }
};

struct BookMeta { // temporary, not RTL

    uint64_t version;
    SideMeta sides[2];
    void sync_level_after_write(Side_t side, Price_t price, QtySum_t old_sum, QtySum_t new_sum, Pos_t touched_line, QtySum_t touched_line_sum, Pos_t tail_pos);
    std::optional<Price_t> best_ask();        // 最低卖价
    std::optional<Price_t> best_bid();        // 最高买价
    std::optional<Price_t> next_ask_after(Price_t p, Price_t limit); // 从 p 向上走下一个活跃卖价，止于 limit
    std::optional<Price_t> next_bid_below(Price_t p, Price_t limit); // 从 p 向下走
    uint64_t get_version();
};

void BookMeta::sync_level_after_write(Side_t side, Price_t price, QtySum_t old_sum, QtySum_t new_sum, Pos_t touched_line, QtySum_t touched_line_sum, Pos_t tail_pos)
{
    if(sides[side].PriceMetaRAM.find(price) == sides[side].PriceMetaRAM.end()){
        sides[side].PriceMetaRAM[price] = PriceMeta();
    }
    PriceMeta &p = sides[side].PriceMetaRAM[price];

    bool was_active = p.active;
    bool is_active  = (new_sum != 0);

    // Modify PriceMeta
    if(!was_active && !is_active){
        p.active = false;
        p.head_valid = false;
    }
    else if (!was_active &&  is_active){
        p.active = true;
        p.head_valid = true;
        p.head_line = touched_line; // this modification activates the level

    }
    else if ( was_active && !is_active){
        p.active = false;
        p.head_valid = false;
        p.head_line = touched_line; // at this time head = tail-1 = last modified position
    }
    else if ( was_active &&  is_active){
        if (p.head_valid && p.head_line == touched_line && touched_line_sum == 0 ){
            p.head_valid = false;
        }
    }

    //if active changes, update masks and version
    if( was_active && !is_active){
        sides[side].set_active(price, false);
        sides[side].refresh_best_after_remove(side, price);
        version++;
    }
    if(!was_active && is_active){
        sides[side].set_active(price, false);
        sides[side].refresh_best_after_insert(side, price);
        version++;
    }
}
