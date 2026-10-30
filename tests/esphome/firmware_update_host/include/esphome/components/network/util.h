#pragma once
#include <array>
#include <cstring>
namespace esphome::network {
struct IP {bool is_set() const {return true;} void str_to(char *p)const{std::strcpy(p,"192.0.2.1");}};
inline std::array<IP,1> get_ip_addresses(){return {};}
}
