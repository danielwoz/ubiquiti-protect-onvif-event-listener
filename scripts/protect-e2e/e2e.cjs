#!/usr/bin/env node
// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Protect event sharing end to end: Protect's own event-subscription code,
// loaded from a service.js bundle patched by //test:patch_service_js,
// against //test:pullpoint_demo (the recorder's real PullPoint server, hub
// and listener) and the demo's emulated cameras.  See README.md.
//
// usage: node e2e.cjs --bundle=<service.js> --nm=<node_modules> --demo=<pullpoint_demo>
//          --patcher=<patch_service_js> --capture=<capture.jsonl>
//          [--fxp=<fast-xml-parser dir>] [--label=<name>] [--work=<dir>]
//          [--out=<results.json>] [--port=7892] [--only=A,B,...]
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const { buildProtect, makeCamera, makeContext, settle, sleep, Scheduler } =
  require('./lib/protect.cjs');
const { Demo, killAll, killAllNow, hungServer, portInUse } = require('./lib/demo.cjs');

// ------------------------------------------------------------------ args
const args = {};
for (const a of process.argv.slice(2)) {
  const m = /^--([\w-]+)=(.*)$/.exec(a);
  if (!m) {
    console.error(`unknown argument ${a}`);
    process.exit(2);
  }
  args[m[1]] = m[2];
}
for (const k of ['bundle', 'nm', 'demo', 'patcher', 'capture']) {
  if (!args[k]) {
    console.error(`missing --${k}=...  (see README.md)`);
    process.exit(2);
  }
}
const label = args.label || path.basename(args.bundle).replace(/\.js$/, '');
const nmDir = path.resolve(args.nm);
const fxpDir = path.resolve(args.fxp || path.join(nmDir, 'fast-xml-parser'));
const work = path.resolve(args.work || fs.mkdtempSync(path.join(os.tmpdir(), 'protect-e2e-')));
const outFile = path.resolve(args.out || path.join(work, `results-${label}.json`));
const port = Number(args.port || 7892);
const only = args.only ? new Set(args.only.split(',')) : null;
fs.mkdirSync(work, { recursive: true });

// ------------------------------------------------------- patched bundle
const patched = path.join(work, `service-${label}.js`);
const pr = spawnSync(args.patcher, [`--in=${args.bundle}`, `--out=${patched}`,
  '--onvif_addressing=true', '--event_proxy=true'], { encoding: 'utf8' });
let patchReport;
try {
  patchReport = JSON.parse(pr.stdout.trim().split('\n').pop());
} catch (e) {
  console.error(`patch_service_js failed: ${pr.stderr}`);
  process.exit(1);
}
if (pr.status !== 0) {
  console.error(`patch_service_js: ${JSON.stringify(patchReport)}`);
  process.exit(1);
}
let text = fs.readFileSync(patched, 'latin1');
fs.unlinkSync(patched);
// The route patch names port 7892; another port is only for a machine
// where 7892 is taken.
if (port !== 7892) {
  const lit = 'hostname:"127.0.0.1",port:7892}';
  if (text.split(lit).length !== 2) throw new Error('route patch port literal not found once');
  text = text.replace(lit, `hostname:"127.0.0.1",port:${port}}`);
}

// --------------------------------------------------------------- helpers
const EVENT_OPS = new Set(['CreatePullPointSubscription', 'PullMessages', 'Renew', 'Unsubscribe']);
const RECORDER = `127.0.0.1:${port}`;

class Checks {
  constructor() {
    this.checks = {};
    this.numbers = {};
  }
  ok(name, cond, detail) {
    this.checks[name] = { pass: !!cond };
    if (detail !== undefined) this.checks[name].detail = detail;
    if (!cond) console.log(`    FAIL ${name}${detail !== undefined ? ` ${JSON.stringify(detail).slice(0, 400)}` : ''}`);
    return !!cond;
  }
  num(name, v) {
    this.numbers[name] = v;
  }
  result() {
    return { pass: Object.values(this.checks).every((c) => c.pass), checks: this.checks,
             numbers: this.numbers };
  }
}

let scenarioName = '';
let demoSeq = 0;

function demoLog(name) {
  return path.join(work, `${label}-${scenarioName}-${++demoSeq}-${name}.log`);
}

async function startCameras({ slots = 1, second = 0 } = {}) {
  const a = ['--no_recorder', `--capture=${args.capture}`, `--slots=${slots}`,
             '--replay_pulls=false', '--port=0'];
  if (second) a.push(`--second_capture=${args.capture}`, `--second_slots=${second}`);
  return Demo.start(args.demo, a, { name: 'cameras', logFile: demoLog('cameras') });
}

async function startRecorder(cams, { share = 'all', stateDir, extra = [], recPort = port } = {}) {
  const addrs = cams.info.cameras.map((c) => c.address).join(',');
  return Demo.start(args.demo, [`--cameras=${addrs}`, `--share=${share}`, `--port=${recPort}`,
    `--state_dir=${stateDir}`, ...extra], { name: 'recorder', logFile: demoLog('recorder') });
}

function stateDir() {
  return fs.mkdtempSync(path.join(work, `state-${scenarioName}-`));
}

async function hubCamera(rec, key) {
  const h = (await rec.cmd('hub')).hub;
  return h && h.cameras.find((c) => c.key === key);
}

