#pragma once
#include "esphome/components/desk_firmware_export/desk_firmware_export.h"
#include "esphome/core/preferences.h"
#include "desk_protocol/firmware_install.h"

namespace esphome::desk_firmware_update {
class FirmwareUpdate : public desk_firmware_export::FirmwareExport {
 public:
  void set_research_url(const std::string &url) { research_url_=url; }
  float get_setup_priority() const override { return setup_priority::DATA + 1.0f; }
  void setup() override;
  void loop() override;
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  std::string maintenance_status();
 private:
  bool start_read() = delete;
  bool cancel_read() = delete;
  bool clear_image() = delete;
 protected:
  enum class Phase { Idle, Reading, Ready, Uploading, Staged, Writing, AwaitVerify, Verifying, Verified, Error };
  static uint8_t *allocate_image_(void *context,size_t size);
  void execute_(const std::string &action);
  void read_(bool verify);
  void digest_();
  void block_hashes_(uint8_t output[54][32]);
  bool recovery_source_matches_();
  bool validate_partial_recovery_() const;
  void fail_(const char *reason);
  bool active_() const;
  bool admit_() const;
  bool save_recovery_(bool value);
  const char *phase_name_() const;
  desk_protocol::FirmwareInstall sender_;
  struct RecoveryRecord {
    uint32_t magic{0}, phase{0}, attempt{0};
    char source[65]{}, target[65]{}, write_target[65]{};
    uint8_t source_blocks[54][32]{}, target_blocks[54][32]{};
  } recovery_record_;
  uint8_t preflight_blocks_[54][32]{};
  int partial_block_{-1};
  uint8_t partial_image_[1024]{};
  bool recovery_source_valid_{false}, attempt_had_recovery_{false};
  ESPPreferenceObject recovery_preference_;
  Phase phase_{Phase::Idle};
  std::string owner_, csrf_, pending_, research_url_;
  uint32_t revision_{0}, preflight_at_{0};
  size_t size_{0}, upload_offset_{0};
  char hash_[65]{}, source_hash_[65]{};
  const char *error_{nullptr};
  bool preflight_{false}, tail_erased_{false}, recovery_{false}, backup_{false};
};
}  // namespace esphome::desk_firmware_update
