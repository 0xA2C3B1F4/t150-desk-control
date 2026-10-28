#include "desk_protocol/target.h"
#include <cstdlib>
#include <iostream>
using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
using Phase = TemporaryPreset::Phase;
static Frame value(uint8_t selector, uint16_t raw) {
  Frame f; f.size=6; f.bytes[0]=0x16; f.bytes[1]=selector;
  f.bytes[2]=raw>>8; f.bytes[3]=raw&255; return f;
}
static Frame ack() { Frame f; f.size=5; f.bytes[0]=0x1A; f.bytes[1]=0x1F; return f; }
static Frame take(TemporaryPreset &t, uint8_t kind, uint8_t selector) {
  Frame f; CHECK(t.take_frame(f)); CHECK(f.bytes[0]==kind && f.bytes[1]==selector); return f;
}
static void backup(TemporaryPreset &t, uint32_t now=0) {
  CHECK(t.begin(800,now)); take(t,0x15,0x0B);
  t.accept(value(0x0C,39),now+1); CHECK(t.phase()==Phase::ScaleA);
  t.accept(value(0x0B,24),now+2); take(t,0x15,0x0C);
  t.accept(value(0x0C,39),now+3); take(t,0x15,0x1F);
  t.accept(value(0x1F,456),now+4); CHECK(t.phase()==Phase::Backup);
  CHECK(t.original()==456 && t.target()==122);
  Frame f; CHECK(!t.take_frame(f)); // No write until durable journal succeeds.
}
static void recalled(TemporaryPreset &t, uint16_t baseline=0xFFFF) {
  backup(t); t.commit(true,5); auto f=take(t,0x19,0x1F); CHECK(be16(f.bytes+2)==122);
  t.accept(ack(),6); take(t,0x15,0x1F);
  t.accept(value(0x1F,122),7); CHECK(t.phase()==Phase::Recall);
  t.recalled(8,baseline); CHECK(t.phase()==Phase::Moving);
}
static void early_restored(TemporaryPreset &t) {
  recalled(t,6); t.observe_height(7); t.tick(100,true,0,true);
  take(t,0x15,0x1F); t.accept(value(0x1F,122),101);
  auto f=take(t,0x19,0x1F); CHECK(be16(f.bytes+2)==456);
  t.accept(ack(),102); take(t,0x15,0x1F); t.accept(value(0x1F,456),103);
  t.journal_cleared(true); CHECK(t.phase()==Phase::Monitoring && t.busy() && !t.dirty());
}
int main() {
  // Explicit recovery can restore an older temporary value or another stable
  // value only after two distinct replies. Ordinary restoration still refuses
  // a third value; caller acceptance is required for this separate operation.
  {
    TemporaryPreset t; t.recover(456, 122);
    CHECK(t.restore_backup(0)); take(t,0x15,0x1F);
    auto first=value(0x1F,555); first.bytes[5]=1;
    t.accept(first,1); take(t,0x15,0x1F);
    CHECK(t.phase()==Phase::RestoreConfirm);
    t.accept(first,2); Frame f; CHECK(!t.take_frame(f));
    auto second=first; second.bytes[5]=2;
    t.accept(second,3); f=take(t,0x19,0x1F); CHECK(be16(f.bytes+2)==456);
    t.accept(ack(),4); take(t,0x15,0x1F);
    t.accept(value(0x1F,456),5); CHECK(t.phase()==Phase::ClearJournal);
    t.journal_cleared(true); CHECK(!t.dirty() && !t.busy() && !t.failed());
  }
  {
    TemporaryPreset t; t.recover(456,122); CHECK(t.restore_backup(0)); take(t,0x15,0x1F);
    auto first=value(0x1F,555); first.bytes[5]=1;
    t.accept(first,1); take(t,0x15,0x1F);
    auto changed=value(0x1F,556); changed.bytes[5]=2;
    t.accept(changed,2); Frame f;
    CHECK(t.failed() && t.dirty() && t.phase()==Phase::Fault && !t.take_frame(f));
  }
  for (bool stale : {false,true}) {
    TemporaryPreset t; t.recover(456,122); CHECK(t.restore_backup(0)); take(t,0x15,0x1F);
    t.tick(stale?1:700,!stale,2000,true); Frame f;
    CHECK(t.failed() && t.dirty() && t.phase()==Phase::Fault && !t.take_frame(f));
  }
  {
    TemporaryPreset t; t.recover(456,122); CHECK(t.restore_backup(0)); take(t,0x15,0x1F);
    auto first=value(0x1F,456); first.bytes[5]=1;
    t.accept(first,1); take(t,0x15,0x1F);
    first.bytes[5]=2; t.accept(first,2); Frame f;
    CHECK(t.phase()==Phase::ClearJournal && !t.take_frame(f));
  }
  // A user button may interrupt every phase before recall. A dirty preset
  // still needs settling and verified restoration; no temporary recall leaks.
  for (int stage = 0; stage <= 6; ++stage) {
    TemporaryPreset t; Frame f;
    CHECK(t.begin_keep_awake(0)); take(t,0x15,0x17);
    if (stage >= 1) { t.accept(value(0x17,123),1); take(t,0x15,0x1F); }
    if (stage >= 2) t.accept(value(0x1F,456),2);
    if (stage >= 3) { t.commit(true,3); take(t,0x19,0x1F); }
    if (stage >= 4) { t.accept(ack(),4); take(t,0x15,0x1F); }
    if (stage >= 5) { t.accept(value(0x1F,123),5); take(t,0x15,0x17); }
    if (stage >= 6) t.accept(value(0x17,123),6);
    CHECK(t.yield_keep_awake(10));
    CHECK(!t.failed() && !t.take_frame(f) && !t.take_brake());
    if (stage < 3) { CHECK(!t.busy() && !t.dirty()); continue; }
    CHECK(t.phase()==Phase::Settle && t.dirty());
    // Late replies to the abandoned operation cannot advance restoration.
    t.accept(ack(),11); t.accept(value(0x1F,123),12);
    t.tick(2009,true,3000,true); CHECK(!t.take_frame(f));
    CHECK(t.yield_keep_awake(2009)); // Repeated buttons do not restart settling.
    t.tick(2010,true,3000,false); CHECK(!t.take_frame(f));
    t.tick(2011,true,3000,true); take(t,0x15,0x1F);
    CHECK(t.yield_keep_awake(2012)); // Keep the pending restoration read.
    t.accept(value(0x1F,123),2013); f=take(t,0x19,0x1F); CHECK(be16(f.bytes+2)==456);
    CHECK(t.yield_keep_awake(2014));
    t.accept(ack(),2015); take(t,0x15,0x1F);
    CHECK(t.yield_keep_awake(2016));
    t.accept(value(0x1F,456),2017); CHECK(t.yield_keep_awake(2018));
    t.journal_cleared(true); CHECK(!t.busy() && !t.dirty() && !t.failed());
  }
  {
    TemporaryPreset t; CHECK(t.begin_keep_awake(0)); take(t,0x15,0x17);
    t.accept(value(0x17,123),1); take(t,0x15,0x1F);
    t.accept(value(0x1F,456),2); t.commit(true,3); take(t,0x19,0x1F);
    t.accept(ack(),4); take(t,0x15,0x1F); t.accept(value(0x1F,123),5); take(t,0x15,0x17);
    t.accept(value(0x17,123),6); t.recalled(7);
    CHECK(t.yield_keep_awake(8) && t.phase()==Phase::Moving);
    t.tick(3007,true,3000,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,555),3008);
    CHECK(t.failed() && t.dirty() && !t.yield_keep_awake(3009));
    TemporaryPreset manual; CHECK(manual.begin(800,0)); CHECK(!manual.yield_keep_awake(1));
  }
  for (uint16_t baseline : {uint16_t(6), uint16_t(100), uint16_t(74), uint16_t(76)}) {
    TemporaryPreset t; recalled(t,baseline); Frame f;
    const int direction=baseline<75?1:-1;
    t.observe_height(baseline); t.tick(100,true,0,true); CHECK(!t.take_frame(f));
    t.observe_height(baseline-direction); t.tick(101,true,0,true); CHECK(!t.take_frame(f));
    t.observe_height(baseline+direction);
    t.tick(102,true,0,false); CHECK(!t.take_frame(f)); // Release must be confirmed.
    t.tick(103,true,0,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,122),104); f=take(t,0x19,0x1F); CHECK(be16(f.bytes+2)==456);
    t.accept(ack(),105); take(t,0x15,0x1F); t.accept(value(0x1F,456),106);
    t.journal_cleared(true); CHECK(t.phase()==Phase::Monitoring && t.busy() && !t.dirty());
    CHECK(!t.begin(900,200) && !t.begin_keep_awake(200));
    t.tick(3007,true,2000,true); CHECK(t.busy()); // Retain recall-based timing.
    t.tick(3008,true,1499,true); CHECK(t.busy());
    t.tick(3009,true,1500,false); CHECK(t.busy());
    t.tick(3010,true,1500,true); CHECK(!t.busy() && !t.failed() && !t.take_frame(f));
  }
  {
    TemporaryPreset t; early_restored(t);
    t.tick(104,false,0,true); CHECK(t.take_brake() && t.failed() && !t.busy() && !t.dirty());
  }
  {
    TemporaryPreset t; early_restored(t);
    t.tick(60008,true,0,true); CHECK(t.take_brake() && t.failed() && !t.busy());
  }
  {
    TemporaryPreset t; early_restored(t); t.cancel(110);
    CHECK(!t.busy() && !t.dirty() && t.failed());
  }
  {
    TemporaryPreset t; recalled(t,6); t.observe_height(7); t.tick(100,true,0,true);
    take(t,0x15,0x1F); t.tick(800,true,0,true);
    CHECK(t.take_brake() && t.phase()==Phase::Fault && t.dirty());
  }
  {
    TemporaryPreset t; recalled(t,6); t.observe_height(7); t.tick(100,true,0,true);
    take(t,0x15,0x1F); t.accept(value(0x1F,999),101);
    CHECK(t.take_brake() && t.phase()==Phase::Fault && t.dirty()); // Never overwrite a local edit.
  }
  {
    TemporaryPreset t; recalled(t,6); t.observe_height(7); t.tick(100,true,0,true);
    take(t,0x15,0x1F); t.accept(value(0x1F,122),101); take(t,0x19,0x1F);
    t.tick(102,false,0,true); CHECK(t.take_brake() && t.phase()==Phase::Fault && t.dirty());
  }
  {
    TemporaryPreset t; recalled(t,6); t.observe_height(7); t.tick(100,true,0,true);
    take(t,0x15,0x1F); t.accept(value(0x1F,122),101); take(t,0x19,0x1F);
    t.accept(ack(),102); take(t,0x15,0x1F); t.accept(value(0x1F,456),103);
    t.journal_cleared(false); CHECK(t.take_brake() && t.phase()==Phase::Fault && t.dirty());
  }
  {
    TemporaryPreset t; recalled(t,75); t.observe_height(75); t.tick(3008,true,2000,true);
    take(t,0x15,0x1F); t.accept(value(0x1F,456),3009); t.journal_cleared(true);
    CHECK(!t.busy()); // No movement: retain the settled fallback.
  }
  {
    TemporaryPreset t; early_restored(t);
    t.connection_lost(104); Frame f;
    CHECK(t.take_brake() && !t.take_frame(f) && t.failed() && !t.busy());
  }
  uint16_t raw=999;
  CHECK(!metric_target_raw(729,raw)); CHECK(!metric_target_raw(1181,raw));
  CHECK(metric_target_raw(730,raw) && raw==8);
  CHECK(metric_target_raw(780,raw) && raw==90);
  CHECK(metric_target_raw(800,raw) && raw==122);
  CHECK(metric_target_raw(900,raw) && raw==285);
  CHECK(metric_target_raw(1180,raw) && raw==740);
  for (uint16_t h=733;h<=1180;++h) {
    CHECK(metric_target_raw(h,raw));
    CHECK(metric_display(static_cast<uint16_t>(raw*24/39)).before_rounding_tenths_cm==h);
  }
  Frame f;
  CHECK(read_target_parameter(0x17,f) && f.bytes[0]==0x15); // Read only, never 19/17.
  {
    TemporaryPreset t; CHECK(t.begin_keep_awake(0)); take(t,0x15,0x17);
    t.accept(value(0x17,123),1); take(t,0x15,0x1F); // Exact raw, not rounded 80.0 -> 122.
    t.accept(value(0x1F,456),2); CHECK(t.phase()==Phase::Backup);
    t.commit(true,3); f=take(t,0x19,0x1F); CHECK(be16(f.bytes+2)==123);
    t.accept(ack(),4); take(t,0x15,0x1F);
    t.accept(value(0x1F,123),5); take(t,0x15,0x17);
    t.accept(value(0x17,123),6); CHECK(t.phase()==Phase::Recall);
    t.recalled(7); t.tick(3007,true,2000,false); CHECK(t.phase()==Phase::Moving);
    t.tick(3008,true,2000,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,123),3009); f=take(t,0x19,0x1F); CHECK(be16(f.bytes+2)==456);
    t.accept(ack(),3010); take(t,0x15,0x1F);
    t.accept(value(0x1F,456),3011); t.journal_cleared(true); CHECK(!t.busy() && !t.failed());
  }
  {
    TemporaryPreset t; CHECK(t.begin_keep_awake(0)); take(t,0x15,0x17);
    t.accept(value(0x17,0xffff),1); CHECK(t.failed() && !t.busy() && !t.take_frame(f));
  }
  {
    TemporaryPreset t; CHECK(t.begin_keep_awake(0)); take(t,0x15,0x17);
    t.accept(value(0x17,123),1); take(t,0x15,0x1F); t.accept(value(0x1F,456),2);
    t.commit(true,3); take(t,0x19,0x1F); t.accept(ack(),4); take(t,0x15,0x1F);
    t.accept(value(0x1F,123),5); take(t,0x15,0x17);
    t.accept(value(0x17,124),6); CHECK(t.failed() && t.phase()==Phase::Settle && !t.take_frame(f));
    CHECK(t.interrupted_by_change() && !t.take_brake());
    t.tick(2006,true,2000,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,456),2007); t.journal_cleared(true); CHECK(!t.busy());
  }
  {
    TemporaryPreset t; CHECK(t.begin_keep_awake(0)); take(t,0x15,0x17);
    t.tick(700,true,2000,true); CHECK(t.failed() && !t.busy());
    CHECK(!t.interrupted_by_change()); // A timeout is not a height change.
  }
  {
    // Interrupted after the temporary write, before any press: no brake, the
    // original is restored and verified, and a new cycle starts clean.
    TemporaryPreset t; CHECK(t.begin_keep_awake(0)); take(t,0x15,0x17);
    t.accept(value(0x17,123),1); take(t,0x15,0x1F); t.accept(value(0x1F,456),2);
    t.commit(true,3); take(t,0x19,0x1F); CHECK(t.phase()==Phase::Write && t.dirty());
    t.abort_keep_awake(4,false);
    CHECK(t.failed() && t.interrupted_by_change() && t.phase()==Phase::Settle && !t.take_brake());
    t.accept(ack(),5); CHECK(!t.take_frame(f)); // A late ACK does not resume the cycle.
    t.tick(1000,true,2000,true); CHECK(!t.take_frame(f)); // Settle waits two seconds.
    t.tick(2004,true,2000,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,123),2005); f=take(t,0x19,0x1F); CHECK(be16(f.bytes+2)==456);
    t.accept(ack(),2006); take(t,0x15,0x1F);
    t.accept(value(0x1F,456),2007); CHECK(t.phase()==Phase::ClearJournal);
    t.journal_cleared(true); CHECK(!t.busy() && !t.dirty());
    CHECK(t.begin_keep_awake(40000) && !t.interrupted_by_change() && !t.failed());
  }
  {
    // Interrupted before the backup was committed: nothing was written.
    TemporaryPreset t; CHECK(t.begin_keep_awake(0)); take(t,0x15,0x17);
    t.accept(value(0x17,123),1); take(t,0x15,0x1F);
    t.abort_keep_awake(2,false); CHECK(t.interrupted_by_change() && !t.busy() && !t.dirty() && !t.take_frame(f));
  }
  {
    // An abort after the press keeps its brake request.
    TemporaryPreset t; CHECK(t.begin_keep_awake(0)); take(t,0x15,0x17);
    t.accept(value(0x17,123),1); take(t,0x15,0x1F); t.accept(value(0x1F,456),2);
    t.commit(true,3); take(t,0x19,0x1F); t.accept(ack(),4); take(t,0x15,0x1F);
    t.accept(value(0x1F,123),5); take(t,0x15,0x17); t.accept(value(0x17,123),6);
    t.recalled(7); t.abort_keep_awake(8,true); CHECK(t.take_brake() && t.phase()==Phase::Settle);
  }
  {
    TemporaryPreset t; recalled(t);
    CHECK(!t.begin(900,100));
    t.tick(3200,true,100,false); CHECK(t.phase()==Phase::Moving);
    t.tick(4000,true,1600,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,122),4001); f=take(t,0x19,0x1F); CHECK(be16(f.bytes+2)==456);
    t.accept(ack(),4002); take(t,0x15,0x1F);
    t.accept(value(0x1F,456),4003); CHECK(t.phase()==Phase::ClearJournal && t.dirty());
    t.journal_cleared(true); CHECK(!t.busy() && !t.dirty() && !t.failed());
  }
  {
    TemporaryPreset t; backup(t); t.commit(false,5);
    CHECK(!t.busy() && !t.dirty() && t.failed() && !t.take_frame(f));
  }
  {
    TemporaryPreset t; CHECK(t.begin(800,0)); take(t,0x15,0x0B);
    t.accept(value(0x0B,25),1); CHECK(!t.busy() && t.failed() && !t.take_frame(f));
  }
  {
    TemporaryPreset t; backup(t); t.cancel(5); t.commit(true,6);
    CHECK(!t.busy() && !t.take_frame(f));
  }
  {
    TemporaryPreset t; backup(t); t.commit(true,5); take(t,0x19,0x1F);
    t.tick(705,true,2000,true); CHECK(t.phase()==Phase::Settle);
    t.accept(ack(),706); CHECK(!t.take_frame(f)); // Late ACK never recalls.
    t.tick(2800,true,2000,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,456),2801); CHECK(t.phase()==Phase::ClearJournal);
    t.journal_cleared(true); CHECK(!t.busy());
  }
  {
    TemporaryPreset t; recalled(t); t.cancel(100);
    CHECK(t.phase()==Phase::Settle);
    t.tick(2200,true,1700,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,999),2201); CHECK(t.phase()==Phase::Fault && t.dirty() && !t.take_frame(f));
    CHECK(t.retry_restore(2300)); t.tick(4400,true,3000,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,122),4401); take(t,0x19,0x1F);
    t.accept(ack(),4402); take(t,0x15,0x1F);
    t.accept(value(0x1F,888),4403); CHECK(t.phase()==Phase::Fault && t.dirty());
  }
  {
    TemporaryPreset t; recalled(t); t.tick(4000,false,0,true);
    CHECK(t.take_brake() && !t.take_brake() && t.phase()==Phase::Settle);
    t.tick(10000,false,0,true); CHECK(!t.take_frame(f));
    t.tick(10001,true,1600,true); take(t,0x15,0x1F);
    t.tick(10701,true,2300,true); CHECK(t.phase()==Phase::Fault && t.dirty());
  }
  {
    TemporaryPreset t; t.recover(456,122); CHECK(t.busy() && !t.take_frame(f));
    CHECK(!t.begin(800,0)); CHECK(t.retry_restore(10));
    t.tick(2010,true,2000,true); take(t,0x15,0x1F);
    t.accept(value(0x1F,456),2011); t.journal_cleared(false); CHECK(t.phase()==Phase::Fault && t.dirty());
  }
  {
    TemporaryPreset t; const uint32_t start=0xFFFFFF00U; backup(t,start);
    t.commit(true,start+5); take(t,0x19,0x1F); t.tick(start+705,true,2000,true);
    CHECK(t.phase()==Phase::Settle);
  }
  {
    TemporaryPreset t; recalled(t); t.tick(60008,true,0,true); CHECK(t.take_brake());
  }
  // A retained automatic backup is ambiguous after reboot. Even a value
  // matching the old temporary target must never trigger an automatic write.
  for (uint16_t current : {uint16_t(456), uint16_t(122), uint16_t(555)}) {
    TemporaryPreset t; t.recover(456,122); CHECK(t.check_recovery(0));
    take(t,0x15,0x1F); t.accept(value(0x1F,current),1);
    CHECK(!t.take_frame(f));
    if (current==456) {
      CHECK(t.phase()==Phase::ClearJournal); t.journal_cleared(true);
      CHECK(!t.busy() && !t.dirty() && !t.failed());
    } else CHECK(t.phase()==Phase::Fault && t.dirty() && t.failed());
  }
  {
    TemporaryPreset t; t.recover(456,122); CHECK(t.accept_current(0));
    take(t,0x15,0x1F); t.accept(value(0x1F,555),1); take(t,0x15,0x1F);
    t.accept(value(0x1F,555),2); CHECK(t.phase()==Phase::AcceptVerify); // Duplicate sequence.
    auto fresh=value(0x1F,555); fresh.bytes[5]=1; t.accept(fresh,3);
    CHECK(t.phase()==Phase::ClearJournal && !t.take_frame(f));
    t.journal_cleared(false); CHECK(t.phase()==Phase::Fault && t.dirty());
    CHECK(t.accept_current(4)); take(t,0x15,0x1F);
    t.accept(value(0x1F,555),5); take(t,0x15,0x1F); t.accept(fresh,6);
    t.journal_cleared(true); CHECK(!t.busy() && !t.dirty() && !t.failed());
    CHECK(!t.accept_current(7) && !t.retry_restore(7));
  }
  {
    TemporaryPreset t; t.recover(456,122); CHECK(t.accept_current(0));
    take(t,0x15,0x1F); t.accept(value(0x1F,555),1); take(t,0x15,0x1F);
    auto changed=value(0x1F,556); changed.bytes[5]=1; t.accept(changed,2);
    CHECK(t.phase()==Phase::Fault && t.dirty() && !t.take_frame(f));
    CHECK(t.accept_current(3)); take(t,0x15,0x1F);
    t.tick(703,true,2000,true); CHECK(t.phase()==Phase::Fault && t.dirty());
    CHECK(t.accept_current(704)); take(t,0x15,0x1F);
    t.tick(705,false,2000,true); CHECK(t.phase()==Phase::Fault && t.dirty());
    CHECK(t.accept_current(706)); take(t,0x15,0x1F);
    t.cancel(707); CHECK(t.phase()==Phase::Fault && !t.take_frame(f));
    t.tick(4000,true,3000,true); CHECK(!t.take_frame(f)); // No surprise restore after Stop.
  }
  {
    TemporaryPreset t; CHECK(t.begin_keep_awake(0));
    CHECK(!t.retry_restore(1) && !t.accept_current(1)); CHECK(t.keep_awake());
  }
  {
    TemporaryPreset t; t.recover(456,122); CHECK(t.accept_current(0));
    take(t,0x15,0x1F); t.accept(value(0x1F,555),1); take(t,0x15,0x1F);
    auto fresh=value(0x1F,555); fresh.bytes[5]=1; t.accept(fresh,2);
    CHECK(t.phase()==Phase::ClearJournal);
    t.abort_recovery(3); t.journal_cleared(true);
    CHECK(t.phase()==Phase::Fault && t.dirty() && !t.take_frame(f));
  }
  std::cout << "Target mapping, journal handshake, restore, stop, stale, conflict, timeout and restart tests passed\n";
}