async function waitUp(rec, key, limitMs = 15000) {
  const t0 = Date.now();
  while (Date.now() - t0 < limitMs) {
    const c = await hubCamera(rec, key);
    if (c && c.upstream === 'up') return Date.now() - t0;
    await sleep(200);
  }
  throw new Error(`recorder never got ${key} up`);
}

async function stats(cams, i = 0) {
  return (await cams.cmd('stats')).cameras[i];
}

async function requestsSince(cams, i, from) {
  return (await cams.cmd(`requests ${i} ${from}`)).requests;
}

// Every Protect runtime the current scenario built, for protectLogChecks().
let runtimes = [];

function newProtect() {
  const rt = buildProtect(text, { nmDir, fxpDir });
  runtimes.push(rt);
  return rt;
}

// Checks on everything Protect logged during a scenario, debug lines
// included: Protect switches a host to its raw-socket transport for an hour
// after a response undici cannot use (logged once, as a warning), and it
// replaces a TerminationTime it considers bogus (less than 15 s or more
// than 600 s after CurrentTime) with now + 55 s, logged at debug level only.
function protectLogChecks(c) {
  if (runtimes.length === 0) return;
  const lines = runtimes.flatMap((rt) => rt.logs);
  const raw = lines.filter((l) => /raw socket/i.test(l.msg)).map((l) => l.msg.slice(0, 200));
  const bogus = lines.filter((l) => /bogus ONVIF TerminationTime/.test(l.msg))
    .map((l) => l.msg.slice(0, 200));
  c.ok('Protect never falls back to its raw-socket transport', raw.length === 0, raw);
  c.ok('Protect accepts every TerminationTime it is sent', bogus.length === 0, bogus);
}

function cameraRow(cams, i, pk, mac) {
  const addr = cams.info.cameras[i].address;
  return makeCamera({ host: addr.split(':')[0], port: Number(addr.split(':')[1]), pk, mac });
}

function where(ctx, mac) {
  const s = ctx.eventSubscriptions.get(mac);
  if (!s) return 'none';
  return s.subscriptionPath.startsWith('/onvif-recorder/') ? 'PROXY' : 'CAMERA';
}

function count(rt, topic, from = 0) {
  return rt.published.slice(from).filter((p) => p.topic === topic).length;
}

function warns(rt, from = 0) {
  return rt.logs.slice(from).filter((l) => l.lvl === 'warn' || l.lvl === 'error')
    .map((l) => `${l.logger}.${l.lvl}: ${l.msg.slice(0, 200)}`);
}

function infos(rt, from = 0) {
  return rt.logs.slice(from).filter((l) => l.lvl === 'info').map((l) => l.msg.slice(0, 160));
}

// Wait until @p topic has been published @p n times; resolves with the
// time it took from @p since (null on timeout).
async function waitPublished(rt, topic, n, since, limitMs = 20000) {
  while (Date.now() - since < limitMs) {
    const hits = rt.published.filter((p) => p.topic === topic && p.t >= since);
    if (hits.length >= n) return hits[n - 1].t - since;
    await sleep(25);
  }
  return null;
}

async function motion(cams, i, on) {
  return (await cams.cmd(`motion ${i} ${on}`));
}

function eventRequests(rt, from = 0) {
  return rt.requests.slice(from).filter((r) => EVENT_OPS.has(r.op));
}

function brief(reqs) {
  return reqs.map((r) => `${r.op}@${r.host === RECORDER ? 'recorder' : 'camera'}:${r.status || r.error}(${r.ms}ms)`);
}

function recorderWarnings(rec, fromT = 0) {
  return rec ? rec.logLines(/^[WE]\d{4} .*\[event_proxy\]/, fromT).map((l) => l.slice(0, 220)) : [];
}

// ------------------------------------------------------------- scenarios

