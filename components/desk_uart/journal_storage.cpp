#include "journal_storage.h"
#include "esphome/core/preferences.h"

#include <array>
#include <cstring>

#ifdef USE_ESP32
#include <cinttypes>
#include <cstdio>
#include <nvs.h>
#elif defined(USE_HOST)
#include "esphome/core/application.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#endif

namespace esphome { namespace desk_uart {

JournalLoad read_journal_storage(uint32_t key, uint8_t *out, size_t length) {
  // ESPHome's host preference format stores the size in one byte.
  if (out == nullptr || length == 0 || length > 255) return JournalLoad::Error;
#ifdef USE_ESP32
  if (global_preferences == nullptr || global_preferences->nvs_handle == 0) return JournalLoad::Error;
  char key_text[12];
  std::snprintf(key_text, sizeof(key_text), "%" PRIu32, key);
  size_t stored_length = 0;
  const auto handle = global_preferences->nvs_handle;
  esp_err_t result = nvs_get_blob(handle, key_text, nullptr, &stored_length);
  if (result == ESP_ERR_NVS_NOT_FOUND) return JournalLoad::Missing;
  if (result == ESP_ERR_NVS_TYPE_MISMATCH) return JournalLoad::Invalid;
  if (result != ESP_OK) return JournalLoad::Error;
  if (stored_length != length) return JournalLoad::Invalid;
  result = nvs_get_blob(handle, key_text, out, &stored_length);
  if (result == ESP_ERR_NVS_TYPE_MISMATCH || result == ESP_ERR_NVS_INVALID_LENGTH)
    return JournalLoad::Invalid;
  if (result != ESP_OK) return JournalLoad::Error;
  return stored_length == length ? JournalLoad::Found : JournalLoad::Invalid;
#elif defined(USE_HOST)
  // Host integration builds redirect ESPHome's preference root to this variable.
  const char *home = std::getenv("DESK_TEST_PREFS_HOME");
  if (home == nullptr) home = std::getenv("HOME");
  if (home == nullptr) return JournalLoad::Error;
  std::string path(home);
  path.append("/.esphome/prefs/");
  path.append(App.get_name());
  path.append(".prefs");
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return errno == ENOENT ? JournalLoad::Missing : JournalLoad::Error;

  JournalLoad state = JournalLoad::Missing;
  std::set<uint32_t> keys;
  std::array<uint8_t, 255> candidate{};
  // Validate the entire file before returning even a missing-key result. The
  // host backend otherwise silently treats a truncated tail as absent records.
  while (true) {
    uint32_t stored_key;
    const size_t count = std::fread(&stored_key, 1, sizeof(stored_key), file);
    if (count == 0 && std::feof(file) && !std::ferror(file)) break;
    if (count != sizeof(stored_key)) {
      state = std::ferror(file) ? JournalLoad::Error : JournalLoad::Invalid;
      break;
    }
    uint8_t stored_length;
    if (std::fread(&stored_length, 1, 1, file) != 1) {
      state = std::ferror(file) ? JournalLoad::Error : JournalLoad::Invalid;
      break;
    }
    std::array<uint8_t, 255> bytes{};
    if (std::fread(bytes.data(), 1, stored_length, file) != stored_length) {
      state = std::ferror(file) ? JournalLoad::Error : JournalLoad::Invalid;
      break;
    }
    if (!keys.insert(stored_key).second) {
      state = JournalLoad::Invalid;
      break;
    }
    if (stored_key == key) {
      if (stored_length != length) {
        state = JournalLoad::Invalid;
        break;
      }
      std::memcpy(candidate.data(), bytes.data(), length);
      state = JournalLoad::Found;
    }
  }
  if (std::fclose(file) != 0) return JournalLoad::Error;
  if (state == JournalLoad::Found) std::memcpy(out, candidate.data(), length);
  return state;
#else
  (void) key;
  return JournalLoad::Error;
#endif
}

bool write_journal_storage(uint32_t key, const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0 || length > 255 || global_preferences == nullptr) return false;
  std::array<uint8_t, 255> persisted{};
  const JournalLoad before = read_journal_storage(key, persisted.data(), length);
  if (before != JournalLoad::Missing && before != JournalLoad::Found) return false;
#ifdef USE_ESP32
  esp32::ESP32PreferenceBackend backend;
  backend.key = key;
  backend.nvs_handle = global_preferences->nvs_handle;
  if (!backend.save(data, length)) return false;
#elif defined(USE_HOST)
  if (!global_preferences->save(key, data, length)) return false;
#else
  return false;
#endif
  if (!global_preferences->sync()) return false;
  return read_journal_storage(key, persisted.data(), length) == JournalLoad::Found &&
      std::memcmp(persisted.data(), data, length) == 0;
}

}}  // namespace esphome::desk_uart
