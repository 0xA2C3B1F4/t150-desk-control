#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome { namespace desk_uart {

enum class JournalLoad { Missing, Found, Invalid, Error };

// Read persisted bytes, bypassing ESPHome's pending-write/RAM preference cache.
JournalLoad read_journal_storage(uint32_t key, uint8_t *out, size_t length);
// Commit through ESPHome and compare a fresh persisted read with the requested bytes.
bool write_journal_storage(uint32_t key, const uint8_t *data, size_t length);

}}  // namespace esphome::desk_uart