// A: steady state through the recorder.
async function scenarioA(c) {
  const cams = await startCameras({ slots: 1 });
  const rec = await startRecorder(cams, { stateDir: stateDir() });
  const key = cams.info.cameras[0].key;
  c.num('recorder_up_ms', await waitUp(rec, key));
  const rt = newProtect();
  const cam = cameraRow(cams, 0, 'camA', 'AABBCCDD0001');
  const ctx = makeContext(rt, [cam]);
  const mark = (await stats(cams)).requests;

  let t = Date.now();
  await rt.m.reconcile.reconcileMotionState(ctx, cam.pk);
  await settle(ctx);
  c.num('reconcile_ms', Date.now() - t);
  c.ok('subscribed via the recorder', where(ctx, cam.mac) === 'PROXY', brief(eventRequests(rt)));
  // Protect's subscription lifetime as it computed it from the recorder's
  // CurrentTime and TerminationTime (PT60S asked; 55 s means it fell back).
  const created = ctx.eventSubscriptions.get(cam.mac);
  if (created) c.num('create_termination_s', (created.terminationTime - Date.now()) / 1000);

  const sched = new Scheduler(rt, ctx);
  sched.start();
  await sched.waitTicks(2);
  t = (await motion(cams, 0, true)).t;
  const startMs = await waitPublished(rt, 'events.cameras.motionStart', 1, t);
  c.num('motion_start_latency_ms', startMs);
  await sched.waitTicks(1);
  t = (await motion(cams, 0, false)).t;
  const stopMs = await waitPublished(rt, 'events.cameras.motionStop', 1, t);
  c.num('motion_stop_latency_ms', stopMs);
  await sched.waitTicks(1);

  const sub = ctx.eventSubscriptions.get(cam.mac);
  const before = rt.requests.length;
  sub.terminationTime = Date.now() + 5000;
  await sched.waitTicks(2);
  const renews = rt.requests.slice(before).filter((r) => r.op === 'Renew');
  c.ok('Renew goes to the recorder and is answered 200',
    renews.length >= 1 && renews.every((r) => r.host === RECORDER && r.status === 200), brief(renews));
  c.ok('the renewed subscription lasts about a minute', sub.terminationTime - Date.now() > 30000,
    sub.terminationTime - Date.now());
  c.num('renew_termination_s', (sub.terminationTime - Date.now()) / 1000);
  await sched.stop();

  const st = await stats(cams);
  const protectOps = (await requestsSince(cams, 0, mark)).filter((r) => r.from === 'protect')
    .map((r) => r.op);
  c.ok('the camera sees only Protect\'s direct GetEventProperties',
    protectOps.length >= 1 && protectOps.every((o) => o === 'GetEventProperties'), protectOps);
  c.ok('the camera has one subscription, the recorder\'s',
    st.live === 1 && st.creates === 1 && st.refused === 0 &&
      st.by['recorder.CreatePullPointSubscription'] === 1, st);
  const ev = eventRequests(rt);
  c.ok('every Create, Pull and Renew goes to the recorder', ev.every((r) => r.host === RECORDER),
    brief(ev.filter((r) => r.host !== RECORDER)));
  c.num('protect_event_requests', ev.length);
  c.ok('motionStart and motionStop reach Protect exactly once each',
    count(rt, 'events.cameras.motionStart') === 1 && count(rt, 'events.cameras.motionStop') === 1,
    { start: count(rt, 'events.cameras.motionStart'), stop: count(rt, 'events.cameras.motionStop') });
  c.ok('no warn logs from Protect', warns(rt).length === 0, warns(rt));
  const hc = await hubCamera(rec, key);
  c.ok('the hub serves one pull point', hc.pull_points.length === 1 &&
    hc.pull_points[0].state === 'serving', hc.pull_points);
  c.num('hub_counters', hc.counters);
  c.num('tick_ms', sched.ticks.map((x) => x.ms));
  c.num('recorder_warnings', recorderWarnings(rec));
}

// B: the camera is not shared, so Protect subscribes directly.
async function scenarioB(c) {
  const cams = await startCameras({ slots: 2 });
  const rec = await startRecorder(cams, { share: 'none', stateDir: stateDir() });
  const t0 = Date.now();
  while ((await stats(cams)).live < 1 && Date.now() - t0 < 15000) await sleep(200);
  const rt = newProtect();
  const cam = cameraRow(cams, 0, 'camB', 'AABBCCDD0002');
  const ctx = makeContext(rt, [cam]);

  let t = Date.now();
  await rt.m.reconcile.reconcileMotionState(ctx, cam.pk);
  await settle(ctx);
  c.num('reconcile_ms', Date.now() - t);
  const creates = eventRequests(rt).filter((r) => r.op === 'CreatePullPointSubscription');
  c.ok('the recorder declines and Protect subscribes at the camera',
    where(ctx, cam.mac) === 'CAMERA' && creates.length === 2 && creates[0].host === RECORDER &&
      creates[0].status === 500 && creates[1].host !== RECORDER && creates[1].status === 200,
    brief(creates));
  if (creates.length === 2) c.num('decline_fallback_ms', creates[1].t - creates[0].t);

  const sched = new Scheduler(rt, ctx);
  sched.start();
  await sched.waitTicks(2);
  const sub = ctx.eventSubscriptions.get(cam.mac);
  const before = rt.requests.length;
  sub.terminationTime = Date.now() + 5000;
  await sched.waitTicks(2);
  const renewReqs = rt.requests.slice(before).filter((r) => r.op !== 'PullMessages');
  c.ok('a direct Renew probes the recorder, is told no, and renews at the camera',
    renewReqs.some((r) => r.host === RECORDER && r.path.endsWith('/status') && r.status === 500) &&
      renewReqs.some((r) => r.op === 'Renew' && r.host !== RECORDER && r.status === 200) &&
      where(ctx, cam.mac) === 'CAMERA', brief(renewReqs));
  t = (await motion(cams, 0, true)).t;
  c.num('motion_start_latency_ms', await waitPublished(rt, 'events.cameras.motionStart', 1, t));
  await sched.stop();
  c.ok('no error counted on the subscription', sub.consecutiveErrors === 0, sub.consecutiveErrors);
  c.ok('no warn logs from Protect', warns(rt).length === 0, warns(rt));
  const hc = await hubCamera(rec, cams.info.cameras[0].key);
  c.ok('the hub counted the decline and the status no',
    hc.counters.declines >= 1 && hc.counters.status_no >= 1, hc.counters);
  c.num('recorder_warnings', recorderWarnings(rec));
}

