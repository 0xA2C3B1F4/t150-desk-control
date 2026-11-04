#include <cstdlib>
#include <iostream>
#include <limits>
#include "desk_protocol/motion_trim.h"

using namespace desk_protocol;
using Codec = MotionTrimCodec;
using Client = MotionTrim;
using RequestStatus = Client::RequestStatus;
using Outcome = Client::Outcome;

#define CHECK(x)                             \
  do {                                       \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      std::exit(1);                          \
    }                                        \
  } while (false)

static bool same_frame(const Frame& a, const Frame& b) {
  if (a.size != b.size) {
    return false;
  }
  for (size_t i = 0; i < a.size; ++i) {
    if (a.bytes[i] != b.bytes[i]) {
      return false;
    }
  }
  return true;
}

static Frame response() {
  Frame frame{};
  const uint8_t bytes[] = {0xDA, 1, 3, 1, 0, 1, 1, 0, 0xFA, 0xFE, 0x12, 0x34, 0xFD, 0, 2, 3};
  frame.size = sizeof(bytes);
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    frame.bytes[i] = bytes[i];
  }
  return frame;
}

static void check_transport(const Frame& frame) {
  Packet packet{};
  CHECK(encode_resync(frame, packet));
  Parser parser;
  Frame decoded{};
  size_t frames = 0;
  for (size_t i = 0; i < packet.size; ++i) {
    if (parser.feed(packet.bytes[i], static_cast<uint32_t>(i), decoded) ==
        ParseResult::FrameReady) {
      ++frames;
    }
  }
  CHECK(frames == 1 && same_frame(frame, decoded));
}

static bool same_response(const Codec::Response& a, const Codec::Response& b) {
  if (a.operation != b.operation || a.result != b.result || a.storage != b.storage ||
      a.speed != b.speed || a.flags != b.flags || a.nonce != b.nonce || a.sequence != b.sequence) {
    return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (a.active.trim[i] != b.active.trim[i]) {
      return false;
    }
  }
  return true;
}

static void check_decoded_fields(const Frame& frame, const Codec::Response& decoded) {
  CHECK(decoded.operation == frame.bytes[3] && decoded.result == frame.bytes[4]);
  CHECK(decoded.storage == frame.bytes[5] && decoded.speed == frame.bytes[6]);
  CHECK(decoded.flags == frame.bytes[7]);
  CHECK(decoded.nonce == frame.bytes[8] * 256 + frame.bytes[9]);
  CHECK(decoded.sequence == frame.bytes[10] * 256 + frame.bytes[11]);
  for (size_t i = 0; i < 4; ++i) {
    const int expected = frame.bytes[12 + i] > 3 ? frame.bytes[12 + i] - 256 : frame.bytes[12 + i];
    CHECK(decoded.active.trim[i] == expected);
  }
}

