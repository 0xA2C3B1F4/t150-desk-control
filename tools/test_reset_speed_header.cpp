#include <cstdint>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include "components/desk_firmware_update/reset_speed_patch.h"
using namespace esphome::desk_firmware_update;
std::vector<uint8_t> read(const char* p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
int main(int argc, char** argv) {
  if (argc != 4)
    return 1;
  auto stock = read(argv[1]), cl01 = read(argv[2]), expected = read(argv[3]);
  if (stock.size() != 65536 || cl01.size() != 65536 || expected.size() != 65536)
    return 2;
  std::vector<uint8_t> app(cl01.begin() + 0x2800, cl01.end());
  if (!apply_reset_speed(app.data(), app.size()))
    return 3;
  for (size_t i = 0; i < app.size(); ++i) {
    if (app[i] != expected[0x2800 + i])
      return 4;
    if (rs01_byte(i, stock[0x2800 + i]) != app[i])
      return 5;
  }
  if (apply_reset_speed(app.data(), app.size()))
    return 6;
  if (apply_reset_speed(nullptr, IMAGE_SIZE) || apply_reset_speed(app.data(), IMAGE_SIZE - 1))
    return 7;
  std::cout
      << "All " << app.size()
      << " application bytes match apply_reset_speed and rs01_byte, malformed/repeated inputs refused\n";
}