// C: Protect subscribed directly; the camera becomes shared; Protect's
// next Renew moves the subscription to the recorder.
async function scenarioC(c) {
  const cams = await startCameras({ slots: 2 });
  const sd = stateDir();
  let rec = await startRecorder(cams, { share: 'none', stateDir: sd });
  const key = cams.info.cameras[0].key;
  const t0 = Date.now();
  while ((await stats(cams)).live < 1 && Date.now() - t0 < 15000) await sleep(200);
  const rt = newProtect();
  const cam = cameraRow(cams, 0, 'camC', 'AABBCCDD0003');
  const ctx = makeContext(rt, [cam]);
  await rt.m.reconcile.reconcileMotionState(ctx, cam.pk);
  await settle(ctx);
  c.ok('starts direct', where(ctx, cam.mac) === 'CAMERA', brief(eventRequests(rt)));
  const sched = new Scheduler(rt, ctx);
  sched.start();
  await sched.waitTicks(1);
  let t = (await motion(cams, 0, true)).t;
  c.ok('motion on while direct', (await waitPublished(rt, 'events.cameras.motionStart', 1, t)) !== null);

  // Share the camera: restart the recorder with it shared.
  await rec.stop();
  rec = await startRecorder(cams, { share: 'all', stateDir: sd });
  c.num('recorder_up_ms', await waitUp(rec, key));
  const live2 = (await stats(cams)).live;
  c.ok('before the move the camera has two subscriptions (Protect and the recorder)', live2 === 2,
    live2);

  const sub = ctx.eventSubscriptions.get(cam.mac);
  const L = rt.logs.length;
  const mark = (await stats(cams)).requests;
  const before = rt.requests.length;
  sub.terminationTime = Date.now() + 5000;
  const tRenew = Date.now();
  while (where(ctx, cam.mac) !== 'PROXY' && Date.now() - tRenew < 20000) await sleep(50);
  const moved = rt.requests.slice(before).find((r) => r.op === 'CreatePullPointSubscription' &&
    r.host === RECORDER && r.status === 200);
  c.ok('the next Renew moves the subscription to the recorder', where(ctx, cam.mac) === 'PROXY' &&
    !!moved, brief(rt.requests.slice(before).filter((r) => r.op !== 'PullMessages')));
  const tMoved = moved ? moved.t : Date.now();
  c.num('move_after_forced_renew_ms', tMoved - tRenew);
  let live = 0;
  const tl = Date.now();
  do {
    live = (await stats(cams)).live;
    if (live === 1) break;
    await sleep(100);
  } while (Date.now() - tl < 5000);
  c.ok('the camera then has one subscription, the recorder\'s', live === 1, live);
  c.ok('Protect keeps isMotionActive', sub.isMotionActive === true, sub.isMotionActive);
  c.ok('no error counted on the subscription', sub.consecutiveErrors === 0, sub.consecutiveErrors);

  // The recorder renews its own camera subscription ~10 s after the move.
  let resub = null;
  let maxLive = live;
  const tw = Date.now();
  while (Date.now() - tw < 25000) {
    const reqs = await requestsSince(cams, 0, mark);
    const unsub = reqs.find((r) => r.from === 'recorder' && r.op === 'Unsubscribe' && r.t >= tMoved);
    const create = unsub && reqs.find((r) => r.from === 'recorder' &&
      r.op === 'CreatePullPointSubscription' && r.t >= unsub.t);
    maxLive = Math.max(maxLive, (await stats(cams)).live);
    if (create) {
      resub = { unsubscribe_after_ms: unsub.t - tMoved, create_after_ms: create.t - tMoved };
      break;
    }
    await sleep(250);
  }
  c.ok('the recorder resubscribes about 10 s after the move',
    resub !== null && resub.unsubscribe_after_ms >= 8000 && resub.unsubscribe_after_ms <= 16000, resub);
  c.num('recorder_resubscribe', resub);
  await waitUp(rec, key);
  await sched.waitTicks(1);
  t = (await motion(cams, 0, false)).t;
  c.num('motion_stop_latency_ms', await waitPublished(rt, 'events.cameras.motionStop', 1, t));
  await sched.waitTicks(1);
  await sched.stop();
  c.ok('motion stop after the move reaches Protect through the recorder',
    count(rt, 'events.cameras.motionStop') === 1 && count(rt, 'events.cameras.motionStart') === 1,
    { start: count(rt, 'events.cameras.motionStart'), stop: count(rt, 'events.cameras.motionStop') });
  c.ok('the camera never had more than two subscriptions, one after the move', maxLive <= 2,
    maxLive);
  c.ok('no warn logs from Protect', warns(rt, L).length === 0 && warns(rt).length === 0, warns(rt));
  c.num('protect_info', infos(rt, L));
  const hc = await hubCamera(rec, key);
  c.ok('the hub counted the status yes', hc.counters.status_yes >= 1, hc.counters);
  c.num('recorder_warnings', recorderWarnings(rec));
}