static void check_requests() {
  const Frame get = Codec::get(0xFAFE);
  CHECK(get.size == 17 && get.bytes[0] == 0xD9 && get.bytes[1] == 1 && get.bytes[2] == 0);
  CHECK(get.bytes[3] == 0xFA && get.bytes[4] == 0xFE);
  for (size_t i = 5; i < get.size; ++i) {
    CHECK(get.bytes[i] == 0);
  }
  check_transport(get);
  for (uint32_t nonce = 0; nonce <= 0xFFFF; ++nonce) {
    const Frame frame = Codec::get(static_cast<uint16_t>(nonce));
    CHECK(frame.bytes[3] == nonce / 256 && frame.bytes[4] == nonce % 256);
  }

  Codec::Values expected{};
  Codec::Values desired{};
  expected.trim[0] = -3;
  expected.trim[1] = -2;
  expected.trim[2] = -1;
  expected.trim[3] = 0;
  desired.trim[0] = 3;
  desired.trim[1] = 2;
  desired.trim[2] = 1;
  desired.trim[3] = -3;
  Frame set{};
  CHECK(Codec::set(0xFAFE, 0x12345678, expected, desired, set));
  const uint8_t golden[] = {0xD9, 1,    1,    0xFA, 0xFE, 0x12, 0x34, 0x56, 0x78,
                            0xFD, 0xFE, 0xFF, 0,    3,    2,    1,    0xFD};
  CHECK(set.size == sizeof(golden));
  for (size_t i = 0; i < sizeof(golden); ++i) {
    CHECK(set.bytes[i] == golden[i]);
  }
  check_transport(set);
  const Frame sentinel = set;
  CHECK(!Codec::set(0, 0, expected, desired, set));
  CHECK(same_frame(set, sentinel));
  for (int slot = 0; slot < 4; ++slot) {
    for (int value = -128; value <= 127; ++value) {
      Codec::Values candidate{};
      candidate.trim[slot] = static_cast<int8_t>(value);
      const bool valid = value >= -3 && value <= 3;
      set = sentinel;
      CHECK(Codec::set(0, 1, candidate, desired, set) == valid);
      CHECK(valid || same_frame(set, sentinel));
      set = sentinel;
      CHECK(Codec::set(0, 1, expected, candidate, set) == valid);
      CHECK(valid || same_frame(set, sentinel));
    }
  }
}

static void check_responses() {
  Codec::Response decoded{};
  Codec::Response sentinel{};
  sentinel.operation = Codec::Get;
  sentinel.result = Codec::StorageError;
  sentinel.storage = Codec::Uncertain;
  sentinel.speed = 0;
  sentinel.nonce = 0xAABB;
  sentinel.sequence = 0xCCDD;
  for (size_t i = 0; i < 4; ++i) {
    sentinel.active.trim[i] = static_cast<int8_t>(i) - 2;
  }
  const Frame good = response();
  CHECK(Codec::decode(good, decoded));
  CHECK(decoded.operation == Codec::Set && decoded.result == Codec::Ok);
  CHECK(decoded.storage == Codec::Verified && decoded.speed == 1);
  CHECK(decoded.nonce == 0xFAFE && decoded.sequence == 0x1234);
  CHECK(decoded.active.trim[0] == -3 && decoded.active.trim[1] == 0 &&
        decoded.active.trim[2] == 2 && decoded.active.trim[3] == 3);
  check_transport(good);
  for (size_t size = 0; size <= sizeof(good.bytes); ++size) {
    Frame frame = good;
    frame.size = size;
    decoded = sentinel;
    CHECK(Codec::decode(frame, decoded) == (size == 16));
    if (size != 16) {
      CHECK(same_response(decoded, sentinel));
    }
  }
  for (size_t field = 0; field < 16; ++field) {
    for (unsigned value = 0; value < 256; ++value) {
      Frame frame = good;
      frame.bytes[field] = static_cast<uint8_t>(value);
      bool valid = true;
      switch (field) {
        case 0:
          valid = value == 0xDA;
          break;
        case 1:
          valid = value == 1;
          break;
        case 2:
          valid = value == 3;
          break;
        case 3:
          valid = value <= 1;
          break;
        case 4:
          valid = value <= 5;
          break;
        case 5:
          valid = value <= 3;
          break;
        case 6:
          valid = value <= 1;
          break;
        case 7:
          valid = value <= 3;
          break;
        default:
          valid = field < 12 || value <= 3 || value >= 253;
          break;
      }
      decoded = sentinel;
      CHECK(Codec::decode(frame, decoded) == valid);
      if (valid) {
        check_decoded_fields(frame, decoded);
      } else {
        CHECK(same_response(decoded, sentinel));
      }
    }
  }
  for (unsigned tuple = 0; tuple < 2401; ++tuple) {
    unsigned remaining = tuple;
    Frame frame = good;
    Codec::Values values{};
    for (size_t i = 0; i < 4; ++i) {
      values.trim[i] = static_cast<int8_t>(remaining % 7) - 3;
      remaining /= 7;
      frame.bytes[12 + i] = static_cast<uint8_t>(values.trim[i]);
    }
    CHECK(Codec::decode(frame, decoded));
    Frame set{};
    CHECK(Codec::set(0xFAFE, 1, values, values, set));
    for (size_t i = 0; i < 4; ++i) {
      CHECK(decoded.active.trim[i] == values.trim[i]);
      CHECK(set.bytes[9 + i] == frame.bytes[12 + i] && set.bytes[13 + i] == frame.bytes[12 + i]);
    }
    check_transport(set);
    check_transport(frame);
  }
}

