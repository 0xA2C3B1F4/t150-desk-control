#include <cstdlib>
#include <iostream>
#include "desk_protocol/display_settings.h"

using namespace desk_protocol;
using Codec = DisplaySettingsCodec;
using Client = DisplaySettings;

#define CHECK(x)                             \
  do {                                       \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      std::exit(1);                          \
    }                                        \
  } while (false)

static Frame response(const Frame& request, uint8_t result = 0, uint8_t flags = 5,
                      uint16_t seconds = 60) {
  Frame frame{};
  frame.size = 10;
  frame.bytes[0] = 0xD8;
  frame.bytes[1] = 4;
  frame.bytes[2] = request.bytes[2];
  frame.bytes[3] = request.bytes[3];
  frame.bytes[4] = request.bytes[4];
  frame.bytes[5] = result;
  frame.bytes[6] = 3;
  frame.bytes[7] = flags;
  frame.bytes[8] = static_cast<uint8_t>(seconds >> 8);
  frame.bytes[9] = static_cast<uint8_t>(seconds);
  return frame;
}

static void test_codec() {
  Frame frame = Codec::get(0xFAFD);
  CHECK(frame.size == 12 && frame.bytes[3] == 0xFA && frame.bytes[4] == 0xFD);
  for (unsigned i = 5; i < 12; ++i) {
    CHECK(frame.bytes[i] == 0);
  }
  Codec::Settings settings{};
  settings.mode = Codec::Normal;
  settings.timeout_seconds = 65535;
  CHECK(Codec::set(0xFAFD, settings, frame));
  CHECK(frame.bytes[2] == 1 && frame.bytes[6] == 1 && frame.bytes[7] == 255 &&
        frame.bytes[8] == 255);
  Packet packet{};
  CHECK(encode_resync(frame, packet));
  Parser parser;
  Frame decoded{};
  unsigned count = 0;
  for (unsigned i = 0; i < packet.size; ++i) {
    count += parser.feed(packet.bytes[i], i, decoded) == ParseResult::FrameReady;
  }
  CHECK(count == 1 && decoded.size == 12 && decoded.bytes[8] == 255);
  const Frame good = response(frame, 0, 0, 65535);
  for (size_t size = 0; size <= sizeof(good.bytes); ++size) {
    Frame candidate = good;
    candidate.size = size;
    Codec::Response result{};
    CHECK(Codec::decode(candidate, result) == (size == 10));
  }
  for (unsigned flags = 0; flags < 256; ++flags) {
    for (unsigned result = 0; result < 3; ++result) {
      Codec::Response decoded_response{};
      CHECK(Codec::decode(response(frame, result, flags, result ? 0 : 60), decoded_response) ==
            (result ? flags == 16 : flags < 16));
    }
  }
  for (unsigned field : {0u, 1u, 2u, 5u, 6u}) {
    Frame candidate = good;
    candidate.bytes[field] = 0xFF;
    Codec::Response result{};
    CHECK(Codec::decode(candidate, result) == (field == 6));
  }
}

static void test_request_lifecycle() {
  Client client;
  client.seed(0xFAFD);
  Codec::Settings desired{};
  desired.timeout_seconds = 1;
  CHECK(client.edit(desired) == Client::RequestStatus::Unavailable);
  CHECK(client.probe() == Client::RequestStatus::Started);
  Frame frame{};
  CHECK(client.pending_frame(frame));
  CHECK(client.edit(desired) == Client::RequestStatus::Busy);
  CHECK(!client.accept(response(frame), 0));
  client.tick(9000);
  CHECK(client.busy());
  CHECK(client.mark_transmitted(9000));
  Frame wrong = response(frame);
  wrong.bytes[4] ^= 1;
  CHECK(!client.accept(wrong, 9001));
  wrong = response(frame);
  wrong.bytes[1] = 3;
  CHECK(!client.accept(wrong, 9001));
  wrong = response(frame);
  wrong.bytes[2] = 1;
  CHECK(!client.accept(wrong, 9001));
  CHECK(client.accept(response(frame), 9001));
  CHECK(client.supported() && client.state_known() && !client.busy());
  CHECK(client.edit(desired) == Client::RequestStatus::Started);
  CHECK(client.pending_frame(frame));
  CHECK(client.mark_transmitted(10000));
  CHECK(!client.state_known());
  CHECK(client.accept(response(frame, 0, 5, 1), 10001));
  CHECK(client.state_known() && client.confirmed().settings.timeout_seconds == 1);
  CHECK(client.outcome() == Client::Outcome::Applied);
  CHECK(!client.accept(response(frame, 0, 5, 1), 10002));
  CHECK(client.edit(desired) == Client::RequestStatus::Started);
  CHECK(client.pending_frame(frame));
  CHECK(client.mark_transmitted(11000));
  client.tick(13000);
  CHECK(!client.busy() && !client.state_known() && client.outcome() == Client::Outcome::Timeout);
  CHECK(!client.pending_frame(frame));
  CHECK(!client.accept(response(frame, 0, 5, 1), 13001));
  client.disconnect();
  CHECK(!client.supported());
  CHECK(client.edit(desired) == Client::RequestStatus::Unavailable);
}

static void test_rejection_and_cancellation() {
  Client client;
  Frame frame{};
  client.probe();
  client.pending_frame(frame);
  client.mark_transmitted(UINT32_MAX - 50);
  CHECK(client.accept(response(frame), 20));
  Codec::Settings desired{};
  desired.mode = Codec::Normal;
  client.edit(desired);
  client.cancel();
  CHECK(client.state_known() && !client.busy());
  client.edit(desired);
  client.pending_frame(frame);
  client.mark_transmitted(100);
  CHECK(client.accept(response(frame, 0, 5, 60), 101));
  CHECK(client.outcome() == Client::Outcome::InvalidReply && !client.state_known());
  client.probe();
  client.pending_frame(frame);
  client.mark_transmitted(200);
  CHECK(client.accept(response(frame, 2, 16, 0), 201));
  CHECK(!client.state_known() && client.outcome() == Client::Outcome::Rejected);
  CHECK(client.confirmed().settings.timeout_seconds == 60);
}

int main() {
  test_codec();
  test_request_lifecycle();
  test_rejection_and_cancellation();
}