// D: the recorder restarts while Protect polls through it: between two
// polls, while Protect's PullMessages is held, and with a 2.5 s pause
// before the new process starts (a slow systemd restart).
async function scenarioD(c) {
  const cams = await startCameras({ slots: 1 });
  const sd = stateDir();
  let rec = await startRecorder(cams, { stateDir: sd });
  const key = cams.info.cameras[0].key;
  await waitUp(rec, key);
  const rt = newProtect();
  const cam = cameraRow(cams, 0, 'camD', 'AABBCCDD0004');
  const ctx = makeContext(rt, [cam]);
  await rt.m.reconcile.reconcileMotionState(ctx, cam.pk);
  await settle(ctx);
  c.ok('subscribed via the recorder', where(ctx, cam.mac) === 'PROXY');
  const sub = ctx.eventSubscriptions.get(cam.mac);
  const path0 = sub ? sub.subscriptionPath : '';
  let maxErrors = 0;
  const sched = new Scheduler(rt, ctx, {
    onTick: () => {
      const s = ctx.eventSubscriptions.get(cam.mac);
      if (s) maxErrors = Math.max(maxErrors, s.consecutiveErrors);
    },
  });
  sched.start();
  await sched.waitTicks(1);
  let t = (await motion(cams, 0, true)).t;
  c.ok('motion on before the restarts',
    (await waitPublished(rt, 'events.cameras.motionStart', 1, t)) !== null);
  const st0 = await stats(cams);
  const reqMark = rt.requests.length;

  const restarts = [];
  const restart = async (name, pauseMs) => {
    const tStop = Date.now();
    const stopped = await rec.stop();
    if (pauseMs) await sleep(pauseMs);
    rec = await startRecorder(cams, { stateDir: sd });
    restarts.push({ name, shutdown_ms: stopped.shutdownMs, total_ms: rec.readyAt - tStop });
  };
  // Between two polls.
  await sched.waitTicks(1);
  await sleep(1000);
  await restart('between_polls', 0);
  await sched.waitTicks(2);
  // While Protect's PullMessages is held (500 ms into the next poll).
  await sched.waitTicks(1);
  const last = sched.ticks[sched.ticks.length - 1];
  await sleep(Math.max(0, last.t + sched.pollMs + 500 - Date.now()));
  await restart('during_held_pull', 0);
  await sched.waitTicks(2);
  // A slow restart: nothing listens for 2.5 s.  Protect's Renew is not
  // due in the gap (as if it had just renewed).
  await sched.waitTicks(1);
  sub.terminationTime = Date.now() + 55000;
  await sleep(500);
  await restart('slow_2500ms', 2500);
  await sched.waitTicks(3);
  const errorsBefore = maxErrors;
  // The same with Protect's Renew due in the gap: Protect then tries a new
  // subscription, at the recorder (down) and at the camera (one slot,
  // held by the recorder's kept subscription).
  await sched.waitTicks(1);
  sub.terminationTime = Date.now() + 5000;
  await sleep(500);
  await restart('slow_2500ms_renew_due', 2500);
  await sched.waitTicks(3);
  c.num('max_consecutive_errors_before_renew_due', errorsBefore);
  sub.terminationTime = Date.now() + 5000;
  await sched.waitTicks(2);
  t = (await motion(cams, 0, false)).t;
  const stopMs = await waitPublished(rt, 'events.cameras.motionStop', 1, t);
  c.num('motion_stop_latency_ms_after_restarts', stopMs);
  await sched.waitTicks(1);
  await sched.stop();

  c.num('restarts', restarts);
  c.ok('each restart is back within about 3 s', restarts.every((r) => r.total_ms <= 3500), restarts);
  const st1 = await stats(cams);
  const after = rt.requests.slice(reqMark).filter((r) => EVENT_OPS.has(r.op) || r.path.endsWith('/status'));
  const failed = after.filter((r) => r.error || r.status !== 200);
  c.num('failed_requests', brief(failed));
  c.ok('motion stop after the restarts reaches Protect', stopMs !== null &&
    count(rt, 'events.cameras.motionStop') === 1, count(rt, 'events.cameras.motionStop'));
  c.ok('Protect keeps the same subscription (adopted by each new recorder)',
    sub === ctx.eventSubscriptions.get(cam.mac) && sub.subscriptionPath === path0);
  c.ok('Protect\'s Renew after the restarts is accepted',
    after.some((r) => r.op === 'Renew' && r.host === RECORDER && r.status === 200),
    brief(after.filter((r) => r.op === 'Renew')));
  const hc = await hubCamera(rec, key);
  c.ok('the last recorder adopted it', hc.pull_points.length === 1 &&
    hc.pull_points[0].adopted === true && hc.pull_points[0].state === 'serving', hc.pull_points);
  const recorderCreates = (st) => st.by['recorder.CreatePullPointSubscription'] || 0;
  c.ok('the recorder makes no new Create at the camera (resumed each time)',
    recorderCreates(st1) === recorderCreates(st0) && st1.live === 1, { before: st0, after: st1 });
  // A Renew that falls due while nothing listens makes Protect try a new
  // subscription at the camera, which a one-slot camera refuses.
  c.num('protect_direct_creates', st1.by['protect.CreatePullPointSubscription'] || 0);
  c.num('camera_refused_creates', st1.refused);
  c.ok('each resumed subscription asked for current state once',
    st1.sync_points === st0.sync_points + restarts.length, st1.sync_points);
  const w = warns(rt);
  c.ok('no Deactivated and no circuit breaker in Protect',
    !w.some((x) => /Deactivated|circuit-breaker/.test(x)), w);
  c.ok('no warn logs from Protect', w.length === 0, w);
  c.ok('Protect\'s error count stays below its limit', maxErrors < 3, maxErrors);
  c.num('max_consecutive_errors', maxErrors);
  c.num('recorder_warnings', recorderWarnings(rec));
}

