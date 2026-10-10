#include "unit_order.h"
#include <cstdio>
#include <stdexcept>
static void require(bool ok,const char* label){if(!ok)throw std::runtime_error(label);}
int main() {
  OrderInput in;in.self=1;in.home_x=1000;in.home_y=1000;in.capacity=1700;
  // raw ids double as labels: own 1x, enemy 2x, ally 3x, neutral 9x
  std::vector<UnitEntry> units{
    {90,11,5000,5000},   // far mineral
    {20,2,1500,1000},    // enemy near
    {10,1,1000,1000},    // own
    {30,3,1100,1000},    // ally (player 3)
    {91,11,1050,1000},   // near mineral
    {21,2,9000,9000},    // enemy far
    {11,1,1200,1200},    // own (keeps original order after the first own unit)
    {92,8,2000,2000},    // player 8+ counts as neutral
  };
  require(order_units(units,in).size()==8,"Without hidden owners every unit is kept");
  in.hidden[3]=true;
  size_t dropped=99;
  auto order=order_units(units,in,&dropped);
  require(dropped==0,"Hidden owners are not counted as capacity drops");
  std::vector<uint32_t> raws;for(auto i:order)raws.push_back(units[i].raw);
  require(raws==std::vector<uint32_t>{10,11,20,21,91,92,90},"Expected own, enemies by distance, neutrals by distance, ally removed");
  // Capacity cut removes the tail: far neutrals first, then far enemies, never own units.
  in.capacity=4;
  order=order_units(units,in,&dropped);
  raws.clear();for(auto i:order)raws.push_back(units[i].raw);
  require(raws==std::vector<uint32_t>{10,11,20,21} && dropped==3,"Cut farthest neutrals first");
  in.capacity=3;
  order=order_units(units,in,&dropped);
  raws.clear();for(auto i:order)raws.push_back(units[i].raw);
  require(raws==std::vector<uint32_t>{10,11,20} && dropped==4,"Then farthest enemies");
  in.capacity=1;
  order=order_units(units,in,&dropped);
  require(order.size()==1 && units[order[0]].raw==10 && dropped==6,"Capacity below own count still cuts");
  require(order_units({},in,&dropped).empty() && dropped==0,"Empty list");
  std::puts("unit ordering: ally removal, own/enemy/neutral priority, distance order, capacity cut checks passed");
}
