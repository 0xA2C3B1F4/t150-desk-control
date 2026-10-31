#pragma once
#include <cstdint>
#include <vector>
#include <cstring>
namespace esphome {
inline uint32_t saved_recovery=0; inline bool fail_preferences=false;
inline std::vector<uint8_t> preference_data;
inline bool corrupt_failed_load=false;
class ESPPreferenceObject { public:
 template<typename T> bool load(T *p){if(preference_data.size()!=sizeof(T)){if(corrupt_failed_load)std::memset(static_cast<void *>(p),0xa5,sizeof(T));return false;}std::memcpy(p,preference_data.data(),sizeof(T));return true;}
 template<typename T> bool save(const T *p){if(fail_preferences)return false;auto b=reinterpret_cast<const uint8_t *>(p);preference_data.assign(b,b+sizeof(T));std::memcpy(&saved_recovery,p,4);return true;}
};
struct FakePreferences {template<typename T> ESPPreferenceObject make_preference(uint32_t){return {};}
 bool sync(){return !fail_preferences;}};
inline FakePreferences prefs; inline FakePreferences *global_preferences=&prefs;
}
