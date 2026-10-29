#pragma once

#include "esphome/components/desk_uart/journal_storage.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/resource.h>
#include <vector>

#ifndef USE_HOST
#error "Journal storage regression requires the ESPHome host backend"
#endif

namespace esphome { namespace desk_uart { namespace journal_storage_regression {
namespace fs = std::filesystem;

inline void require(bool value, const char *message) {
  if (!value) {
    ESP_LOGE("fixture", "Journal storage assertion failed: %s", message);
    std::abort();
  }
}
inline void passed(const char *name) { ESP_LOGI("fixture", "JOURNAL_STORAGE_CASE=%s", name); }

inline void append_record(std::vector<uint8_t> &file, uint32_t key,
                          const uint8_t *data, uint8_t length) {
  // ESPHome host uses the machine's native uint32 key, then one byte of length.
  const size_t offset = file.size();
  file.resize(offset + sizeof(key));
  std::memcpy(file.data() + offset, &key, sizeof(key));
  file.push_back(length);
  file.insert(file.end(), data, data + length);
}
inline void write_file(const fs::path &path, const std::vector<uint8_t> &bytes) {
  FILE *file = std::fopen(path.c_str(), "wb");
  require(file != nullptr, "open owned fixture file");
  const bool wrote = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  const bool closed = std::fclose(file) == 0;
  require(wrote && closed, "write complete fixture file");
}
inline void remove_file(const fs::path &path) {
  require(!fs::is_symlink(path), "fixture must not be a symlink");
  if (fs::exists(path)) {
    require(fs::is_regular_file(path) || (fs::is_directory(path) && fs::is_empty(path)),
            "remove only the owned file or empty fixture directory");
    require(fs::remove(path), "remove owned fixture");
  }
}

inline void run() {
  const char *test_home = std::getenv("DESK_TEST_PREFS_HOME");
  const char *temporary = std::getenv("TMPDIR");
  require(test_home != nullptr && temporary != nullptr, "explicit scratch environment required");
  const auto root = fs::weakly_canonical(test_home);
  const auto tmp = fs::canonical(temporary);
  const auto relative = root.lexically_relative(tmp);
  require(root != tmp && !relative.empty() && !relative.is_absolute() && *relative.begin() != "..",
          "test preferences must be beneath TMPDIR");
  const auto path = root / ".esphome" / "prefs" / (std::string(App.get_name().c_str()) + ".prefs");
  fs::create_directories(path.parent_path());
  remove_file(path);
  require(global_preferences != nullptr && global_preferences->reset(), "reset this process's host map");

  constexpr uint32_t key = 0x4A4E4C34U, other = 0x4A4E4C35U;
  std::array<uint8_t, 64> first{}, second{}, read{};
  for (size_t i = 0; i < first.size(); ++i) {
    first[i] = static_cast<uint8_t>(i + 1);
    second[i] = static_cast<uint8_t>(255 - i);
  }
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Missing,
          "absent file is Missing");
  write_file(path, {});
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Missing,
          "empty file is Missing");
  std::vector<uint8_t> bytes;
  append_record(bytes, other, first.data(), first.size());
  write_file(path, bytes);
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Missing,
          "valid file without the requested key is Missing");
  passed("missing");

