#pragma once
#include "protocol.h"

namespace desk_protocol {

// Fixed-width text fields verified on T150-22-03. Selector 08 is a counter,
// not text. Field labels do not imply hardware/software/protocol versions.
inline std::size_t identity_text_size(uint8_t selector) {
  switch (selector) {
    case 3: return 16;
    case 4: return 17;
    case 5: case 6: return 5;
    case 7: case 9: return 8;
    default: return 0;
  }
}
inline bool read_identity(uint8_t selector, Frame &out) {
  out = Frame{};
  if (!identity_text_size(selector)) return false;
  out.size = 4; out.bytes[0] = 0x11; out.bytes[1] = selector;
  return true;
}
struct IdentityText {
  uint8_t selector{0};
  char text[18]{};
  uint16_t sequence{0};
};
inline bool decode_identity(const Frame &f, IdentityText &out) {
  if (f.size < 2 || f.bytes[0] != 0x12) return false;
  const auto count = identity_text_size(f.bytes[1]);
  if (!count || f.size != count + 4) return false;
  IdentityText value;
  value.selector = f.bytes[1];
  bool padding = false;
  for (std::size_t i = 0; i < count; ++i) {
    const uint8_t c = f.bytes[i + 2];
    if (c == 0) { padding = true; continue; }
    if (padding || c < 0x20 || c > 0x7E) return false;
    value.text[i] = static_cast<char>(c);
  }
  if (!value.text[0]) return false;
  value.sequence = be16(f.bytes + count + 2);
  out = value;
  return true;
}

}  // namespace desk_protocol
