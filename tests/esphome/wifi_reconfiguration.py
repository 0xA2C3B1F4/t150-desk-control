"""Exercise real Improv and Wi-Fi methods with simulated radio, storage and UART."""

import hashlib
import json
from pathlib import Path
import runpy
import subprocess

import esphome

from desk_test_support import ROOT, parse_args

FILES = {
    "components/improv_serial/improv_serial_component.cpp": "6bca1c07f34d50f8937751f94b28fd1d8ddef948246ca55f42edbc23c6687996",
    "components/wifi/wifi_component.cpp": "70e2d82299b3fb150a7d56e875a5a5a0658c59de2edc48caa50366f1ff428dd3",
    "components/wifi/wifi_component.h": "805ea1d1f16b79f704bdf51186e7ea20ebbf2ac2ff5624113d389eed34066e37",
}


def method(text: str, signature: str) -> str:
    """Extract one complete method without modifying its upstream body."""
    start = text.index(signature)
    body = text.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_patch_files(work: Path, source: str, patch: dict) -> None:
    """Verify exact patching, idempotence and rejection of altered or linked files."""
    build = work / "patch-checks"
    original = build / "desk_upstream_wifi.cpp"
    target = build / "src/esphome/components/wifi/wifi_component.cpp"
    target.parent.mkdir(parents=True)
    original.write_text(source)
    target.write_text(source)
    apply = patch["apply"]
    apply(build)
    expected = patch["patch_wifi"](source)
    assert target.read_text() == expected
    timestamp = target.stat().st_mtime_ns
    apply(build)
    assert target.stat().st_mtime_ns == timestamp
    target.write_text(source)
    apply(build)
    assert target.read_text() == expected
    for path in (original, target):
        saved = path.read_text()
        path.write_text(saved + "\n// Unexpected source change.\n")
        try:
            apply(build)
        except ValueError:
            pass
        else:
            raise AssertionError("Unreviewed source was accepted: " + str(path))
        path.write_text(saved)
        path.unlink()
        path.symlink_to(work / "missing-source")
        try:
            apply(build)
        except ValueError:
            pass
        else:
            raise AssertionError("Source symlink was accepted: " + str(path))
        path.unlink()
        path.write_text(saved)


