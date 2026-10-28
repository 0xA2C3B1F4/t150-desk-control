#include "desk_protocol/identity.h"
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <string>

using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
static Frame wire(const char *hex) {
  Parser parser; Frame f; bool found = false;
  const std::string s(hex);
  for (std::size_t i = 0; i < s.size(); i += 2) {
    const auto b = static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16));
    if (parser.feed(b, 1, f) == ParseResult::FrameReady) found = true;
  }
  CHECK(found); return f;
}
int main() {
  // Exact captured replies from T150-22-03 on 2026-10-27, including escaped
  // sequence FA in the model reply and fixed-width NUL padding.
  const char *captures[] = {
    "fa1203543135302d32322d303300000000000063fefaebfd",
    "fa12044c554d49204c4547454e442047524f55506b457bfd",
    "fa1205312e352e306b460efd", "fa1206312e3600006b4711fd",
    "fa1207312e392e303131386b4836fd", "fa120932312f30312f31386b4932fd"
  };
  const char *expected[] = {"T150-22-03", "LUMI LEGEND GROUP", "1.5.0", "1.6", "1.9.0118", "21/01/18"};
  const uint8_t selectors[] = {3,4,5,6,7,9};
  for (unsigned i=0; i<6; ++i) {
    IdentityText text; auto f=wire(captures[i]);
    CHECK(decode_identity(f,text)); CHECK(text.selector==selectors[i]);
    CHECK(std::strcmp(text.text,expected[i])==0);
    CHECK(text.sequence==(i==0 ? 0x63FA : 0x6B44+i));
    Frame request; CHECK(read_identity(selectors[i],request));
    CHECK(request.size==4 && request.bytes[0]==0x11 && request.bytes[1]==selectors[i]);
    CHECK(request.bytes[2]==0 && request.bytes[3]==0);
    IdentityText unchanged; unchanged.selector=99;
    --f.size; CHECK(!decode_identity(f,unchanged)); CHECK(unchanged.selector==99);
  }
  Frame request; CHECK(!read_identity(8,request) && request.size==0);
  CHECK(!read_identity(0xff,request));
  IdentityText text;
  CHECK(!decode_identity(wire("fa1208007d07ff6b4abefd"),text));
  auto bad=wire(captures[3]); bad.bytes[3]=0; // Text after an embedded NUL.
  CHECK(!decode_identity(bad,text));
  bad=wire(captures[1]); bad.bytes[2]=0x7f; CHECK(!decode_identity(bad,text));
  bad=wire(captures[0]); std::memset(bad.bytes+2,0,16); CHECK(!decode_identity(bad,text));
  bad=wire(captures[0]); bad.bytes[0]=0x16; CHECK(!decode_identity(bad,text));
  bad=Frame{}; CHECK(!decode_identity(bad,text));
}
