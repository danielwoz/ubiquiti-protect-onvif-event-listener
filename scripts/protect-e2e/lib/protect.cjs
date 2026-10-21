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

// Protect's ONVIF event-subscription runtime, built from a service.js
// bundle: Protect's own soap transport, onvif events, reconcile, poll,
// replace-subscription, stop-subscription, motion-probe and constants
// modules run for real, with real undici, fast-xml-parser, node-cache and
// p-queue.  The store, the message bus, the logger and the analytics / IO
// probes are stubs that record what Protect does.  Protect's own poll and
// reconcile timers are not loaded; Scheduler drives the same functions on
// Protect's intervals.
'use strict';

const path = require('path');
const { Bundle } = require('./loader.cjs');

const CAPABILITY = {
  Available: 'available',
  Misconfigured: 'misconfigured',
  Unsupported: 'unsupported',
};

// Exported names that identify each module Protect's event subscriptions
// use (searched for in the bundle; each must occur once).  Modules not in
// REQUIRED are absent from some versions (7.1 has no digital input
// support) and are stubbed there.
const REQUIRED = new Set(['soap', 'events', 'poll', 'replace', 'stop', 'reconcile', 'probe',
                          'consts']);
const NEEDLES = {
  soap: 't.connSoapRequest=(e,r,o,i,a,n)=>',
  events: 't.pullMessages=async(e,t,r,o,i)=>',
  poll: 't.pollMotionEvents=async',
  replace: 't.replaceSubscription=async',
  stop: 't.stopExistingSubscription=async',
  reconcile: 't.reconcileMotionState=async',
  probe: 't.probeMotionState=async',
  consts: 't.PULL_MESSAGES_TIMEOUT_S=2',
  probeState: 't.getOrCreateProbeState=(e,r)=>',
  motionPayload: 't.buildMotionPayload=e=>',
  inputPayload: 't.buildDigitalInputPayload=(e,t)=>',
};

function opOf(body) {
  const m = /<(?:[\w-]+:)?Body[^>]*>\s*<(?:[\w-]+:)?(\w+)/.exec(String(body || ''));
  return m ? m[1] : '';
}

// undici with every request Protect makes recorded in @p log.
function recordingUndici(real, log) {
  return {
    ...real,
    request: async (url, opts = {}) => {
      const u = new URL(url);
      const entry = {
        t: Date.now(),
        host: u.host,
        path: u.pathname + u.search,
        op: opOf(opts.body),
        wsaAction: /Action[^>]*>[^<]*</.test(String(opts.body || '')),
      };
      log.push(entry);
      try {
        const r = await real.request(url, opts);
        entry.status = r.statusCode;
        entry.ms = Date.now() - entry.t;
        return r;
      } catch (e) {
        entry.error = (e && (e.code || e.name)) || String(e);
        entry.ms = Date.now() - entry.t;
        throw e;
      }
    },
  };
}

function formatArg(x) {
  if (x instanceof Error) return `${x.name}: ${x.message}${x.code ? ` [${x.code}]` : ''}`;
  return String(x);
}

// @returns {m, logs, published, cameraStates, requests, bundle}
function buildProtect(text, { nmDir, fxpDir }) {
  const logs = [];
  const published = [];
  const cameraStates = [];
  const requests = [];
  const handlers = {};
  const logger = (name) => {
    const at = (lvl) => (...a) =>
      logs.push({ t: Date.now(), logger: name, lvl, msg: a.map(formatArg).join(' ') });
    return { debug: at('debug'), info: at('info'), warn: at('warn'), error: at('error') };
  };
  const stubs = {
    getLogger: logger,
    publish: async (topic, ...args) => {
      published.push({ t: Date.now(), topic, args });
      const h = handlers[topic];
      return h ? h(...args) : undefined;
    },
    isNoSubscriberError: () => false,
    CameraDecalModel: { modelKey: 'camera' },
    ManagedTraceId: class {
      constructor(o) {
        this.o = o;
      }
      error(...a) {
        logs.push({ t: Date.now(), logger: 'trace', lvl: 'error', msg: a.map(formatArg).join(' ') });
      }
    },
    createSourceTag: (x) => ({ source: x }),
    createReasonTag: (x) => ({ reason: x }),
    createCameraTag: (x) => ({ camera: x }),
    SourceTags: { Camera: 'camera' },
    IoCapabilityState: CAPABILITY,
    MotionDetectionState: CAPABILITY,
    UnknownError: class UnknownError extends Error {},
    getMotionAnalyticsState: async () => ({ kind: 'unknown', reason: 'stub' }),
    summarizeMotionAnalyticsState: () => 'analytics=stub',
    probeDigitalInputState: async () => ({ state: null, tokens: [] }),
    probeRelayOutputState: async () => ({ state: null, tokens: [] }),
    writeCameraState: async (cam, d) => {
      cameraStates.push(d.kind);
    },
    writeIoCameraState: async () => {},
    RelayInputState: { CIRCUIT_CLOSED: 'closed', CIRCUIT_OPEN: 'open' },
  };
  const undici = recordingUndici(require(path.join(nmDir, 'undici')), requests);
  const bundle = new Bundle(text, { nmDir, fxpDir, stubs, packages: { undici } });
  const ids = {};
  for (const [k, needle] of Object.entries(NEEDLES)) {
    try {
      ids[k] = bundle.idOf(needle);
    } catch (e) {
      if (REQUIRED.has(k)) throw e;
    }
  }
  bundle.markReal(...Object.values(ids));
  const m = {};
  for (const [k, id] of Object.entries(ids)) m[k] = bundle.require(id);
  return { m, ids, logs, published, cameraStates, requests, handlers, bundle };
}