static Codec::Values values(int8_t normal_up, int8_t normal_down, int8_t slow_up,
                            int8_t slow_down) {
  Codec::Values result{};
  result.trim[0] = normal_up;
  result.trim[1] = normal_down;
  result.trim[2] = slow_up;
  result.trim[3] = slow_down;
  return result;
}

static bool same_values(const Codec::Values& a, const Codec::Values& b) {
  for (size_t i = 0; i < 4; ++i) {
    if (a.trim[i] != b.trim[i]) {
      return false;
    }
  }
  return true;
}

static Frame reply_to(const Frame& request, uint16_t sequence, const Codec::Values& active,
                      Codec::Result result = Codec::Ok,
                      Codec::StorageState storage = Codec::Verified) {
  Frame frame = response();
  frame.bytes[3] = request.bytes[2];
  frame.bytes[4] = result;
  frame.bytes[5] = storage;
  frame.bytes[8] = request.bytes[3];
  frame.bytes[9] = request.bytes[4];
  frame.bytes[10] = static_cast<uint8_t>(sequence >> 8);
  frame.bytes[11] = static_cast<uint8_t>(sequence);
  for (size_t i = 0; i < 4; ++i) {
    frame.bytes[12 + i] = static_cast<uint8_t>(active.trim[i]);
  }
  return frame;
}

static Frame transmit(Client& client, uint32_t now) {
  Frame frame{};
  CHECK(client.pending_frame(frame));
  CHECK(client.mark_transmitted(now));
  Frame sentinel = frame;
  CHECK(!client.pending_frame(sentinel) && same_frame(sentinel, frame));
  CHECK(!client.mark_transmitted(now));
  return frame;
}

static void detect(Client& client, const Codec::Values& active, uint16_t sequence = 10,
                   Codec::StorageState storage = Codec::Verified) {
  CHECK(client.probe() == RequestStatus::Started);
  const Frame get = transmit(client, 100);
  CHECK(get.bytes[2] == Codec::Get);
  CHECK(client.accept(reply_to(get, sequence, active, Codec::Ok, storage), 101));
  CHECK(!client.busy() && client.supported() && client.state_known());
  CHECK(client.has_last_confirmed() && client.outcome() == Outcome::Read);
  CHECK(same_values(client.last_confirmed().active, active));
}

static Frame prepare_set(Client& client, size_t index, float value,
                         const Codec::Values& fresh = values(0, 0, 0, 0)) {
  CHECK(client.edit(index, value) == RequestStatus::Started);
  const Frame get = transmit(client, 200);
  CHECK(get.bytes[2] == Codec::Get);
  CHECK(client.accept(reply_to(get, 11, fresh), 201));
  CHECK(client.awaiting_token());
  CHECK(client.provide_token(0x12345678, 12));
  Frame set{};
  CHECK(client.pending_frame(set) && set.bytes[2] == Codec::Set);
  return set;
}

