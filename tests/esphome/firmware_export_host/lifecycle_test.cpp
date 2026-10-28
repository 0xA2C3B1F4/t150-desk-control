// Execute the unchanged ESP-IDF adapter with deterministic host platform edges.
#include "platform.h"
#include "desk_firmware_export.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using Adapter = esphome::desk_firmware_export::FirmwareExport;
using Receiver = desk_protocol::FirmwareExport;

static std::string quote(const std::string &s) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') out << '\\' << char(c);
    else if (c == '\n') out << "\\n";
    else if (c == '\r') out << "\\r";
    else if (c == '\t') out << "\\t";
    else { CHECK(c >= 32); out << char(c); }
  }
  out << '"';
  return out.str();
}

static std::vector<uint8_t> image_a() {
  std::vector<uint8_t> out(55296);
  for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(i * 37 + 11);
  return out;
}
static std::vector<uint8_t> image_b() {
  std::vector<uint8_t> out(1025);
  for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(i * 19 + 7);
  return out;
}

class Probe : public Adapter {
 public:
  // A lifecycle fixture, deliberately NOT a successful recognizer result.
  // Synthetic bytes first travel through the real receiver/hash/recognizer.
  // Only the old image's metadata is then seeded to exercise scalar retention
  // and JSON formatting without distributing a manufacturer's firmware image.
  void seed_known_metadata() {
    std::lock_guard<std::mutex> lock(mutex_);
    CHECK(snapshot_.ready && buffer_);
    CHECK(snapshot_.profile.status == desk_protocol::firmware_profile::Status::Unrecognized);
    snapshot_.profile.status = desk_protocol::firmware_profile::Status::Known;
    snapshot_.profile.scalars = desk_protocol::HeightProfile::Scalars(4, 460, 725, -1182, 1188);
  }
};

