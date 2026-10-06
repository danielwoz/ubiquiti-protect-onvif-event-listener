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

#include "admin_server.hpp"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <gperftools/malloc_extension.h>
#include <libpq-fe.h>
#include <microhttpd.h>
#include <netinet/in.h>

#if MHD_VERSION < 0x00097100
typedef int MHD_Result;
#endif

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>  // NOLINT(build/c++11)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/flags/commandlineflag.h"
#include "absl/flags/reflection.h"
#include "absl/log/log.h"
#include "absl/status/status.h"

#include "contention_profiler.hpp"
#include "cpu_profiler.hpp"
#include "dump_sanitizer.hpp"
#include "listener_status.hpp"
#include "log_ring.hpp"
#include "onvif_listener.hpp"
#include "pg_stats.hpp"
#include "protect_user_id_provider.hpp"
#include "runtime_config.hpp"

namespace onvif {

// The admin UI is embedded so the package has no external HTML/JS to manage.
// Styled to match UniFi's dark cards + accent blue.  Kept deliberately small.
static const char kAdminHtml[] = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<title>onvif-recorder admin</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{background:#0e1118;color:#e8eaed;font-family:-apple-system,system-ui,sans-serif;
margin:0;padding:24px;max-width:720px}
h1{font-size:20px;color:#8be9fd;margin:0 0 16px 0}
.card{background:#1a1f2b;border:1px solid #2a3140;border-radius:6px;
padding:16px;margin-bottom:12px}
.card h2{font-size:14px;color:#9aa0a6;margin:0 0 8px 0;text-transform:uppercase;
letter-spacing:.04em}
.row{display:flex;align-items:center;gap:8px;margin:6px 0}
.row label{flex:1;color:#c6c9cc}
button,select,input[type=text]{background:#111621;color:#e8eaed;
border:1px solid #3a4255;border-radius:4px;padding:6px 12px;font-size:13px}
button{cursor:pointer;background:#1e6feb;border-color:#1e6feb;color:#fff}
button:hover{background:#3d85ef}
button.danger{background:#b23;border-color:#b23}
button.danger:hover{background:#d44}
.kv{display:grid;grid-template-columns:150px 1fr;gap:6px;font-size:13px}
.kv .k{color:#9aa0a6}
.msg{padding:8px 12px;border-radius:4px;margin-top:8px;font-size:13px;display:none}
.msg.ok{background:#1a3a1a;color:#a0e0a0;display:block}
.msg.err{background:#3a1a1a;color:#e0a0a0;display:block}
.cfg-row{display:grid;grid-template-columns:240px 1fr;gap:8px;align-items:start;margin:6px 0}
.cfg-row label{color:#c6c9cc;font-size:13px}
.cfg-row .desc{color:#7a8190;font-size:12px;margin-top:2px}
.cfg-row .input input,.cfg-row .input select{width:100%;box-sizing:border-box}
.cfg-row .placeholder{color:#5a6172;font-size:11px;margin-top:2px}
.cfg-section{font-size:13px;color:#9aa0a6;text-transform:uppercase;letter-spacing:.04em;
margin:12px 0 4px 0;border-top:1px solid #2a3140;padding-top:12px}
.cfg-section:first-child{border-top:0;padding-top:0;margin-top:0}
.fp-list{max-height:240px;overflow-y:auto;border:1px solid #2a3140;border-radius:4px;
padding:6px}
.fp-row{display:flex;align-items:center;gap:8px;padding:2px 0;font-size:13px}
.spinner{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(14,17,24,0.9);
display:none;align-items:center;justify-content:center;flex-direction:column;gap:12px;
z-index:99}
.spinner.on{display:flex}
.spinner-circle{width:36px;height:36px;border:3px solid #3a4255;border-top-color:#1e6feb;
border-radius:50%;animation:spin 1s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
.pill{display:inline-block;padding:2px 8px;border-radius:10px;font-size:11px;
font-weight:600;text-transform:uppercase;letter-spacing:.04em}
.pill.green{background:#1a3a1a;color:#a0e0a0}
.pill.amber{background:#3a2a1a;color:#e0c290}
.pill.yellow{background:#3a3020;color:#e6d38a}
.pill.red{background:#3a1a1a;color:#e0a0a0}
.pill.grey{background:#2a3140;color:#9aa0a6}
.help{display:inline-block;width:14px;height:14px;border-radius:50%;
background:#2a3140;color:#9aa0a6;font-size:10px;font-weight:600;text-align:center;
line-height:14px;cursor:help;margin-left:4px;vertical-align:middle}
.help:hover{background:#3a4255;color:#e8eaed}
</style></head><body>
<h1>ONVIF Recorder</h1>

<div class="card"><h2>Status</h2>
<div class="kv" id="status">Loading…</div></div>

<div class="card"><h2>Camera health</h2>
<table id="camhealth-table" style="width:100%;border-collapse:collapse;font-size:13px">
<thead><tr style="color:#9aa0a6;text-align:left">
<th style="padding:4px 6px">Name</th>
<th style="padding:4px 6px">Host</th>
<th style="padding:4px 6px">Role</th>
<th style="padding:4px 6px">Last event</th>
<th style="padding:4px 6px">1h</th>
<th style="padding:4px 6px"></th>
<th style="padding:4px 6px">Via Protect<span class="help" title="Route thumbnail fetches through Protect's own /api/cameras/&lt;id&gt;/snapshot endpoint instead of hitting the camera's own HTTP snapshot server directly. Useful for cheap third-party cameras whose HTTP snapshot endpoint chokes or returns black frames while RTSP is streaming -- Protect is already decoding the stream, so we just grab a frame from that decode. Tradeoff: the frame is Protect's currently-decoded one, so there is 200-500 ms of slop vs the exact ONVIF-event moment. Third-party cameras only.">?</span></th>
<th style="padding:4px 6px" title="Untick to skip this camera entirely (no ONVIF subscription for third-party, no motion polling for first-party). Useful when another integration (e.g. a UniFi AI Port) is already handling the same physical camera and would otherwise produce duplicate events. Saved as excluded_cameras (issue #42).">Enabled</th>
</tr></thead>
<tbody id="camhealth-body"><tr><td colspan="8">Loading…</td></tr></tbody></table>
<div style="margin-top:8px;display:flex;gap:12px;align-items:center;flex-wrap:wrap">
  <button id="save-via-protect" onclick="saveViaProtect()" disabled
          style="padding:4px 12px;font-size:12px">Save via-Protect changes</button>
  <span id="save-via-protect-msg" style="font-size:12px;color:#9aa0a6"></span>
  <button id="save-enabled" onclick="saveEnabled()" disabled
          style="padding:4px 12px;font-size:12px">Save Enabled changes</button>
  <span id="save-enabled-msg" style="font-size:12px;color:#9aa0a6"></span>
</div></div>

<div class="card"><h2>Database load
<span class="help" title="Wall-clock time for every SQL round-trip the recorder issues to Protect's Postgres, aggregated per query shape (first ~60 chars of SQL, whitespace-normalised, so calls with different bound params bucket together). Sorted by total time contributed. Timeouts count the number of round-trips that hit their per-query budget without a result. Reset the counters with the button to compare before/after a config change.">?</span></h2>
<div style="max-height:280px;overflow-y:auto;overflow-x:auto"><table id="pgstats-table" style="width:100%;border-collapse:collapse;font-size:12px;font-family:ui-monospace,monospace">
<thead><tr style="color:#9aa0a6;text-align:left">
<th style="padding:4px 6px">SQL</th>
<th style="padding:4px 6px;text-align:right">count</th>
<th style="padding:4px 6px;text-align:right">timeouts</th>
<th style="padding:4px 6px;text-align:right">total&nbsp;ms</th>
<th style="padding:4px 6px;text-align:right">avg&nbsp;ms</th>
<th style="padding:4px 6px;text-align:right">p95&nbsp;ms</th>
<th style="padding:4px 6px;text-align:right">max&nbsp;ms</th>
</tr></thead>
<tbody id="pgstats-body"><tr><td colspan="7">Loading&hellip;</td></tr></tbody></table></div>
<div style="margin-top:6px;display:flex;gap:12px;align-items:center;color:#9aa0a6;font-size:12px">
  <span id="pgstats-summary"></span>
  <button onclick="resetPgStats()" style="padding:2px 10px;font-size:12px">Reset counters</button>
</div></div>

<div class="card"><h2>Release channel</h2>
<div class="row"><label>APT suite used for upgrades</label>
<select id="channel">
<option value="stable">stable</option>
<option value="rc">rc</option>
<option value="early-access">early-access</option>
</select>
<button onclick="setChannel()">Apply</button></div>
<div class="msg" id="channel-msg"></div></div>

<div class="card"><h2>Auto-update</h2>
<div class="row"><label>Install new releases automatically</label>
<select id="autoupdate-enabled">
<option value="true">Enabled</option>
<option value="false">Disabled</option>
</select></div>
<div class="row"><label>Schedule (systemd OnCalendar)</label>
<input type="text" id="autoupdate-schedule" placeholder="daily"></div>
<div class="row"><button onclick="setAutoupdate()">Apply</button>
<button onclick="checkNow()">Check now</button>
<button onclick="refreshApt()">Refresh package list</button></div>
<div class="msg" id="autoupdate-msg"></div></div>

<div class="card"><h2>First-party cameras</h2>
<p class="desc" style="font-size:12px;color:#9aa0a6;margin:0 0 8px 0">
Tick cameras to enable smart-detect on them via NanoDet-M motion polling.
Saved to the configuration below as <code>first_party_cameras</code>.</p>
<div class="fp-list" id="fp-list">Loading…</div></div>

<div class="card"><h2>Configuration</h2>
<p class="desc" style="font-size:12px;color:#9aa0a6;margin:0 0 8px 0">
Empty fields fall back to flag defaults / unit-file values.  Saving
restarts the service so changes take effect immediately.</p>
<div id="config-form">Loading…</div>
<div class="row"><button onclick="saveConfig()">Save &amp; restart</button></div>
<div class="msg" id="config-msg"></div></div>

<div class="card"><h2>Recent events</h2>
<p class="desc" style="font-size:12px;color:#9aa0a6;margin:0 0 8px 0">
Last 30 events with their snapshot thumbnails.  Thumbnails written
to Protect's <code>thumbnails</code> table are read directly; MSR-stored
snapshots are proxied through Protect's local API.</p>
<div id="recent-events-list" style="display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px">Loading…</div></div>

<div class="card"><h2>Diagnostics</h2>
<p class="desc" style="font-size:12px;color:#9aa0a6;margin:0 0 8px 0">
Download a tar.gz of recent journal output, current config, camera-health
snapshot, and version info.  Attach this to a GitHub issue when reporting
problems.</p>
<div class="row"><button onclick="downloadDump()">Download diagnostic dump</button></div>
<div class="msg" id="dump-msg"></div></div>

<div class="card"><h2>Recent errors
<span class="help" title="Last 20 ERROR-level log lines from the recorder's in-memory ring, newest first. Refreshes every 15 seconds. Empty when everything is healthy. For the full log stream (all levels) open /onvif/events/log.">?</span></h2>
<pre id="errors-body" style="margin:0;max-height:220px;overflow:auto;background:#0a0d14;border:1px solid #2a3140;border-radius:4px;padding:8px;font-family:ui-monospace,monospace;font-size:11px;color:#e0a0a0;white-space:pre-wrap">Loading&hellip;</pre></div>

<div class="card"><h2>Uninstall</h2>
<div class="row"><label>Remove onvif-recorder and all patches</label>
<button class="danger" onclick="uninstall()">Uninstall</button></div>
<div class="msg" id="uninstall-msg"></div></div>

<div class="spinner" id="spinner">
<div class="spinner-circle"></div>
<div id="spinner-text">Restarting service…</div></div>

<script>
// UniFi OS's nginx proxy gates non-GET methods through auth_request, which
// requires the X-Csrf-Token header to match the token nginx attaches to
// every proxied response.  Capture it from each response and echo it on
// subsequent POSTs.  A GET response refreshes csrfToken; if a POST 403s
// (token rotated), retry once after a fresh GET.
let csrfToken = '';
function captureCsrf(r){
  const t = r.headers.get('X-Csrf-Token');
  if (t) csrfToken = t;
  return r;
}
async function fetchStatus(){
  const r = captureCsrf(await fetch('api/status'));
  const j = await r.json();
  const rows = [
    ['Version', j.version],
    ['Channel', j.channel],
    ['Auto-update', j.autoupdate_enabled ? 'enabled ('+j.autoupdate_schedule+')' : 'disabled'],
    ['Last check', j.last_check || 'never'],
  ];
  document.getElementById('status').innerHTML =
    rows.map(([k,v]) => `<span class="k">${k}</span><span>${v}</span>`).join('');
  document.getElementById('channel').value = j.channel;
  document.getElementById('autoupdate-enabled').value = String(!!j.autoupdate_enabled);
  document.getElementById('autoupdate-schedule').value = j.autoupdate_schedule || 'daily';
}
function msg(id, text, ok){
  const e = document.getElementById(id);
  e.textContent = text;
  e.className = 'msg ' + (ok ? 'ok' : 'err');
}
async function postOnce(path, body){
  return captureCsrf(await fetch(path, {method:'POST',
    headers:{'Content-Type':'application/json',
             'X-Csrf-Token': csrfToken},
    body: JSON.stringify(body||{})}));
}
async function post(path, body){
  let r = await postOnce(path, body);
  // 403 commonly means the cached CSRF token rotated -- refresh and retry.
  if (r.status === 403) {
    captureCsrf(await fetch('api/status'));
    r = await postOnce(path, body);
  }
  return {ok: r.ok, text: await r.text()};
}
async function setChannel(){
  const v = document.getElementById('channel').value;
  const r = await post('api/channel', {channel: v});
  msg('channel-msg', r.text, r.ok);
  if (r.ok) fetchStatus();
}
async function setAutoupdate(){
  const enabled = document.getElementById('autoupdate-enabled').value === 'true';
  const schedule = document.getElementById('autoupdate-schedule').value || 'daily';
  const r = await post('api/autoupdate', {enabled, schedule});
  msg('autoupdate-msg', r.text, r.ok);
  if (r.ok) fetchStatus();
}
async function checkNow(){
  const r = await post('api/check', {});
  msg('autoupdate-msg', r.text, r.ok);
}
async function refreshApt(){
  msg('autoupdate-msg', 'Refreshing apt package list…', true);
  const r = await post('api/refresh_apt', {});
  msg('autoupdate-msg', r.text, r.ok);
}
async function downloadDump(){
  msg('dump-msg', 'Building diagnostic archive…', true);
  try {
    const r = await fetch('api/diagnostic_dump');
    if (!r.ok) {
      msg('dump-msg', 'Failed: ' + (await r.text()), false);
      return;
    }
    const blob = await r.blob();
    const ts = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'onvif-recorder-dump-' + ts + '.tar.gz';
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(a.href);
    msg('dump-msg', 'Downloaded.', true);
  } catch (e) {
    msg('dump-msg', 'Failed: ' + e, false);
  }
}
async function uninstall(){
  if (!confirm('Uninstall onvif-recorder? This stops the service, removes the package, and rolls back UI/nginx patches.')) return;
  const r = await post('api/uninstall', {});
  msg('uninstall-msg', r.text, r.ok);
}

// --- Configuration form (driven by /api/config schema) -----------------

let configEntries = [];        // schema entries (one per editable flag)
let firstPartyCameras = [];    // {id, name, mac}
let firstPartyChecked = new Set();  // ticked IDs

function renderConfigForm(entries){
  const root = document.getElementById('config-form');
  let lastGroup = '';
  let html = '';
  for (const e of entries){
    if (e.name === 'first_party_cameras') continue;  // rendered separately
    if (e.name === 'camera_snapshot_via_protect') continue;  // rendered as tickboxes on the Camera Health card
    if (e.name === 'excluded_cameras') continue;              // rendered as Enabled tickboxes on the Camera Health card
    if (e.group !== lastGroup){
      html += `<div class="cfg-section">${e.group}</div>`;
      lastGroup = e.group;
    }
    let inputHtml;
    if (e.type === 'bool'){
      inputHtml = `<select id="cfg-${e.name}">`
        + `<option value="">(default: ${e.flag_default})</option>`
        + `<option value="true">true</option>`
        + `<option value="false">false</option></select>`;
    } else {
      const ph = `(default: ${e.flag_default || '""'})`;
      inputHtml = `<input type="text" id="cfg-${e.name}" placeholder="${ph}">`;
    }
    html += `<div class="cfg-row"><label>${e.name}<div class="desc">${e.description}</div></label>`
         +  `<div class="input">${inputHtml}</div></div>`;
  }
  root.innerHTML = html;
  // Apply override values.
  for (const e of entries){
    const el = document.getElementById('cfg-' + e.name);
    if (el && e.override) el.value = e.override;
  }
}

function renderFirstPartyList(cams, checked){
  const root = document.getElementById('fp-list');
  if (!cams.length){
    root.innerHTML = `<div style="color:#7a8190">No first-party cameras adopted in Protect.</div>`;
    return;
  }
  root.innerHTML = cams.map(c => {
    const isChecked = checked.has(c.id) ? 'checked' : '';
    return `<div class="fp-row"><input type="checkbox" id="fp-${c.id}" ${isChecked}>`
        +  `<label for="fp-${c.id}">${c.name || '(unnamed)'} `
        +  `<span style="color:#7a8190">[${c.id}]</span></label></div>`;
  }).join('');
}

async function loadConfig(){
  const r = captureCsrf(await fetch('api/config'));
  const j = await r.json();
  configEntries = j.entries || [];
  // first_party_cameras override is a CSV string; expand into the Set.
  firstPartyChecked = new Set();
  const fp = configEntries.find(e => e.name === 'first_party_cameras');
  if (fp && fp.override){
    fp.override.split(',').map(s => s.trim()).filter(Boolean)
      .forEach(id => firstPartyChecked.add(id));
  }
  renderConfigForm(configEntries);
  renderFirstPartyList(firstPartyCameras, firstPartyChecked);
}

async function loadFirstPartyCameras(){
  const r = captureCsrf(await fetch('api/first_party_cameras'));
  const j = await r.json();
  firstPartyCameras = j.cameras || [];
  renderFirstPartyList(firstPartyCameras, firstPartyChecked);
}

async function saveConfig(){
  const values = {};
  for (const e of configEntries){
    if (e.name === 'first_party_cameras') continue;
    if (e.name === 'camera_snapshot_via_protect') continue;  // saved by the tickbox flow
    if (e.name === 'excluded_cameras') continue;              // saved by the Enabled tickbox flow
    const el = document.getElementById('cfg-' + e.name);
    if (el) values[e.name] = el.value || '';
  }
  // Collect ticked first-party cameras into a CSV string.
  const ticked = firstPartyCameras
    .filter(c => document.getElementById('fp-' + c.id)?.checked)
    .map(c => c.id);
  values.first_party_cameras = ticked.join(',');

  const sp = document.getElementById('spinner');
  document.getElementById('spinner-text').textContent = 'Saving config and restarting service…';
  sp.classList.add('on');
  try {
    const r = await post('api/config', values);
    if (!r.ok) {
      msg('config-msg', r.text, false);
      sp.classList.remove('on');
      return;
    }
    // Service is exiting; poll /api/status until it answers again.
    await waitForRestart();
    await loadConfig();
    await fetchStatus();
    msg('config-msg', 'Saved.', true);
  } finally {
    sp.classList.remove('on');
  }
}

async function waitForRestart(){
  const deadline = Date.now() + 30000;  // 30s
  // Brief delay so the old process has time to die before we start polling.
  await new Promise(r => setTimeout(r, 1500));
  while (Date.now() < deadline){
    try {
      const r = await fetch('api/status', {cache: 'no-store'});
      if (r.ok){ captureCsrf(r); return; }
    } catch (_) { /* connection refused while service is down */ }
    await new Promise(r => setTimeout(r, 1000));
  }
  throw new Error('service did not come back within 30s');
}

// --- Camera health card ---
function fmtAge(ms){
  if (!ms || ms <= 0) return 'never';
  const s = Math.max(0, Math.floor(ms / 1000));
  if (s < 60) return s + 's ago';
  const m = Math.floor(s / 60);
  if (m < 60) return m + 'm ago';
  const h = Math.floor(m / 60);
  if (h < 48) return h + 'h ago';
  return Math.floor(h / 24) + 'd ago';
}
function pillFor(ageMs){
  if (ageMs == null) return '<span class="pill grey">unknown</span>';
  const m = ageMs / 60000;
  if (m < 60)   return '<span class="pill green">healthy</span>';
  if (m < 360)  return '<span class="pill amber">stale</span>';
  return '<span class="pill red">silent</span>';
}
// via-Protect tick-box state.  Reset from server on every fetch; tracks
// user edits so the Save button can commit them.  Keyed by camera IP
// because that is the value shape camera_snapshot_via_protect uses.
let viaProtectServer = new Set();  // as-persisted on server
let viaProtectLocal  = new Set();  // includes unsaved changes

function updateSaveViaProtectState(){
  const dirty = viaProtectServer.size !== viaProtectLocal.size
             || [...viaProtectLocal].some(ip => !viaProtectServer.has(ip));
  const btn = document.getElementById('save-via-protect');
  if (btn) btn.disabled = !dirty;
  const msg = document.getElementById('save-via-protect-msg');
  if (msg && !dirty) msg.textContent = '';
}
function toggleViaProtect(host, checked){
  if (checked) viaProtectLocal.add(host); else viaProtectLocal.delete(host);
  updateSaveViaProtectState();
}
async function saveViaProtect(){
  const csv = [...viaProtectLocal].sort().join(',');
  const msg = document.getElementById('save-via-protect-msg');
  const sp  = document.getElementById('spinner');
  document.getElementById('spinner-text').textContent =
      'Saving snapshot routing and restarting service…';
  sp.classList.add('on');
  try {
    const r = await post('api/config', { camera_snapshot_via_protect: csv });
    if (!r.ok) { if (msg) msg.textContent = r.text; sp.classList.remove('on'); return; }
    await waitForRestart();
    await loadCameraHealth();
    if (msg) { msg.textContent = 'Saved.'; msg.style.color = '#a0e0a0'; }
  } finally {
    sp.classList.remove('on');
  }
}

// Enabled tick-box state.  Semantically the *inverse* of the
// excluded_cameras config field: an unchecked box means the camera's
// MAC is included in the excluded_cameras CSV.  Keyed by MAC because
// that is what the enforcement side in main.cpp compares against.
let enabledLocal   = new Set();  // includes unsaved changes (excluded MACs)
let enabledServer  = new Set();  // as-persisted on server (excluded MACs)

function updateSaveEnabledState(){
  const dirty = enabledServer.size !== enabledLocal.size
             || [...enabledLocal].some(m => !enabledServer.has(m));
  const btn = document.getElementById('save-enabled');
  if (btn) btn.disabled = !dirty;
  const msg = document.getElementById('save-enabled-msg');
  if (msg && !dirty) msg.textContent = '';
}
function toggleEnabled(mac, checked){
  // checked = camera runs normally.  unchecked = add MAC to the
  // excluded_cameras set so the listener skips it after restart.
  if (checked) enabledLocal.delete(mac);
  else         enabledLocal.add(mac);
  updateSaveEnabledState();
}
async function saveEnabled(){
  const csv = [...enabledLocal].sort().join(',');
  const msg = document.getElementById('save-enabled-msg');
  const sp  = document.getElementById('spinner');
  document.getElementById('spinner-text').textContent =
      'Saving Enabled changes and restarting service…';
  sp.classList.add('on');
  try {
    const r = await post('api/config', { excluded_cameras: csv });
    if (!r.ok) { if (msg) msg.textContent = r.text; sp.classList.remove('on'); return; }
    await waitForRestart();
    await loadCameraHealth();
    if (msg) { msg.textContent = 'Saved.'; msg.style.color = '#a0e0a0'; }
  } finally {
    sp.classList.remove('on');
  }
}

async function loadCameraHealth(){
  try {
    const r = captureCsrf(await fetch('api/camera_health'));
    const j = await r.json();
    const body = document.getElementById('camhealth-body');
    if (!j.cameras || !j.cameras.length){
      body.innerHTML = '<tr><td colspan="8" style="color:#7a8190">No adopted cameras.</td></tr>';
      return;
    }
    // Re-seed via-Protect state from server, but preserve any local
    // pending edits that haven't been saved yet.
    const wasDirty = document.getElementById('save-via-protect')?.disabled === false;
    const nextServer = new Set();
    for (const c of j.cameras)
      if (c.is_third_party && c.snapshot_via_protect) nextServer.add(c.host);
    viaProtectServer = nextServer;
    if (!wasDirty) viaProtectLocal = new Set(viaProtectServer);
    updateSaveViaProtectState();
    // Same seed/preserve pattern for the Enabled column.
    const wasEnabledDirty =
        document.getElementById('save-enabled')?.disabled === false;
    const nextEnabledServer = new Set();
    for (const c of j.cameras) {
      if (c.excluded && c.mac) nextEnabledServer.add(c.mac);
    }
    enabledServer = nextEnabledServer;
    if (!wasEnabledDirty) enabledLocal = new Set(enabledServer);
    updateSaveEnabledState();
    const now = j.now_ms || Date.now();
    body.innerHTML = j.cameras.map(c => {
      const age = c.last_event_ms ? (now - c.last_event_ms) : null;
      const hintRow = c.hint === 'needs_onvif_admin'
        ? `<tr><td colspan="8" style="padding:4px 6px 8px 22px;color:#f0c674;font-size:11px;border-top:0">
             &#9888; ONVIF event subscription rejected with <code>ter:NotAuthorized</code>.
             Most Hikvision firmware (and OEM rebadges) require a separate ONVIF user with
             <b>Administrator</b> privileges -- web-UI admin alone is not enough.
             See <a href="https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/issues/20"
                   target="_blank" rel="noopener" style="color:#f0c674">issue #20</a> for the fix.
           </td></tr>`
        : '';
      // Via-Protect tickbox: third-party only.  First-party cameras use
      // Protect's native pipeline so this workaround doesn't apply.
      const checked = viaProtectLocal.has(c.host) ? 'checked' : '';
      const viaCell = c.is_third_party
        ? `<input type="checkbox" ${checked}
                  onchange="toggleViaProtect('${c.host}', this.checked)">`
        : '<span style="color:#5a6070">—</span>';
      // Enabled tickbox: shown for every camera we can identify by MAC
      // (cameras without a MAC in the DB, which is rare, can't be
      // excluded because the enforcement side matches on MAC).
      // Checked = camera runs normally; unchecked = excluded.
      const enabledCell = c.mac
        ? `<input type="checkbox" ${enabledLocal.has(c.mac) ? '' : 'checked'}
                  onchange="toggleEnabled('${c.mac}', this.checked)">`
        : '<span style="color:#5a6070" title="No MAC on file -- cannot exclude">—</span>';
      return `<tr>
        <td style="padding:4px 6px">${c.name || '(unnamed)'}</td>
        <td style="padding:4px 6px;color:#9aa0a6">${c.host || '-'}</td>
        <td style="padding:4px 6px">${c.is_third_party ? 'third-party' : 'first-party'}</td>
        <td style="padding:4px 6px">${fmtAge(age)}</td>
        <td style="padding:4px 6px">${c.events_1h}</td>
        <td style="padding:4px 6px">${c.hint === 'needs_onvif_admin'
            ? '<span class="pill red">auth</span>'
            : pillFor(age)}</td>
        <td style="padding:4px 6px;text-align:center">${viaCell}</td>
        <td style="padding:4px 6px;text-align:center">${enabledCell}</td>
      </tr>${hintRow}`;
    }).join('');
  } catch (e) {
    document.getElementById('camhealth-body').innerHTML =
      '<tr><td colspan="8" style="color:#e0a0a0">Failed to load camera health.</td></tr>';
  }
}

// --- Recent events panel ---
function fmtClock(ms){
  if (!ms) return '';
  const d = new Date(ms);
  const pad = n => String(n).padStart(2, '0');
  return pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}
function shortDate(ms){
  const d = new Date(ms);
  const m = d.getMonth() + 1, day = d.getDate();
  return m + '/' + day;
}
async function loadRecentEvents(){
  try {
    const r = captureCsrf(await fetch('api/recent_events'));
    const j = await r.json();
    const root = document.getElementById('recent-events-list');
    if (!j.events || !j.events.length){
      root.innerHTML = '<div style="color:#7a8190">No recent events.</div>';
      return;
    }
    root.innerHTML = j.events.map(ev => {
      const thumb = ev.thumbnail_id
        ? '<img src="api/thumbnail?id=' + encodeURIComponent(ev.thumbnail_id)
            + '" style="width:100%;height:90px;object-fit:cover;border-radius:4px;background:#0e1118" '
            + 'onerror="this.replaceWith(Object.assign(document.createElement(\'div\'),'
            + '{style:\'height:90px;display:flex;align-items:center;justify-content:center;'
            + 'background:#0e1118;border:1px dashed #2a3140;border-radius:4px;color:#5a6172;font-size:11px\','
            + 'textContent:\'no thumb\'}))" loading="lazy">'
        : '<div style="height:90px;display:flex;align-items:center;justify-content:center;'
            + 'background:#0e1118;border:1px dashed #2a3140;border-radius:4px;color:#5a6172;font-size:11px">'
            + 'no thumb</div>';
      const sdt = (ev.sdt && ev.sdt.length)
        ? ev.sdt.join(',')
        : ev.type;
      return `<div style="font-size:11px;color:#c6c9cc">
        ${thumb}
        <div style="margin-top:4px"><span style="color:#8be9fd">${sdt}</span></div>
        <div style="color:#9aa0a6">${ev.camera_name || '-'}</div>
        <div style="color:#7a8190">${shortDate(ev.start_ms)} ${fmtClock(ev.start_ms)}</div>
      </div>`;
    }).join('');
  } catch (e) {
    document.getElementById('recent-events-list').innerHTML =
      '<div style="color:#e0a0a0">Failed to load events.</div>';
  }
}

fetchStatus();
loadFirstPartyCameras();
loadConfig();
loadCameraHealth();
loadRecentEvents();
loadPgStats();
loadRecentErrors();
setInterval(fetchStatus, 30000);
setInterval(loadCameraHealth, 30000);
setInterval(loadRecentEvents, 30000);
setInterval(loadPgStats, 15000);
setInterval(loadRecentErrors, 15000);
async function loadRecentErrors(){
  try{
    const r = captureCsrf(await fetch('api/recent_errors'));
    if (!r.ok) throw new Error('http ' + r.status);
    const text = await r.text();
    const el = document.getElementById('errors-body');
    if (!text.trim()) {
      el.style.color = '#9aa0a6';
      el.textContent = 'No recent errors.';
    } else {
      el.style.color = '#e0a0a0';
      el.textContent = text;
    }
  } catch(e) {
    const el = document.getElementById('errors-body');
    el.style.color = '#e0a0a0';
    el.textContent = 'Failed to load errors: ' + e.message;
  }
}
async function loadPgStats(){
  try{
    const r = captureCsrf(await fetch('api/pg_stats'));
    if (!r.ok) throw new Error('http ' + r.status);
    const s = await r.json();
    const total = (s.queries||[]).reduce((a,q)=>a+q.total_ms,0);
    const secs = (s.since_boot_ms/1000).toFixed(0);
    document.getElementById('pgstats-summary').textContent =
      `${(s.queries||[]).length} query shapes, ${total} ms total DB time in the last ${secs}s`;
    document.getElementById('pgstats-body').innerHTML =
      (s.queries||[]).slice(0,20).map(q=>{
        const cls = q.timeouts > 0 ? 'style="color:#e0a0a0"' : '';
        const sql = q.sql.replace(/</g,'&lt;').replace(/>/g,'&gt;');
        return `<tr ${cls}>
          <td style="padding:2px 6px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:340px" title="${sql}">${sql}</td>
          <td style="padding:2px 6px;text-align:right">${q.count}</td>
          <td style="padding:2px 6px;text-align:right">${q.timeouts||0}</td>
          <td style="padding:2px 6px;text-align:right">${q.total_ms}</td>
          <td style="padding:2px 6px;text-align:right">${q.avg_ms}</td>
          <td style="padding:2px 6px;text-align:right">${q.p95_ms}</td>
          <td style="padding:2px 6px;text-align:right">${q.max_ms}</td></tr>`;
      }).join('') || '<tr><td colspan="7">No queries yet.</td></tr>';
  } catch(e) {
    document.getElementById('pgstats-body').innerHTML =
      `<tr><td colspan="7" style="color:#e0a0a0">Failed to load pg stats: ${e.message}</td></tr>`;
  }
}
async function resetPgStats(){
  try { await post('api/pg_stats?reset=1', {}); } catch(e){}
  await loadPgStats();
}
</script>
</body></html>
)HTML";

// Replace the query string of each named URL-valued key in a flat JSON
// object with "?[REDACTED]".  A webhook URL commonly carries its access
// token there, and the generic sanitiser only knows a few query keys.
std::string redact_url_queries(std::string json,
                               const std::vector<std::string>& keys) {
  for (const auto& key : keys) {
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) continue;
    const size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) continue;
    const size_t v = json.find('"', colon + 1);
    if (v == std::string::npos) continue;
    size_t end = v + 1;
    while (end < json.size() && json[end] != '"') {
      if (json[end] == '\\') ++end;
      ++end;
    }
    if (end >= json.size()) continue;
    const size_t q = json.find('?', v + 1);
    if (q != std::string::npos && q < end)
      json.replace(q, end - q, "?[REDACTED]");
  }
  return json;
}

namespace {

// Helper: run a shell command, capture stdout+stderr, return exit code.
// Output is bounded by @p max_bytes to avoid runaway memory if a command
// misbehaves.  The default is 16 KiB which suits status-style probes
// (`systemctl is-enabled`, `dpkg-query -W`, …); the diagnostic dump
// passes a much larger cap because the journal is the whole point of
// the dump.
int run_cmd(const std::string& cmd, std::string* output,
            size_t max_bytes = 16384) {
  std::string full = cmd + " 2>&1";
  FILE* pipe = popen(full.c_str(), "r");
  if (!pipe) {
    if (output) *output = "popen failed";
    return -1;
  }
  char buf[4096];
  size_t total = 0;
  while (total < max_bytes && std::fgets(buf, sizeof(buf), pipe)) {
    size_t n = std::strlen(buf);
    if (output) output->append(buf, n);
    total += n;
  }
  int rc = pclose(pipe);
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

// Read the contents of a file into a string.  Returns empty on failure.
std::string read_file(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// One-shot GET of the Protect thumbnail.  Returns (http_code, body).
// http_code == 0 indicates a curl-level failure.
struct ProtectThumbResult {
  long code;  // NOLINT(runtime/int)
  std::string body;
};
ProtectThumbResult perform_protect_thumb_get(const std::string& protect_url,
                                              const std::string& user_id,
                                              const std::string& thumb_id) {
  ProtectThumbResult out{0, {}};
  if (protect_url.empty() || user_id.empty()) return out;
  std::string url = protect_url + "/api/thumbnails/" + thumb_id;
  std::string buf;
  CURL* curl = curl_easy_init();
  if (!curl) return out;
  struct curl_slist* hdrs = nullptr;
  std::string user_hdr = "X-UserId: " + user_id;
  hdrs = curl_slist_append(hdrs, user_hdr.c_str());
  hdrs = curl_slist_append(hdrs, "X-Source: unifi-os");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
      +[](char* p, size_t s, size_t n, void* ud) -> size_t {
        static_cast<std::string*>(ud)->append(p, s * n);
        return s * n;
      });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.code);
  }
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  if (out.code == 200) out.body = std::move(buf);
  return out;
}

// GET <protect_url>/api/thumbnails/<id> with X-UserId auth.  Returns the
// JPEG bytes on 200, or an empty string on any error / non-200.  Used to
// proxy MSR-stored thumbnails (whose IDs are length != 24 and live as
// native UBV files served by the Protect msp media server) into the
// admin UI's <img src="api/thumbnail?id=..."> path.
//
// On observed 401, asks the provider to refresh the user_id and retries
// once with the new value.
std::string fetch_protect_thumbnail(
    const std::string& protect_url,
    onvif::ProtectUserIdProvider* provider,
    const std::string& thumb_id) {
  if (!provider) return {};
  ProtectThumbResult r = perform_protect_thumb_get(
      protect_url, provider->current(), thumb_id);
  if (r.code == 401 && provider->try_refresh()) {
    LOG(INFO) << "[admin_server] retrying thumbnail GET after user_id refresh";
    r = perform_protect_thumb_get(protect_url, provider->current(), thumb_id);
  }
  return r.body;
}

// Write a string atomically to a file: write to .tmp, rename.
bool write_file(const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp);
    if (!f.is_open()) return false;
    f << content;
    if (!f.good()) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// Trim trailing whitespace/newlines.
std::string trim(std::string s) {
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  return s;
}

// Very small JSON string extractor: finds `"key":"..."` and returns the value.
// Does not handle escapes beyond \\ and \" — sufficient for our single-field
// bodies from the admin page.
// Like json_string_field but distinguishes "key absent" (nullopt) from
// "key present with an empty value" (empty string).  handle_config needs
// that difference: an absent key must keep whatever is already on disk,
// whereas an explicitly empty one means the user cleared the setting.
std::optional<std::string> json_string_field_opt(const std::string& body,
                                                  const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) return std::nullopt;
  p = body.find(':', p);
  if (p == std::string::npos) return std::nullopt;
  p = body.find('"', p);
  if (p == std::string::npos) return std::nullopt;
  ++p;
  std::string out;
  while (p < body.size() && body[p] != '"') {
    if (body[p] == '\\' && p + 1 < body.size()) {
      out += body[p + 1];
      p += 2;
    } else {
      out += body[p++];
    }
  }
  return out;
}

// Convenience wrapper: absent and empty both collapse to "".  Callers
// that must tell those apart use json_string_field_opt directly.
std::string json_string_field(const std::string& body, const std::string& key) {
  return json_string_field_opt(body, key).value_or(std::string());
}

// Extract a boolean field.  Returns default_val if not found.
bool json_bool_field(const std::string& body,
                     const std::string& key,
                     bool default_val) {
  const std::string needle = "\"" + key + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) return default_val;
  p = body.find(':', p);
  if (p == std::string::npos) return default_val;
  while (p + 1 < body.size() &&
         (body[p + 1] == ' ' || body[p + 1] == '\t'))
    ++p;
  if (body.compare(p + 1, 4, "true") == 0) return true;
  if (body.compare(p + 1, 5, "false") == 0) return false;
  return default_val;
}

// Per-connection context for accumulating POST bodies across libmicrohttpd's
// upload-data callbacks.
struct PostCtx {
  std::string body;
};

// Request-scoped state passed to handler via MHD_create_response_from_buffer.
struct Ctx {
  const char* version;
  const char* channel_file;
  const char* config_path;
  const unifi::DbConfig* db;  // optional; may have empty conn_string
  const char* protect_url;        // empty if Protect API not available
  // Shared user_id provider used for the thumbnail proxy.  Null if no
  // X-UserId was discovered at startup.  AdminServer outlives all
  // pending requests so the pointer remains valid.
  onvif::ProtectUserIdProvider* protect_user_id_provider;
  const char* event_log_path;     // value of --event_log; empty if disabled
  // Snapshot of per-camera health from the ONVIF listener.  Optional --
  // may be empty in dev-server / unit tests.  Callable from any thread.
  std::function<std::vector<onvif::CameraHealth>()> get_onvif_healths;
  // Optional shared log ring for /api/recent_errors.  null in tests.
  const onvif::LogRing* log_ring{nullptr};
};

// Build a JSON response body for GET /api/status.
std::string build_status_json(const Ctx& ctx) {
  std::string channel = trim(read_file(ctx.channel_file));
  if (channel.empty()) channel = "stable";

  // Prefer the installed package version (accurate when invoked via the .deb)
  // over the compile-time ONVIF_RECORDER_VERSION constant, which is "dev" for
  // any binary Bazel produced outside a release build.
  std::string version;
  {
    std::string out;
    if (run_cmd("dpkg-query -W -f='${Version}' onvif-recorder 2>/dev/null",
                &out) == 0) {
      version = trim(out);
    }
    if (version.empty()) version = ctx.version;
  }

  // `systemctl is-enabled onvif-recorder-autoupdate.timer` returns
  // "enabled" / "disabled" / "masked" on stdout.  We treat non-enabled as
  // disabled.
  std::string enabled_out;
  run_cmd("systemctl is-enabled onvif-recorder-autoupdate.timer",
          &enabled_out);
  bool enabled = trim(enabled_out) == "enabled";

  // Read the current OnCalendar= from a drop-in override if present.
  std::string schedule = "daily";
  std::string dropin = read_file(
      "/etc/systemd/system/onvif-recorder-autoupdate.timer.d/schedule.conf");
  size_t p = dropin.find("OnCalendar=");
  if (p != std::string::npos) {
    p += std::strlen("OnCalendar=");
    size_t end = dropin.find('\n', p);
    schedule = dropin.substr(p, end - p);
  }

  std::string last_check;
  {
    std::string out;
    run_cmd("systemctl show onvif-recorder-autoupdate.service "
            "-p ExecMainStartTimestamp --value",
            &out);
    last_check = trim(out);
  }

  // Hand-rolled JSON; all fields are ASCII-safe.
  std::string j;
  j += "{\"version\":\"";
  j += version;
  j += "\",\"channel\":\"";
  j += channel;
  j += "\",\"autoupdate_enabled\":";
  j += enabled ? "true" : "false";
  j += ",\"autoupdate_schedule\":\"";
  j += schedule;
  j += "\",\"last_check\":\"";
  j += last_check;
  j += "\"}";
  return j;
}

// Run `apt-get update` against the recorder's sources.list only.  Used by
// /api/refresh_apt and /api/channel after a channel switch -- duplicating
// the flag list keeps both endpoints consistent.
std::pair<int, std::string> run_apt_update_recorder_only() {
  std::string out;
  int rc = run_cmd(
      "apt-get update "
      "-o Dir::Etc::sourcelist=/etc/apt/sources.list.d/onvif-recorder.list "
      "-o Dir::Etc::sourceparts=- "
      "-o APT::Get::List-Cleanup=0",
      &out);
  if (rc != 0) return {500, "apt update failed: " + out};
  return {200, "package list refreshed"};
}

// Handle POST /api/refresh_apt -- the standalone "Refresh package list"
// button.  Issue #28 confused users who ran `apt-get install --only-upgrade`
// without `apt-get update` first; this exposes the refresh as an explicit
// UI action so they can see when the cache was last updated.
std::pair<int, std::string> handle_refresh_apt(const Ctx&) {
  LOG(INFO) << "[admin] refreshing apt package list";
  return run_apt_update_recorder_only();
}

// Handle POST /api/channel — body {"channel":"stable|rc|early-access"}.
std::pair<int, std::string> handle_channel(const Ctx& ctx,
                                           const std::string& body) {
  std::string channel = json_string_field(body, "channel");
  if (channel != "stable" && channel != "rc" && channel != "early-access") {
    return {400, "invalid channel (stable, rc, early-access)"};
  }
  if (!write_file(ctx.channel_file, channel + "\n")) {
    return {500, std::string("failed to write ") + ctx.channel_file};
  }
  // Drop a sticky marker recording the user's explicit choice.
  // detect-channel.sh checks for this and skips overwriting the
  // channel file on subsequent postinst runs / daily timer runs.
  // Loss of the marker (e.g. apt purge wiping /etc/onvif-recorder)
  // is the only path back to auto-detection.
  std::string pinned_path = std::string(ctx.channel_file) + ".pinned";
  if (!write_file(pinned_path, channel + "\n")) {
    LOG(WARNING) << "[channel] failed to write " << pinned_path
                 << " -- channel may revert on upgrade";
  }
  std::string out;
  int rc = run_cmd(
      "/usr/libexec/onvif-recorder/install-apt-source.sh && "
      "apt-get update -o Dir::Etc::sourcelist="
      "/etc/apt/sources.list.d/onvif-recorder.list "
      "-o Dir::Etc::sourceparts=- "
      "-o APT::Get::List-Cleanup=0",
      &out);
  if (rc != 0) return {500, "apt update failed: " + out};
  return {200, "channel set to " + channel};
}

// Handle POST /api/check — fire force-check via autoupdate.service.
std::pair<int, std::string> handle_check() {
  std::string out;
  int rc = run_cmd(
      "systemctl start --no-block onvif-recorder-autoupdate.service",
      &out);
  if (rc != 0) return {500, "systemctl start failed: " + out};
  return {200, "update check started"};
}

// Handle POST /api/autoupdate — body {enabled:bool, schedule:"..."}.
std::pair<int, std::string> handle_autoupdate(const std::string& body) {
  bool enabled = json_bool_field(body, "enabled", true);
  std::string schedule = json_string_field(body, "schedule");
  if (schedule.empty()) schedule = "daily";

  // Reject any character that isn't safe in an OnCalendar= value to avoid
  // shell / ini injection.  OnCalendar syntax is quite liberal so we allow
  // letters, digits, colon, dash, comma, slash, dot, asterisk, space.
  for (char c : schedule) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == ':' || c == '-' || c == ',' || c == '/' ||
              c == '.' || c == '*' || c == ' ';
    if (!ok) return {400, "invalid character in schedule"};
  }

  // Write/remove the drop-in file that sets OnCalendar=.
  const char kDropinDir[] =
      "/etc/systemd/system/onvif-recorder-autoupdate.timer.d";
  const char kDropinFile[] =
      "/etc/systemd/system/onvif-recorder-autoupdate.timer.d/schedule.conf";
  std::string mkdir_out;
  (void)run_cmd(std::string("mkdir -p ") + kDropinDir, &mkdir_out);
  std::string conf =
      "[Timer]\nOnCalendar=\nOnCalendar=" + schedule + "\n";
  if (!write_file(kDropinFile, conf)) {
    return {500, "failed to write timer drop-in"};
  }

  std::string out;
  if (enabled) {
    (void)run_cmd("systemctl daemon-reload", &out);
    int rc = run_cmd(
        "systemctl enable --now onvif-recorder-autoupdate.timer", &out);
    if (rc != 0) return {500, "systemctl enable failed: " + out};
    return {200, "auto-update enabled (" + schedule + ")"};
  }
  (void)run_cmd("systemctl daemon-reload", &out);
  int rc = run_cmd(
      "systemctl disable --now onvif-recorder-autoupdate.timer", &out);
  if (rc != 0) return {500, "systemctl disable failed: " + out};
  return {200, "auto-update disabled"};
}

// Escape @p s for embedding inside a JSON string literal.
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Build JSON for GET /api/config: schema + override values + flag defaults.
std::string build_config_json(const Ctx& ctx) {
  auto overrides = runtime_config::ReadFromFile(ctx.config_path);

  std::string j = "{\"entries\":[";
  bool first = true;
  for (const auto& e : runtime_config::Schema()) {
    if (!first) j += ',';
    first = false;
    const char* type_str = "string";
    switch (e.type) {
      case runtime_config::Type::Bool:    type_str = "bool";    break;
      case runtime_config::Type::Int:     type_str = "int";     break;
      case runtime_config::Type::UInt16:  type_str = "uint16";  break;
      case runtime_config::Type::String:  type_str = "string";  break;
    }
    std::string flag_default;
    auto* flag = absl::FindCommandLineFlag(e.name);
    if (flag != nullptr) flag_default = flag->CurrentValue();
    j += "{\"name\":\"";    j += json_escape(e.name);
    j += "\",\"type\":\"";  j += type_str;
    j += "\",\"description\":\""; j += json_escape(e.description);
    j += "\",\"group\":\""; j += json_escape(e.group);
    j += "\",\"flag_default\":\""; j += json_escape(flag_default);
    j += "\",\"override\":\"";
    auto it = overrides.find(e.name);
    j += json_escape(it != overrides.end() ? it->second : std::string());
    j += "\"}";
  }
  j += "]}";
  return j;
}

// Build JSON for GET /api/camera_health.  Single DB round-trip via
// unifi::load_camera_health; emits one row per adopted camera with last
// event time and 1-hour event count.
std::string build_camera_health_json(const Ctx& ctx) {
  const uint64_t now_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  std::string j = "{\"now_ms\":";
  j += std::to_string(now_ms);
  j += ",\"cameras\":[";
  if (ctx.db == nullptr) {
    j += "]}";
    return j;
  }
  auto rows_or = unifi::load_camera_health(*ctx.db);
  if (!rows_or.ok()) {
    LOG(WARNING) << "[admin] load_camera_health: "
                 << rows_or.status().message();
    j += "]}";
    return j;
  }
  // Load the persisted camera_snapshot_via_protect and excluded_cameras
  // settings so the UI can pre-tick the right rows.  Reads the same
  // config.json the config form writes to, so this stays in sync with
  // the tickboxes.  excluded_cameras is keyed by MAC (lower-case,
  // colon-stripped) to match the same normalisation the enforcement
  // side in main.cpp does.
  auto parse_csv_tokens = [](const std::string& s) {
    std::set<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
      size_t comma = s.find(',', start);
      std::string tok = s.substr(start,
          comma == std::string::npos ? std::string::npos : comma - start);
      while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
        tok.erase(tok.begin());
      while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
        tok.pop_back();
      if (!tok.empty()) out.insert(tok);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    return out;
  };
  auto norm_mac = [](std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      if (c == ':' || c == '-' || c == ' ') continue;
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
  };
  std::set<std::string> via_protect_ips;
  std::set<std::string> excluded_macs;
  {
    auto overrides = runtime_config::ReadFromFile(
        ctx.config_path ? ctx.config_path : "");
    auto vit = overrides.find("camera_snapshot_via_protect");
    if (vit != overrides.end()) via_protect_ips = parse_csv_tokens(vit->second);
    auto eit = overrides.find("excluded_cameras");
    if (eit != overrides.end()) {
      for (const auto& m : parse_csv_tokens(eit->second))
        excluded_macs.insert(norm_mac(m));
    }
  }
  bool first = true;
  for (const auto& r : *rows_or) {
    if (!first) j += ',';
    first = false;
    j += "{\"id\":\"";   j += json_escape(r.id);
    j += "\",\"name\":\""; j += json_escape(r.name);
    j += "\",\"host\":\""; j += json_escape(r.host);
    j += "\",\"mac\":\"";  j += json_escape(r.mac);
    j += "\",\"is_third_party\":";
    j += r.is_third_party ? "true" : "false";
    j += ",\"last_event_ms\":";
    j += std::to_string(r.last_event_ms);
    j += ",\"events_1h\":";
    j += std::to_string(r.events_1h);
    if (r.is_third_party) {
      j += ",\"snapshot_via_protect\":";
      j += via_protect_ips.count(r.host) ? "true" : "false";
    }
    // Enabled tickbox state: reflect the persisted excluded_cameras
    // config so the UI can pre-tick the right rows.  Normalised on
    // both sides (lower-case, colon-stripped).
    if (!r.mac.empty()) {
      j += ",\"excluded\":";
      j += excluded_macs.count(norm_mac(r.mac)) ? "true" : "false";
    }
    if (camera_needs_onvif_admin(r.host)) {
      j += ",\"hint\":\"needs_onvif_admin\"";
    }
    j += "}";
  }
  j += "]}";
  return j;
}

// Build JSON for GET /api/recent_events.  Calls unifi::load_recent_events
// and shapes the rows for the admin UI.  Each row carries a
// `thumbnail_url` that the UI can drop into <img src=...>; empty when the
// thumbnail isn't in the DB (MSR-stored events show a placeholder).
std::string build_recent_events_json(const Ctx& ctx) {
  std::string j = "{\"events\":[";
  if (ctx.db == nullptr) {
    j += "]}";
    return j;
  }
  auto rows_or = unifi::load_recent_events(30, *ctx.db);
  if (!rows_or.ok()) {
    LOG(WARNING) << "[admin] load_recent_events: "
                 << rows_or.status().message();
    j += "]}";
    return j;
  }
  bool first = true;
  for (const auto& r : *rows_or) {
    if (!first) j += ',';
    first = false;
    j += "{\"id\":\"";   j += json_escape(r.id);
    j += "\",\"type\":\""; j += json_escape(r.type);
    j += "\",\"camera_id\":\""; j += json_escape(r.camera_id);
    j += "\",\"camera_name\":\""; j += json_escape(r.camera_name);
    j += "\",\"thumbnail_id\":\""; j += json_escape(r.thumbnail_id);
    j += "\",\"sdt\":";
    j += r.smart_detect_types_json.empty()
             ? std::string("[]")
             : r.smart_detect_types_json;
    j += ",\"start_ms\":"; j += std::to_string(r.start_ms);
    j += ",\"end_ms\":";   j += std::to_string(r.end_ms);
    j += ",\"thumbnail_in_db\":";
    j += r.thumbnail_in_db ? "true" : "false";
    j += "}";
  }
  j += "]}";
  return j;
}

// Build JSON for GET /api/first_party_cameras.
std::string build_first_party_json(const Ctx& ctx) {
  std::string j = "{\"cameras\":[";
  if (ctx.db == nullptr) {
    j += "]}";
    return j;
  }
  auto cams_or = unifi::load_all_first_party(*ctx.db);
  if (!cams_or.ok()) {
    LOG(WARNING) << "[admin] load_all_first_party: "
                 << cams_or.status().message();
    j += "]}";
    return j;
  }
  bool first = true;
  for (const auto& c : *cams_or) {
    if (!first) j += ',';
    first = false;
    j += "{\"id\":\"";   j += json_escape(c.id);
    j += "\",\"name\":\""; j += json_escape(c.name);
    j += "\",\"mac\":\"";  j += json_escape(c.mac);
    j += "\"}";
  }
  j += "]}";
  return j;
}

// Handle POST /api/config — body is a JSON object of name->string values.
// Validates each value against its schema entry, writes the file, then
// signals SIGTERM to ourselves so systemd restarts the service with the
// new overrides applied.
std::pair<int, std::string> handle_config(const Ctx& ctx,
                                          const std::string& body) {
  // Start from what is already persisted and overlay only the keys this
  // request actually carries.  The admin page posts to this endpoint from
  // several independent controls -- the Configuration card's "Save &
  // Restart", the Camera Health card's via-Protect tickboxes, and the
  // Enabled tickboxes -- and each sends only its own fields.  Rebuilding
  // the whole file from a single request wrote every other key back as
  // empty, so saving one section silently wiped the others (issue #46).
  std::map<std::string, std::string> values =
      runtime_config::ReadFromFile(ctx.config_path);
  for (const auto& e : runtime_config::Schema()) {
    auto raw_opt = json_string_field_opt(body, e.name);
    if (!raw_opt.has_value()) continue;  // absent -> keep existing value
    const std::string& raw = *raw_opt;
    // Validate non-empty values against the flag's parser.  An empty
    // value is the user clearing the setting, which is always allowed.
    if (!raw.empty()) {
      auto* flag = absl::FindCommandLineFlag(e.name);
      if (flag == nullptr) continue;  // schema/flag mismatch -- skip
      std::string err;
      // ParseFrom mutates the flag, but we only call it for validation.
      // The change is overwritten on the next process restart by
      // LoadFromFile reading the persisted JSON.
      if (!flag->ParseFrom(raw, &err)) {
        return {400, std::string("invalid ") + e.name + ": " + err};
      }
    }
    values[e.name] = raw;
  }
  auto s = runtime_config::WriteToFile(ctx.config_path, values);
  if (!s.ok()) return {500, std::string(s.message())};

  // Detach a child to send SIGTERM after we've returned the response, so
  // the client sees a clean 200 before the service exits and systemd
  // restarts it.
  pid_t self = getpid();
  if (fork() == 0) {
    // Child: brief delay then signal the parent.
    sleep(1);
    kill(self, SIGTERM);
    _exit(0);
  }
  return {200, "saved; service is restarting"};
}

// Build a tar.gz diagnostic archive in @p out_path.  Contents:
//   - config.json        copy of /etc/onvif-recorder/config.json
//   - status.json        output of /api/status
//   - camera_health.json output of /api/camera_health
//   - journal.log        journalctl -u onvif-recorder --since "12 hours ago"
//   - journal-prev.log   journalctl -u onvif-recorder -b -1 (previous boot)
//   - service-status.txt systemctl status onvif-recorder
//   - raw-onvif.jsonl    in-memory ring of recent SOAP exchanges (always on)
//   - event-log.jsonl    tail of --event_log file (last 5000 lines) if set
//   - nginx-error.log    tail of /var/log/nginx/error.log
//   - dpkg.txt           dpkg-query -W onvif-recorder, unifi-protect, etc.
//   - dmesg.log          kernel ring buffer (dmesg -T) -- catches hardware
//                        issues invisible to userspace: SATA link resets,
//                        OOM kills, filesystem errors, USB hot-unplug, etc.
//   - system.txt         uname -a, /sys/firmware/devicetree/base/model, free, df
// Full row dump from the Protect `cameras` table, keyed by the columns
// that matter for triage.  Uses jsonb minus the large opaque-blob
// columns to keep file size sane and let a triager `jq` through it
// without hunting through hundreds of KB per row.  Sanitiser handles
// the interesting bits inside the JSON:
//   * mac / id / channels.<any>.mac -> mac-<hash> / id-<hash>
//   * embedded RTSP URLs with creds -> [REDACTED]:[REDACTED]@ (see
//     the url_creds rule extension for rtsps? / rtmp schemes).
//   * host IPs -> per-prefix remap.
//   * camera names -> Camera-<hash> (registered at dump start).
std::string build_cameras_json(const Ctx& ctx) {
  if (ctx.db == nullptr) return "[]";
  const std::string conn_str = unifi::internal::build_connstr(*ctx.db);
  PGconn* c = PQconnectdb(conn_str.c_str());
  if (PQstatus(c) != CONNECTION_OK) {
    PQfinish(c);
    return "[]";
  }
  // Strip the largest opaque-blob columns to keep the output triage-
  // friendly.  A missing column produces NULL in the row jsonb (the
  // `-` operator is a no-op on absent keys), so the query stays
  // schema-tolerant across Protect versions.
  static const char* kSql =
      "SELECT COALESCE("
      "  jsonb_agg(row_data ORDER BY tp DESC NULLS LAST, name)::text, "
      "  '[]'"
      ") FROM ("
      "  SELECT to_jsonb(c) "
      "         - 'ispSettings' - 'talkbackSettings' "
      "         - 'smartDetectSettings' - 'smartDetectLines' "
      "         - 'smartDetectZones' - 'audioSettings' "
      "         - 'ledSettings' - 'osdSettings' - 'pirSettings' "
      "         - 'ptzSettings' - 'speakerSettings' - 'lcdMessage' "
      "         - 'featureFlags' - 'hdrMode' - 'videoMode' "
      "         - 'homekitSettings' - 'recordingSettings' "
      "         - 'wifiConnectionState' - 'stats' - 'timelapseSettings' "
      "    AS row_data, "
      "    \"isThirdPartyCamera\" AS tp, name "
      "  FROM cameras c "
      ") q";
  PGresult* r = PQexec(c, kSql);
  std::string out = "[]";
  if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0 &&
      !PQgetisnull(r, 0, 0)) {
    out = PQgetvalue(r, 0, 0);
  }
  PQclear(r);
  PQfinish(c);
  return out;
}

// Per-camera recordingFiles aggregate: 1 h count, 24 h count, all-time
// count, and the min/max start/end timestamps.  Directly answers "is
// Protect recording video for this camera?" without a triager having
// to guess from event counts.
std::string build_recording_files_json(const Ctx& ctx) {
  if (ctx.db == nullptr) return "[]";
  const std::string conn_str = unifi::internal::build_connstr(*ctx.db);
  PGconn* c = PQconnectdb(conn_str.c_str());
  if (PQstatus(c) != CONNECTION_OK) {
    PQfinish(c);
    return "[]";
  }
  static const char* kSql =
      "SELECT COALESCE("
      "  jsonb_agg(row_data ORDER BY cid)::text, '[]'"
      ") FROM ("
      "  SELECT \"cameraId\" AS cid, jsonb_build_object("
      "    'cameraId', \"cameraId\", "
      "    'count_1h', COUNT(*) FILTER (WHERE start > ("
      "        extract(epoch from now())*1000 - 3600000)::bigint), "
      "    'count_24h', COUNT(*) FILTER (WHERE start > ("
      "        extract(epoch from now())*1000 - 86400000)::bigint), "
      "    'count_total', COUNT(*), "
      "    'min_start_ms', MIN(start), "
      "    'max_end_ms', MAX(\"end\")"
      "  ) AS row_data "
      "  FROM \"recordingFiles\" "
      "  GROUP BY \"cameraId\""
      ") q";
  PGresult* r = PQexec(c, kSql);
  std::string out = "[]";
  if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0 &&
      !PQgetisnull(r, 0, 0)) {
    out = PQgetvalue(r, 0, 0);
  }
  PQclear(r);
  PQfinish(c);
  return out;
}

// Count occurrences of @p needle within @p haystack.  Simple linear
// scan; the log files we scan are bounded to a few MB apiece.
int count_occurrences(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return 0;
  int n = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++n;
    pos += needle.size();
  }
  return n;
}

// Read every 8-hex-char identifier of the form `<prefix>-XXXXXXXX` out
// of @p text into @p out (deduped).  Used to enumerate the hashed
// cameraIds / macs the sanitiser produced elsewhere in the dump.
void collect_hashes(const std::string& text, const char* prefix,
                     std::set<std::string>* out) {
  const size_t plen = std::strlen(prefix);
  size_t pos = 0;
  while ((pos = text.find(prefix, pos)) != std::string::npos) {
    size_t start = pos + plen;
    if (start + 8 > text.size()) break;
    bool all_hex = true;
    for (size_t i = 0; i < 8; ++i) {
      const char c = text[start + i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
        all_hex = false;
        break;
      }
    }
    if (all_hex) out->insert(text.substr(pos, plen + 8));
    pos = start;
  }
}

// Cross-reference summary: for every hashed cameraId / MAC we found in
// the dump, count occurrences of a small set of error-marker strings
// across the already-sanitised captures.  Answers "which camera
// dominates the error signal in this dump?" without a triager having
// to open five log files and correlate by hand.
//
// Consumes files already written to @p dump_dir, so it MUST run after
// the captures at the end of build_diagnostic_dump().
// Small string-scanning helpers for consuming the JSONB output that
// build_cameras_json / build_recording_files_json produced earlier in
// the dump.  We avoid pulling in a real JSON library because the
// producer side is under our control and the shape is stable.

// Iterate top-level `{...}` objects within a JSON array text, invoking
// @p on_object with a view into each.  Handles nested braces + quoted
// strings so nested objects (thirdPartyCameraInfo, ptz, etc.) don't
// throw off the depth counter.
static void for_each_top_level_object(std::string_view arr,
    const std::function<void(std::string_view)>& on_object) {
  size_t pos = 0;
  while (pos < arr.size()) {
    const size_t start = arr.find('{', pos);
    if (start == std::string_view::npos) return;
    int depth = 0;
    size_t i = start;
    while (i < arr.size()) {
      const char c = arr[i];
      if (c == '"') {
        // Skip past the string, honouring backslash escapes.
        ++i;
        while (i < arr.size() && arr[i] != '"') {
          if (arr[i] == '\\' && i + 1 < arr.size()) ++i;
          ++i;
        }
      } else if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
        if (depth == 0) {
          on_object(arr.substr(start, i - start + 1));
          pos = i + 1;
          break;
        }
      }
      ++i;
    }
    if (i >= arr.size()) return;
  }
}

// Fetch `"key": "value"` from a JSON object view.  Returns empty when
// the key is absent or its value is null / non-string.
static std::string json_string_field(std::string_view obj, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t p0 = obj.find(needle);
  if (p0 == std::string_view::npos) return {};
  size_t p = p0 + needle.size();
  while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
  if (p >= obj.size()) return {};
  if (obj[p] != '"') return {};
  ++p;
  const size_t q = obj.find('"', p);
  if (q == std::string_view::npos) return {};
  return std::string(obj.substr(p, q - p));
}

// Fetch `"key": <int>` from a JSON object view.  Returns 0 on absence.
static int64_t json_int_field(std::string_view obj, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t p0 = obj.find(needle);
  if (p0 == std::string_view::npos) return 0;
  size_t p = p0 + needle.size();
  while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
  if (p >= obj.size()) return 0;
  const int sign = (obj[p] == '-') ? (++p, -1) : 1;
  int64_t v = 0;
  bool any = false;
  while (p < obj.size() && obj[p] >= '0' && obj[p] <= '9') {
    v = v * 10 + (obj[p] - '0');
    ++p; any = true;
  }
  return any ? v * sign : 0;
}

// Fetch `"key": true|false` from a JSON object view; @p dflt for
// absence or non-boolean values.  null returns @p dflt.
static bool json_bool_field(std::string_view obj, const char* key, bool dflt) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t p0 = obj.find(needle);
  if (p0 == std::string_view::npos) return dflt;
  size_t p = p0 + needle.size();
  while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
  if (obj.substr(p, 4) == "true")  return true;
  if (obj.substr(p, 5) == "false") return false;
  return dflt;
}

// Fetch a nested `"key": {...}` sub-object as a string_view into @p obj.
// Returns empty on absence or non-object value.
static std::string_view json_object_field(
    std::string_view obj, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t p0 = obj.find(needle);
  if (p0 == std::string_view::npos) return {};
  size_t p = p0 + needle.size();
  while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
  if (p >= obj.size() || obj[p] != '{') return {};
  const size_t start = p;
  int depth = 0;
  while (p < obj.size()) {
    const char c = obj[p];
    if (c == '"') {
      ++p;
      while (p < obj.size() && obj[p] != '"') {
        if (obj[p] == '\\' && p + 1 < obj.size()) ++p;
        ++p;
      }
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) return obj.substr(start, p - start + 1);
    }
    ++p;
  }
  return {};
}

std::string build_triage_json(const std::string& dump_dir) {
  const auto read = [&dump_dir](const char* name) -> std::string {
    std::ifstream f(dump_dir + "/" + name, std::ios::binary);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const std::string api      = read("protect-api.log");
  const std::string grpc     = read("protect-grpc-client.log");
  const std::string addon    = read("protect-addon.log");
  const std::string cameras  = read("cameras.json");
  const std::string rec      = read("recording_files.json");

  // Per-camera row.  Consolidates the id- and mac- hashes of a single
  // camera into one entry (they used to be emitted separately, which
  // meant every camera occupied two rows in the output and doubled the
  // work triagers had to do), and carries enough metadata to attach a
  // human-readable hint on the failure mode.
  struct Row {
    std::string id_key;         // "id-<8hex>" -- may be empty
    std::string mac_key;        // "mac-<8hex>" -- may be empty
    std::string name;           // "Camera-<8hex>"
    std::string type;           // "Reolink", "Amcrest", ...
    std::string host;           // sanitised host
    bool        is_third_party = false;
    bool        is_connected   = true;
    bool        has_substream  = true;
    bool        creds_blank    = false;
    // Recording file coverage from recording_files.json (0 if absent).
    int64_t     rec_count_1h   = 0;
    int64_t     rec_count_24h  = 0;
    int64_t     rec_count_total = 0;
    // Cross-referenced error counters (both hashes contribute).
    int         snap_err  = 0;
    int         lost_rec  = 0;
    int         list_frag = 0;
    int score() const { return snap_err + lost_rec + list_frag; }
  };
  std::vector<Row> rows;

  // Parse cameras.json -> one Row per camera.
  for_each_top_level_object(cameras, [&](std::string_view obj) {
    Row r;
    r.id_key         = json_string_field(obj, "id");
    r.mac_key        = json_string_field(obj, "mac");
    r.name           = json_string_field(obj, "name");
    r.type           = json_string_field(obj, "type");
    r.host           = json_string_field(obj, "host");
    r.is_third_party = json_bool_field(obj, "isThirdPartyCamera", false);
    r.is_connected   = json_bool_field(obj, "isConnected", true);
    // thirdPartyCameraInfo shape check: rtspUrlLQ null means Protect has
    // only a main-stream URL for this camera; credentials of literal " "
    // are the broken-adoption marker (issue #34 gleep52-style).
    if (r.is_third_party) {
      const auto tpi = json_object_field(obj, "thirdPartyCameraInfo");
      if (!tpi.empty()) {
        r.has_substream = !json_string_field(tpi, "rtspUrlLQ").empty();
        const std::string u = json_string_field(tpi, "username");
        const std::string p = json_string_field(tpi, "password");
        auto blank = [](const std::string& s) {
          for (char c : s) if (c != ' ' && c != '\t') return false;
          return true;
        };
        r.creds_blank = blank(u) || blank(p);
      }
    }
    rows.push_back(std::move(r));
  });

  // Layer recording_files.json onto the rows keyed by cameraId (id_key).
  {
    std::unordered_map<std::string, Row*> by_id;
    by_id.reserve(rows.size());
    for (auto& r : rows) if (!r.id_key.empty()) by_id[r.id_key] = &r;
    for_each_top_level_object(rec, [&](std::string_view obj) {
      const std::string cam = json_string_field(obj, "cameraId");
      auto it = by_id.find(cam);
      if (it == by_id.end()) return;
      it->second->rec_count_1h    = json_int_field(obj, "count_1h");
      it->second->rec_count_24h   = json_int_field(obj, "count_24h");
      it->second->rec_count_total = json_int_field(obj, "count_total");
    });
  }

  // Index every non-empty key (id + mac) to its owning row so a single
  // log-file scan can increment the right counter for either identifier.
  std::unordered_map<std::string_view, Row*> row_by_key;
  row_by_key.reserve(rows.size() * 2);
  for (auto& r : rows) {
    if (!r.id_key.empty())  row_by_key[r.id_key]  = &r;
    if (!r.mac_key.empty()) row_by_key[r.mac_key] = &r;
  }

  // Cross-reference each source-file / needle in ONE pass per file.
  // Per-line: cheap needle check first; only iterate the row map when
  // the needle actually matches.
  auto scan_file =
      [&row_by_key](const std::string& haystack,
                    std::string_view needle,
                    int Row::*counter) {
    if (haystack.empty() || needle.empty()) return;
    size_t line_start = 0;
    while (line_start < haystack.size()) {
      const size_t nl = haystack.find('\n', line_start);
      const size_t line_end =
          (nl == std::string::npos) ? haystack.size() : nl;
      const std::string_view line(haystack.data() + line_start,
                                  line_end - line_start);
      if (line.find(needle) != std::string_view::npos) {
        for (auto& kv : row_by_key) {
          if (line.find(kv.first) != std::string_view::npos) {
            ++(kv.second->*counter);
          }
        }
      }
      if (nl == std::string::npos) break;
      line_start = nl + 1;
    }
  };
  scan_file(api,   "Snapshot not available",       &Row::snap_err);
  scan_file(grpc,  "Lost recording notification",  &Row::lost_rec);
  scan_file(addon, "ListFragments",                &Row::list_frag);

  // Compute a per-row hint capturing the most likely diagnosis.  Order
  // matters: pick the strongest signal first so triagers see a single
  // clear message rather than a shopping list.
  auto compute_hint = [](const Row& r) -> std::string {
    // Credentials are sanitised in cameras.json (username/password
    // replaced with "[REDACTED]"), so we can't reliably detect the
    // literal-whitespace-creds case from the dump.  That signal is
    // instead surfaced at service start by unifi_camera_config
    // (see WARNING lines in journal.log).
    if (r.is_third_party) {
      if (!r.is_connected)
        return "Protect reports the camera as disconnected "
               "(cameras.isConnected=false)";
      if (r.snap_err > 0 && r.rec_count_24h == 0)
        return "Protect isn't recording this camera (0 recordingFiles "
               "in the last 24h) -- check RTSP adoption / substream";
      if (!r.has_substream)
        return "no substream (thirdPartyCameraInfo.rtspUrlLQ is null); "
               "Protect struggles with playback for this shape";
      if (r.lost_rec > 0)
        return "MSR is dropping recording segments for this camera "
               "(Lost recording notification hits) -- typically an "
               "unstable RTSP stream";
    }
    if (r.snap_err > 0 && r.score() == r.snap_err)
      return "camera has snapshot-fetch failures (may be an intermittent "
             "concurrent-HTTP-session limit on the camera side)";
    return {};
  };

  // Emit: third-party rows first (they're what #34-shaped reports are
  // usually about), then by score desc, then by name for stable output.
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.is_third_party != b.is_third_party)
      return a.is_third_party;  // third-party first
    if (a.score() != b.score()) return a.score() > b.score();
    return a.name < b.name;
  });

  std::string j = "{\"per_camera\":[";
  bool first = true;
  for (const Row& r : rows) {
    if (!first) j += ',';
    first = false;
    j += "{\"name\":\"";     j += r.name;
    j += "\",\"type\":\"";   j += r.type;
    j += "\",\"host\":\"";   j += r.host;
    j += "\",\"is_third_party\":";
    j += r.is_third_party ? "true" : "false";
    if (!r.id_key.empty())  { j += ",\"id\":\"";  j += r.id_key;  j += "\""; }
    if (!r.mac_key.empty()) { j += ",\"mac\":\""; j += r.mac_key; j += "\""; }
    j += ",\"snapshot_errors\":";
    j += std::to_string(r.snap_err);
    j += ",\"lost_recording_notifications\":";
    j += std::to_string(r.lost_rec);
    j += ",\"list_fragments_lines\":";
    j += std::to_string(r.list_frag);
    j += ",\"recording_files_24h\":";
    j += std::to_string(r.rec_count_24h);
    const std::string hint = compute_hint(r);
    if (!hint.empty()) {
      j += ",\"hint\":\"";
      j += hint;
      j += "\"";
    }
    j += "}";
  }
  j += "],";
  // Global totals -- handy top-of-file summary + phantom detection.
  std::set<std::string> ids_seen, macs_seen;
  collect_hashes(cameras, "id-",  &ids_seen);
  collect_hashes(cameras, "mac-", &macs_seen);
  j += "\"totals\":{";
  j += "\"snapshot_errors\":" +
       std::to_string(count_occurrences(api, "Snapshot not available"));
  j += ",\"lost_recording_notifications\":" +
       std::to_string(count_occurrences(grpc,
           "Lost recording notification"));
  j += ",\"list_fragments_lines\":" +
       std::to_string(count_occurrences(addon, "ListFragments"));
  j += ",\"cameras_in_dump\":"     + std::to_string(rows.size());
  j += ",\"distinct_ids_seen\":"   + std::to_string(ids_seen.size());
  j += ",\"distinct_macs_seen\":"  + std::to_string(macs_seen.size());
  j += "}}";
  return j;
}

// Returns ok status when the tarball exists at out_path.
absl::Status build_diagnostic_dump(const Ctx& ctx,
                                   const std::string& out_path) {
  char tmpl[] = "/tmp/onvif-dump.XXXXXX";
  if (mkdtemp(tmpl) == nullptr)
    return absl::InternalError("mkdtemp failed");
  const std::string dir = tmpl;

  // One sanitiser shared across every file in this dump so a given IP /
  // credential maps to the same redacted value across files.
  DumpSanitizer san;

  // Pre-register user-given camera names so they get redacted in every
  // file the sanitiser touches (camera_health.json, recent_events.json,
  // journal.log, raw-onvif.jsonl).  User-chosen names are personally
  // identifying (real people / room labels) and were leaking through
  // diagnostic dumps before this hook.
  if (ctx.db != nullptr) {
    auto rows_or = unifi::load_camera_health(*ctx.db);
    if (rows_or.ok()) {
      for (const auto& r : *rows_or) {
        san.register_camera_name(r.name);
      }
    }
  }

  // Helper: sanitise then write content to a file inside dir.
  auto write = [&dir, &san](const char* name, const std::string& content) {
    std::ofstream f(dir + "/" + name, std::ios::binary | std::ios::trunc);
    if (f.is_open()) f << san.sanitize(content);
  };
  // Helper: capture the stdout of a shell command into a sanitised file.
  // 8 MiB cap per file keeps a verbose 12-hour journal intact (journald
  // averages ~150 B/line on this service so 8 MiB ≈ 50 k lines) while
  // still bounding worst-case memory.
  auto capture = [&dir, &san](const char* name, const std::string& cmd,
                              size_t max_bytes = 3 * 1024 * 1024) {
    std::string out;
    run_cmd(cmd, &out, max_bytes);
    std::ofstream f(dir + "/" + name, std::ios::binary | std::ios::trunc);
    if (f.is_open()) f << san.sanitize(out);
  };

  write("config.json",
        redact_url_queries(read_file(ctx.config_path ? ctx.config_path : ""),
                           {"thumbnail_hook_url"}));
  write("status.json", build_status_json(ctx));
  write("camera_health.json", build_camera_health_json(ctx));
  // Per-camera Protect DB rows and recordingFiles aggregates.  Both are
  // JSON blobs coming straight from Postgres via jsonb_agg / jsonb_
  // build_object; the standard DumpSanitizer pipeline redacts embedded
  // MACs, Mongo IDs, UUIDs, RTSP credentials, and remaps IPs before
  // they land on disk.
  write("cameras.json",         build_cameras_json(ctx));
  write("recording_files.json", build_recording_files_json(ctx));
  // 3h window covers the typical "just tried it and it broke" report
  // without blowing past journald's default retention on UDM.  Larger
  // windows on installs with high event volume produced 8+ MiB of log
  // that then dominated dump-generation CPU on 1.6.12 (every byte
  // hits the full sanitiser regex pipeline); users saw the box hang.
  capture("journal.log",
          "journalctl -u onvif-recorder --since '3 hours ago' "
          "--no-pager 2>/dev/null");
  // Previous-boot journal — invaluable when the service crashed and is
  // the very thing the user is reporting.  Falls back gracefully if
  // there is no prior boot recorded.
  capture("journal-prev.log",
          "journalctl -u onvif-recorder -b -1 --no-pager 2>/dev/null "
          "|| echo '(no previous boot recorded)'");
  capture("service-status.txt",
          "systemctl status onvif-recorder --no-pager -l 2>/dev/null");
  capture("nginx-error.log",
          "tail -n 500 /var/log/nginx/error.log 2>/dev/null "
          "|| echo '(nginx error log not readable)'");
  // Protect-side logs.  Highest triage value for "video not found" /
  // preview / stream-URL bug reports.  All go through the same
  // DumpSanitizer pipeline as everything else in this dump — MACs,
  // Mongo-style IDs, UUIDs, session tokens, and Sentry DSNs are
  // hashed or blanked before the tarball is written.
  //   protect-addon.log        UBV ListFragments trace (does video exist?)
  //   protect-api.log          Every Protect API request from web/mobile
  //   protect-grpc-client.log  Protect app -> MSR gRPC calls
  //   protect-app.log          Bootstrap + middleware lifecycle
  //   mst.log                  Media Server Transcoder decode/rescale ops
  // Protect-log tails.  Prior sizes (5000 lines on api.log, 2000 on
  // three others) drove dump-generation CPU to 100% on installs with
  // large Protect log volumes -- every captured file goes through the
  // full DumpSanitizer regex pipeline (~15 passes) before landing on
  // disk, so file bytes translate directly into CPU seconds.  Halved
  // across the board here; triage.json's new single-pass cross-
  // referencing recovers the coverage we lose.
  capture("protect-addon.log",
          "tail -n 1000 /srv/unifi-protect/logs/addon.log 2>/dev/null "
          "|| echo '(addon.log not readable)'");
  capture("protect-api.log",
          "tail -n 1500 /srv/unifi-protect/logs/api.log 2>/dev/null "
          "|| echo '(api.log not readable)'");
  capture("protect-grpc-client.log",
          "tail -n 1000 /srv/unifi-protect/logs/GRPC.client.log 2>/dev/null "
          "|| echo '(GRPC.client.log not readable)'");
  capture("protect-app.log",
          "tail -n 500 /srv/unifi-protect/logs/app.log 2>/dev/null "
          "|| echo '(app.log not readable)'");
  capture("mst.log",
          "tail -n 1000 /srv/ms/logs/mst.log 2>/dev/null "
          "|| echo '(mst.log not readable)'");
  // Triage summary -- per-camera error counts across the sanitised
  // captures above.  Must run AFTER those writes so it reads back the
  // hashed identifiers.  Idempotent under sanitize(): the file only
  // contains hashes already, so passing through the pipeline is a
  // no-op.
  write("triage.json", build_triage_json(dir));
  // Raw ONVIF SOAP exchange log — sourced from the always-on in-memory
  // ring (see RawSink in onvif_listener).  We cap the snapshot to the
  // last ~2 MiB before sanitising because the compressed 64 MiB ring
  // can decompress to 100+ MiB of SOAP XML and that dominated dump-
  // generation CPU on 1.6.12 (every byte hits the full sanitiser regex
  // pipeline).  Whole-line preserving: back up to the previous '\n'
  // so no partial JSON object leaks.  --raw_log additionally tees
  // uncompressed to disk for long-form captures.
  {
    std::string raw = RawSink::instance().snapshot();
    constexpr size_t kRawCapBytes = 2 * 1024 * 1024;
    if (raw.size() > kRawCapBytes) {
      const size_t drop = raw.size() - kRawCapBytes;
      const size_t nl = raw.find('\n', drop);
      raw = (nl == std::string::npos)
                ? std::string()
                : raw.substr(nl + 1);
    }
    write("raw-onvif.jsonl", raw);
  }
  // Parsed-event log (--event_log) is still file-based; tail last 5000
  // lines when the flag is set, skip silently otherwise.
  if (ctx.event_log_path && ctx.event_log_path[0] != '\0') {
    capture("event-log.jsonl",
            std::string("tail -n 5000 ") + ctx.event_log_path
            + " 2>/dev/null || echo '(file not readable: "
            + ctx.event_log_path + ")'");
  }
  capture("dpkg.txt",
          "dpkg-query -W -f='${Package} ${Version}\\n' "
          "onvif-recorder unifi-protect unifi-core uos 2>/dev/null");
  // Kernel ring buffer.  -T renders human-readable timestamps so triagers
  // can correlate events without converting jiffies.  Falls back to plain
  // dmesg if -T is unsupported by the kernel.  The kernel ring buffer is
  // itself bounded (typically 1 MiB), well within the 8 MiB capture cap;
  // the DumpSanitizer redacts MAC/IPv4/credentials that may appear in
  // hardware-probe lines.
  capture("dmesg.log",
          "dmesg -T 2>/dev/null || dmesg 2>/dev/null "
          "|| echo '(dmesg not readable -- requires CAP_SYSLOG)'");
  capture("system.txt",
          "uname -a; "
          "echo '---'; "
          "cat /sys/firmware/devicetree/base/model 2>/dev/null; "
          "echo; "
          "free -h 2>/dev/null; "
          "echo '---'; "
          "df -h /var /srv 2>/dev/null");

  // Note in the archive that sanitisation was applied -- so a recipient
  // sees obviously redacted values and knows IP remapping is consistent
  // within this dump but not across dumps.
  {
    std::ofstream f(dir + "/SANITIZED.txt",
                    std::ios::binary | std::ios::trunc);
    if (f.is_open()) {
      f << "This archive was sanitised before tarballing.\n"
        << "  - IPv4 addresses are remapped via a per-prefix counter\n"
        << "    (1.1.1.1, 1.1.1.2, 1.1.2.1, ...) so the same source IP\n"
        << "    maps to the same value across files in this dump.\n"
        << "  - Usernames, passwords, password digests, nonces, URL\n"
        << "    credentials, Basic auth headers, and the X-UserId\n"
        << "    Protect bypass token are replaced with [REDACTED].\n"
        << "  - The mapping is regenerated for every dump, so values in\n"
        << "    different archives are NOT comparable.\n";
    }
  }

  const std::string cmd = "tar czf " + out_path + " -C " + dir + " . "
                          "&& rm -rf " + dir;
  std::string out;
  int rc = run_cmd(cmd, &out);
  if (rc != 0)
    return absl::InternalError("tar/rm failed: " + out);
  return absl::OkStatus();
}

// Read a binary file fully into a string.
std::string read_file_binary(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Handle POST /api/uninstall — fire apt-get remove in the background.
std::pair<int, std::string> handle_uninstall() {
  // Detach: the running process is about to be stopped by apt-get.
  std::string out;
  int rc = run_cmd(
      "systemd-run --unit=onvif-recorder-uninstall "
      "--no-block apt-get remove -y onvif-recorder",
      &out);
  if (rc != 0) return {500, "systemd-run failed: " + out};
  return {200, "uninstall scheduled"};
}

MHD_Result handler(
    void* cls,
    struct MHD_Connection* connection,
    const char* url,
    const char* method,
    const char* /*version*/,
    const char* upload_data,
    size_t* upload_data_size,
    void** con_cls) {
  auto* ctx = static_cast<Ctx*>(cls);
  const bool is_post = std::strcmp(method, "POST") == 0;
  // Treat HEAD identically to GET for dispatch; libmicrohttpd strips the
  // body automatically on HEAD responses.  Without this, HEAD requests
  // (used by some health-check probes and curl -I) fall through to 404.
  const bool is_get  = std::strcmp(method, "GET") == 0 ||
                       std::strcmp(method, "HEAD") == 0;

  // libmicrohttpd calls us at least twice for POST: once to begin, then for
  // each chunk of upload_data.  Accumulate into a PostCtx.
  if (is_post) {
    if (*con_cls == nullptr) {
      *con_cls = new PostCtx();
      return MHD_YES;
    }
    auto* pc = static_cast<PostCtx*>(*con_cls);
    if (*upload_data_size > 0) {
      pc->body.append(upload_data, *upload_data_size);
      *upload_data_size = 0;
      return MHD_YES;
    }
  }

  // Dispatch.
  int status = 200;
  std::string body;
  std::string content_type = "text/plain; charset=utf-8";

  if (is_get &&
      (std::strcmp(url, "/admin/") == 0 || std::strcmp(url, "/admin") == 0 ||
       std::strcmp(url, "/") == 0)) {
    body = kAdminHtml;
    content_type = "text/html; charset=utf-8";
  } else if (is_get &&
             std::strcmp(url, "/api/status") == 0) {
    body = build_status_json(*ctx);
    content_type = "application/json";
  } else if (is_get &&
             std::strcmp(url, "/api/config") == 0) {
    body = build_config_json(*ctx);
    content_type = "application/json";
  } else if (is_get &&
             std::strcmp(url, "/api/first_party_cameras") == 0) {
    body = build_first_party_json(*ctx);
    content_type = "application/json";
  } else if (is_get &&
             std::strcmp(url, "/api/camera_health") == 0) {
    body = build_camera_health_json(*ctx);
    content_type = "application/json";
  } else if (is_get &&
             std::strcmp(url, "/api/recent_events") == 0) {
    body = build_recent_events_json(*ctx);
    content_type = "application/json";
  } else if (is_get &&
             std::strcmp(url, "/api/recent_errors") == 0) {
    // Serve the last 20 ERROR-level lines from the shared log ring so
    // the admin page can render them without shipping the full 1 MiB
    // dump.  Plain text keeps the client-side rendering trivial.
    if (ctx->log_ring) {
      body = ctx->log_ring->tail_errors(20);
    } else {
      body = "";
    }
    content_type = "text/plain; charset=utf-8";
  } else if (is_get &&
             std::strcmp(url, "/api/pg_stats") == 0) {
    // Optional ?reset=1 clears the counters after emitting -- handy for
    // before/after comparisons.  We emit first so the caller sees the
    // last window before it's zeroed.
    body = onvif::pg::StatsAsJson(/*top_n=*/50);
    const char* reset = MHD_lookup_connection_value(
        connection, MHD_GET_ARGUMENT_KIND, "reset");
    if (reset != nullptr && (reset[0] == '1' || reset[0] == 't')) {
      onvif::pg::ResetStats();
    }
    content_type = "application/json";
  } else if (is_get &&
             std::strncmp(url, "/api/thumbnail", 14) == 0) {
    // /api/thumbnail?id=<thumb_id>.  IDs of length 24 live in the Protect
    // thumbnails table (read directly via libpq); other IDs are MSR-stored
    // UBV thumbnails which we proxy through Protect's local API at
    // <protect_url>/api/thumbnails/<id> using the cached X-UserId header.
    const char* qs = MHD_lookup_connection_value(
        connection, MHD_GET_ARGUMENT_KIND, "id");
    if (qs == nullptr || qs[0] == '\0') {
      status = 400;
      body = "missing id";
    } else {
      bool served = false;
      if (ctx->db != nullptr && std::strlen(qs) == 24) {
        auto bytes_or = unifi::load_thumbnail_bytes(qs, *ctx->db);
        if (!bytes_or.ok()) {
          status = 500;
          body = std::string(bytes_or.status().message());
          served = true;
        } else if (!bytes_or->empty()) {
          body.assign(reinterpret_cast<const char*>(bytes_or->data()),
                      bytes_or->size());
          content_type = "image/jpeg";
          served = true;
        }
      }
      if (!served) {
        std::string jpeg = fetch_protect_thumbnail(
            ctx->protect_url ? ctx->protect_url : "",
            ctx->protect_user_id_provider,
            qs);
        if (!jpeg.empty()) {
          body = std::move(jpeg);
          content_type = "image/jpeg";
        } else {
          status = 404;
          body = "thumbnail not available";
        }
      }
    }
  } else if (is_get &&
             std::strcmp(url, "/api/diagnostic_dump") == 0) {
    const std::string tar_path = "/tmp/onvif-dump.tar.gz";
    auto s = build_diagnostic_dump(*ctx, tar_path);
    if (!s.ok()) {
      status = 500;
      body = std::string(s.message());
    } else {
      body = read_file_binary(tar_path);
      std::remove(tar_path.c_str());
      content_type = "application/gzip";
    }
  } else if (is_get &&
             std::strcmp(url, "/api/mallocz") == 0) {
    // tcmalloc heap stats — direct dump from MallocExtension.
    // Free; reads atomic counters maintained by the allocator.
    char buf[16384];
    MallocExtension::instance()->GetStats(buf, sizeof(buf));
    body = buf;
    content_type = "text/plain; charset=utf-8";
  } else if (is_get &&
             std::strcmp(url, "/api/contentionz") == 0) {
    // Per-mutex contention stats from the always-on absl::Mutex
    // tracer.  Format: fixed-width text table.
    body = ContentionProfiler::instance().snapshot();
    content_type = "text/plain; charset=utf-8";
  } else if (is_get &&
             std::strcmp(url, "/api/cpuz") == 0) {
    // Periodic CPU sample dump.  Empty when --cpu_profile_hz=0
    // (the default).  Flip on via the admin Configuration card.
    body = CpuProfiler::instance().snapshot();
    content_type = "text/plain; charset=utf-8";
  } else if (is_post) {
    auto* pc = static_cast<PostCtx*>(*con_cls);
    const std::string& pb = pc->body;
    std::pair<int, std::string> r{404, "not found"};
    if (std::strcmp(url, "/api/channel") == 0)
      r = handle_channel(*ctx, pb);
    else if (std::strcmp(url, "/api/check") == 0)
      r = handle_check();
    else if (std::strcmp(url, "/api/refresh_apt") == 0)
      r = handle_refresh_apt(*ctx);
    else if (std::strcmp(url, "/api/autoupdate") == 0)
      r = handle_autoupdate(pb);
    else if (std::strcmp(url, "/api/config") == 0)
      r = handle_config(*ctx, pb);
    else if (std::strcmp(url, "/api/uninstall") == 0)
      r = handle_uninstall();
    status = r.first;
    body = std::move(r.second);
    delete pc;
    *con_cls = nullptr;
  } else {
    status = 404;
    body = "not found";
  }

  struct MHD_Response* resp = MHD_create_response_from_buffer(
      body.size(),
      const_cast<char*>(body.data()),
      MHD_RESPMEM_MUST_COPY);
  MHD_add_response_header(resp, "Content-Type", content_type.c_str());
  MHD_add_response_header(resp, "Cache-Control", "no-store");
  MHD_Result ret = MHD_queue_response(connection, status, resp);
  MHD_destroy_response(resp);
  return ret;
}

void request_completed(void* /*cls*/,
                       struct MHD_Connection* /*conn*/,
                       void** con_cls,
                       enum MHD_RequestTerminationCode /*toe*/) {
  if (*con_cls) {
    delete static_cast<PostCtx*>(*con_cls);
    *con_cls = nullptr;
  }
}

}  // namespace

bool AdminServer::start(const std::string& version,
                        const std::string& channel_file,
                        uint16_t port,
                        const std::string& config_path,
                        const unifi::DbConfig& db,
                        const std::string& protect_url,
                        onvif::ProtectUserIdProvider* protect_user_id_provider,
                        const std::string& event_log_path,
                        GetOnvifHealthsFn get_onvif_healths) {
  version_ = version;
  channel_file_ = channel_file;
  config_path_ = config_path;
  db_ = db;
  protect_url_ = protect_url;
  protect_user_id_provider_ = protect_user_id_provider;
  event_log_path_ = event_log_path;
  get_onvif_healths_ = std::move(get_onvif_healths);

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  // Leaked Ctx: one per server instance; lives for the program lifetime.
  auto* ctx = new Ctx{version_.c_str(), channel_file_.c_str(),
                      config_path_.c_str(), &db_,
                      protect_url_.c_str(), protect_user_id_provider_,
                      event_log_path_.c_str(), get_onvif_healths_,
                      log_ring_};

  daemon_ = MHD_start_daemon(
      MHD_USE_INTERNAL_POLLING_THREAD,
      port,
      nullptr, nullptr,
      &handler, ctx,
      MHD_OPTION_NOTIFY_COMPLETED, &request_completed, nullptr,
      MHD_OPTION_SOCK_ADDR,
      reinterpret_cast<struct sockaddr*>(&addr),
      MHD_OPTION_END);
  if (!daemon_) return false;

  // Record the bound port so callers that passed port=0 can discover it.
  const union MHD_DaemonInfo* info =
      MHD_get_daemon_info(daemon_, MHD_DAEMON_INFO_BIND_PORT);
  port_ = info ? info->port : port;
  return true;
}

void AdminServer::stop() {
  if (daemon_) {
    MHD_stop_daemon(daemon_);
    daemon_ = nullptr;
  }
}

AdminServer::~AdminServer() {
  stop();
}

}  // namespace onvif