static void check_fresh_edit() {
  Client client;
  Frame frame{};
  CHECK(!client.busy() && !client.supported() && !client.state_known());
  CHECK(!client.has_last_confirmed() && client.outcome() == Outcome::None);
  CHECK(!client.pending_frame(frame) && !client.mark_transmitted(0));
  CHECK(!client.provide_token(1, 1));
  CHECK(client.edit(0, 1) == RequestStatus::Unavailable);
  CHECK(client.seed(0xFF00));
  CHECK(client.observe_sequence(9));
  CHECK(client.probe() == RequestStatus::Started);
  CHECK(client.pending_frame(frame));
  const Frame probe = frame;
  CHECK(!client.accept(reply_to(probe, 10, values(0, 0, 0, 0)), 100));
  client.tick(100000);
  CHECK(client.busy() && client.outcome() == Outcome::None);
  CHECK(client.probe() == RequestStatus::Busy && !client.seed(123));
  CHECK(client.edit(1, 2) == RequestStatus::Busy);
  CHECK(same_frame(transmit(client, 100000), probe));
  CHECK(client.accept(reply_to(probe, 10, values(0, 0, 0, 0)), 100001));

  CHECK(client.edit(2, -3) == RequestStatus::Started);
  const Frame get = transmit(client, 100100);
  CHECK(get.bytes[2] == Codec::Get && be16(get.bytes + 3) == 0xFF01);
  const Codec::Values fresh = values(1, -1, 2, -2);
  CHECK(client.accept(reply_to(get, 11, fresh), 100101));
  CHECK(client.awaiting_token() && client.state_known());
  CHECK(client.outcome() == Outcome::None && !client.pending_frame(frame));
  CHECK(same_values(client.last_confirmed().active, fresh));
  CHECK(!client.provide_token(0, 12) && !client.provide_token(1, 10));
  CHECK(!client.provide_token(1, 11) && !client.provide_token(1, 0x800B));
  CHECK(client.observe_sequence(12));
  CHECK(client.provide_token(0x12345678, 12));
  CHECK(!client.provide_token(0x11111111, 13));
  CHECK(client.pending_frame(frame));
  const Frame set = frame;
  CHECK(set.bytes[2] == Codec::Set && be16(set.bytes + 3) == 0xFF02);
  CHECK(set.bytes[5] == 0x12 && set.bytes[6] == 0x34 && set.bytes[7] == 0x56 &&
        set.bytes[8] == 0x78);
  Codec::Values desired = fresh;
  desired.trim[2] = -3;
  for (size_t i = 0; i < 4; ++i) {
    CHECK(set.bytes[9 + i] == static_cast<uint8_t>(fresh.trim[i]));
    CHECK(set.bytes[13 + i] == static_cast<uint8_t>(desired.trim[i]));
  }
  CHECK(client.edit(0, 3) == RequestStatus::Busy);
  CHECK(client.edit(9, std::numeric_limits<float>::quiet_NaN()) == RequestStatus::Busy);
  CHECK(client.probe() == RequestStatus::Busy && client.pending_frame(frame));
  CHECK(same_frame(frame, set));
  CHECK(!client.accept(reply_to(set, 13, desired), 100200));
  client.tick(500000);
  CHECK(client.busy() && client.state_known() && client.outcome() == Outcome::None);
  CHECK(same_values(client.last_confirmed().active, fresh));
  CHECK(same_frame(transmit(client, 500000), set));
  CHECK(!client.state_known() && client.outcome() == Outcome::None);
  CHECK(same_values(client.last_confirmed().active, fresh));
  CHECK(client.accept(reply_to(set, 13, desired), 501999));
  CHECK(!client.busy() && client.state_known() && client.outcome() == Outcome::Saved);
  CHECK(same_values(client.last_confirmed().active, desired));
  CHECK(!client.pending_frame(frame));
  client.tick(900000);
  CHECK(!client.pending_frame(frame) && client.outcome() == Outcome::Saved);
}