PRELUDE = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#define ESP_LOGV(...)
#define ESP_LOGW(...)
#define ESP_LOGD(...)
#define ESP_LOGE(...)
#define ESP_LOGI(...)
#define USE_WIFI
#define USE_WEBSERVER
constexpr size_t MAC_ADDRESS_PRETTY_BUFFER_SIZE=18;
struct AppStub { uint32_t get_loop_component_start_time() const; } App;
#define USE_WEBSERVER_PORT 80
using std::optional;
uint32_t now_ms=100;
uint32_t millis(){ return now_ms; }
uint32_t AppStub::get_loop_component_start_time() const { return now_ms; }
namespace network {
constexpr size_t IP_ADDRESS_BUFFER_SIZE=40;
struct IP { std::string value; bool is_ip4()const{return true;} void str_to(char *buf)const{std::strcpy(buf,value.c_str());} };
}
namespace improv {
enum State { STATE_AUTHORIZED=2,STATE_PROVISIONING=3,STATE_PROVISIONED=4 };
enum Command { WIFI_SETTINGS=1 };
enum Error { ERROR_UNABLE_TO_CONNECT=3 };
struct ImprovCommand { Command command=WIFI_SETTINGS; std::string ssid,password; };
std::vector<std::string> last_urls;
std::vector<uint8_t> build_rpc_response(Command,const std::vector<std::string>&urls,bool){last_urls=urls;return {1};}
}
namespace wifi {
constexpr size_t SSID_BUFFER_SIZE=33;
enum { WIFI_COMPONENT_STATE_STA_CONNECTED, WIFI_COMPONENT_STATE_STA_CONNECTING, WIFI_COMPONENT_STATE_COOLDOWN };
enum class WiFiSTAConnectStatus { CONNECTED, CONNECTING };
struct WiFiAP {
 std::string ssid_,password_;
 void set_ssid(const char*s){ssid_=s;} void set_password(const char*s){password_=s;}
 const std::string&get_ssid()const{return ssid_;} const std::string&get_password()const{return password_;}
 bool has_bssid()const{return false;} std::array<uint8_t,6>get_bssid()const{return {};}
};
void format_mac_addr_upper(const uint8_t*,char*){}
struct SavedWifiSettings { char ssid[33]; char password[65]; };
struct Pref { int count=0; SavedWifiSettings saved{}; bool save(const SavedWifiSettings*s){saved=*s;++count;return true;} };
struct Preferences { int count=0; void sync(){++count;} } prefs;
auto *global_preferences=&prefs;
struct WiFiComponent {
 bool connected_=false,error_from_callback_=false,driver_ok=true,skip_cooldown_next_cycle_=false,driver_connected=false;
 int state_=WIFI_COMPONENT_STATE_STA_CONNECTING,selected_sta_index_=-1; uint32_t action_started_=0;
 std::string driver_ssid="old-net",ip="0.0.0.0"; WiFiAP sta; Pref pref_;
 const char *wifi_ssid_to(char *buf){std::strcpy(buf,driver_ssid.c_str());return buf;}
 bool is_disabled()const{return false;}
 void clear_sta(){sta={};} void init_sta(int){} void add_sta(const WiFiAP &ap){sta=ap;}
 int get_sta_priority(std::array<uint8_t,6>){return 0;}
 void retry_connect(){state_=WIFI_COMPONENT_STATE_STA_CONNECTING;}
 // Radio is a stub. No success event is delivered until test explicitly injects it.
 bool wifi_sta_connect_(const WiFiAP&){driver_connected=false;return driver_ok;}
 WiFiSTAConnectStatus wifi_sta_connect_status_()const{return driver_connected?WiFiSTAConnectStatus::CONNECTED:WiFiSTAConnectStatus::CONNECTING;}
 std::vector<network::IP>wifi_sta_ip_addresses(){return {{ip}};}
 void set_sta(const WiFiAP&); void save_wifi_sta(const char*,const char*);
 void save_wifi_sta(const std::string&s,const std::string&p){save_wifi_sta(s.c_str(),p.c_str());}
 void connect_soon_();void start_connecting(const WiFiAP&);
 INLINES
};
WiFiComponent *global_wifi_component=nullptr;
}
using wifi::WiFiComponent;
WMETHODS
constexpr unsigned IMPROV_SERIAL_TIMEOUT=100;
constexpr unsigned WIFI_SWITCH_TIMEOUT_MS=15000, WIFI_CONNECT_TIMEOUT_MS=30000;
constexpr const char *WIFI_CONNECT_TIMEOUT_ID="wifi-connect";
struct ImprovSerialComponent {
 uint32_t last_read_byte_=0;std::vector<uint8_t>rx_buffer_;improv::State state_=improv::STATE_AUTHORIZED;
 wifi::WiFiAP connecting_sta_;std::deque<improv::ImprovCommand> commands;
 std::vector<int>states,errors;int responses=0;std::function<void()> timeout;
 optional<uint8_t>read_byte_(){if(commands.empty())return {};return 1;}
 // The UART decoder is stubbed at its valid WIFI_SETTINGS callback boundary.
 bool parse_improv_serial_byte_(uint8_t){auto c=commands.front();commands.pop_front();return payload(c);}
 void set_state_(improv::State s){state_=s;states.push_back(s);}
 void set_error_(improv::Error e){errors.push_back(e);}
 void send_response_(std::vector<uint8_t>&){++responses;}
 void cancel_timeout(const char*){timeout={};}
 void set_timeout(const char*,unsigned ms,std::function<void()> f){assert(ms==30000 || ms==15000);timeout=f;}
 void loop();bool payload(improv::ImprovCommand&);void on_wifi_connect_timeout_();
 void send_settings_response_(improv::Command){++responses;}
};
IMETHODS
"""

TESTS = r"""
int main(int argc,char**argv){
 bool fixed=std::string(argv[1])=="invalidate";

 int cases=0,premature=0;
 bool require_correct=argc>2;
 // Same/different SSID, already connected/offline, radio start accepted/rejected.
 for(bool online:{false,true})for(bool same:{false,true})for(bool driver_ok:{false,true}){
  wifi::WiFiComponent w;wifi::global_wifi_component=&w;wifi::prefs.count=0;
  w.connected_=online;w.state_=online?wifi::WIFI_COMPONENT_STATE_STA_CONNECTED:wifi::WIFI_COMPONENT_STATE_STA_CONNECTING;
  w.driver_ok=driver_ok;ImprovSerialComponent p;p.commands.push_back({improv::WIFI_SETTINGS,same?"old-net":"new-net","wrong-test-password"});
  p.loop();
  if(require_correct&&p.state_==improv::STATE_PROVISIONED){std::cout<<"FAIL: persisted/provisioned before new connection success\n";return 1;}
  bool false_success=online && !fixed && same;
  assert((p.state_==improv::STATE_PROVISIONED)==false_success);
  assert(w.pref_.count==int(false_success));assert(wifi::prefs.count==int(false_success));
  if(false_success){
   ++premature;
   assert(!p.timeout);assert(p.responses==1);
   assert(std::string(w.pref_.saved.password)=="wrong-test-password");
   w.update_connected_state_();assert(!w.is_connected());p.loop();assert(p.errors.empty());
  }else{
   assert(p.timeout);w.update_connected_state_();assert(!w.is_connected());
   now_ms+=30001;auto callback=p.timeout;p.timeout={};callback();
   assert(p.errors.size()==1&&p.errors[0]==improv::ERROR_UNABLE_TO_CONNECT);
   assert(p.state_==improv::STATE_AUTHORIZED&&w.pref_.count==0);
   // Correct retry must remain pending until a new success + IP is injected.
   w.driver_ok=true;p.commands.push_back({improv::WIFI_SETTINGS,"old-net","right-test-password"});p.loop();
   assert(p.state_==improv::STATE_PROVISIONING&&w.pref_.count==0);
   w.state_=wifi::WIFI_COMPONENT_STATE_STA_CONNECTED;w.driver_connected=true;w.driver_ssid="old-net";w.ip="192.0.2.10";w.update_connected_state_();p.loop();
   assert(p.state_==improv::STATE_PROVISIONED&&w.pref_.count==1&&wifi::prefs.count==1);
   assert(std::string(w.pref_.saved.password)=="right-test-password");assert(!p.timeout);
   p.loop();assert(w.pref_.count==1);
  }
  ++cases;
 }
 std::cout<<argv[1]<<": "<<cases<<"/"<<cases<<" cases characterized; premature successes="<<premature<<"\n";
}
"""


def main() -> None:
    """Characterize the upstream failure and verify the generated fix in eight cases."""
    work = parse_args(__doc__).work_dir
    package = Path(esphome.__file__).parent
    source_hashes = {}
    for name, digest in FILES.items():
        actual = hashlib.sha256((package / name).read_bytes()).hexdigest()
        assert actual == digest, name
        source_hashes[name] = actual
    improv, wifi, wifi_header = [(package / name).read_text() for name in FILES]
    wifi_methods = "\n".join(
        method(wifi, signature)
        for signature in (
            "void WiFiComponent::set_sta(",
            "void WiFiComponent::save_wifi_sta(const char *",
            "void WiFiComponent::connect_soon_(",
            "void WiFiComponent::start_connecting(",
        )
    )
    inlines = "\n".join(
        method(wifi_header, signature)
        for signature in (
            "bool is_connected() const",
            "bool is_connected_() const",
            "void update_connected_state_()",
        )
    )
    improv_methods = "\n".join(
        method(improv, signature)
        for signature in (
            "void ImprovSerialComponent::loop()",
            "void ImprovSerialComponent::on_wifi_connect_timeout_()",
        )
    )
    case = improv[
        improv.index("    case improv::WIFI_SETTINGS: {") : improv.index(
            "    case improv::GET_CURRENT_STATE:"
        )
    ]
    improv_methods += (
        "\nbool ImprovSerialComponent::payload(improv::ImprovCommand &command) { switch(command.command) {\n"
        + case
        + "default: return false; }}\n"
    )
    patch = runpy.run_path(
        str(ROOT / "components/desk_provisioning/t150_provisioning_patches/apply.py")
    )
    check_patch_files(work, wifi, patch)
    report = {"source_sha256": source_hashes, "runs": [], "hardware_access": False}
    for mode in ("stock", "invalidate"):
        methods = wifi_methods
        if mode == "invalidate":
            signature = "void WiFiComponent::start_connecting("
            methods = methods.replace(
                method(wifi, signature), method(patch["patch_wifi"](wifi), signature)
            )
        source = (
            PRELUDE.replace("INLINES", inlines)
            .replace("WMETHODS", methods)
            .replace("IMETHODS", improv_methods)
            + TESTS
        )
        cpp = work / (mode + ".cpp")
        executable = work / mode
        cpp.write_text(source)
        subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-O0",
                "-g",
                "-fsanitize=address,undefined",
                str(cpp),
                "-o",
                str(executable),
            ],
            check=True,
        )
        result = subprocess.run(
            [str(executable), mode], check=True, text=True, capture_output=True
        )
        regression = subprocess.run(
            [str(executable), mode, "--require-correct"], text=True, capture_output=True
        )
        expected_exit = 0 if mode == "invalidate" else 1
        assert regression.returncode == expected_exit, regression
        report["runs"].append(
            {
                "mode": mode,
                "stdout": result.stdout.strip(),
                "regression_exit": regression.returncode,
                "regression_stdout": regression.stdout.strip(),
                "source_sha256": hashlib.sha256(cpp.read_bytes()).hexdigest(),
            }
        )
        print(result.stdout.strip())
    (work / "summary.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
