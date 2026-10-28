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

// Runs UniFi Protect's own third-party camera code against a camera, before the camera is
// adopted on a real console: the adoption probe (getCameraDetails, getStreamData, getMAC),
// the imaging, encoder-options and deviceIO probes, the snapshot fetch, and the ONVIF event
// client (GetEventProperties, CreatePullPointSubscription, PullMessages, Renew,
// Unsubscribe).  The modules come from the Protect service.js passed with --bundle and run
// with Protect's npm packages (--nm); logging and storage are stubs.
//
// With --control (MockCam's control API) it also makes the camera emit motion and digital
// input events and checks that Protect's pull loop sees them, and runs the knob scenarios
// named with --scenarios (digest, media2, wsa, refparams, slots).
//
// usage:
//   node tools/mock-camera/preflight/protect-preflight.cjs --bundle=<service.js>
//       --nm=<node_modules> --host=192.168.3.216 [--port=80] --cred=<file with user=/password=>
//       [--control=http://192.168.3.216:8080] [--scenarios=digest,media2,wsa,refparams,slots]
//       [--out=<results.json>]
'use strict';

const fs = require('fs');
const path = require('path');
const { Bundle } = require('../../../scripts/protect-e2e/lib/loader.cjs');

const args = Object.fromEntries(process.argv.slice(2).map((a) => {
  const m = /^--([^=]+)(?:=(.*))?$/.exec(a);
  if (!m) throw new Error(`bad argument ${a}`);
  return [m[1], m[2] === undefined ? '1' : m[2]];
}));
for (const k of ['bundle', 'nm', 'host', 'cred']) {
  if (!args[k]) {
    console.error(fs.readFileSync(__filename, 'utf8').split('\n')
      .filter((l) => l.startsWith('//')).slice(13).join('\n'));
    process.exit(2);
  }
}
const HOST = args.host;
const PORT = args.port || '80';
const NM = path.resolve(args.nm);
const CONTROL = args.control || '';
const SCENARIOS = (args.scenarios || '').split(',').filter(Boolean);
const cred = Object.fromEntries(fs.readFileSync(args.cred, 'utf8').split('\n')
  .filter((l) => l.includes('=')).map((l) => [l.slice(0, l.indexOf('=')).trim(),
    l.slice(l.indexOf('=') + 1).trim()]));
const USER = cred.user;
const PASSWORD = cred.password;

// Exported names that identify each Protect module (each must occur once in the bundle).
const NEEDLES = {
  soap: 't.isNetworkError=e=>',
  timeShift: 't.getTimeShift=async e=>',
  servicePaths: 't.getServicePaths=async e=>',
  profiles: 't.enrichMedia2Profiles=async(e,t)=>',
  deviceInfo: 't.getDeviceInformation=async',
  snapshotUri: 't.getSnapshotUri=async(e,t)=>',
  streamUri: 't.getStreamUri=async(e,t)=>',
  connection: 't.createOnvifConnection=async',
  details: 't.normalizeBitrate=e=>',
  netIfaces: 't.getNetworkInterfaces=async e=>',
  validators: 't.createApiParserAsync=(e,t)=>',
  result: 't.failure=e=>',
  url: 't.replaceUrlHostname=(e,t)=>',
  classify: 't.classifyOnvifConnectionError=e=>',
  streamData: 't.getStreamData=e=>',
  ladder: 't.getOnvifCameraStreams=e=>',
  codecLabel: '(t.VideoCodecLabel=',
  imagingModes: 't.isUsableRange=e=>',
  encodings: 't.isSupportedOnvifEncoding=e=>',
  mac: 't.getMAC=async(e,t)=>',
  imaging: 't.parseSourceOptions=e=>',
  imagingUtil: 't.str=e=>',
  imagingSettings: 't.readImagingSettings=async(e,t)=>',
  encoderOptions: 't.normalizeEncoderOptions=e=>',
  nodes: 't.first=e=>',
  deviceIo: 't.isUnsupportedFault=e=>',
  ioProbe: 't.probeDigitalInputState=e=>',
  events: 't.pullMessages=async(e,t,r,o,i)=>',
  snapshot: 't.snapshot=async(e,t)=>',
  assert: 'ok:(e,t)=>{if(!e)throw t instanceof Error?t:new Error(t)}',
  analytics: 't.summarizeMotionAnalyticsState=e=>',
};

