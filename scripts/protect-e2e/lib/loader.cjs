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

// Runs single webpack modules out of a Protect service.js bundle.
//
// A module is located by a short piece of its own text (a "needle"), cut
// out of the bundle and evaluated as a method of an object literal, the
// way webpack's runtime calls it: fn(module, exports, require).  Modules
// marked real are evaluated; a module that only re-exports an npm package
// gets that package (from the directories the caller supplies); every
// other module is a stub whose properties come from the caller's stub
// table or are no-op functions that record their calls.
'use strict';

const path = require('path');

// The head of a module in the bundle's module table: `,<id>(e,t,r){`.
const MODULE_HEAD = /[,{](\d+)\((?:e,t,r|e,t|e|)\)\{/g;

class Bundle {
  // @p text: the bundle source.  @p opts.packages: npm package name ->
  // exports object (overrides lookup); @p opts.nmDir: directory with the
  // npm packages; @p opts.fxpDir: fast-xml-parser's package directory;
  // @p opts.stubs: property name -> value for stub modules.
  constructor(text, { nmDir, fxpDir, packages = {}, stubs = {} }) {
    this.s = text;
    this.nmDir = nmDir;
    this.fxpDir = fxpDir;
    this.packages = packages;
    this.stubs = stubs;
    this.stubCalls = [];
    this.cache = new Map();
    this.real = new Set();
  }

  // Id of the module whose text contains @p needle, which must occur once.
  idOf(needle) {
    const at = this.s.indexOf(needle);
    if (at < 0) throw new Error(`not in this bundle: ${needle}`);
    if (this.s.indexOf(needle, at + 1) >= 0) throw new Error(`not unique: ${needle}`);
    const re = new RegExp(MODULE_HEAD.source, 'g');
    re.lastIndex = Math.max(0, at - 4000000);
    let m;
    let best = null;
    while ((m = re.exec(this.s)) && m.index < at) best = m;
    if (!best) throw new Error(`no module head before: ${needle}`);
    return best[1];
  }

  // Source of module @p id: `<id>(e,t,r){...}`.
  text(id) {
    const head = new RegExp(`[,{]${id}\\((?:e,t,r|e,t|e|)\\)\\{`).exec(this.s);
    if (!head) throw new Error(`module ${id} not found`);
    const start = head.index + 1;
    const next = /\},(\d+)\((?:e,t,r|e,t|e|)\)\{/g;
    next.lastIndex = start + 5;
    const n = next.exec(this.s);
    return this.s.slice(start, n ? n.index + 1 : undefined);
  }

  markReal(...ids) {
    for (const id of ids) this.real.add(String(id));
  }

  require(id) {
    id = String(id);
    if (this.cache.has(id)) return this.cache.get(id).exports;
    const text = this.text(id);
    const mod = { exports: {} };
    const ext = /^\d+\(e\)\{"use strict";e\.exports=require\("([^"]+)"\)\}$/.exec(text);
    if (ext) {
      mod.exports = this.external(ext[1]);
    } else if (!this.real.has(id)) {
      mod.exports = this.stubModule(id);
    } else {
      this.cache.set(id, mod);
      const holder = (0, eval)(`({${text}})`);
      holder[id].call(holder, mod, mod.exports, (dep) => this.require(dep));
      return mod.exports;
    }
    this.cache.set(id, mod);
    return mod.exports;
  }

  external(name) {
    if (name in this.packages) return this.packages[name];
    switch (name) {
      case 'crypto':
      case 'net':
      case 'http':
      case 'url':
        return require(name);
      case 'fast-xml-parser':
        return require(this.fxpDir);
      case 'undici':
      case 'node-cache':
      case 'p-queue':
        return require(path.join(this.nmDir, name));
      default:
        return this.stubModule(`package:${name}`);
    }
  }

  stubModule(id) {
    const stubs = this.stubs;
    const calls = this.stubCalls;
    return new Proxy({}, {
      get(_, k) {
        if (k === '__esModule') return true;
        if (k in stubs) return stubs[k];
        if (typeof k === 'symbol') return undefined;
        return (...a) => {
          calls.push(`${id}.${String(k)}(${a.length} args)`);
          return undefined;
        };
      },
    });
  }
}

module.exports = { Bundle };