// A third-party camera row as Protect's store holds it.
function makeCamera({ host, port, pk, mac }) {
  return {
    pk,
    mac,
    host,
    logId: `${pk} [${mac} @ ${host}:${port}]`,
    isThirdPartyCamera: true,
    isAdopted: true,
    isPairedWithAiPort: false,
    isDestroyed: false,
    recordingSettings: { enableMotionDetection: true },
    thirdPartyCameraInfo: {
      digitalInput: 'unsupported',
      relayOutput: 'unsupported',
      motionDetection: 'available',
      port,
      username: 'admin',
      password: 'password',
    },
    featureFlags: { hasMotionDetection: true },
  };
}

// The state Protect's ONVIF event code keeps (ctx), with the connection
// lookup answered from the camera rows.
function makeContext(rt, cams) {
  const byPk = new Map(cams.map((c) => [c.pk, c]));
  const ctx = {
    store: { findByPk: (k, id) => byPk.get(id) || null, filter: () => [...byPk.values()] },
    eventSubscriptions: new Map(),
    inFlightStarts: new Set(),
    probeState: new Map(),
    connectionCache: new Map(),
    cameras: cams,
  };
  rt.handlers['onvif.getConnection'] = async (cam) => {
    let c = ctx.connectionCache.get(cam.mac);
    if (!c) {
      c = {
        hostname: cam.host,
        port: String(cam.thirdPartyCameraInfo.port),
        username: cam.thirdPartyCameraInfo.username,
        password: cam.thirdPartyCameraInfo.password,
        timeShift: 0,
        devicePath: '/onvif/device_service',
        mediaPath: '/onvif/media_service',
        ptzPath: null,
        eventServicePath: '/onvif/event_service',
        analyticsServicePath: null,
        deviceIoPath: null,
        media2: false,
        profiles: [],
        defaultProfileToken: null,
      };
      ctx.connectionCache.set(cam.mac, c);
    }
    return c;
  };
  rt.handlers['onvif.clearConnection'] = async (mac) => {
    ctx.connectionCache.delete(mac);
  };
  return ctx;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Wait until no subscription start is in flight (reconciles started in the
// background by Protect after a deactivation included).
async function settle(ctx, limitMs = 20000) {
  const end = Date.now() + limitMs;
  while (Date.now() < end) {
    await sleep(10);
    if (ctx.inFlightStarts.size === 0) {
      await sleep(30);
      if (ctx.inFlightStarts.size === 0) return;
    }
  }
}

// Protect's two event timers: the poll task every POLL_TASK_INTERVAL_MS,
// which never overlaps itself (a run that overruns skips the ticks it
// covered), and the reconcile task every RECONCILE_INTERVAL_MS.
class Scheduler {
  constructor(rt, ctx, { pollMs, reconcileMs, onTick } = {}) {
    this.rt = rt;
    this.ctx = ctx;
    this.pollMs = pollMs || rt.m.consts.POLL_TASK_INTERVAL_MS;
    this.reconcileMs = reconcileMs || rt.m.consts.RECONCILE_INTERVAL_MS;
    this.onTick = onTick;
    this.ticks = [];
    this.waiters = [];
    this.running = false;
  }

  start() {
    if (this.running) return;
    this.running = true;
    this.loopDone = this.loop();
    this.reconcileTimer = setInterval(() => {
      for (const cam of this.ctx.cameras) {
        this.rt.m.reconcile.reconcileMotionState(this.ctx, cam.pk).catch(() => {});
      }
    }, this.reconcileMs);
  }

  async loop() {
    while (this.running) {
      const start = Date.now();
      await this.rt.m.poll.pollMotionEvents(this.ctx);
      const ms = Date.now() - start;
      this.ticks.push({ t: start, ms });
      if (this.onTick) this.onTick(this.ticks.length, ms);
      for (const w of this.waiters.splice(0)) w();
      const next = start + this.pollMs * Math.max(1, Math.ceil(ms / this.pollMs));
      await sleep(Math.max(0, next - Date.now()));
    }
  }

  // Resolves after @p n more poll ticks.
  async waitTicks(n = 1) {
    for (let i = 0; i < n; i++) await new Promise((r) => this.waiters.push(r));
  }

  async stop() {
    this.running = false;
    clearInterval(this.reconcileTimer);
    if (this.loopDone) await this.loopDone;
    await settle(this.ctx);
  }
}

module.exports = { buildProtect, makeCamera, makeContext, settle, sleep, Scheduler, opOf };