// E: no recorder running.
async function scenarioE(c) {
  c.ok('nothing listens on the PullPoint port', !(await portInUse(port)));
  const cams = await startCameras({ slots: 1 });
  const rt = newProtect();
  const cam = cameraRow(cams, 0, 'camE', 'AABBCCDD0005');
  const ctx = makeContext(rt, [cam]);
  const conn = await rt.handlers['onvif.getConnection'](cam);
  const t = Date.now();
  let r;
  try {
    r = await rt.m.events.createPullPointSubscription(conn);
  } catch (e) {
    r = { error: e.message };
  }
  const ms = Date.now() - t;
  c.num('create_fallback_ms', ms);
  c.ok('Create falls back to the camera quickly', r.subscriptionPath &&
    !r.subscriptionPath.startsWith('/onvif-recorder/') && ms < 500, { ms, r });
  if (r.subscriptionPath) await rt.m.events.unsubscribe(conn, r.subscriptionPath, r.referenceParameters);
  const t2 = Date.now();
  await rt.m.reconcile.reconcileMotionState(ctx, cam.pk);
  await settle(ctx);
  c.num('reconcile_ms', Date.now() - t2);
  c.ok('reconcile subscribes at the camera', where(ctx, cam.mac) === 'CAMERA');
  c.ok('no warn logs from Protect', warns(rt).length === 0, warns(rt));
  c.num('connect_errors', brief(rt.requests.filter((r) => r.host === RECORDER)));
}

// F: a recorder that accepts connections and never answers.
async function scenarioF(c) {
  // F1: Create gives up on the recorder after 4 s.
  {
    const hung = await hungServer(port);
    const cams = await startCameras({ slots: 2 });
    const rt = newProtect();
    const cam = cameraRow(cams, 0, 'camF', 'AABBCCDD0006');
    makeContext(rt, [cam]);
    const conn = await rt.handlers['onvif.getConnection'](cam);
    const t = Date.now();
    let r;
    try {
      r = await rt.m.events.createPullPointSubscription(conn);
    } catch (e) {
      r = { error: e.message };
    }
    const ms = Date.now() - t;
    c.num('hung_create_fallback_ms', ms);
    c.ok('Create falls back to the camera after about 4 s', r.subscriptionPath &&
      !r.subscriptionPath.startsWith('/onvif-recorder/') && ms >= 3900 && ms < 5500, { ms, r });
    if (r.subscriptionPath) await rt.m.events.unsubscribe(conn, r.subscriptionPath, r.referenceParameters);
    await cams.stop();
    await hung.close();
  }
  // F2: one proxied and one direct camera; the recorder hangs.  F3: the
  // same with the proxied subscription due for renewal when it hangs.  F4:
  // the proxied camera has one slot, which the recorder's kept camera
  // subscription holds, and Protect's Renew falls due right after its
  // second failed pull, so one poll renews, re-creates (recorder, then the
  // camera, which refuses) and deactivates (Unsubscribe at the recorder).
  const runs = [
    ['F2', {}],
    ['F3', { renewDue: true }],
    ['F4', { slots: 1, renewAfterErrors: 2 }],
  ];
  for (const [tag, opts] of runs) {
    const r = await hungRun(opts);
    c.num(`${tag}_baseline_tick_ms`, r.baseline);
    c.num(`${tag}_hung_tick_ms`, r.hung);
    c.num(`${tag}_direct_camera_pulls_in_40s`, r.bPulls);
    c.num(`${tag}_camera_a_after`, r.aAfter);
    c.num(`${tag}_protect_warnings_while_hung`, r.warnings);
    c.num(`${tag}_slow_tick_requests`, r.slowTickRequests);
    const maxTick = Math.max(...r.hung);
    c.ok(`${tag}: with the recorder hung, every poll tick stays under ~6 s`, maxTick < 6000, r.hung);
    c.ok(`${tag}: the direct camera keeps being polled`, r.bPulls >= Math.floor(40000 / 5000) - 1,
      r.bPulls);
    // With one slot held by the recorder's kept subscription, the camera
    // refuses Protect until that subscription expires (up to 120 s).
    if (opts.slots !== 1) {
      c.ok(`${tag}: camera a ends up subscribed at the camera`, r.aAfter === 'CAMERA', r.aAfter);
    }
  }
}