struct Rig {
  esphome::desk_uart::DeskUART desk;
  Probe reader;
  std::string name;
  explicit Rig(std::string case_name) : name(std::move(case_name)) {
    export_host::reset();
    reader.set_desk(&desk);
    reader.setup();
    CHECK(!reader.is_failed());
  }
  void loop() { reader.loop(); ++export_host::now; }
  void feed(const std::vector<uint8_t> &bytes) {
    CHECK(export_host::rx.empty());
    export_host::rx.insert(export_host::rx.end(), bytes.begin(), bytes.end());
    loop();
    CHECK(export_host::rx.empty());
  }
  static std::vector<uint8_t> packet(uint8_t index, const std::vector<uint8_t> &data) {
    CHECK(data.size() == 128 || data.size() == 1024);
    std::vector<uint8_t> out{uint8_t(data.size() == 128 ? 1 : 2), index, uint8_t(index ^ 255)};
    out.insert(out.end(), data.begin(), data.end());
    // Independent bitwise CRC oracle for the synthetic peer.
    uint16_t crc = 0;
    for (uint8_t byte : data) {
      crc ^= uint16_t(byte) << 8;
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = uint16_t((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    out.push_back(uint8_t(crc >> 8)); out.push_back(uint8_t(crc));
    return out;
  }
  void start() {
    CHECK(reader.start_read()); loop();
    CHECK(reader.status_text() == "Waiting for desk power cycle");
  }
  void header(size_t size) {
    feed({0xFA, 0x30, 0, 1, 0x31, 0xFD});
    const std::string prompt = "Select Receive File";
    feed(std::vector<uint8_t>(prompt.begin(), prompt.end()));
    std::vector<uint8_t> data(128, 0);
    const std::string filename = "UploadedFlashImage.bin";
    std::copy(filename.begin(), filename.end(), data.begin());
    const auto length = std::to_string(size);
    std::copy(length.begin(), length.end(), data.begin() + filename.size() + 1);
    feed(packet(0, data));
  }
  void release() {
    const auto before = desk.end_count;
    export_host::now += 3000; loop();
    CHECK(desk.end_count == before + 1);
  }
  void complete(const std::vector<uint8_t> &image) {
    start(); header(image.size());
    for (size_t offset = 0; offset < image.size(); offset += 1024) {
      std::vector<uint8_t> block(1024, 0x1A);
      const size_t count = std::min(block.size(), image.size() - offset);
      std::copy_n(image.begin() + offset, count, block.begin());
      feed(packet(uint8_t(offset / 1024 + 1), block));
    }
    feed({4}); feed(packet(0, std::vector<uint8_t>(128, 0))); feed({4});
    CHECK(reader.status_text() == "Complete" && reader.image_ready());
    release();
  }
  void baseline() {
    complete(image_a());
    CHECK(reader.profile_status_text() == "Unrecognized firmware");
    snapshot("complete", "unrecognized");
    download("complete", "A");
    reader.seed_known_metadata();
    snapshot("baseline", "known");
  }
  void snapshot(const std::string &checkpoint, const std::string &fixture) {
    AsyncWebServerRequest request("/desk-firmware/status");
    CHECK(reader.canHandle(&request)); reader.handleRequest(&request);
    CHECK(request.response_code == 200 && request.content_type == "application/json");
    CHECK(request.headers.at("Cache-Control") == "no-store");
    std::cout << "{\"kind\":\"snapshot\",\"case\":" << quote(name)
      << ",\"checkpoint\":" << quote(checkpoint) << ",\"profile_fixture\":" << quote(fixture)
      << ",\"http\":" << std::string(request.body.begin(), request.body.end())
      << ",\"entities\":{\"status\":" << quote(reader.status_text())
      << ",\"error\":" << quote(reader.error_text()) << ",\"sha256\":" << quote(reader.sha256_text())
      << ",\"profile\":" << quote(reader.profile_status_text()) << ",\"values\":" << quote(reader.profile_values_text())
      << ",\"ready\":" << (reader.image_ready() ? "true" : "false")
      << ",\"progress\":" << reader.progress() << "}}\n";
  }
  void record_download(AsyncWebServerRequest &request, const std::string &checkpoint,
                       const std::string &expected) {
    const std::string filename = name + "-" + checkpoint + ".bin";
    if (expected != "none") {
      CHECK(request.response_code == 200 && request.pointer_response);
      std::ofstream file(filename, std::ios::binary);
      file.write(reinterpret_cast<const char *>(request.body.data()), std::streamsize(request.body.size()));
      CHECK(file.good());
    } else CHECK(request.response_code == 409);
    std::cout << "{\"kind\":\"download\",\"case\":" << quote(name)
      << ",\"checkpoint\":" << quote(checkpoint) << ",\"expected_image\":" << quote(expected)
      << ",\"status\":" << request.response_code << ",\"file\":" << quote(filename)
      << ",\"content_type\":" << quote(request.content_type) << ",\"headers\":{";
    bool first = true;
    for (const auto &h : request.headers) {
      if (!first) std::cout << ',';
      first = false; std::cout << quote(h.first) << ':' << quote(h.second);
    }
    std::cout << "}}\n";
  }
  void download(const std::string &checkpoint, const std::string &expected) {
    AsyncWebServerRequest request("/desk-firmware/download");
    CHECK(reader.canHandle(&request)); reader.handleRequest(&request);
    record_download(request, checkpoint, expected);
  }
  void clear() {
    CHECK(reader.clear_image()); loop();
    CHECK(!reader.image_ready() && reader.sha256_text().empty());
    CHECK(reader.profile_status_text() == "No firmware image" && reader.profile_values_text() == "Unavailable");
    snapshot("cleared", "empty"); download("cleared", "none");
  }
};

static void failed_transfer(const std::string &kind) {
  Rig r(kind); r.baseline();
  const auto old_hash = r.reader.sha256_text();
  r.start(); r.snapshot("armed", "known"); r.download("armed", "A");
  CHECK(!r.reader.clear_image());  // Busy readers cannot discard a retained image.
  if (kind == "allocation_failure") {
    export_host::fail_malloc = true;
    r.header(2049);
    export_host::fail_malloc = false;
  } else if (kind == "boot_timeout") {
    export_host::now += 120000; r.loop();
  } else {
    r.header(2049);
    r.feed(Rig::packet(1, std::vector<uint8_t>(1024, 0xA5)));
    CHECK(export_host::live_allocations() == 2);
    r.snapshot("partial", "known"); r.download("partial", "A");
    if (kind == "crc_failure") {
      auto corrupt = Rig::packet(2, std::vector<uint8_t>(1024, 0x5A));
      corrupt.back() ^= 1;
      for (unsigned i = 0; i < 4; ++i) r.feed(corrupt);
    } else if (kind == "partial_timeout") {
      export_host::now += 5000; r.loop();
    } else {
      CHECK(kind == "partial_cancel");
      CHECK(r.reader.cancel_read()); r.loop();
    }
  }
  CHECK(r.reader.status_text() == (kind == "partial_cancel" ? "Cancelled" : "Failed"));
  CHECK(r.reader.image_ready() && r.reader.sha256_text() == old_hash);
  CHECK(r.reader.profile_status_text() == "Known T150 profile");
  CHECK(export_host::live_allocations() == 1 && export_host::live_bytes() == image_a().size());
  r.snapshot("finished", "known"); r.download("finished", "A");
  r.release();
  r.snapshot("released", "known");
  r.clear();
  CHECK(export_host::live_allocations() == 0);
}

static void download_clear() {
  Rig r("download_clear"); r.baseline();
  AsyncWebServerRequest pending("/desk-firmware/download");
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, resume = false;
  pending.before_copy = [&] {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(pending.pointer_response && pending.body.empty());
    entered = true; cv.notify_all();
    CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return resume; }));
  };
  std::thread sender([&] { r.reader.handleRequest(&pending); });
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
  }
  // No owning copy is held by the fixture or HTTP response. The only remaining
  // owner after Clear must be the local shared_ptr in production handleRequest.
  const void *old_data = pending.response_data;
  const auto frees = export_host::free_calls();
  CHECK(export_host::is_live(old_data));
  r.clear();
  CHECK(export_host::is_live(old_data));
  CHECK(export_host::free_calls() == frees && export_host::live_allocations() == 1);
  {
    std::lock_guard<std::mutex> lock(mutex); resume = true;
  }
  cv.notify_all(); sender.join();
  CHECK(pending.body == image_a());
  r.record_download(pending, "in_flight", "A");
  CHECK(!export_host::is_live(old_data));
  CHECK(export_host::free_calls() == frees + 1 && export_host::live_allocations() == 0);
  // A successful transfer also works after Clear.
  r.complete(image_b());
  CHECK(r.reader.profile_status_text() == "Unsupported image size");
  r.snapshot("replacement", "unsupported"); r.download("replacement", "B");
  r.clear();
  CHECK(export_host::live_allocations() == 0);
}

static void successful_replacement() {
  Rig r("successful_replacement"); r.baseline();
  r.complete(image_b());
  CHECK(r.reader.profile_status_text() == "Unsupported image size");
  CHECK(export_host::live_allocations() == 1 && export_host::live_bytes() == image_b().size());
  r.snapshot("replacement", "unsupported"); r.download("replacement", "B");
  r.clear();
  CHECK(export_host::live_allocations() == 0);
}

int main(int argc, char **argv) {
  const std::string selected = argc == 2 ? argv[1] : "all";
  const std::vector<std::string> cases{"crc_failure", "partial_timeout", "allocation_failure", "boot_timeout", "partial_cancel", "download_clear", "successful_replacement"};
  unsigned count = 0;
  for (const auto &name : cases) {
    if (selected != "all" && selected != name) continue;
    if (name == "download_clear") download_clear();
    else if (name == "successful_replacement") successful_replacement();
    else failed_transfer(name);
    ++count;
  }
  CHECK(count > 0);
  std::cout << "{\"kind\":\"result\",\"passed\":true,\"cases\":" << count << "}\n";
}
