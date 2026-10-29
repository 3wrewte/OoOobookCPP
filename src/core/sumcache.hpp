#include <array>
#include <unordered_map>
#include <numeric>
#include "phase.hpp"
#include "types.hpp"
#include "addr.hpp"

struct SumMap { // a temporary substitution of LineSumCache and LevelSumCache, write immediate, actually not RTL structure!!!
  std::unordered_map<uint64_t, std::array<QtySum_t,16>> ram;  // key = line_key
  void apply_delta(uint64_t key, uint32_t offset, QtyDelta_t delta); // write-through
  std::array<QtySum_t,16> read_row_through(uint64_t key);                     // read through !!!
  QtySum_t read_single_through(uint64_t key, uint32_t offset);                // read through !!!
};

void SumMap::apply_delta(uint64_t key, uint32_t offset, QtyDelta_t delta){
    if(ram.find(key) == ram.end()){ //empty line
        assert(delta >= 0);
        ram[key] = std::array<QtySum_t,16>();
        ram[key][offset] = delta;
        return;
    }
    ram[key][offset] = std::saturating_add(int64_t(ram[key][offset]), delta);
    for(size_t j = 0; j < 16; j++)if (ram[key][j] != 0) return; //if all clear, then delete from map
    ram.erase(ram.find(key));
}

std::array<QtySum_t,16> SumMap::read_row_through(uint64_t key){
  if(ram.find(key) == ram.end())return std::array<QtySum_t, 16>();
  else return ram[key];
}

QtySum_t SumMap::read_single_through(uint64_t key, uint32_t offset){
  if(ram.find(key) == ram.end())return 0;
  else return ram[key][offset];
}
/*
template <size_t W, size_t R>
struct LineSumCache
{
    std::array<std::array<QtySum_t, 16>, 32> slots;                  // 一行 16 槽 reg
    std::array<bool, 32> valid;                                      // reg
    std::array<LineSum_tag_t, 32> tag;                               // reg
    std::unordered_map<LineSum_key_t, std::array<QtySum_t, 16>> ram; // 下层（稀疏，全零即删） reg

    std::array<LineSumCacheView, W> pending_addr  ;                       // wire
    std::array<QtyDelta_t, W>       pending_deltas;                       // wire
    std::array<bool, W>             pending_valid ;                       // wire
    std::array<bool, W>             w_port_driven ;                       // wire

    std::array<LineSum_key_t, R>            fill_request  ; // wire
    std::array<bool, R>                     fill_valid    ; // wire
    std::array<bool, R>                     r_port_driven ;                       // wire
  
    //void reset();
    //void prepare();
    //void commit();
  
    //void apply_delta(constexpr size_t port, LineSumCacheView &addr, QtyDelta_t delta); // 写穿+命中镜
    //QtySum_t read(constexpr size_t port, LineSumCacheView &addr);                      // if miss, 填充窗口
    //QtySum_t read_ram(LineSumCacheView &addr);                  // for debug only, will not trigger filling
    //std::array<QtySum_t, 16> read_row_ram(LineSum_key_t &key);  // for debug only, will not trigger filling

    void reset()
    {
      slots = std::array<std::array<QtySum_t, 16>, 32>();
      valid.fill(false);
      tag.fill(0);
      ram = std::unordered_map<LineSum_key_t, std::array<QtySum_t, 16>>();
    }

    void prepare()
    {
      assert(global_phase == Phase::Prepare);
      pending_addr  .fill(0);
      pending_deltas.fill(0);
      pending_valid .fill(false);
      w_port_driven.fill(false);
      fill_request.fill(0);
      fill_valid.fill(false);
      r_port_driven.fill(false);
      //pending_RAM_addr    .fill(0);
      //pending_RAM_deltas  .fill(0);
      //pending_RAM_valid   .fill(false);
      //fill_do   .fill(0);
      //fill_index.fill(0);
      //fill_data .fill(std::array<QtySum_t, 16>());
    }

    void commit(){
      assert(global_phase == Phase::Commit);
      for (size_t i = R - 1; i >= 0; i--){ // fill in lower port is priortized and will cover fillinf in upper port
        if(fill_valid[i]){
          std::array<QtySum_t, 16> value = (ram.find(fill_request[i]) == ram.end())? std::array<QtySum_t, 16>() : ram[fill_request[i]];
          //if key exists in ram, use ram, if not, use new empty array
          const LineSum_index_t idx = LineSum_key2index(fill_request[i]);
          const LineSum_tag_t   t   = LineSum_key2tag  (fill_request[i]);
          tag[idx] = t;
          slots[idx] = value;
          valid[idx] = true;
        }
      }
      for (size_t i = W - 1; i >= 0; i--){ // write in lower port is priortized and will cover the write in upper port
        if(pending_valid[i]){
          const LineSum_index_t idx = pending_addr[i].index;
          if(valid[idx] && tag[idx] == pending_addr[i].tag ){ // srote hit, updating cached value
            slots[idx][pending_addr[i].offset] += pending_deltas[i];
          }
        }
      }
      for (size_t i = W - 1; i >= 0; i--){ // write in lower port is priortized and will cover the write in upper port
        if(pending_valid[i]){
          const LineSum_key_t k   = get_LineSum_key(pending_addr[i]);
          QtySum_t sum = 0;
          for(size_t j = 0; j < 16; j++)sum += ram[k][j];
          if(sum + pending_deltas[i] == 0)ram.erase(ram.find(k));
          else ram[k][pending_addr[i].offset] += pending_deltas[i];
        }
      }
    }

    void apply_delta(constexpr size_t port, LineSumCacheView &addr, QtyDelta_t delta){
      assert(global_phase == Phase::Comb);
      assert(port < W);
      assert(!w_port_driven[port]);
      w_port_driven [port] = true;
      pending_addr  [port] = addr;
      pending_deltas[port] = delta;
      pending_valid [port] = true;
    }

    QtySum_t read(constexpr size_t port, LineSumCacheView &addr){
      assert(global_phase == Phase::Comb);
      assert(port < R);
      assert(!r_port_driven[port]);
      r_port_driven[port] = true;
      bool hit = addr.tag == tag[addr.index] && valid[addr.index];
      if(!hit){
        fill_request[port] = get_LineSum_key(addr);
        fill_valid[port] = true;
        return ...
      }
      return ...
    }

    QtySum_t read_ram(LineSumCacheView &addr){
      std::array<QtySum_t, 16> line = (ram.find(get_LineSum_key(addr)) == ram.end())? std::array<QtySum_t, 16>() : ram[get_LineSum_key(addr)];
      return line[addr.offset];
    }
    std::array<QtySum_t, 16> read_row_ram(LineSum_key_t &key){
        return (ram.find(key) == ram.end())? std::array<QtySum_t, 16>() : ram[key];
    }
};
*/