async function hungRun({ renewDue = false, slots = 2, renewAfterErrors = 0 } = {}) {
  const cams = await startCameras({ slots, second: 64 });
  const rec = await startRecorder(cams, { share: '0', stateDir: stateDir() });
  await waitUp(rec, cams.info.cameras[0].key);
  const rt = newProtect();
  const a = cameraRow(cams, 0, 'a', 'AAAAAAAAAAAA');
  const b = cameraRow(cams, 1, 'b', 'BBBBBBBBBBBB');
  const ctx = makeContext(rt, [a, b]);
  await rt.m.reconcile.reconcileMotionState(ctx, a.pk);
  await rt.m.reconcile.reconcileMotionState(ctx, b.pk);
  await settle(ctx);
  if (where(ctx, a.mac) !== 'PROXY' || where(ctx, b.mac) !== 'CAMERA') {
    throw new Error(`setup: a ${where(ctx, a.mac)}, b ${where(ctx, b.mac)}`);
  }
  let hungNow = false;
  let renewForced = false;
  const sched = new Scheduler(rt, ctx, {
    onTick: () => {
      const s = ctx.eventSubscriptions.get(a.mac);
      if (hungNow && renewAfterErrors && !renewForced && s &&
          s.consecutiveErrors === renewAfterErrors) {
        renewForced = true;
        s.terminationTime = Date.now() + 5000;
      }
    },
  });
  sched.start();
  await sleep(15000);
  await sched.waitTicks(1);
  const baseTicks = sched.ticks.length;
  const markB = (await stats(cams, 1)).requests;
  await rec.stop();
  const hung = await hungServer(port);
  hungNow = true;
  if (renewDue) ctx.eventSubscriptions.get(a.mac).terminationTime = Date.now() + 5000;
  const L = rt.logs.length;
  const R = rt.requests.length;
  const tHung = Date.now();
  await sleep(40000);
  await sched.stop();
  const hungTicks = sched.ticks.slice(baseTicks);
  const bPulls = (await requestsSince(cams, 1, markB))
    .filter((r) => r.from === 'protect' && r.op === 'PullMessages' && r.t >= tHung).length;
  const slow = hungTicks.find((x) => x.ms >= 6000);
  const slowTickRequests = slow ? brief(rt.requests.slice(R).filter((q) =>
    q.t >= slow.t && q.t < slow.t + slow.ms && q.host === RECORDER)) : [];
  const out = {
    baseline: sched.ticks.slice(0, baseTicks).map((x) => x.ms),
    hung: hungTicks.map((x) => x.ms),
    bPulls,
    aAfter: where(ctx, a.mac),
    warnings: warns(rt, L),
    slowTickRequests,
  };
  await hung.close();
  await killAll();
  return out;
}

// G: Protect stops its subscription and starts again (a Protect restart).
async function scenarioG(c) {
  const cams = await startCameras({ slots: 1 });
  const rec = await startRecorder(cams, { stateDir: stateDir() });
  const key = cams.info.cameras[0].key;
  await waitUp(rec, key);
  const mark = (await stats(cams)).requests;

  const rt1 = newProtect();
  const cam = cameraRow(cams, 0, 'camG', 'AABBCCDD0007');
  const ctx1 = makeContext(rt1, [cam]);
  await rt1.m.reconcile.reconcileMotionState(ctx1, cam.pk);
  await settle(ctx1);
  c.ok('first Protect subscribed via the recorder', where(ctx1, cam.mac) === 'PROXY');
  const s1 = new Scheduler(rt1, ctx1);
  s1.start();
  await s1.waitTicks(1);
  const t = (await motion(cams, 0, true)).t;
  c.ok('motion on reaches the first Protect',
    (await waitPublished(rt1, 'events.cameras.motionStart', 1, t)) !== null);
  await s1.stop();
  const oldPath = ctx1.eventSubscriptions.get(cam.mac).subscriptionPath;
  await rt1.m.stop.stopExistingSubscription(ctx1, cam.mac);
  const unsub = rt1.requests.filter((r) => r.op === 'Unsubscribe').pop();
  c.ok('Protect\'s Unsubscribe goes to the recorder', unsub && unsub.host === RECORDER &&
    unsub.status === 200, unsub);
  const conn = await rt1.handlers['onvif.getConnection'](cam);
  let oldPull;
  try {
    await rt1.m.events.pullMessages(conn, oldPath, 1, 10);
    oldPull = 'ok';
  } catch (e) {
    oldPull = e.message;
  }
  c.ok('the unsubscribed pull point faults', /unknown subscription/.test(oldPull), oldPull);

  const rt2 = newProtect();
  const ctx2 = makeContext(rt2, [cam]);
  const t2 = Date.now();
  await rt2.m.reconcile.reconcileMotionState(ctx2, cam.pk);
  await settle(ctx2);
  c.ok('the restarted Protect subscribes via the recorder again', where(ctx2, cam.mac) === 'PROXY' &&
    ctx2.eventSubscriptions.get(cam.mac).subscriptionPath !== oldPath);
  const s2 = new Scheduler(rt2, ctx2);
  s2.start();
  const initMs = await waitPublished(rt2, 'events.cameras.motionStart', 1, t2, 15000);
  c.num('current_state_latency_ms', initMs);
  c.ok('the restarted Protect gets the current state (motion on) at once', initMs !== null, initMs);
  const t3 = (await motion(cams, 0, false)).t;
  c.num('motion_stop_latency_ms', await waitPublished(rt2, 'events.cameras.motionStop', 1, t3));
  await s2.waitTicks(1);
  await s2.stop();
  const st = await stats(cams);
  const protectOps = (await requestsSince(cams, 0, mark)).filter((r) => r.from === 'protect')
    .map((r) => r.op);
  c.ok('the camera kept one subscription, the recorder\'s',
    st.creates === 1 && st.live === 1 && st.refused === 0, st);
  c.ok('the camera only sees Protect\'s GetEventProperties',
    protectOps.every((o) => o === 'GetEventProperties'), protectOps);
  const snapLines = rec.logLines(/Protect subscribed via recorder/);
  c.ok('the recorder replayed the current state to the new subscription',
    snapLines.length === 2 && /snapshot=[1-9]/.test(snapLines[1]), snapLines.map((l) => l.slice(-80)));
  c.ok('no warn logs from either Protect', warns(rt1).length === 0 && warns(rt2).length === 0,
    [...warns(rt1), ...warns(rt2)]);
  c.num('recorder_warnings', recorderWarnings(rec));
}