static void check_correlation_and_wrap() {
  Client client;
  CHECK(client.seed(0xFFFF) && client.observe_sequence(0xFFFE));
  detect(client, values(0, 0, 0, 0), 0xFFFF);
  CHECK(!client.observe_sequence(0xFFFF) && !client.observe_sequence(0xFFFE));
  CHECK(client.edit(3, 3) == RequestStatus::Started);
  const Frame get = transmit(client, 200);
  CHECK(be16(get.bytes + 3) == 0);
  const Frame good = reply_to(get, 0, values(1, 2, 3, -1));
  Frame bad = good;
  bad.bytes[3] = Codec::Set;
  CHECK(!client.accept(bad, 201));
  bad = good;
  ++bad.bytes[9];
  CHECK(!client.accept(bad, 201));
  bad = good;
  bad.bytes[10] = bad.bytes[11] = 0xFF;
  CHECK(!client.accept(bad, 201));
  bad = good;
  bad.bytes[10] = 0x7F;
  bad.bytes[11] = 0xFF;
  CHECK(!client.accept(bad, 201));
  bad = good;
  bad.bytes[7] = 4;
  CHECK(!client.accept(bad, 201));
  CHECK(client.busy() && !client.awaiting_token());
  CHECK(client.accept(good, 201));
  CHECK(!client.accept(good, 202));
  CHECK(client.observe_sequence(2));
  CHECK(!client.provide_token(1, 1));
  CHECK(client.provide_token(1, 2));
  const Frame set = transmit(client, 300);
  CHECK(be16(set.bytes + 3) == 1);
  const Codec::Values desired = values(1, 2, 3, 3);
  CHECK(!client.accept(reply_to(set, 2, desired), 301));
  CHECK(client.observe_sequence(3));
  CHECK(!client.accept(reply_to(set, 3, desired), 301));
  CHECK(client.accept(reply_to(set, 4, desired), 302));
  CHECK(client.outcome() == Outcome::Saved && client.state_known());
}

static void check_rejections_and_storage() {
  Frame frame{};
  for (uint8_t result = Codec::Bad; result <= Codec::StorageError; ++result) {
    Client client;
    detect(client, values(1, 1, 1, 1));
    const Frame set = prepare_set(client, 0, 3);
    CHECK(same_frame(set, transmit(client, 300)));
    CHECK(client.accept(
        reply_to(set, 13, values(3, 0, 0, 0), static_cast<Codec::Result>(result), Codec::Uncertain),
        301));
    CHECK(client.outcome() == Outcome::Rejected && !client.busy() && !client.state_known());
    CHECK(client.reply().result == result && client.reply().storage == Codec::Uncertain);
    CHECK(same_values(client.last_confirmed().active, values(0, 0, 0, 0)));
    client.tick(100000);
    CHECK(!client.pending_frame(frame));
    // Explicit readback recovers knowledge without repeating the failed SET.
    detect(client, values(0, 0, 0, 0), 14, Codec::Uncertain);
    CHECK(!client.pending_frame(frame));
  }
  for (uint8_t storage = Codec::Default; storage <= Codec::Uncertain; ++storage) {
    Client client;
    detect(client, values(0, 0, 0, 0), 10, static_cast<Codec::StorageState>(storage));
    CHECK(client.last_confirmed().storage == storage);
    // A no-op SET confirms the values without repairing fallback bytes.
    const Frame set = prepare_set(client, 0, 0);
    CHECK(same_frame(set, transmit(client, 300)));
    CHECK(client.accept(
        reply_to(set, 13, values(0, 0, 0, 0), Codec::Ok, static_cast<Codec::StorageState>(storage)),
        301));
    CHECK(client.outcome() ==
          (storage <= Codec::Verified ? Outcome::Saved : Outcome::InvalidReply));
    CHECK(client.state_known() == (storage <= Codec::Verified));
    CHECK(!client.pending_frame(frame));
  }
  {
    Client client;
    detect(client, values(0, 2, -1, 0), 10, Codec::Default);
    const Frame set = prepare_set(client, 1, 3, values(0, 2, -1, 0));
    CHECK(same_frame(set, transmit(client, 300)));
    CHECK(client.accept(reply_to(set, 13, values(0, 3, -1, 0), Codec::Ok, Codec::Default), 301));
    CHECK(client.outcome() == Outcome::Saved && client.state_known());
    CHECK(same_values(client.last_confirmed().active, values(0, 3, -1, 0)));
    CHECK(client.last_confirmed().storage == Codec::Default);
  }
  for (size_t index = 0; index < 4; ++index) {
    Client client;
    detect(client, values(0, 0, 0, 0));
    const Frame set = prepare_set(client, 0, 3);
    transmit(client, 300);
    Codec::Values mismatch = values(3, 0, 0, 0);
    mismatch.trim[index] = -1;
    CHECK(client.accept(reply_to(set, 13, mismatch), 301));
    CHECK(client.outcome() == Outcome::InvalidReply && !client.state_known());
    CHECK(same_values(client.last_confirmed().active, values(0, 0, 0, 0)));
    CHECK(!client.pending_frame(frame));
  }
  Client client;
  detect(client, values(0, 0, 0, 0));
  CHECK(client.edit(0, 3) == RequestStatus::Started);
  const Frame get = transmit(client, 200);
  CHECK(client.accept(reply_to(get, 11, values(3, 3, 3, 3), Codec::Busy), 201));
  CHECK(client.outcome() == Outcome::Rejected && !client.awaiting_token());
  CHECK(!client.provide_token(1, 12) && !client.pending_frame(frame));
  CHECK(same_values(client.last_confirmed().active, values(0, 0, 0, 0)));
}