const enumOf = (...v) => Object.fromEntries(v.map((x) => [x, x]));
const logs = [];
const logger = (name) => {
  const at = (lvl) => (...a) => logs.push({ t: Date.now(), logger: name, lvl,
    msg: a.map((x) => (x instanceof Error ? `${x.name}: ${x.message}` :
      typeof x === 'object' ? JSON.stringify(x) : String(x))).join(' ') });
  return { debug: at('debug'), info: at('info'), warn: at('warn'), error: at('error'),
    silly: at('silly') };
};
const stubs = {
  getLogger: logger,
  OnvifError: enumOf('SnapshotNotSupported', 'EncoderConfigurationProblem',
    'AuthorizationError', 'DeviceLocked', 'ImagingConfigurationProblem',
    'OperatorAuthorizationRequired'),
  MotionDetectionState: { Available: 'available', Misconfigured: 'misconfigured',
    Unsupported: 'unsupported' },
  IoCapabilityState: { Available: 'available', Misconfigured: 'misconfigured',
    Unsupported: 'unsupported' },
  isValidMAC: (m) => /^(?:([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}|[0-9a-fA-F]{12})$/.test(m),
  normalizeMAC: (m) => String(m).replace(/[^0-9a-fA-F]/g, '').toUpperCase(),
  DEFAULT_SNAPSHOT_REQUEST_TIMEOUT: 60000,
  // Validation failures (the error classes are not loaded from the bundle).
  AjvParseError: class AjvParseError extends Error {
    constructor(o) {
      super(`validation failed for ${o.info.entity}: ${JSON.stringify(o.info.issues)}`);
      this.info = o.info;
    }
  },
  ZodParseError: class ZodParseError extends Error {
    constructor(o) {
      super(`validation failed for ${o.info.entity}: ${JSON.stringify(o.info.issues)}`);
      this.info = o.info;
    }
  },
  UnknownError: class UnknownError extends Error {},
  conciseAjvErrors: (errs) => (errs || []).map((e) => `${e.instancePath} ${e.message}`),
};
// Resolves through each package's "exports", as Protect's own require does.
const pkg = require('module').createRequire(path.join(NM, '..', 'package.json'));
const bundle = new Bundle(fs.readFileSync(args.bundle, 'utf8'), {
  nmDir: NM,
  fxpDir: path.join(NM, 'fast-xml-parser'),
  stubs,
  packages: {
    ajv: pkg('ajv'), typebox: pkg('typebox'), zod: pkg('zod'),
    'zod-to-json-schema': pkg('zod-to-json-schema'), 'digest-fetch': pkg('digest-fetch'),
    'node:https': require('https'),
  },
});
const ids = {};
for (const [k, needle] of Object.entries(NEEDLES)) ids[k] = bundle.idOf(needle);
bundle.markReal(...Object.values(ids));
const P = {};
for (const [k, id] of Object.entries(ids)) P[k] = bundle.require(id);

const results = { host: `${HOST}:${PORT}`, bundle: path.basename(args.bundle), checks: [] };
let failed = 0;
function check(name, ok, detail) {
  results.checks.push({ name, ok: !!ok, detail });
  if (!ok) failed++;
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== undefined ?
    `: ${typeof detail === 'string' ? detail : JSON.stringify(detail)}` : ''}`);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function control(method, p, body) {
  const r = await fetch(CONTROL + p, { method, headers: { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body) });
  if (!r.ok) throw new Error(`control ${p}: ${r.status} ${await r.text()}`);
  return r.json();
}

// Pulls (as Protect's poll loop does: PT2S, limit 10) until @p pred matches or @p ms pass.
async function pullUntil(conn, sub, pred, ms = 10000) {
  const seen = [];
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const evs = await P.events.pullMessages(conn, sub.subscriptionPath, 2, 10,
      sub.referenceParameters);
    seen.push(...evs);
    const hit = evs.find(pred);
    if (hit) return { hit, seen };
  }
  return { hit: null, seen };
}

async function adoption(label) {
  const details = await P.details.getCameraDetails(HOST, PORT, USER, PASSWORD,
    { fetchHwAddress: true });
  check(`${label}: getCameraDetails success`, details.success, details.errors);
  if (!details.success) return null;
  check(`${label}: name`, details.name === 'MockCam ONVIF-SIM-1', details.name);
  check(`${label}: no adoption errors`, details.errors.length === 0, details.errors);
  check(`${label}: two streams`, details.streams.length === 2,
    details.streams.map((s) => `${s.profileToken} ${s.resolution.width}x${s.resolution.height}` +
      ` ${s.encoding} ${s.fps}fps ${s.bitrate}kbps ${s.uri}`));
  check(`${label}: hwAddress`, details.hwAddress === '02:4D:4F:43:4B:01', details.hwAddress);
  check(`${label}: snapshotUri`, /\/snapshot\.jpg$/.test(details.snapshotUri || ''),
    details.snapshotUri);
  const sd = P.streamData.getStreamData(details);
  check(`${label}: getStreamData ok`, sd.ok && sd.encoding === 'h264' &&
    sd.hqStream.resolution.width === 1280 && sd.lqStream.resolution.width === 848,
    sd.ok ? { encoding: sd.encoding, hq: sd.hqStream.uri, lq: sd.lqStream.uri,
      channels: sd.channelSources } : sd);
  const mac = await P.mac.getMAC(HOST, details);
  check(`${label}: getMAC`, mac === '024D4F434B01', mac);
  return { details, sd };
}

async function main() {
  const t0 = Date.now();
  if (CONTROL) await control('POST', '/mock/config/reset', {});

  // 1. Adoption: what POST /api/third-party-cameras/adopt runs.
  const ad = await adoption('adopt');
  if (!ad) throw new Error('adoption failed; later steps need it');
  const conn = await P.connection.createOnvifConnection({ hostname: HOST, port: PORT,
    username: USER, password: PASSWORD });
  check('connection: paths', conn.mediaPath === '/onvif/media_service' &&
    conn.eventServicePath === '/onvif/event_service' && !conn.media2,
  { media2: conn.media2, mediaPath: conn.mediaPath, events: conn.eventServicePath,
    deviceIo: conn.deviceIoPath, ptz: conn.ptzPath, analytics: conn.analyticsServicePath,
    timeShift: conn.timeShift });

  // 2. Imaging, encoder options, deviceIO and snapshot.
  const caps = await P.imaging.getImagingCapabilities(conn);
  check('imaging capabilities', caps && caps.servicePath === '/onvif/imaging_service', caps);
  if (caps) {
    const st = await P.imagingSettings.readImagingSettings(conn, caps);
    check('imaging settings read', st && typeof st === 'object', st);
  }
  for (const p of conn.profiles) {
    // Already normalised by Protect (normalizeEncoderOptions).
    const norm = await P.encoderOptions.getVideoEncoderOptions(conn, p.token);
    check(`encoder options ${p.token}`, norm.length === 1 && norm[0].resolutions.length > 0 &&
      norm[0].fps.includes(10), norm.map((o) => ({ enc: o.encoding, res: o.resolutions.map((r) =>
        `${r.width}x${r.height}`), fps: o.fps, bitrate: o.bitrateRange, gov: o.govRange })));
  }
  const di = await P.ioProbe.probeDigitalInputState(conn);
  const ro = await P.ioProbe.probeRelayOutputState(conn);
  check('deviceIO probes', di.state === 'available' && ro.state === 'available', { di, ro });
  const snap = await P.snapshot.snapshot({}, { camera: { host: HOST, thirdPartyCameraInfo: {
    snapshotUrl: ad.details.snapshotUri, username: USER, password: PASSWORD } } });
  check('snapshot (digest-fetch)', snap.content[0] === 0xff && snap.content[1] === 0xd8,
    `${snap.content.length} bytes`);
  const mstate = await P.analytics.getMotionAnalyticsState(conn);
  check('motion analytics state', !!mstate, mstate);

  // 3. Protect's event client.
  const props = await P.events.getEventProperties(conn);
  check('GetEventProperties', props.kind === 'known' && props.supportsPullPoint &&
    props.motionTopics.length > 0, props);
  const sub = await P.events.createPullPointSubscription(conn);
  const offset = sub.terminationTime - Date.now();
  check('CreatePullPointSubscription', sub.subscriptionPath.startsWith('/onvif/subscription') &&
    offset > 50000 && offset <= 61000, { path: sub.subscriptionPath, terminationInS:
    Math.round(offset / 1000), referenceParameters: sub.referenceParameters });
  const init = await P.events.pullMessages(conn, sub.subscriptionPath, 2, 10,
    sub.referenceParameters);
  check('initial pull (Initialized state)', init.some((e) => e.kind === 'motion' &&
    !e.isMotion), init);
  if (CONTROL) {
    await control('POST', '/mock/event', { kind: 'cell_motion', state: true });
    const on = await pullUntil(conn, sub, (e) => e.kind === 'motion' && e.isMotion);
    check('motion start pulled', !!on.hit, on.seen);
    await control('POST', '/mock/event', { kind: 'cell_motion', state: false });
    const off = await pullUntil(conn, sub, (e) => e.kind === 'motion' && !e.isMotion);
    check('motion stop pulled', !!off.hit, off.seen);
    await control('POST', '/mock/event', { kind: 'digital_input', state: true });
    const din = await pullUntil(conn, sub, (e) => e.kind === 'digitalInput' && e.active);
    check('digital input pulled', !!din.hit && din.hit.token === 'DI_1', din.seen);
    await control('POST', '/mock/event', { kind: 'digital_input', state: false });
  } else {
    const any = await pullUntil(conn, sub, (e) => e.kind === 'motion' && e.isMotion, 4000);
    check('pull without control API (no events expected)', true, any.seen.length);
  }
  const ren = await P.events.renewSubscription(conn, sub.subscriptionPath,
    sub.referenceParameters);
  const rOff = ren.terminationTime - Date.now();
  check('Renew', rOff > 50000 && rOff <= 61000, `${Math.round(rOff / 1000)} s`);
  await P.events.unsubscribe(conn, sub.subscriptionPath, sub.referenceParameters);
  if (CONTROL) {
    const subs = await control('GET', '/mock/subscriptions');
    check('Unsubscribe ended the subscription',
      subs.ended.some((s) => sub.subscriptionPath.endsWith(s.id) && s.ended === 'unsubscribed') ||
      subs.ended.some((s) => s.ended === 'unsubscribed'), subs.ended.slice(-2));
  }

  // 4. Knob scenarios (need the control API).
  for (const sc of SCENARIOS) {
    if (!CONTROL) break;
    await control('POST', '/mock/config/reset', {});
    if (sc === 'digest') {
      await control('POST', '/mock/config', { auth_mode: 'digest' });
      const before = logs.length;
      const r = await adoption('digest-only camera');
      check('digest: Protect logged the Digest fallback', r && logs.slice(before).some(
        (l) => /Digest fallback resolved/.test(l.msg)), logs.slice(before)
        .filter((l) => /Digest/.test(l.msg)).map((l) => l.msg).slice(0, 3));
    } else if (sc === 'media2') {
      await control('POST', '/mock/config', { media2: true });
      const r = await adoption('media2');
      const c2 = await P.connection.createOnvifConnection({ hostname: HOST, port: PORT,
        username: USER, password: PASSWORD });
      check('media2: connection uses media2', r && c2.media2 &&
        c2.mediaPath === '/onvif/media2_service', { media2: c2.media2, path: c2.mediaPath });
    } else if (sc === 'wsa' || sc === 'refparams') {
      await control('POST', '/mock/config', sc === 'wsa' ? { wsa_strict: true } :
        { ref_params_required: true });
      const s2 = await P.events.createPullPointSubscription(conn);
      let err = null;
      try {
        await P.events.pullMessages(conn, s2.subscriptionPath, 2, 10, s2.referenceParameters);
      } catch (e) {
        err = e.message;
      }
      if (sc === 'wsa') {
        // Either outcome is valid (it depends on whether the bundle is patched), but a
        // rejection must be the camera's WS-Addressing fault, not some other failure.
        check('wsa_strict: pull from this bundle', !err || /WS-Addressing/.test(err),
          err ? `rejected: ${err} (bundle sends no WS-Addressing)` :
            'accepted (bundle sends wsa:To/Action)');
      } else {
        check('ref_params_required: Protect echoes ReferenceParameters', !err &&
          s2.referenceParameters, { err, ref: s2.referenceParameters });
      }
      await P.events.unsubscribe(conn, s2.subscriptionPath, s2.referenceParameters);
    } else if (sc === 'slots') {
      await control('DELETE', '/mock/subscriptions');
      await control('POST', '/mock/config', { slots: 1 });
      const a = await P.events.createPullPointSubscription(conn);
      let err = null;
      try {
        await P.events.createPullPointSubscription(conn);
      } catch (e) {
        err = e.message;
      }
      check('slots=1: second Create refused', /BlockedThreadUpperLimit/.test(err || ''), err);
      await P.events.unsubscribe(conn, a.subscriptionPath, a.referenceParameters);
    } else {
      check(`unknown scenario ${sc}`, false);
    }
  }
  if (CONTROL) await control('POST', '/mock/config/reset', {});

  const bad = logs.filter((l) => /raw socket|Rejecting bogus|Invalid SOAP response/.test(l.msg));
  check('no raw-socket fallback or rejected TerminationTime', bad.length === 0,
    bad.map((l) => l.msg));
  results.ms = Date.now() - t0;
  results.warnings = logs.filter((l) => l.lvl === 'warn' || l.lvl === 'error')
    .map((l) => `${l.logger} ${l.lvl}: ${l.msg}`);
  results.info = logs.filter((l) => l.lvl === 'info').map((l) => `${l.logger}: ${l.msg}`);
}

main().catch((e) => {
  check('preflight ran to completion', false, e.stack || String(e));
}).finally(() => {
  results.failed = failed;
  if (args.out) fs.writeFileSync(args.out, JSON.stringify(results, null, 1));
  console.log(`\n${failed ? `${failed} check(s) FAILED` : 'all checks passed'}` +
    ` (${results.checks.length} checks)`);
  if (results.warnings && results.warnings.length) {
    console.log('Protect warnings/errors logged:');
    for (const w of results.warnings) console.log(`  ${w.slice(0, 300)}`);
  }
  process.exit(failed ? 1 : 0);
});