  bytes.clear();
  append_record(bytes, key, first.data(), first.size() - 1);
  write_file(path, bytes);
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Invalid,
          "present wrong length is Invalid, not Missing");
  require(!write_journal_storage(key, second.data(), second.size()), "wrong length is not overwritten");
  require(fs::file_size(path) == bytes.size(), "invalid file is preserved");
  passed("wrong_size");

  bytes.clear();
  append_record(bytes, key, first.data(), first.size());
  append_record(bytes, key, second.data(), second.size());
  write_file(path, bytes);
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Invalid,
          "duplicate requested key is Invalid");
  bytes.clear();
  append_record(bytes, key, first.data(), first.size());
  append_record(bytes, other, first.data(), first.size());
  append_record(bytes, other, second.data(), second.size());
  write_file(path, bytes);
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Invalid,
          "duplicate unrelated key invalidates the complete file");
  require(read_journal_storage(key + 2, read.data(), read.size()) == JournalLoad::Invalid,
          "missing key cannot hide duplicate records");
  passed("duplicate_keys");

  std::vector<uint8_t> complete;
  append_record(complete, key, first.data(), first.size());
  for (size_t tail : {size_t(1), size_t(3), size_t(4), size_t(5), size_t(20)}) {
    bytes = complete;
    std::vector<uint8_t> unfinished;
    append_record(unfinished, other, second.data(), second.size());
    bytes.insert(bytes.end(), unfinished.begin(), unfinished.begin() + tail);
    write_file(path, bytes);
    require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Invalid,
            "found key cannot hide a truncated tail");
    require(read_journal_storage(key + 2, read.data(), read.size()) == JournalLoad::Invalid,
            "missing key cannot hide a truncated tail");
  }
  passed("truncated_records");

  remove_file(path);
  require(fs::create_directory(path), "create unreadable fixture directory");
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Error,
          "reading a directory is an I/O error, not Missing");
  require(!write_journal_storage(key, first.data(), first.size()), "I/O error prevents writes");
  remove_file(path);
  passed("unreadable_file");

  require(global_preferences->reset(), "clear host RAM before cache test");
  require(global_preferences->save(key, first.data(), first.size()), "save only to host RAM map");
  require(global_preferences->load(key, read.data(), read.size()) && read == first,
          "ordinary preference load sees pending RAM bytes");
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Missing,
          "pending RAM bytes cannot impersonate persisted data");
  require(write_journal_storage(key, second.data(), second.size()), "commit and verify round trip");
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Found && read == second,
          "fresh persisted bytes match committed data");
  require(global_preferences->save(key, first.data(), first.size()), "replace only the RAM map again");
  require(global_preferences->load(key, read.data(), read.size()) && read == first,
          "RAM cache contains different bytes");
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Found && read == second,
          "persisted read bypasses a newer RAM value");
  passed("persisted_not_cached");

  // Verify the host backend reports an actual fopen failure after its map has
  // been initialized. A cached value still must not be accepted as persisted.
  remove_file(path);
  require(fs::create_directory(path), "block sync with a directory at the file path");
  require(!global_preferences->sync(), "host sync reports fopen failure");
  require(global_preferences->load(key, read.data(), read.size()) && read == first,
          "failed sync leaves the cached value readable");
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Error,
          "fresh persisted read rejects cache after failed sync");
  remove_file(path);
  passed("sync_open_failure");

  // Host sync ignores fwrite/fclose failures. Limit this test process's writes
  // to one byte so its sync returns true but persisted verification must fail.
  // Restore the limit and signal handler before any assertion or log output.
  require(global_preferences->reset(), "clear host RAM before short-write test");
  struct rlimit original{};
  require(getrlimit(RLIMIT_FSIZE, &original) == 0, "read process file-size limit");
  require(original.rlim_max == RLIM_INFINITY || original.rlim_max >= 1, "one-byte file limit supported");
  const auto previous_signal = std::signal(SIGXFSZ, SIG_IGN);
  require(previous_signal != SIG_ERR, "ignore file-limit signal in this process");
  auto limited = original;
  limited.rlim_cur = 1;
  const bool limit_set = setrlimit(RLIMIT_FSIZE, &limited) == 0;
  bool accepted = false;
  if (limit_set) accepted = write_journal_storage(key, second.data(), second.size());
  const bool limit_restored = setrlimit(RLIMIT_FSIZE, &original) == 0;
  const bool signal_restored = std::signal(SIGXFSZ, previous_signal) != SIG_ERR;
  require(limit_set && limit_restored && signal_restored, "restore process limits and signal handler");
  require(!accepted, "post-sync disk verification rejects a short write despite successful sync");
  require(fs::file_size(path) == 1, "actual host file was truncated by the injected write failure");
  require(global_preferences->load(key, read.data(), read.size()) && read == second,
          "cached read alone would falsely report the short write as successful");
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Invalid,
          "short persisted file is Invalid");
  passed("sync_short_write");

  remove_file(path);
  require(global_preferences->reset(), "clear fault fixture RAM");
  require(write_journal_storage(key, first.data(), first.size()), "normal round trip after fault fixtures");
  require(read_journal_storage(key, read.data(), read.size()) == JournalLoad::Found && read == first,
          "normal persisted bytes after fault fixtures");
  require(global_preferences->reset(), "clear owned host cache after tests");
  remove_file(path);
  passed("round_trip");
  ESP_LOGI("fixture", "JOURNAL_STORAGE_ALL=passed");
}

}}}  // namespace esphome::desk_uart::journal_storage_regression