static void check_timeouts() {
  Frame frame{};
  Client absent;
  CHECK(absent.probe() == RequestStatus::Started);
  const uint32_t start = 0xFFFFFFF0;
  const Frame probe = transmit(absent, start);
  absent.tick(start + 1999);
  CHECK(absent.busy());
  CHECK(!absent.accept(reply_to(probe, 1, values(0, 0, 0, 0)), start + 2000));
  CHECK(absent.outcome() == Outcome::Timeout && !absent.supported());
  CHECK(!absent.state_known() && !absent.has_last_confirmed() && !absent.pending_frame(frame));
  CHECK(absent.probe() == RequestStatus::Started);
  transmit(absent, 10000);
  absent.tick(12000);
  CHECK(absent.outcome() == Outcome::Timeout && !absent.pending_frame(frame));

  Client client;
  detect(client, values(1, 2, 3, 0));
  CHECK(client.edit(0, -3) == RequestStatus::Started);
  const Frame get = transmit(client, 200);
  client.tick(2200);
  CHECK(client.outcome() == Outcome::Timeout && !client.state_known());
  CHECK(same_values(client.last_confirmed().active, values(1, 2, 3, 0)));
  CHECK(!client.accept(reply_to(get, 11, values(0, 0, 0, 0)), 2201));
  CHECK(!client.provide_token(1, 12) && !client.pending_frame(frame));

  const Frame set = prepare_set(client, 1, -3);
  transmit(client, 3000);
  client.tick(4999);
  CHECK(client.busy() && !client.state_known());
  client.tick(5000);
  CHECK(client.outcome() == Outcome::Timeout && !client.busy());
  CHECK(same_values(client.last_confirmed().active, values(0, 0, 0, 0)));
  CHECK(!client.accept(reply_to(set, 13, values(0, -3, 0, 0)), 5001));
  client.tick(1000000);
  CHECK(!client.pending_frame(frame) && !client.provide_token(1, 14));
}