// H: the recorder stalls once.  A relay on the PullPoint port forwards to
// the recorder but holds every connection that arrives in a 4.6 s window
// starting just before a poll, so one Protect pull times out (4 s).
async function scenarioH(c) {
  const net = require('net');
  const cams = await startCameras({ slots: 1 });
  const inner = port + 1;
  if (await portInUse(inner)) throw new Error(`127.0.0.1:${inner} is in use`);
  const rec = await startRecorder(cams, { stateDir: stateDir(), recPort: inner });
  await waitUp(rec, cams.info.cameras[0].key);
  let stallUntil = 0;
  const socks = new Set();
  const relay = net.createServer((cl) => {
    socks.add(cl);
    cl.on('close', () => socks.delete(cl));
    cl.on('error', () => {});
    if (Date.now() < stallUntil) {
      cl.on('data', () => {});
      return;
    }
    const up = net.connect(inner, '127.0.0.1');
    socks.add(up);
    up.on('close', () => socks.delete(up));
    up.on('error', () => cl.destroy());
    cl.pipe(up);
    up.pipe(cl);
  });
  await new Promise((resolve, reject) => {
    relay.once('error', reject);
    relay.listen(port, '127.0.0.1', resolve);
  });
  try {
    const rt = newProtect();
    const cam = cameraRow(cams, 0, 'camH', 'AABBCCDD0008');
    const ctx = makeContext(rt, [cam]);
    await rt.m.reconcile.reconcileMotionState(ctx, cam.pk);
    await settle(ctx);
    c.ok('subscribed via the recorder', where(ctx, cam.mac) === 'PROXY');
    const errors = [];
    const sched = new Scheduler(rt, ctx, {
      onTick: () => {
        const s = ctx.eventSubscriptions.get(cam.mac);
        errors.push(s ? s.consecutiveErrors : 'gone');
      },
    });
    sched.start();
    await sched.waitTicks(2);
    const last = sched.ticks[sched.ticks.length - 1];
    await sleep(Math.max(0, last.t + sched.pollMs - 200 - Date.now()));
    stallUntil = Date.now() + 4600;
    await sleep(30000);
    await sched.stop();
    c.num('errors_after_each_tick', errors);
    c.num('tick_ms', sched.ticks.map((x) => x.ms));
    const w = warns(rt);
    c.ok('the stall cost Protect one failed pull', errors.includes(1) && Math.max(
      ...errors.filter((e) => typeof e === 'number')) === 1, errors);
    c.ok('no Deactivated and no circuit breaker in Protect',
      !w.some((x) => /Deactivated|circuit-breaker/.test(x)), w);
    c.ok('Protect stays subscribed via the recorder', where(ctx, cam.mac) === 'PROXY',
      where(ctx, cam.mac));
    c.num('recorder_warnings', recorderWarnings(rec));
  } finally {
    for (const sk of socks) sk.destroy();
    await new Promise((r) => relay.close(() => r()));
  }
}

const SCENARIOS = { A: scenarioA, B: scenarioB, C: scenarioC, D: scenarioD, E: scenarioE,
                    F: scenarioF, G: scenarioG, H: scenarioH };

async function main() {
  if (await portInUse(port)) {
    console.error(`127.0.0.1:${port} is in use; stop it or pass --port`);
    process.exit(1);
  }
  const results = { label, bundle: args.bundle, patch: patchReport, port, started: new Date().toISOString(),
                    scenarios: {} };
  for (const [name, fn] of Object.entries(SCENARIOS)) {
    if (only && !only.has(name)) continue;
    scenarioName = name;
    runtimes = [];
    const c = new Checks();
    const t0 = Date.now();
    console.log(`[${label}] scenario ${name} ...`);
    try {
      await fn(c);
    } catch (e) {
      c.ok('scenario ran to the end', false, String(e && e.stack || e));
    }
    protectLogChecks(c);
    await killAll();
    await sleep(300);
    results.scenarios[name] = { ...c.result(), seconds: Math.round((Date.now() - t0) / 100) / 10 };
    console.log(`[${label}] scenario ${name}: ${results.scenarios[name].pass ? 'PASS' : 'FAIL'}` +
      ` (${results.scenarios[name].seconds}s)`);
  }
  results.pass = Object.values(results.scenarios).every((s) => s.pass);
  fs.writeFileSync(outFile, JSON.stringify(results, null, 1));
  console.log(`[${label}] ${results.pass ? 'ALL PASS' : 'FAILURES'}; results in ${outFile}`);
  process.exit(results.pass ? 0 : 1);
}

for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
  process.on(sig, () => {
    killAllNow();
    process.exit(130);
  });
}
process.on('exit', killAllNow);
main().catch(async (e) => {
  console.error(e);
  await killAll();
  process.exit(1);
});
