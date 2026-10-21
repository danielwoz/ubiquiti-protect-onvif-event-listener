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

// Runs //test:pullpoint_demo processes: camera emulators (--no_recorder)
// and recorders (--cameras=...), with the demo's stdin commands and its
// JSON-line answers, and a "hung recorder" that accepts connections on
// the PullPoint port and never answers.
'use strict';

const fs = require('fs');
const net = require('net');
const { spawn } = require('child_process');

const live = new Set();

class Demo {
  // Start @p bin with @p args; resolves once it prints its ready line.
  static async start(bin, args, { name, logFile, timeoutMs = 15000 } = {}) {
    const d = new Demo(bin, args, name, logFile);
    await d.ready(timeoutMs);
    return d;
  }

  constructor(bin, args, name, logFile) {
    this.name = name || 'demo';
    this.args = args;
    this.lines = [];        // JSON lines from stdout
    this.stderr = [];       // log lines from stderr
    this.waiters = [];      // {pred, resolve}
    this.exited = null;
    this.log = logFile ? fs.createWriteStream(logFile, { flags: 'a' }) : null;
    this.startedAt = Date.now();
    this.proc = spawn(bin, args, { stdio: ['pipe', 'pipe', 'pipe'] });
    live.add(this);
    let out = '';
    this.proc.stdout.setEncoding('utf8');
    this.proc.stdout.on('data', (chunk) => {
      out += chunk;
      let nl;
      while ((nl = out.indexOf('\n')) >= 0) {
        const line = out.slice(0, nl);
        out = out.slice(nl + 1);
        if (!line.trim()) continue;
        let msg;
        try {
          msg = JSON.parse(line);
        } catch (e) {
          msg = { unparsed: line };
        }
        msg._t = Date.now();
        this.lines.push(msg);
        this.waiters = this.waiters.filter((w) => !(w.pred(msg) && (w.resolve(msg), true)));
      }
    });
    let err = '';
    this.proc.stderr.setEncoding('utf8');
    this.proc.stderr.on('data', (chunk) => {
      if (this.log) this.log.write(chunk);
      err += chunk;
      let nl;
      while ((nl = err.indexOf('\n')) >= 0) {
        this.stderr.push({ t: Date.now(), line: err.slice(0, nl) });
        err = err.slice(nl + 1);
      }
    });
    this.exitPromise = new Promise((resolve) => {
      this.proc.on('exit', (code, signal) => {
        this.exited = { code, signal, t: Date.now() };
        live.delete(this);
        for (const w of this.waiters.splice(0)) w.reject(new Error(`${this.name} exited`));
        if (this.log) this.log.end();
        resolve(this.exited);
      });
    });
  }

  waitLine(pred, timeoutMs = 10000) {
    const have = this.lines.find(pred);
    if (have) return Promise.resolve(have);
    return new Promise((resolve, reject) => {
      const w = { pred, resolve, reject };
      this.waiters.push(w);
      setTimeout(() => {
        const i = this.waiters.indexOf(w);
        if (i >= 0) {
          this.waiters.splice(i, 1);
          reject(new Error(`${this.name}: timed out waiting for output`));
        }
      }, timeoutMs);
    });
  }

  async ready(timeoutMs) {
    const msg = await this.waitLine((m) => m.event === 'ready' || m.event === 'error', timeoutMs);
    if (msg.event === 'error') throw new Error(`${this.name}: ${msg.error}`);
    this.info = msg;
    this.readyAt = msg._t;
    return msg;
  }

  // Send a command; resolves with its answer.
  async cmd(line, timeoutMs = 10000) {
    const name = line.split(' ')[0];
    const from = this.lines.length;
    const p = new Promise((resolve, reject) => {
      const w = { pred: (m) => m.cmd === name && this.lines.indexOf(m) >= from, resolve, reject };
      this.waiters.push(w);
      setTimeout(() => {
        const i = this.waiters.indexOf(w);
        if (i >= 0) {
          this.waiters.splice(i, 1);
          reject(new Error(`${this.name}: no answer to ${line}`));
        }
      }, timeoutMs);
    });
    this.proc.stdin.write(line + '\n');
    return p;
  }

  // SIGTERM (or @p signal); resolves with the shutdown time it reported
  // and the wall time until it exited.
  async stop(signal = 'SIGTERM', timeoutMs = 40000) {
    if (this.exited) return { already: true };
    const t0 = Date.now();
    this.proc.kill(signal);
    const timer = setTimeout(() => this.proc.kill('SIGKILL'), timeoutMs);
    const ex = await this.exitPromise;
    clearTimeout(timer);
    const stopped = this.lines.find((m) => m.event === 'stopped');
    return { code: ex.code, signal: ex.signal, wallMs: Date.now() - t0,
             shutdownMs: stopped ? stopped.ms : null };
  }

  logLines(re, fromT = 0) {
    return this.stderr.filter((l) => l.t >= fromT && re.test(l.line)).map((l) => l.line);
  }
}

// Kill every demo still running and close every hung server (end of a
// scenario or of the run).
async function killAll() {
  await Promise.all([...live].map((d) => d.stop('SIGKILL').catch(() => {})));
  await Promise.all([...hungServers].map((h) => h.close()));
}

// Kill the demos synchronously when the harness itself is killed.
function killAllNow() {
  for (const d of live) {
    try {
      d.proc.kill('SIGKILL');
    } catch (e) {
      // already gone
    }
  }
}

const hungServers = new Set();

// Accepts connections on 127.0.0.1:@p port and never answers.
async function hungServer(port) {
  const socks = new Set();
  const s = net.createServer((c) => {
    socks.add(c);
    c.on('data', () => {});
    c.on('error', () => {});
    c.on('close', () => socks.delete(c));
  });
  await new Promise((resolve, reject) => {
    s.once('error', reject);
    s.listen(port, '127.0.0.1', resolve);
  });
  const h = {
    accepted: () => socks.size,
    close: () => new Promise((r) => {
      hungServers.delete(h);
      for (const c of socks) c.destroy();
      s.close(() => r());
    }),
  };
  hungServers.add(h);
  return h;
}

// Whether something listens on 127.0.0.1:@p port.
function portInUse(port) {
  return new Promise((resolve) => {
    const c = net.connect(port, '127.0.0.1');
    c.once('connect', () => {
      c.destroy();
      resolve(true);
    });
    c.once('error', () => resolve(false));
  });
}

module.exports = { Demo, killAll, killAllNow, hungServer, portInUse };