static void check_cancel_and_reconnect() {
  for (unsigned stage = 0; stage < 5; ++stage) {
    for (bool disconnect : {false, true}) {
      Client client;
      const Codec::Values original = values(1, 2, 3, 0);
      detect(client, original);
      CHECK(client.edit(0, -3) == RequestStatus::Started);
      Frame request{};
      CHECK(client.pending_frame(request));
      if (stage >= 1) {
        request = transmit(client, 200);
      }
      if (stage >= 2) {
        CHECK(client.accept(reply_to(request, 11, original), 201));
        CHECK(client.awaiting_token());
        client.tick(1000000);
        CHECK(client.awaiting_token());
      }
      if (stage >= 3) {
        CHECK(client.provide_token(1, 12) && client.pending_frame(request));
      }
      if (stage >= 4) {
        request = transmit(client, 300);
      }
      if (disconnect) {
        client.disconnect();
        CHECK(client.outcome() == Outcome::Disconnected && !client.supported());
        CHECK(!client.state_known() && client.edit(0, 3) == RequestStatus::Unavailable);
      } else {
        CHECK(client.cancel() && client.outcome() == Outcome::Cancelled);
        CHECK(client.supported() && client.state_known() == (stage != 4));
      }
      CHECK(!client.busy() && !client.awaiting_token() && client.has_last_confirmed());
      CHECK(same_values(client.last_confirmed().active, original));
      CHECK(!client.cancel() && !client.provide_token(1, 13));
      Frame frame{};
      CHECK(!client.pending_frame(frame));
      CHECK(!client.accept(reply_to(request, 13, values(-3, 2, 3, 0)), 301));
      client.tick(1000000);
      CHECK(!client.pending_frame(frame));
      // A fresh connection explicitly probes a restarted sequence epoch and
      // never resumes the discarded edit or writes stored adapter values.
      if (disconnect) {
        detect(client, values(0, 0, 0, 0), 1, Codec::Default);
        CHECK(!client.pending_frame(frame));
      }
    }
  }
}

static void check_fault_flags() {
  for (uint8_t flags = 1; flags <= 3; ++flags) {
    Client client;
    detect(client, values(0, 0, 0, 0));
    CHECK(client.edit(0, 1) == RequestStatus::Started);
    const Frame get = transmit(client, 200);
    Frame fault = reply_to(get, 11, values(0, 0, 0, 0));
    fault.bytes[7] = flags;
    CHECK(client.accept(fault, 201));
    CHECK(!client.busy() && !client.awaiting_token());
    CHECK(client.outcome() == Outcome::Rejected && client.reply().flags == flags);
    CHECK(client.state_known() && client.last_confirmed().flags == flags);
    CHECK(client.edit(0, 1) == RequestStatus::Unavailable);
    CHECK(!client.provide_token(0x12345678, 12));
    Frame pending{};
    CHECK(!client.pending_frame(pending));
    CHECK(client.probe() == RequestStatus::Started);
    const Frame refresh = transmit(client, 300);
    CHECK(client.accept(reply_to(refresh, 12, values(0, 0, 0, 0)), 301));
    CHECK(client.edit(0, 1) == RequestStatus::Started);
    CHECK(client.cancel());

    Client sent;
    detect(sent, values(0, 0, 0, 0));
    prepare_set(sent, 0, 1);
    const Frame set = transmit(sent, 300);
    fault = reply_to(set, 13, values(1, 0, 0, 0));
    fault.bytes[7] = flags;
    CHECK(sent.accept(fault, 301));
    CHECK(sent.outcome() == Outcome::InvalidReply && !sent.state_known());
    CHECK(sent.reply().flags == flags && !sent.pending_frame(pending));
  }
}

static void check_numeric_boundary() {
  Client client;
  detect(client, values(0, 0, 0, 0));
  const float invalid[] = {-4,
                           4,
                           -2.5,
                           0.5,
                           std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::max()};
  for (float value : invalid) {
    CHECK(client.edit(0, value) == RequestStatus::InvalidValue);
    CHECK(!client.busy() && client.state_known() && client.outcome() == Outcome::Read);
  }
  CHECK(client.edit(4, 0) == RequestStatus::InvalidValue);
  CHECK(client.edit(std::numeric_limits<size_t>::max(), 0) == RequestStatus::InvalidValue);
  for (size_t index = 0; index < 4; ++index) {
    for (int value = -3; value <= 3; ++value) {
      CHECK(client.edit(index, static_cast<float>(value)) == RequestStatus::Started);
      CHECK(client.cancel());
    }
  }
}

int main() {
  check_requests();
  check_responses();
  check_fresh_edit();
  check_correlation_and_wrap();
  check_rejections_and_storage();
  check_timeouts();
  check_cancel_and_reconnect();
  check_fault_flags();
  check_numeric_boundary();
  std::cout << "Motion trim wire and client transaction checks passed\n";
}
