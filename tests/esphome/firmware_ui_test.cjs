// Exercise the real page script with simulated status/HTTP, without device access.
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");
const assert = require("node:assert/strict");
const html = readFileSync(join(__dirname, "../../components/desk_firmware_update/ui.html"), "utf8");
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const base = {
  state: "idle",
  claimed: false,
  active: false,
  pending: false,
  recovery_required: false,
  waiting_for_power_cycle: false,
  preflight_seconds: 120,
  size: 55296,
  progress: 0,
  revision: 0,
  error: "",
  research_url: "",
  sha256: "",
  source_sha256: "",
};
async function page() {
  const elements = {};
  for (const tag of html.matchAll(/<[^>]+\bid="([^"]+)"[^>]*>/g))
    elements[tag[1]] = {
      textContent: "",
      hidden: /\bhidden\b/.test(tag[0]),
      disabled: /\bdisabled\b/.test(tag[0]),
      checked: false,
      files: [],
      tag: tag[0],
      scrollIntoView() {},
      focus() {},
    };
  const io = { status: { ...base }, offline: false, reject: false, actions: [] };
  const context = vm.createContext({
    document: {
      getElementById: (id) => elements[id],
      querySelectorAll: () => Object.values(elements).filter((x) => x.tag.startsWith("<button")),
    },
    crypto: require("node:crypto").webcrypto,
    URL,
    URLSearchParams,
    Uint8Array,
    Blob,
    setInterval() {},
    setTimeout: (fn) => {
      fn();
    },
    fetch: async (url, options) => {
      if (io.offline) throw Error("offline");
      if (options?.method === "POST") {
        if (io.reject) return { ok: false, text: async () => "This tab does not own maintenance" };
        const action = options.body.get("action");
        io.actions.push(action);
        if (action === "exit") io.status = { ...base };
        if (action === "claim") io.status.claimed = true;
        return { ok: true, json: async () => ({ revision: io.status.revision }) };
      }
      return { ok: true, json: async () => ({ ...io.status }) };
    },
  });
  vm.runInContext(script, context);
  await vm.runInContext("refresh()", context);
  return {
    elements,
    io,
    run: (s) => vm.runInContext(s, context),
    state: async (s, owned = true) => {
      io.status = { ...base, claimed: owned, ...s };
      vm.runInContext(`sessionOwned=${owned}`, context);
      await vm.runInContext("refresh()", context);
    },
  };
}
(async () => {
  const p = await page(),
    e = p.elements;
  const pinned = readFileSync(
    join(__dirname, "../../components/desk_firmware_update/reset_speed_patch.h"),
    "utf8",
  ).match(/RS01_SHA = "([0-9a-f]{64})"/)[1];
  assert.equal(p.run("RS01"), pinned);
  for (const [identity, header] of [
    ["TB01", "tb01_patch.h"],
    ["FP01", "platform_patch.h"],
    ["TR01", "motion_trim_patch.h"],
  ]) {
    const source = readFileSync(
      join(__dirname, "../../components/desk_firmware_update", header),
      "utf8",
    );
    const hash = source.match(new RegExp(identity + '_SHA = "([0-9a-f]{64})"'))[1];
    assert.equal(p.run(identity), hash);
  }

  await p.state({ state: "installing", active: true, waiting_for_power_cycle: true });
  assert.match(e.guidanceTitle.textContent, /OFF, then ON/);
  assert.equal(e.cancel.hidden, false);
  p.io.offline = true;
  await assert.rejects(p.run("refresh()"));
  assert.equal(e.message.hidden, false);
  assert.match(e.guidanceTitle.textContent, /Connection unavailable/);
  assert.equal(e.cancel.disabled, true);
  p.io.offline = false;
  await p.state({ state: "installing", active: true });
  assert.match(e.guidanceTitle.textContent, /keep controller power ON/);
  assert.equal(e.message.textContent, "");
  await p.state({ state: "staged", recovery_required: true });
  assert.equal(e.verify.hidden, false);
  assert.equal(e.verify.disabled, false);
  await p.state({
    state: "read_complete",
    sha256: "e5324473e0c7cdb43b0a3328d023454f56724875ab61973a5640c683727dabff",
  });
  assert.equal(e.guidanceTitle.textContent, "Direct height control is already installed");
  assert.equal(e.patch.disabled, true);
  assert.equal(e.patchChildLock.disabled, false);
  await p.state({ state: "read_complete", sha256: p.run("STOCK"), source_sha256: p.run("STOCK") });
  assert.equal(e.patchChildLock.disabled, false);
  assert.equal(e.patch.disabled, false);
  p.io.actions = [];
  await e.patchChildLock.onclick();
  assert.match(e.message.textContent, /saved backup/);
  assert.deepEqual(p.io.actions, []);
  p.run("original=new Uint8Array(55296);originalHash=STOCK");
  e.saved.checked = true;
  await e.patchChildLock.onclick();
  assert.deepEqual(p.io.actions, ["backup", "patch_child_lock"]);
  await p.state({ state: "read_complete", sha256: p.run("CL01") });
  assert.equal(e.guidanceTitle.textContent, "Panel lock is already installed");
  assert.equal(e.patchChildLock.disabled, true);
  assert.equal(e.patch.disabled, true);
  assert.match(e.guidanceDetail.textContent, /remove panel lock/);
  assert.match(e.guidanceDetail.textContent, /unlock the panel remotely/i);
  await p.state({ state: "staged", sha256: p.run("CL01") });
  assert.doesNotMatch(e.guidanceTitle.textContent, /already installed/);
  assert.match(e.guidanceDetail.textContent, /starts locked/);
  assert.match(e.guidanceDetail.textContent, /buttons used to stop movement/);
  await p.state({ state: "verified", sha256: p.run("CL01") });
  assert.match(e.guidanceDetail.textContent, /unlock the panel remotely/i);
  // RS01 can be prepared only from an exact supported source with a saved backup.
  for (const identity of ["STOCK", "DT02", "CL01"]) {
    await p.state({
      state: "read_complete",
      sha256: p.run(identity),
      source_sha256: p.run(identity),
    });
    assert.equal(e.patchResetSpeed.disabled, false);
  }
  p.run("original=new Uint8Array(55296);originalHash=CL01");
  e.saved.checked = true;
  p.io.actions = [];
  await e.patchResetSpeed.onclick();
  assert.deepEqual(p.io.actions, ["backup", "patch_reset_speed"]);
  await p.state({ state: "read_complete", sha256: p.run("RS01") });
  assert.match(e.guidanceTitle.textContent, /Reset and speed controls.*already installed/);
  assert.equal(e.patchResetSpeed.disabled, true);
  assert.equal(e.patchChildLock.disabled, true);
  assert.equal(e.patch.disabled, true);
  await p.state({ state: "staged", sha256: p.run("RS01") });
  assert.match(e.guidanceDetail.textContent, /unlock the panel remotely/i);
  await p.state({ state: "verified", sha256: p.run("RS01") });
  assert.match(e.guidanceDetail.textContent, /unlock the panel remotely/i);
  for (const s of [
    { state: "read_complete", sha256: "unknown" },
    { state: "read_complete", sha256: p.run("CL01"), preflight_seconds: 0 },
    { state: "reading", sha256: p.run("CL01"), active: true },
  ]) {
    await p.state(s);
    assert.equal(e.patchResetSpeed.disabled, true);
  }
  // Published timer firmware upgrades directly to the shared foundation.
  for (const identity of ["STOCK", "DT02", "CL01", "RS01", "TB01"]) {
    await p.state({
      state: "read_complete",
      sha256: p.run(identity),
      source_sha256: p.run(identity),
    });
    assert.equal(e.patchPlatform.disabled, false);
    assert.equal(e.patchTimerBuzzer.disabled, identity === "TB01");
  }
  p.run("original=new Uint8Array(55296);originalHash=TB01");
  e.saved.checked = true;
  p.io.actions = [];
  await e.patchPlatform.onclick();
  assert.deepEqual(p.io.actions, ["backup", "patch_platform"]);
  await p.state({ state: "read_complete", sha256: p.run("FP01") });
  assert.match(e.guidanceTitle.textContent, /foundation.*already installed/);
  assert.equal(e.patchPlatform.disabled, true);
  assert.equal(e.patchTimerBuzzer.disabled, true);
  for (const identity of ["STOCK", "DT02", "CL01", "RS01", "TB01", "FP01"]) {
    await p.state({
      state: "read_complete",
      sha256: p.run(identity),
      source_sha256: p.run(identity),
    });
    assert.equal(e.patchMotionTrims.disabled, false);
  }
  p.run("original=new Uint8Array(55296);originalHash=FP01");
  e.saved.checked = true;
  p.io.actions = [];
  await e.patchMotionTrims.onclick();
  assert.deepEqual(p.io.actions, ["backup", "patch_motion_trims"]);
  await p.state({ state: "read_complete", sha256: p.run("TR01") });
  assert.match(e.guidanceTitle.textContent, /Motion trims.*already installed/);
  assert.equal(e.patchMotionTrims.disabled, true);
  for (const identity of ["TB01", "FP01", "TR01"]) {
    await p.state({ state: "verified", sha256: p.run(identity) });
    assert.match(e.guidanceDetail.textContent, /unlock the panel remotely/i);
  }
  for (const status of [
    { state: "read_complete", sha256: "unknown" },
    { state: "read_complete", sha256: p.run("TB01"), preflight_seconds: 0 },
    { state: "reading", sha256: p.run("TB01"), active: true },
  ]) {
    await p.state(status);
    assert.equal(e.patchPlatform.disabled, true);
    assert.equal(e.patchTimerBuzzer.disabled, true);
    assert.equal(e.patchMotionTrims.disabled, true);
  }
  await p.state({ state: "read_complete", sha256: "unknown" });
  assert.equal(e.patchChildLock.disabled, true);
  await p.state({ state: "read_complete", sha256: p.run("STOCK"), preflight_seconds: 0 });
  assert.equal(e.patchChildLock.disabled, true);
  await p.state({ state: "reading", sha256: p.run("STOCK"), active: true });
  assert.equal(e.patchChildLock.disabled, true);
  await p.state({
    state: "staged",
    sha256: "e5324473e0c7cdb43b0a3328d023454f56724875ab61973a5640c683727dabff",
  });
  assert.doesNotMatch(e.guidanceTitle.textContent, /already installed/);
  await p.state({ state: "staged", preflight_seconds: 0 });
  assert.match(e.guidanceTitle.textContent, /expired/);
  assert.equal(e.read.hidden, false);
  assert.equal(e.install.disabled, true);
  await p.state({ state: "error", error: "Read timed out" });
  assert.match(e.guidanceTitle.textContent, /stopped/);
  assert.equal(e.message.hidden, false);
  p.run("finishedAt=0;render()");
  await e.exit.onclick();
  assert.match(e.guidanceTitle.textContent, /Restart the controller/);
  await p.state({ claimed: true }, false);
  assert.equal(e.sessionControls.hidden, false);
  assert.match(e.guidanceTitle.textContent, /Take over/);
  await p.state({ claimed: true });
  p.io.reject = true;
  await e.read.onclick();
  assert.equal(e.sessionControls.hidden, false);
  await p.state({ state: "uploading" });
  assert.match(e.guidanceTitle.textContent, /not running/);
  assert.equal(e.cancel.hidden, false);
  const errors = await page();
  await errors.state({ state: "error", error: "Old failure" });
  await errors.state({ state: "idle" });
  assert.equal(errors.elements.message.textContent, "");
  await p.state({ state: "read_complete", recovery_required: true, sha256: "partial" });
  assert.match(e.guidanceDetail.textContent, /saved original application file/);
  assert.doesNotMatch(e.guidanceDetail.textContent, /finish maintenance/);
  assert.equal(e.exit.hidden, true);
  await p.state({ state: "read_complete", sha256: "unknown", research_url: "" });
  assert.doesNotMatch(e.guidanceDetail.textContent, /research section/);
  e.file.files = [{ size: 1 }];
  await e.file.onchange();
  assert.match(e.message.textContent, /55,296/);
  e.file.files = [{ size: 55296, arrayBuffer: async () => new ArrayBuffer(55296) }];
  await e.file.onchange();
  assert.equal(e.message.textContent, "");
  await p.state({ state: "read_complete", sha256: p.run("TR01"), source_sha256: p.run("TR01") });
  p.io.reject = false;
  p.run("original=new Uint8Array(55296);originalHash=TR01");
  e.saved.checked = true;
  assert.equal(e.stage.disabled, false);
  p.io.actions = [];
  await e.stage.onclick();
  assert.equal(p.io.actions[0], "backup");
  assert.equal(p.io.actions[1], "stage");
  assert.equal(p.io.actions.filter((a) => a === "chunk").length, 432);
  assert.equal(p.io.actions.at(-1), "seal");
  const fresh = await page();
  fresh.io.offline = true;
  fresh.run("status=null");
  await assert.rejects(fresh.run("refresh()"));
  assert.equal(fresh.elements.message.hidden, false);
  console.log(
    "Firmware UI: power instructions, connection loss, hash-bound DT02/CL01 preparation, saved backup, lock disclosure, expiry, recovery and takeover passed",
  );
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
