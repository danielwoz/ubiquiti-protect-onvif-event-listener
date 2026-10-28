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

#include "protect_ui_patch.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <mutex>  // NOLINT(build/c++11)
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "protect_version.hpp"

namespace protect_ui {

// ---------------------------------------------------------------
// Patch table.  Each entry is {original, replacement}.  Replacements that
// only disable code keep the original byte length so file offsets are
// preserved; a replacement that adds code changes the length.  A
// replacement must never contain its original, or every pass would apply
// it again.
//
// A patch is applied only when `original` occurs exactly once in its
// search range.  Minified bundles reuse short expressions in unrelated
// places, so a second occurrence means the anchor no longer identifies
// the intended code and the patch is skipped rather than guessed.
//
// `region_begin` / `region_end` optionally narrow the search range to the
// text between the single occurrence of `region_begin` and the next
// `region_end` after it (or end of file).
//
// Patches are selected by matching code shape, not by Protect version:
// the UI bundle version (swai-7.0.57.js) differs from the package version
// (7.0.107), and a pattern can survive across many releases.  Checked
// against Protect 6.2.88, 7.0.88, 7.0.107, 7.1.87, 7.2.105 and 7.3.70.
//
// `refuse` optionally vets the single occurrence of `original` at `pos`
// before it is replaced: it returns nullptr to go ahead, or the reason the
// patch must not be applied to this file.
// ---------------------------------------------------------------
using RefuseFn = const char* (*)(const std::string& content, size_t pos);

struct Patch {
  const char* original;
  const char* replacement;
  const char* region_begin = nullptr;
  const char* region_end = nullptr;
  RefuseFn refuse = nullptr;
};

// --- Frontend patches (swai*.js, vantage*.js) ---

// 1a. Camera picker filter (Protect 6.x - 7.0.x): always pass third-party
// cameras.
static const Patch kUiPatch1a = {
"!e.isThirdPartyCamera||e.isPairedWithAiPort",
"!0/*sThirdPartyCamera||isPairedWithAiPort*/"};

// 1b. Camera picker filter (UI bundle 7.0.57, shipped in Protect 7.0.107):
// new exclusion filter.
static const Patch kUiPatch1b = {
"!e.isThirdPartyCamera&&!v.includes(e.id)",
"!0/*hirdPartyCamera&&!v.includes(e.id)*/"};

// 2. Automation camera list filter
//    !(… e.nvrMac!==… || e.isThirdPartyCamera&&!e.isPairedWithAiPort)
// (Protect 6.x - 7.2.x; gone in 7.3).  Anchored on the surrounding "||" and
// ")" because later bundles use the bare expression for the microphone
// capability check and the ONVIF profile settings form.
static const Patch kUiPatch2 = {
"||e.isThirdPartyCamera&&!e.isPairedWithAiPort)",
"||!1/*ThirdPartyCamera&&!isPairedWithAiPort*/)"};

// 3. hasFullFeatureSet getter: always true.
static const Patch kUiPatch3 = {
"return!this.isThirdPartyCamera||this.isPairedWithAiPort",
"return!0/*isThirdPartyCamera||this.isPairedWithAiPort*/"};

static const Patch kUiPatches[] = {
kUiPatch1a, kUiPatch1b, kUiPatch2, kUiPatch3};
static constexpr size_t kUiPatchCount = 4;

// --- Backend patches (service.js) ---

// 4. scope_all_ui_cameras: remove third-party exclusion.
// The same fragment also guards the ONVIF stream-settings validator, so
// the search is confined to the scope predicate's own definition.
static const Patch kBackendPatch1 = {
"&&!e.isThirdPartyCamera",
"/*isThirdPartyCamera */",
"scope_all_ui_cameras:e=>", ",scope_"};

static const Patch kBackendPatches[] = {kBackendPatch1};
static constexpr size_t kBackendPatchCount = 1;

// Prefix of the marker comment in code this program injects.
static const char kInjectedMarker[] = "/*onvif-recorder:";

// 5. ONVIF client (Protect 7.1+): add the WS-Addressing To and Action
// headers to PullMessages, Renew and Unsubscribe.  Protect sends neither,
// and cameras whose SOAP stack requires them (Reolink) answer every pull
// with HTTP 400, so Protect's own motion detection for them keeps failing
// (#58).  The headers are the ones onvif-recorder sends to every camera:
// To is the URL Protect posts to (without the default port 80), Action the
// standard URI for the operation.  A request whose ReferenceParameters
// already carry To or Action is left alone.  The definition is identical
// in 7.1.87, 7.2.105 and 7.3.70; 7.0 and earlier have no ONVIF event
// client.
//
// Code this program injects carries an "/*onvif-recorder:" marker so a
// file patched by any version is recognised.  If this replacement text
// ever changes, the old text must stay in the removed list so it is taken
// out before the new one goes in.
static const Patch kBackendPatchWsa = {
"t.connSoapRequest=(e,r,o,i,a,n)=>(0,t.soapRequest)({hostname:e.hostname,"
"port:e.port,path:r,body:{[o]:{\"@_xmlns\":i,...a}},username:e.username,"
"password:e.password,timeShift:e.timeShift,extraHeaders:n})",
"t.connSoapRequest=(e,r,o,i,a,n)=>(0,t.soapRequest)({hostname:e.hostname,"
"port:e.port,path:r,body:{[o]:{\"@_xmlns\":i,...a}},username:e.username,"
"password:e.password,timeShift:e.timeShift,extraHeaders:"
"/*onvif-recorder:wsa*/((h,u,c)=>{const w=c===\"PullMessages\"?"
"\"http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/"
"PullMessagesRequest\":c===\"Renew\"?\"http://docs.oasis-open.org/wsn/"
"bw-2/SubscriptionManager/RenewRequest\":c===\"Unsubscribe\"?"
"\"http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/"
"UnsubscribeRequest\":\"\";if(!w||Object.keys(h||{}).some(k=>"
"/(^|:)(To|Action)$/.test(k)))return h;"
"const x=\"http://www.w3.org/2005/08/addressing\";"
"return{...h,\"wsa:To\":{\"@_xmlns:wsa\":x,\"#text\":u},"
"\"wsa:Action\":{\"@_xmlns:wsa\":x,\"#text\":w}}})"
"(n,`http://${e.hostname}${e.port==80?\"\":\":\"+e.port}${r}`,o)})"};

static const char* route_patch_refusal(const std::string& content,
                                       size_t pos);

// 6. ONVIF SOAP transport (Protect 7.1+): share a camera's event
// subscription with onvif-recorder.  Every SOAP request Protect sends goes
// through t.soapRequestRaw; the injected prologue changes three of them and
// leaves the rest to the original body:
//   - CreatePullPointSubscription is first offered to the recorder's
//     PullPoint service at 127.0.0.1:7892, under
//     /onvif-recorder/pullpoint/<hostname>/<port><event service path>.  When
//     that fails, refuses or takes longer than 4 s the request goes to the
//     camera as before.
//   - Requests whose path starts with /onvif-recorder/pullpoint/ (Pull,
//     Renew and Unsubscribe on a subscription the recorder created) go to
//     the recorder, also limited to 4 s, or 1 s for Unsubscribe, whose
//     result Protect ignores.
//   - A limit counts only time in which Protect's event loop runs on
//     time.  It is measured in steps of at most 500 ms, and a step counts
//     no more than its own length however late its timer runs, so time in
//     which Protect's event loop itself was blocked (a long garbage
//     collection, say), and the request perhaps not even sent yet, is left
//     out.  When the limit is reached the request fails only after
//     Protect's event loop has read its sockets once more, so an answer
//     that has already arrived is used.
//   - For 500 ms after a request for a camera times out, further requests
//     to the recorder for that camera fail at once.  One Protect poll can
//     chain several of them (a Renew, then the Create that replaces the
//     subscription, then an Unsubscribe), so a recorder that has stopped
//     answering holds a camera's poll for about 4 s, not 4 s per request;
//     the next poll, at least about 1 s later, waits the full 4 s again.
//   - When a request times out and, within the last 10 s, a request for
//     another camera has also gone 3 s without an answer, requests for
//     every camera fail at once for those 500 ms.  Protect polls six
//     cameras at a time and creates subscriptions one camera after
//     another, so with the recorder hung a poll takes about 4 s however
//     many cameras are shared, and once two Creates have timed out those
//     that follow within 500 ms fail at once.  One camera that gets no
//     answer does not affect the others.
//   - An Unsubscribe, whose result Protect ignores and whose limit is
//     shorter than 3 s, opens neither window.  Nor does a stall in Protect,
//     whose time is not counted, so a stall does not send the Create that
//     follows a failed Renew to the camera.  The windows are measured on
//     performance.now(), which wall-clock changes do not move.
//   - A Renew of a subscription held directly on the camera first asks the
//     recorder's /status whether it serves that camera now; if so the Renew
//     fails, so Protect creates its next subscription through the recorder.
// Requests the prologue re-sends carry a second argument, so they run the
// original body unchanged.  The function becomes a named function
// expression so the prologue can call itself by name: inside the body `t`
// is the body's own `const t`, not yet initialised where the prologue runs.
// route_patch_refusal() makes sure the body does not depend on arrow
// function semantics.  Identical anchor in 7.1.87, 7.2.105 and 7.3.70; 7.0
// and earlier have no ONVIF event client.
//
// If this replacement text ever changes, the old text must go into
// kRetiredRoutePatches so it is taken out before the new one goes in.
static const Patch kBackendPatchRoute = {
"t.soapRequestRaw=async e=>{const t=`${e.hostname}:${e.port}`;",
"t.soapRequestRaw=async function __onvrSoapRaw(e,__onvrRouted){"
"/*onvif-recorder:pullpoint*/if(!__onvrRouted&&e&&\"string\"==typeof e.path){"
"const __onvrP=\"/onvif-recorder/pullpoint/\","
"__onvrO={...e,hostname:\"127.0.0.1\",port:7892},__onvrB=e.body||{},"
"__onvrK=__onvrP+encodeURIComponent(e.hostname)+\"/\"+"
"encodeURIComponent(e.port),"
"__onvrW=__onvrSoapRaw.w||(__onvrSoapRaw.w=new Map),"
"__onvrS=__onvrSoapRaw.s||(__onvrSoapRaw.s=new Map),"
"__onvrR=(q,u)=>{const n=Error(\"onvif-recorder: no answer\"),"
"c=performance.now(),l=u?1e3:4e3;"
"if(c<(__onvrW.get(__onvrK)||0)||c<(__onvrW.get(\"\")||0))"
"return Promise.reject(n);"
"let h,d=0,g=0;return Promise.race([__onvrSoapRaw(q,1).finally(()=>{d=1}),"
"new Promise((_,j)=>{const a=b=>{const k=Math.min(l-g,500);"
"h=setTimeout(()=>{const t=performance.now();g+=Math.min(t-b,k);"
"g<3e3||__onvrS.set(__onvrK,t);if(g<l)return a(t);"
"setImmediate(()=>{if(d)return;if(!u){const z=performance.now();"
"[...__onvrS].some(([x,v])=>x!==__onvrK&&v>z-1e4)&&"
"__onvrW.set(\"\",z+500);__onvrW.set(__onvrK,z+500)}j(n)})},k)};"
"a(c)})]).finally(()=>clearTimeout(h))};"
"if(e.path.startsWith(__onvrP))"
"return __onvrR(__onvrO,__onvrB.Unsubscribe);"
"if(__onvrB.CreatePullPointSubscription)try{"
"return await __onvrR({...__onvrO,path:__onvrK+e.path})}catch(x){}"
"if(__onvrB.Renew){let m=0;try{await __onvrR({...__onvrO,"
"path:__onvrK+\"/status\",body:{GetServiceCapabilities:"
"{\"@_xmlns\":\"http://www.onvif.org/ver10/events/wsdl\"}}});m=1}catch(x){}"
"if(m)throw Error(\"onvif-recorder: moving subscription to proxy\")}}"
"const t=`${e.hostname}:${e.port}`;",
nullptr, nullptr, route_patch_refusal};

// Earlier texts of kBackendPatchRoute.  Each is taken out wherever it is
// found, so the current text replaces it while sharing is on and turning
// sharing off still removes it.
static const Patch kRetiredRoutePatches[] = {
// onvif-recorder 1.6.32: one fail-fast window for all cameras, opened by
// every timeout, including a missed Unsubscribe and a stall in Protect.
{"t.soapRequestRaw=async e=>{const t=`${e.hostname}:${e.port}`;",
"t.soapRequestRaw=async function __onvrSoapRaw(e,__onvrRouted){"
"/*onvif-recorder:pullpoint*/if(!__onvrRouted&&e&&\"string\"==typeof e.path){"
"const __onvrP=\"/onvif-recorder/pullpoint/\","
"__onvrO={...e,hostname:\"127.0.0.1\",port:7892},__onvrB=e.body||{},"
"__onvrK=__onvrP+encodeURIComponent(e.hostname)+\"/\"+"
"encodeURIComponent(e.port),"
"__onvrR=(q,l)=>{const n=Error(\"onvif-recorder: no answer\");"
"if(performance.now()<(__onvrSoapRaw.d||0))return Promise.reject(n);"
"let h;return Promise.race([__onvrSoapRaw(q,1),new Promise((_,j)=>{"
"h=setTimeout(()=>{__onvrSoapRaw.d=performance.now()+500;j(n)},l||4e3)"
"})]).finally(()=>clearTimeout(h))};"
"if(e.path.startsWith(__onvrP))"
"return __onvrR(__onvrO,__onvrB.Unsubscribe?1e3:0);"
"if(__onvrB.CreatePullPointSubscription)try{"
"return await __onvrR({...__onvrO,path:__onvrK+e.path})}catch(x){}"
"if(__onvrB.Renew){let m=0;try{await __onvrR({...__onvrO,"
"path:__onvrK+\"/status\",body:{GetServiceCapabilities:"
"{\"@_xmlns\":\"http://www.onvif.org/ver10/events/wsdl\"}}});m=1}catch(x){}"
"if(m)throw Error(\"onvif-recorder: moving subscription to proxy\")}}"
"const t=`${e.hostname}:${e.port}`;"}};

// Prefix of every name kBackendPatchRoute injects.
static const char kRouteNamePrefix[] = "__onvr";

// ---------------------------------------------------------------
// Paths
// ---------------------------------------------------------------
// NOLINTNEXTLINE
static const char kUiDir[] = "/usr/share/unifi-protect/app/node_modules/@ubnt/unifi-protect-ui-internal/dist";
static const char kServicePath[] = "/usr/share/unifi-protect/app/service.js";

// ---------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------
static std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return {std::istreambuf_iterator<char>(f),
          std::istreambuf_iterator<char>()};
}

static bool write_file(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return f.good();
}

// Replace @p path with @p data through a temporary file and rename(), so a
// failed write never leaves a truncated file behind.  Keeps an existing
// file's mode and, where permitted, its owner; a new file gets mode 0644.  A
// symlink is written through in place.
static bool replace_file(const std::string& path, const std::string& data) {
  struct stat st;
  if (lstat(path.c_str(), &st) == 0) {
    if (S_ISLNK(st.st_mode)) return write_file(path, data);
  } else {
    st = {};
    st.st_mode = 0644;
    st.st_uid = geteuid();
    st.st_gid = getegid();
  }
  const std::string tmp = path + ".onvif-recorder.tmp";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      st.st_mode & 07777);
  if (fd < 0) return false;
  bool ok = true;
  size_t off = 0;
  while (ok && off < data.size()) {
    const ssize_t n = write(fd, data.data() + off, data.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = false;
    } else {
      off += static_cast<size_t>(n);
    }
  }
  ok = ok && fchmod(fd, st.st_mode & 07777) == 0;
  // Only root can hand the file back to another owner; tests run as the
  // file's own owner, where this is a no-op.
  if (ok && fchown(fd, st.st_uid, st.st_gid) != 0 && geteuid() == 0) {
    ok = false;
  }
  ok = ok && fsync(fd) == 0;
  ok = close(fd) == 0 && ok;
  ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) unlink(tmp.c_str());
  return ok;
}

// ---------------------------------------------------------------
// dpkg md5sum verification
// ---------------------------------------------------------------
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kDpkgMd5sums[] = "/var/lib/dpkg/info/unifi-protect.md5sums";

// Compute the MD5 hex digest of a file on disk via md5sum(1).
// Returns empty string on failure.
static std::string md5_of_file(const std::string& path) {
  std::string cmd = "md5sum " + path + " 2>/dev/null";
  std::FILE* p = popen(cmd.c_str(), "r");  // NOLINT(runtime/int)
  if (!p) return {};
  char buf[128] = {};
  char* ok = std::fgets(buf, sizeof(buf), p);
  pclose(p);
  if (!ok || std::strlen(buf) < 32) return {};
  return std::string(buf, 32);
}

// Load the dpkg md5sums file into a map: relative_path -> md5hex.
static std::unordered_map<std::string, std::string> load_dpkg_md5sums() {
  std::unordered_map<std::string, std::string> result;
  std::ifstream f(kDpkgMd5sums);
  if (!f) return result;
  std::string line;
  while (std::getline(f, line)) {
    // Format: "<md5hex>  <relative_path>"
    if (line.size() < 35) continue;
    std::string md5 = line.substr(0, 32);
    std::string path = line.substr(34);  // skip "  "
    result[path] = md5;
  }
  return result;
}

// Look up the expected dpkg md5 for an absolute path.
// Returns empty string if the file is not in the md5sums database.
static std::string dpkg_expected_md5(
    const std::string& abs_path,
    const std::unordered_map<std::string, std::string>& md5sums) {
  // dpkg md5sums uses paths relative to /, without leading slash.
  std::string rel = abs_path;
  if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
  auto it = md5sums.find(rel);
  if (it == md5sums.end()) return {};
  return it->second;
}

// Check if a file on disk matches its dpkg md5sum.
static bool dpkg_md5_matches(
    const std::string& abs_path,
    const std::unordered_map<std::string, std::string>& md5sums) {
  std::string expected = dpkg_expected_md5(abs_path, md5sums);
  if (expected.empty()) return false;
  return md5_of_file(abs_path) == expected;
}

// ---------------------------------------------------------------
// Scan the UI dist directory for files matching a prefix
// (e.g. "swai" matches swai.js, swai-7.0.57.js).
// ---------------------------------------------------------------
static std::vector<std::string> find_ui_files(const char* dir,
                                              const char* prefix) {
  std::vector<std::string> result;
  size_t prefix_len = std::strlen(prefix);
  DIR* d = opendir(dir);
  if (!d) return result;
  while (struct dirent* entry = readdir(d)) {
    const char* name = entry->d_name;
    if (std::strncmp(name, prefix, prefix_len) != 0) continue;
    // Must be .js (not .js.bak, .js.map, etc.)
    const char* ext = std::strrchr(name, '.');
    if (!ext || std::strcmp(ext, ".js") != 0) continue;
    result.push_back(std::string(dir) + "/" + name);
  }
  closedir(d);
  return result;
}

// ---------------------------------------------------------------
// Matching helpers.
// ---------------------------------------------------------------
static size_t count_in(const std::string& s, const char* needle,
                       size_t from, size_t to) {
  const size_t n = std::strlen(needle);
  size_t count = 0;
  for (size_t pos = s.find(needle, from);
       pos != std::string::npos && pos + n <= to;
       pos = s.find(needle, pos + n)) {
    ++count;
  }
  return count;
}

// Resolve the [begin, end) search range for a patch.  Returns false if the
// patch defines a region whose start marker is absent or not unique.
static bool resolve_region(const std::string& content, const Patch& p,
                           size_t* begin, size_t* end) {
  *begin = 0;
  *end = content.size();
  if (p.region_begin == nullptr) return true;
  if (count_in(content, p.region_begin, 0, content.size()) != 1) return false;
  *begin = content.find(p.region_begin);
  if (p.region_end != nullptr) {
    size_t stop = content.find(p.region_end,
                               *begin + std::strlen(p.region_begin));
    if (stop != std::string::npos) *end = stop;
  }
  return true;
}

// Whether @p p's replacement occurs in its search range of @p content.
static bool patch_present(const std::string& content, const Patch& p) {
  size_t begin = 0;
  size_t end = 0;
  return resolve_region(content, p, &begin, &end) &&
         count_in(content, p.replacement, begin, end) > 0;
}

// Whether @p p's original occurs in its search range of @p content.
static bool original_present(const std::string& content, const Patch& p) {
  size_t begin = 0;
  size_t end = 0;
  return resolve_region(content, p, &begin, &end) &&
         count_in(content, p.original, begin, end) > 0;
}

// ---------------------------------------------------------------
// JavaScript block scanner.
// ---------------------------------------------------------------
// Longest block scan_js_block() follows before giving up.
static constexpr size_t kMaxScanBytes = 256 * 1024;

struct JsBlockScan {
  size_t end = std::string::npos;  // offset of the closing '}', or npos
  bool uses_this_or_arguments = false;
};

static bool is_ident_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// Whether a '/' starts a regular expression literal rather than a division,
// given the previous significant character @p prev ('\0' at the start) and,
// when that token was an identifier or keyword, its text @p word.  A ')'
// counts as the end of an operand, so `if(x)/re/` is read as a division.
static bool slash_starts_regex(char prev, const std::string& word) {
  static const char* const kKeywords[] = {
      "return", "typeof", "case", "do", "else", "in", "of", "new",
      "delete", "void", "throw", "instanceof", "yield", "await"};
  if (!word.empty()) {
    for (const char* k : kKeywords) {
      if (word == k) return true;
    }
    return false;
  }
  return prev == '\0' || std::strchr("(,=:[!&|?{};+-*%<>~^", prev) != nullptr;
}

// Find the '}' that closes the '{' at @p open in JavaScript source @p s,
// skipping string literals, template literals (including nested ${...}),
// comments and regular expression literals.  Also reports whether the
// block's code uses `this` or `arguments` other than as a property name.
// end is npos when the block does not close within kMaxScanBytes or a
// literal or comment in it is not terminated.
static JsBlockScan scan_js_block(const std::string& s, size_t open) {
  JsBlockScan r;
  if (open >= s.size() || s[open] != '{') return r;
  const size_t limit = std::min(s.size(), open + kMaxScanBytes);
  // 'b': brace of a block or object, 'e': ${ of a template substitution,
  // 't': text of a template literal.
  std::vector<char> stack = {'b'};
  char prev = '{';   // last significant character
  std::string word;  // the last token, when it was a keyword or identifier
  size_t i = open + 1;
  while (i < limit) {
    const char c = s[i];
    if (stack.back() == 't') {
      if (c == '\\') {
        i += 2;
      } else if (c == '`') {
        stack.pop_back();
        prev = '`';
        word.clear();
        ++i;
      } else if (c == '$' && i + 1 < limit && s[i + 1] == '{') {
        stack.push_back('e');
        prev = '{';
        word.clear();
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++i;
      continue;
    }
    const char next = i + 1 < limit ? s[i + 1] : '\0';
    if (c == '"' || c == '\'') {
      size_t j = i + 1;
      while (j < limit && s[j] != c) {
        if (s[j] == '\n') return r;
        j += s[j] == '\\' ? 2 : 1;
      }
      if (j >= limit) return r;
      prev = c;
      word.clear();
      i = j + 1;
    } else if (c == '`') {
      stack.push_back('t');
      ++i;
    } else if (c == '/' && next == '/') {
      const size_t nl = s.find('\n', i);
      if (nl == std::string::npos || nl >= limit) return r;
      i = nl + 1;
    } else if (c == '/' && next == '*') {
      const size_t close = s.find("*/", i + 2);
      if (close == std::string::npos || close + 2 > limit) return r;
      i = close + 2;
    } else if (c == '/' && slash_starts_regex(prev, word)) {
      size_t j = i + 1;
      bool in_class = false;
      while (j < limit) {
        const char d = s[j];
        if (d == '\n') return r;
        if (d == '\\') {
          j += 2;
          continue;
        }
        if (d == '[') {
          in_class = true;
        } else if (d == ']') {
          in_class = false;
        } else if (d == '/' && !in_class) {
          break;
        }
        ++j;
      }
      if (j >= limit) return r;
      // The literal is an operand: a '/' after it (or its flags) divides.
      prev = ')';
      word.clear();
      i = j + 1;
    } else if (c == '{') {
      stack.push_back('b');
      prev = c;
      word.clear();
      ++i;
    } else if (c == '}') {
      // Closing a ${ substitution resumes its template literal's text.
      stack.pop_back();
      if (stack.empty()) {
        r.end = i;
        return r;
      }
      prev = c;
      word.clear();
      ++i;
    } else if (is_ident_char(c)) {
      size_t j = i;
      while (j < limit && is_ident_char(s[j])) ++j;
      const std::string w = s.substr(i, j - i);
      // After a '.' the token is a property name, never `this`, `arguments`
      // or a keyword.
      const bool property = prev == '.';
      if (!property && (w == "this" || w == "arguments")) {
        r.uses_this_or_arguments = true;
      }
      word = property ? std::string() : w;
      prev = s[j - 1];
      i = j;
    } else if (c == '.' && next == '.' && i + 2 < limit && s[i + 2] == '.') {
      // Spread or rest: an operand follows.
      prev = ',';
      word.clear();
      i += 3;
    } else if ((c == '+' || c == '-') && next == c) {
      // A '/' right after `++` or `--` divides (`i++/2`); a regular
      // expression there would only be valid as `++/re/.lastIndex`.
      prev = ')';
      word.clear();
      i += 2;
    } else {
      prev = c;
      word.clear();
      ++i;
    }
  }
  return r;
}

// Why kBackendPatchRoute must not replace its anchor at @p pos of
// @p content, or nullptr when it may.  The patch turns an arrow function
// into a named function expression and injects names that start with
// kRouteNamePrefix, so it is refused when the function's end cannot be
// found, when its body uses `this` or `arguments` (whose meaning would
// change), or when the file uses that prefix outside the patch itself.
static const char* route_patch_refusal(const std::string& content,
                                       size_t pos) {
  const std::string replacement = kBackendPatchRoute.replacement;
  const size_t ours =
      count_in(replacement, kRouteNamePrefix, 0, replacement.size()) *
      count_in(content, kBackendPatchRoute.replacement, 0, content.size());
  if (count_in(content, kRouteNamePrefix, 0, content.size()) > ours) {
    return "service.js already uses names starting with __onvr";
  }
  const char* anchor = kBackendPatchRoute.original;
  const size_t open =
      pos + static_cast<size_t>(std::strchr(anchor, '{') - anchor);
  const JsBlockScan scan = scan_js_block(content, open);
  if (scan.end == std::string::npos) {
    return "the end of soapRequestRaw was not found";
  }
  if (scan.uses_this_or_arguments) {
    return "soapRequestRaw uses this or arguments";
  }
  return nullptr;
}

enum class PatchState { kApply, kAlreadyApplied, kNotApplicable, kAmbiguous };

static PatchState classify_patch(const std::string& content, const Patch& p,
                                 size_t* pos) {
  size_t begin = 0;
  size_t end = 0;
  if (!resolve_region(content, p, &begin, &end)) {
    return PatchState::kNotApplicable;
  }
  const size_t originals = count_in(content, p.original, begin, end);
  if (originals == 1) {
    *pos = content.find(p.original, begin);
    return PatchState::kApply;
  }
  if (originals > 1) return PatchState::kAmbiguous;
  return count_in(content, p.replacement, begin, end) == 1
             ? PatchState::kAlreadyApplied
             : PatchState::kNotApplicable;
}

static bool file_md5_is(const std::string& path, const std::string& md5) {
  return !md5.empty() && md5_of_file(path) == md5;
}

// Whether @p content holds code this program patched in, by any version.
static bool has_known_patch(const std::string& content) {
  if (content.find(kInjectedMarker) != std::string::npos) return true;
  for (const Patch& p : kUiPatches) {
    if (content.find(p.replacement) != std::string::npos) return true;
  }
  for (const Patch& p : kBackendPatches) {
    if (content.find(p.replacement) != std::string::npos) return true;
  }
  return false;
}

// Put back the original of @p p where its replacement occurs exactly once in
// its search range.  Returns true if the content changed.
static bool unapply_patch(std::string* content, const Patch& p) {
  size_t pos = 0;
  if (classify_patch(*content, p, &pos) != PatchState::kAlreadyApplied) {
    return false;
  }
  size_t begin = 0;
  size_t end = 0;
  resolve_region(*content, p, &begin, &end);
  pos = content->find(p.replacement, begin);
  content->replace(pos, std::strlen(p.replacement), p.original);
  return true;
}

// ---------------------------------------------------------------
// Apply patches to a single file.
// Returns number of patches applied, or -1 if file not readable.
// Patches in @p removed that are present in the file are taken out again.
//
// The source the patches are applied to:
//   - live matches its dpkg md5 -> it is the package original: patch it and
//     make it the new .bak.
//   - live differs and .bak matches the dpkg md5 -> rebuild from .bak, so a
//     file patched by an older rule set, damaged or edited by hand is
//     corrected.
//   - otherwise (no md5 on record, or neither matches) -> patch the live
//     file.  It becomes the new .bak only if it carries none of our
//     patches, so a patched file never replaces the backup.
// While dpkg unpacks a new Protect its md5sums still describe the old
// files, so patch_alarm_picker() does not run then.
//
// @p outcomes and @p removed_outcomes, when set, receive what happened to
// each entry of @p patches and @p removed; they are left alone when the
// file cannot be read.
// ---------------------------------------------------------------
static int apply_patches(
    const std::string& path, const Patch* patches, size_t count,
    const std::unordered_map<std::string, std::string>& md5sums,
    const Patch* removed = nullptr, size_t removed_count = 0,
    PatchOutcome* outcomes = nullptr,
    PatchOutcome* removed_outcomes = nullptr) {
  const std::string live = read_file(path);
  if (live.empty()) return -1;

  const std::string bak_path = path + ".bak";
  const std::string expected = dpkg_expected_md5(path, md5sums);
  const bool live_is_original = file_md5_is(path, expected);

  std::string source = live;
  bool source_is_bak = false;
  if (!expected.empty() && !live_is_original &&
      file_md5_is(bak_path, expected)) {
    source = read_file(bak_path);
    source_is_bak = !source.empty();
    if (!source_is_bak) source = live;
  }

  std::string result = source;
  int unapplied = 0;
  for (size_t i = 0; i < removed_count; ++i) {
    if (unapply_patch(&result, removed[i])) ++unapplied;
  }
  int applied = 0;
  std::vector<PatchState> states(count, PatchState::kNotApplicable);
  for (size_t i = 0; i < count; ++i) {
    const Patch& p = patches[i];
    size_t pos = 0;
    states[i] = classify_patch(result, p, &pos);
    if (states[i] == PatchState::kApply && p.refuse != nullptr) {
      if (const char* why = p.refuse(result, pos)) {
        LOG_FIRST_N(WARNING, 1) << "[ui_patch] " << path << ": pattern \""
                                << p.original << "\" not patched: " << why;
        states[i] = PatchState::kNotApplicable;
      }
    }
    switch (states[i]) {
      case PatchState::kApply:
        result.replace(pos, std::strlen(p.original), p.replacement);
        // A replacement that forms its original again with the code around
        // it would be applied once more on every pass over a file without a
        // verified .bak, each time writing the file again.
        if (original_present(result, p)) {
          result.replace(pos, std::strlen(p.replacement), p.original);
          LOG(WARNING) << "[ui_patch] " << path << ": pattern \""
                       << p.original << "\" not patched: its replacement "
                          "forms the pattern again";
          states[i] = PatchState::kNotApplicable;
          break;
        }
        ++applied;
        break;
      case PatchState::kAmbiguous:
        LOG(WARNING) << "[ui_patch] " << path << ": pattern \""
                     << p.original << "\" is not unique -- skipped";
        break;
      case PatchState::kAlreadyApplied:
      case PatchState::kNotApplicable:
        break;
    }
  }

  // Outcomes compare the file before this call with the file after it:
  // @p written says whether it now holds `result` or still `live`.
  auto report = [&](bool written) {
    const std::string& now = written ? result : live;
    for (size_t i = 0; outcomes != nullptr && i < count; ++i) {
      const Patch& p = patches[i];
      PatchOutcome o = PatchOutcome::kNotRun;
      if (states[i] == PatchState::kNotApplicable) {
        o = PatchOutcome::kNotApplicable;
      } else if (states[i] == PatchState::kAmbiguous) {
        o = PatchOutcome::kAmbiguous;
      } else if (patch_present(now, p)) {
        o = patch_present(live, p) ? PatchOutcome::kAlready
                                   : PatchOutcome::kApplied;
      }
      outcomes[i] = o;
    }
    for (size_t i = 0; removed_outcomes != nullptr && i < removed_count;
         ++i) {
      const Patch& p = removed[i];
      PatchOutcome o = PatchOutcome::kAbsent;
      if (patch_present(live, p)) {
        if (!patch_present(now, p)) {
          o = PatchOutcome::kRemoved;
        } else {
          // Still there after a write: it was not unique, so not removed.
          o = written ? PatchOutcome::kAmbiguous : PatchOutcome::kNotRun;
        }
      }
      removed_outcomes[i] = o;
    }
  };

  if (result == live) {
    LOG(INFO) << "[ui_patch] " << path << " already patched";
    report(true);
    return 0;
  }

  if (source_is_bak) {
    LOG(WARNING) << "[ui_patch] " << path
                 << " differs from the patch set for this version -- "
                    "rebuilding from " << bak_path;
  } else if (live_is_original || !has_known_patch(live)) {
    if (!replace_file(bak_path, live)) {
      LOG(ERROR) << "[ui_patch] failed to write backup: " << bak_path;
      report(false);
      return 0;
    }
  } else {
    LOG(INFO) << "[ui_patch] " << path
              << " already modified -- keeping existing .bak";
  }

  if (!replace_file(path, result)) {
    LOG(ERROR) << "[ui_patch] failed to write: " << path;
    report(false);
    return 0;
  }
  report(true);

  LOG(INFO) << "[ui_patch] patched " << path << " (" << applied
            << " replacement(s), " << unapplied << " removed)";
  return applied;
}

// Whether a dpkg status string says the package is part-way through being
// unpacked, when its files may be newer than the md5sums on record.
static bool status_is_half_installed(const std::string& status) {
  return status.find("half-installed") != std::string::npos;
}

// ---------------------------------------------------------------
// Local patches file (--local_patches)
// ---------------------------------------------------------------
// The only file local patches may change.
static const char kLocalPatchFile[] = "service.js";

// Read position in the text of a local patches file.
struct JsonCursor {
  const std::string& s;
  size_t pos = 0;
};

static void skip_json_ws(JsonCursor* c) {
  while (c->pos < c->s.size() &&
         (c->s[c->pos] == ' ' || c->s[c->pos] == '\t' ||
          c->s[c->pos] == '\n' || c->s[c->pos] == '\r')) {
    ++c->pos;
  }
}

// Skip white space, then consume @p ch if it comes next.
static bool consume_json(JsonCursor* c, char ch) {
  skip_json_ws(c);
  if (c->pos < c->s.size() && c->s[c->pos] == ch) {
    ++c->pos;
    return true;
  }
  return false;
}

// "line L, column C" (both from 1) of byte offset @p pos of @p s.
static std::string json_where(const std::string& s, size_t pos) {
  size_t line = 1;
  size_t column = 1;
  for (size_t i = 0; i < pos && i < s.size(); ++i) {
    if (s[i] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
  return "line " + std::to_string(line) + ", column " + std::to_string(column);
}

static absl::Status json_syntax_error(const JsonCursor& c,
                                      const std::string& what) {
  return absl::InvalidArgumentError("not valid JSON at " +
                                    json_where(c.s, c.pos) + ": " + what);
}

// An entry that is well-formed JSON but not a valid entry, at c.pos.
static absl::Status json_entry_error(const JsonCursor& c,
                                     const std::string& what) {
  return absl::InvalidArgumentError(json_where(c.s, c.pos) + ": " + what);
}

// @p s as a JSON string literal.
static std::string json_quote(const std::string& s) {
  std::string out = "\"";
  for (const char ch : s) {
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (static_cast<unsigned char>(ch) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
      out += buf;
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
  return out;
}

static void append_utf8(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// The four hex digits of a \u escape at c->pos, consumed; false if they are
// not four hex digits, with c->pos at the end of the text if it ends first.
static bool parse_json_hex4(JsonCursor* c, uint32_t* cp) {
  if (c->pos + 4 > c->s.size()) {
    c->pos = c->s.size();
    return false;
  }
  *cp = 0;
  for (int i = 0; i < 4; ++i) {
    const char h = c->s[c->pos + i];
    *cp <<= 4;
    if (h >= '0' && h <= '9') {
      *cp |= static_cast<uint32_t>(h - '0');
    } else if (h >= 'a' && h <= 'f') {
      *cp |= static_cast<uint32_t>(h - 'a' + 10);
    } else if (h >= 'A' && h <= 'F') {
      *cp |= static_cast<uint32_t>(h - 'A' + 10);
    } else {
      return false;
    }
  }
  c->pos += 4;
  return true;
}

// Parse the JSON string starting at c->pos into @p out.
static absl::Status parse_json_string(JsonCursor* c, std::string* out) {
  if (c->pos >= c->s.size() || c->s[c->pos] != '"') {
    return json_syntax_error(*c, "expected a string");
  }
  ++c->pos;
  while (c->pos < c->s.size()) {
    const char ch = c->s[c->pos];
    if (ch == '"') {
      ++c->pos;
      return absl::OkStatus();
    }
    if (static_cast<unsigned char>(ch) < 0x20) {
      return json_syntax_error(
          *c, "line break or control character inside a string "
              "(write it as \\n, \\t, ...)");
    }
    ++c->pos;
    if (ch != '\\') {
      out->push_back(ch);
      continue;
    }
    const char esc = c->pos < c->s.size() ? c->s[c->pos] : '\0';
    ++c->pos;
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        out->push_back(esc);
        break;
      case 'b':
        out->push_back('\b');
        break;
      case 'f':
        out->push_back('\f');
        break;
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      case 'u': {
        uint32_t cp = 0;
        if (!parse_json_hex4(c, &cp)) {
          return json_syntax_error(*c, "\\u needs four hex digits");
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
          return json_syntax_error(*c, "unpaired \\u surrogate");
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          // A high surrogate must be followed by an escaped low one.
          uint32_t low = 0;
          const bool escaped = c->s.compare(c->pos, 2, "\\u") == 0;
          const size_t left = c->s.size() - c->pos;
          if (!escaped && left < 2 &&
              c->s.compare(c->pos, left, "\\u", left) == 0) {
            c->pos = c->s.size();  // the text ends before the low half
          }
          if (escaped) c->pos += 2;
          if (!escaped || !parse_json_hex4(c, &low) || low < 0xDC00 ||
              low > 0xDFFF) {
            return json_syntax_error(*c, "unpaired \\u surrogate");
          }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(cp, out);
        break;
      }
      default:
        --c->pos;
        return json_syntax_error(*c, "unknown escape in a string");
    }
  }
  return json_syntax_error(*c, "string not terminated");
}

// Check the values of one entry parsed from @p text; @p at is the offset
// of its '{'.
static absl::Status check_local_patch(const std::string& text, size_t at,
                                      bool has_file, const LocalPatch& e) {
  auto bad = [&text, at](const std::string& what) {
    return absl::InvalidArgumentError(json_where(text, at) + ": " + what);
  };
  if (!has_file) return bad("\"file\" is missing; use \"service.js\"");
  if (e.file != kLocalPatchFile) {
    return bad("\"file\" is " + json_quote(e.file) +
               "; only \"service.js\" can be patched");
  }
  if (e.original.empty()) return bad("\"original\" is missing or empty");
  if (e.replacement.empty()) {
    return bad("\"replacement\" is missing or empty");
  }
  for (const std::string* s : {&e.name, &e.original, &e.replacement}) {
    if (s->find('\0') != std::string::npos) {
      return bad("contains a \\u0000 character");
    }
  }
  if (e.replacement.find(e.original) != std::string::npos) {
    return bad("\"replacement\" contains \"original\", so it would be "
               "applied again on every pass");
  }
  return absl::OkStatus();
}

// Whether @p ch can be part of a JSON number or of true, false or null.
static bool is_json_token_char(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '+' ||
         ch == '-' || ch == '.';
}

// Parse the entry at c->pos into @p e, leaving c->pos after it.  Returns
// why it cannot be read: malformed JSON, a value that is not an object, or
// a member that is unknown, repeated or not a string; c->pos is then
// somewhere inside it.  A member of @p e is set only once its string has
// been read in full.  @p has_file tells whether "file" was given.
static absl::Status parse_local_patch(JsonCursor* c, LocalPatch* e,
                                      bool* has_file) {
  skip_json_ws(c);
  if (!consume_json(c, '{')) {
    if (c->pos < c->s.size() &&
        (c->s[c->pos] == '"' || c->s[c->pos] == '[' ||
         is_json_token_char(c->s[c->pos]))) {
      return json_entry_error(*c, "each entry must be an object ({...})");
    }
    return json_syntax_error(*c, "expected '{' (each entry is an object)");
  }
  std::vector<std::string> seen;
  if (!consume_json(c, '}')) {
    do {
      skip_json_ws(c);
      const size_t key_at = c->pos;
      std::string key;
      absl::Status s = parse_json_string(c, &key);
      if (!s.ok()) return s;
      if (!consume_json(c, ':')) return json_syntax_error(*c, "expected ':'");
      std::string* field = key == "name"          ? &e->name
                           : key == "file"        ? &e->file
                           : key == "original"    ? &e->original
                           : key == "replacement" ? &e->replacement
                                                  : nullptr;
      if (field == nullptr ||
          std::count(seen.begin(), seen.end(), key) > 0) {
        JsonCursor k{c->s, key_at};
        return json_entry_error(
            k, field == nullptr
                   ? "unknown key " + json_quote(key) +
                         " (allowed: name, file, original, replacement)"
                   : json_quote(key) + " given twice");
      }
      seen.push_back(key);
      skip_json_ws(c);
      if (c->pos >= c->s.size() || c->s[c->pos] != '"') {
        return json_entry_error(*c, "the value of " + json_quote(key) +
                                        " must be a string");
      }
      std::string value;
      s = parse_json_string(c, &value);
      if (!s.ok()) return s;
      *field = std::move(value);
    } while (consume_json(c, ','));
    if (!consume_json(c, '}')) {
      return json_syntax_error(*c, "expected ',' or '}'");
    }
  }
  *has_file = std::count(seen.begin(), seen.end(), std::string("file")) > 0;
  return absl::OkStatus();
}

// How scan_json_value() ended.
enum class JsonScan {
  kEnd,        // after the value
  kTextEnds,   // the text ends inside the value, or before one starts
  kNotValue,   // no value starts here, or its brackets do not match
};

// Skip the JSON value at c->pos (after white space) by its quotes and
// brackets alone: a string, an array or object up to its matching bracket,
// or a run of number and literal characters.  Nothing else in it is
// checked, so a value with malformed JSON inside can be skipped too.
static JsonScan scan_json_value(JsonCursor* c) {
  const std::string& s = c->s;
  skip_json_ws(c);
  if (c->pos >= s.size()) return JsonScan::kTextEnds;
  if (is_json_token_char(s[c->pos])) {
    while (c->pos < s.size() && is_json_token_char(s[c->pos])) ++c->pos;
    return c->pos < s.size() ? JsonScan::kEnd : JsonScan::kTextEnds;
  }
  if (s[c->pos] != '"' && s[c->pos] != '[' && s[c->pos] != '{') {
    return JsonScan::kNotValue;
  }
  std::string closers;  // brackets still open, innermost last
  while (c->pos < s.size()) {
    const char ch = s[c->pos++];
    if (ch == '"') {
      bool closed = false;
      while (!closed && c->pos < s.size()) {
        const char in = s[c->pos++];
        if (in == '\\') {
          ++c->pos;
        } else {
          closed = in == '"';
        }
      }
      if (!closed) {
        c->pos = s.size();
        return JsonScan::kTextEnds;
      }
    } else if (ch == '{' || ch == '[') {
      closers.push_back(ch == '{' ? '}' : ']');
    } else if (ch == '}' || ch == ']') {
      if (closers.empty() || closers.back() != ch) return JsonScan::kNotValue;
      closers.pop_back();
    }
    if (closers.empty()) return JsonScan::kEnd;
  }
  return JsonScan::kTextEnds;
}

// Whether a ',' or ']' follows at c->pos, after white space.
static bool json_separator_follows(JsonCursor* c) {
  skip_json_ws(c);
  return c->pos < c->s.size() && (c->s[c->pos] == ',' || c->s[c->pos] == ']');
}

// Whether a '{' after offset @p from and before @p to starts what looks
// like an entry: white space, then "name", "file", "original" or
// "replacement" and ':'.  In well-formed JSON such a '{' can only start a
// nested object; after a missing or extra '"' it is usually the start of
// the next entry, which a scan by quotes and brackets has run past.
static bool holds_entry_start(const std::string& s, size_t from, size_t to) {
  for (size_t at = s.find('{', from + 1); at < to; at = s.find('{', at + 1)) {
    JsonCursor c{s, at + 1};
    skip_json_ws(&c);
    for (const char* key : {"\"name\"", "\"file\"", "\"original\"",
                            "\"replacement\""}) {
      const size_t n = std::strlen(key);
      if (s.compare(c.pos, n, key) != 0) continue;
      c.pos += n;
      if (consume_json(&c, ':')) return true;
      break;
    }
  }
  return false;
}

// A local patches file as parse_local_patches_text() read it.
struct ParsedLocalPatches {
  std::vector<LocalPatch> entries;  // the first kMaxLocalPatches
  std::string too_many;  // about the entries after those, or empty
  std::string stopped;   // the JSON error that ended reading, or empty
  // With `stopped`: offset of the first byte not read, the start of the
  // entry or text the error is in.  npos when the text is not an array.
  size_t rest_at = std::string::npos;
  // With `stopped`: where the parser gave up, the end of the text when it
  // ran out of text.
  size_t stopped_at = std::string::npos;
};

// Parse the text of a local patches file; see parse_local_patches().  The
// end of an entry that cannot be read is found with scan_json_value(), and
// is trusted only when a ',' or ']' follows it and the text it skips does
// not hold the start of another entry.  Entries after the first
// kMaxLocalPatches are read the same way, only to count them.
static ParsedLocalPatches parse_local_patches_text(const std::string& text) {
  ParsedLocalPatches out;
  auto stop = [&out, &text](const absl::Status& why, size_t stopped_at,
                            size_t rest_at) {
    out.stopped = std::string(why.message()) + "; the rest of the file, from " +
                  json_where(text, rest_at) + ", is ignored";
    out.stopped_at = stopped_at;
    out.rest_at = rest_at;
  };
  JsonCursor c{text};
  // A UTF-8 byte order mark, as some editors write, is skipped.
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) c.pos = 3;
  skip_json_ws(&c);
  if (c.pos == text.size()) return out;
  if (!consume_json(&c, '[')) {
    out.stopped = std::string(
        json_syntax_error(c, "expected '[' (the file holds an array of "
                             "entries)").message());
    return out;
  }
  size_t ignored = 0;     // entries after the first kMaxLocalPatches
  size_t ignored_at = 0;  // offset of the first of them
  bool closed = consume_json(&c, ']');
  for (size_t index = 0; !closed; ++index) {
    skip_json_ws(&c);
    const size_t at = c.pos;
    LocalPatch e;
    bool has_file = false;
    absl::Status s = parse_local_patch(&c, &e, &has_file);
    if (s.ok()) {
      s = check_local_patch(text, at, has_file, e);
    } else {
      JsonCursor end{text, at};
      if (scan_json_value(&end) != JsonScan::kEnd ||
          holds_entry_start(text, at, end.pos) ||
          !json_separator_follows(&end)) {
        stop(s, c.pos, at);
        break;
      }
      c.pos = end.pos;
    }
    if (index >= kMaxLocalPatches) {
      if (ignored++ == 0) ignored_at = at;
    } else {
      if (!s.ok()) {
        // Only the name of an entry that cannot be used is kept.
        LocalPatch invalid;
        invalid.name = std::move(e.name);
        invalid.error = std::string(s.message());
        e = std::move(invalid);
      }
      if (e.name.empty()) e.name = "entry " + std::to_string(index + 1);
      out.entries.push_back(std::move(e));
    }
    if (consume_json(&c, ',')) continue;
    closed = consume_json(&c, ']');
    if (!closed) {
      stop(json_syntax_error(c, "expected ',' or ']'"), c.pos, c.pos);
      break;
    }
  }
  if (closed) {
    skip_json_ws(&c);
    if (c.pos != text.size()) {
      stop(json_syntax_error(c, "unexpected text after the array"), c.pos,
           c.pos);
    }
  }
  if (ignored > 0) {
    // Where reading stopped inside the array, more entries may follow.
    const std::string count = ignored == 1 ? "one" : std::to_string(ignored);
    const std::string limit = std::to_string(kMaxLocalPatches);
    out.too_many = "more than " + limit + " entries; " +
                   (closed ? "the " : "at least ") + count +
                   " after the first " + limit + ", from " +
                   json_where(text, ignored_at) +
                   (ignored == 1 ? ", is ignored" : ", are ignored");
  }
  return out;
}

// @p parts that are not empty, joined by "; ".
static std::string join_errors(std::initializer_list<std::string> parts) {
  std::string out;
  for (const std::string& p : parts) {
    if (p.empty()) continue;
    if (!out.empty()) out += "; ";
    out += p;
  }
  return out;
}

LocalPatches parse_local_patches(const std::string& text) {
  ParsedLocalPatches p = parse_local_patches_text(text);
  return {std::move(p.entries), join_errors({p.too_many, p.stopped})};
}

LocalPatches load_local_patches(const std::string& path) {
  LocalPatches out;
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    if (errno != ENOENT) out.error = std::strerror(errno);
    return out;
  }
  if (!S_ISREG(st.st_mode)) {
    out.error = "not a regular file";
    return out;
  }
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    out.error = std::strerror(errno);
    return out;
  }
  // One byte more than is parsed shows whether the file is larger.
  std::string text(kMaxLocalPatchesBytes + 1, '\0');
  f.read(&text[0], static_cast<std::streamsize>(text.size()));
  if (f.bad()) {
    out.error = "read failed";
    return out;
  }
  text.resize(static_cast<size_t>(f.gcount()));
  const bool too_big = text.size() > kMaxLocalPatchesBytes;
  if (too_big) text.resize(kMaxLocalPatchesBytes);
  ParsedLocalPatches p = parse_local_patches_text(text);
  if (too_big) {
    const std::string kib = std::to_string(kMaxLocalPatchesBytes / 1024);
    const std::string larger = "larger than " + kib + " KiB";
    if (p.stopped.empty()) {
      p.stopped = larger + "; what follows its first " + kib +
                  " KiB is ignored";
    } else if (p.rest_at != std::string::npos &&
               p.stopped_at >= text.size()) {
      // The parser ran out of text, so the size limit cut off the entry
      // (or the gap between entries) where reading stopped.  An error
      // found before the end of the text is in the file itself and is
      // reported as it is.
      p.stopped = larger + "; the rest of the file, from " +
                  json_where(text, p.rest_at) + ", is ignored";
    } else {
      p.stopped += "; the file is also " + larger;
    }
  }
  out.entries = std::move(p.entries);
  out.error = join_errors({p.too_many, p.stopped});
  return out;
}

// The entries of the local patches file of @p opts, none when it is not
// configured or does not exist.  Records the path and the file's error in
// @p rep.
static LocalPatches read_local_patches(const PatchOptions& opts,
                                       PatchReport* rep) {
  rep->local_patches_path = opts.local_patches;
  rep->local_patches_error.clear();
  if (opts.local_patches.empty()) return {};
  LocalPatches loaded = load_local_patches(opts.local_patches);
  rep->local_patches_error = loaded.error;
  return loaded;
}

// Patch the service.js at @p path: the backend scope filter always, the
// optional patches of @p opts, each of which is taken out again when off,
// and then the entries of the local patches file.  Earlier texts of the
// route patch are always taken out.  Sets all outcomes in @p rep (kNotRun
// when the file cannot be read).
// Returns the number of patches applied, or -1 if the file is not readable.
static int patch_service_js(
    const std::string& path, const PatchOptions& opts,
    const std::unordered_map<std::string, std::string>& md5sums,
    PatchReport* rep) {
  std::vector<Patch> backend(kBackendPatches,
                             kBackendPatches + kBackendPatchCount);
  std::vector<Patch> removed;
  auto add = [&](const Patch& p, bool enabled) {
    std::vector<Patch>& list = enabled ? backend : removed;
    list.push_back(p);
    return list.size() - 1;
  };
  const size_t wsa = add(kBackendPatchWsa, opts.onvif_addressing);
  const size_t route = add(kBackendPatchRoute, opts.event_proxy);
  const size_t retired = removed.size();
  for (const Patch& p : kRetiredRoutePatches) removed.push_back(p);
  // Valid local patches go last, so they apply to the file with ours in
  // place.  Their Patch entries point into `local`.
  const LocalPatches local = read_local_patches(opts, rep);
  // Index in `backend` of each entry, used only for the valid ones.
  std::vector<size_t> local_at;
  for (const LocalPatch& lp : local.entries) {
    local_at.push_back(backend.size());
    if (!lp.error.empty()) continue;
    backend.push_back({lp.original.c_str(), lp.replacement.c_str()});
  }
  std::vector<PatchOutcome> outcomes(backend.size(), PatchOutcome::kNotRun);
  std::vector<PatchOutcome> removed_outcomes(removed.size(),
                                             PatchOutcome::kNotRun);
  const int n = apply_patches(path, backend.data(), backend.size(), md5sums,
                              removed.data(), removed.size(), outcomes.data(),
                              removed_outcomes.data());
  rep->onvif_addressing =
      (opts.onvif_addressing ? outcomes : removed_outcomes)[wsa];
  rep->event_proxy = (opts.event_proxy ? outcomes : removed_outcomes)[route];
  // With sharing off, what happened to an earlier route text found in the
  // file is the route patch's outcome.
  for (size_t i = retired; !opts.event_proxy && i < removed.size(); ++i) {
    if (rep->event_proxy == PatchOutcome::kAbsent) {
      rep->event_proxy = removed_outcomes[i];
    }
  }
  rep->local_patches.clear();
  for (size_t i = 0; i < local.entries.size(); ++i) {
    const LocalPatch& lp = local.entries[i];
    if (!lp.error.empty()) {
      rep->local_patches.push_back({lp.name, PatchOutcome::kInvalid, lp.error});
    } else {
      rep->local_patches.push_back({lp.name, outcomes[local_at[i]]});
    }
  }
  return n;
}

// The error lines for the local patches in @p rep: the file's error, and
// each entry not applied with why.
static std::vector<std::string> local_patch_errors(const PatchReport& rep) {
  std::vector<std::string> lines;
  if (!rep.local_patches_error.empty()) {
    lines.push_back("local patches " + rep.local_patches_path + ": " +
                    rep.local_patches_error +
                    (rep.local_patches.empty()
                         ? " -- none of its entries is applied"
                         : ""));
  }
  for (const LocalPatchOutcome& e : rep.local_patches) {
    const std::string head =
        "local patch " + json_quote(e.name) + " not applied: ";
    switch (e.outcome) {
      case PatchOutcome::kInvalid:
        lines.push_back(head + e.error);
        break;
      case PatchOutcome::kNotApplicable:
        lines.push_back(head +
                        "its original text is not in service.js (a Protect "
                        "update may have changed it), or its replacement "
                        "would form that text again");
        break;
      case PatchOutcome::kAmbiguous:
        lines.push_back(head +
                        "its original text occurs more than once in "
                        "service.js");
        break;
      case PatchOutcome::kNotRun:
      case PatchOutcome::kApplied:
      case PatchOutcome::kAlready:
      case PatchOutcome::kRemoved:
      case PatchOutcome::kAbsent:
        break;
    }
  }
  return lines;
}

// The lines of @p lines that are not in @p last, which then holds
// @p lines.
static std::vector<std::string> new_lines(const std::vector<std::string>& lines,
                                          std::set<std::string>* last) {
  std::vector<std::string> fresh;
  for (const std::string& line : lines) {
    if (last->count(line) == 0) fresh.push_back(line);
  }
  last->clear();
  last->insert(lines.begin(), lines.end());
  return fresh;
}

// Log what became of the local patches in @p rep.  The file's error and
// each entry not applied are errors.  Patch passes repeat with nothing
// changed (the patch watcher also fires on onvif-recorder's own writes),
// so an error line is logged only when the previous pass did not log it.
static void log_local_patches(const PatchReport& rep) {
  static std::mutex mu;
  static auto* const last_pass = new std::set<std::string>();
  {
    std::lock_guard<std::mutex> lock(mu);
    for (const std::string& line :
         new_lines(local_patch_errors(rep), last_pass)) {
      LOG(ERROR) << "[ui_patch] " << line;
    }
  }
  for (const LocalPatchOutcome& e : rep.local_patches) {
    LOG(INFO) << "[ui_patch] local patch " << json_quote(e.name) << ": "
              << patch_outcome_name(e.outcome);
  }
}

static bool protect_unpack_in_progress() {
  std::FILE* p = popen("dpkg-query -W -f='${Status}' unifi-protect "
                       "2>/dev/null", "r");  // NOLINT
  if (!p) return false;
  char buf[128] = {};
  const char* ok = std::fgets(buf, sizeof(buf), p);
  pclose(p);
  return ok && status_is_half_installed(buf);
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------
const char* patch_outcome_name(PatchOutcome outcome) {
  switch (outcome) {
    case PatchOutcome::kNotRun:
      return "not_run";
    case PatchOutcome::kApplied:
      return "applied";
    case PatchOutcome::kAlready:
      return "already";
    case PatchOutcome::kRemoved:
      return "removed";
    case PatchOutcome::kAbsent:
      return "absent";
    case PatchOutcome::kNotApplicable:
      return "not_applicable";
    case PatchOutcome::kAmbiguous:
      return "ambiguous";
    case PatchOutcome::kInvalid:
      return "invalid";
  }
  return "not_run";
}

std::string local_patches_json(const PatchReport& report) {
  std::string j = "{\"path\":" + json_quote(report.local_patches_path) +
                  ",\"error\":" + json_quote(report.local_patches_error) +
                  ",\"entries\":[";
  for (size_t i = 0; i < report.local_patches.size(); ++i) {
    const LocalPatchOutcome& e = report.local_patches[i];
    if (i > 0) j += ',';
    j += "{\"name\":" + json_quote(e.name) + ",\"outcome\":\"" +
         patch_outcome_name(e.outcome) + "\",\"error\":" +
         json_quote(e.error) + "}";
  }
  j += "]}";
  return j;
}

absl::Status patch_alarm_picker(const PatchOptions& opts,
                                PatchReport* report) {
  PatchReport local_report;
  PatchReport* rep = report != nullptr ? report : &local_report;
  *rep = PatchReport();
  rep->local_patches_path = opts.local_patches;

  // dpkg's trigger restarts onvif-recorder once the unpack has finished,
  // and that start-up patches the new files.
  if (protect_unpack_in_progress()) {
    LOG(INFO) << "[ui_patch] unifi-protect is being unpacked -- patching "
                 "after dpkg finishes";
    return absl::OkStatus();
  }

  // Log the detected Protect firmware so a journal capture from a user
  // running a brand-new firmware makes the version visible up-front.
  // Fast popen path; failure is silent (we still proceed with patching).
  {
    std::string ver;
    FILE* p = popen("dpkg-query -W -f='${Version}' "
                    "unifi-protect 2>/dev/null", "r");  // NOLINT
    if (p) {
      char buf[64] = {0};
      if (std::fgets(buf, sizeof(buf), p)) {
        ver = buf;
        while (!ver.empty() &&
               (ver.back() == '\n' || ver.back() == ' '))
          ver.pop_back();
      }
      pclose(p);  // NOLINT
    }
    if (!ver.empty()) {
      LOG(INFO) << "[ui_patch] detected unifi-protect " << ver;
      // Publish the version so live writers (detection_recorder,
      // motion_poller) can gate on it via IsAtLeast(7, 1, 0).
      if (auto v = onvif::protect_version::Parse(ver)) {
        onvif::protect_version::SetCurrent(*v);
        LOG(INFO) << "[ui_patch] protect_version published: "
                  << v->major << "." << v->minor << "." << v->patch;
      }
    } else {
      LOG(INFO) << "[ui_patch] unifi-protect version unknown";
    }
  }

  auto md5sums = load_dpkg_md5sums();
  int total = 0;
  int files_found = 0;

  // Patch all swai*.js and vantage*.js variants (versioned and unversioned).
  for (const char* prefix : {"swai", "vantage"}) {
    for (const auto& path : find_ui_files(kUiDir, prefix)) {
      int n = apply_patches(path, kUiPatches, kUiPatchCount, md5sums);
      if (n >= 0) {
        ++files_found;
        total += n;
      }
    }
  }

  // Patch service.js (backend scope filter, ONVIF WS-Addressing, event
  // sharing route, local patches).
  const int n = patch_service_js(kServicePath, opts, md5sums, rep);
  if (n >= 0) {
    ++files_found;
    total += n;
  }
  LOG(INFO) << "[ui_patch] service.js: onvif_addressing="
            << patch_outcome_name(rep->onvif_addressing)
            << " event_proxy=" << patch_outcome_name(rep->event_proxy);
  log_local_patches(*rep);

  if (files_found == 0) {
    return absl::NotFoundError(
        "Protect UI files not found -- not running on a Dream Router/NVR?");
  }

  if (total > 0) {
    LOG(INFO) << "[ui_patch] applied " << total
              << " patch(es) across " << files_found << " file(s)";
  } else {
    LOG(INFO) << "[ui_patch] all files already patched";
  }

  return absl::OkStatus();
}

absl::Status revert_alarm_picker_in(const std::string& ui_dir,
                                     const std::string& service_path) {
  auto md5sums = load_dpkg_md5sums();
  int restored = 0;

  auto try_restore = [&](const std::string& path) {
    std::string bak = path + ".bak";
    std::string content = read_file(bak);
    if (content.empty()) return;

    if (!md5sums.empty()) {
      bool bak_matches = dpkg_md5_matches(bak, md5sums);
      bool live_matches = dpkg_md5_matches(path, md5sums);

      if (live_matches) {
        // Live file is already the correct original -- nothing to do.
        LOG(INFO) << "[ui_patch] " << path << " already matches dpkg md5";
        return;
      }

      if (!bak_matches) {
        // Neither file matches -- .bak is stale but still better than
        // a patched file.  Warn but proceed with restore.
        LOG(WARNING) << "[ui_patch] " << bak << " does not match dpkg md5 "
                     << "-- may be from a prior firmware version";
      }
    }

    if (!replace_file(path, content)) {
      LOG(ERROR) << "[ui_patch] failed to restore " << path;
      return;
    }
    LOG(INFO) << "[ui_patch] restored " << path << " from backup";
    ++restored;
  };

  // Revert all swai*.js.bak and vantage*.js.bak in the UI dist directory.
  for (const char* prefix : {"swai", "vantage"}) {
    for (const auto& path : find_ui_files(ui_dir.c_str(), prefix))
      try_restore(path);
  }

  // Revert service.js.
  if (!service_path.empty())
    try_restore(service_path);

  if (restored > 0) {
    LOG(INFO) << "[ui_patch] reverted " << restored << " file(s)";
  } else {
    LOG(INFO) << "[ui_patch] no backup files found -- nothing to revert";
  }

  return absl::OkStatus();
}

absl::Status revert_alarm_picker() {
  return revert_alarm_picker_in(kUiDir, kServicePath);
}

// ---------------------------------------------------------------
// Protect restart check
// ---------------------------------------------------------------

// Seconds since process @p pid started, from its start time in
// /proc/<pid>/stat and the system uptime.  Negative if unknown.
static double process_age_sec(pid_t pid) {
  std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(stat_file, line)) return -1;
  // The command name (field 2) may contain spaces; count fields after it.
  const size_t rp = line.rfind(')');
  if (rp == std::string::npos) return -1;
  std::istringstream rest(line.substr(rp + 1));
  std::string field;
  unsigned long long start_ticks = 0;  // NOLINT(runtime/int)
  for (int i = 3; i <= 22; ++i) {
    if (!(rest >> field)) return -1;
    if (i == 22) start_ticks = std::strtoull(field.c_str(), nullptr, 10);
  }
  std::ifstream uptime_file("/proc/uptime");
  double uptime = -1;
  if (!(uptime_file >> uptime)) return -1;
  const long hz = sysconf(_SC_CLK_TCK);  // NOLINT(runtime/int)
  if (hz <= 0) return -1;
  return uptime - static_cast<double>(start_ticks) / static_cast<double>(hz);
}

// Whether @p path was last written after process @p pid started.
static bool file_newer_than_process(const std::string& path, pid_t pid) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return false;
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return false;
  const double file_age =
      static_cast<double>(now.tv_sec - st.st_mtim.tv_sec) +
      static_cast<double>(now.tv_nsec - st.st_mtim.tv_nsec) / 1e9;
  const double proc_age = process_age_sec(pid);
  // A modification time in the future (clock stepped back) proves nothing.
  if (file_age < 0 || proc_age < 0) return false;
  return file_age < proc_age;
}

pid_t protect_main_pid() {
  std::FILE* p = popen(
      "systemctl show unifi-protect --property=MainPID --value 2>/dev/null",
      "r");
  if (!p) return 0;
  char buf[32] = {};
  const char* ok = std::fgets(buf, sizeof(buf), p);
  pclose(p);
  if (!ok) return 0;
  const long pid = std::strtol(buf, nullptr, 10);  // NOLINT(runtime/int)
  return pid > 0 ? static_cast<pid_t>(pid) : 0;
}

bool protect_predates_service_js() {
  const pid_t pid = protect_main_pid();
  return pid > 0 && file_newer_than_process(kServicePath, pid);
}

// ---------------------------------------------------------------
// Nginx proxy patches (log viewer + admin page)
// ---------------------------------------------------------------
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kNginxConf[] =
    "/data/unifi-core/config/http/site-local-ip.conf";  // NOLINT

// Log viewer markers (pre-existing; kept verbatim for upgrade compat).
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kLogMarkerBegin[] =
    "    # --- onvif-recorder log viewer "  // NOLINT
    "(managed by onvif_recorder) ---\n";  // NOLINT
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kLogMarkerEnd[] =
    "    # --- end onvif-recorder log viewer ---\n";  // NOLINT

// Admin page markers.
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kAdminMarkerBegin[] =
    "    # --- onvif-recorder admin "  // NOLINT
    "(managed by onvif_recorder) ---\n";  // NOLINT
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kAdminMarkerEnd[] =
    "    # --- end onvif-recorder admin ---\n";  // NOLINT

// Remove any block bounded by @p begin/@p end from @p content.
static bool strip_block(std::string* content,
                        const char* begin, const char* end) {
  size_t b = content->find(begin);
  if (b == std::string::npos) return false;
  size_t e = content->find(end, b);
  if (e == std::string::npos) return false;
  content->erase(b, e + std::strlen(end) - b);
  return true;
}

// Walk forward from the position of `listen 443` counting braces to find
// the closing `}` of the enclosing `server { ... }` block.  We start at
// depth=1 because the `listen` directive is already inside the opened
// server block.  Returns std::string::npos if `listen 443` isn't in @p
// content or the block is unbalanced.
//
// Naive content.find('}', s443) lands inside nested location blocks from
// earlier injections, which breaks nginx config validity when a second
// block is inserted later.
static size_t find_server_close_brace(const std::string& content) {
  size_t s443 = content.find("listen 443");
  if (s443 == std::string::npos) return std::string::npos;
  int depth = 1;
  for (size_t i = s443; i < content.size(); ++i) {
    char c = content[i];
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) return i;
    }
  }
  return std::string::npos;
}

// Returns @p content with any existing block bounded by begin/end removed,
// and @p block inserted just before the closing `}` of the `listen 443`
// server block.  Returns empty absl::StatusOr on structural failure.
static absl::StatusOr<std::string> inject_nginx_block_into(
    const std::string& content_in,
    const std::string& block,
    const char* begin,
    const char* end) {
  std::string content = content_in;
  strip_block(&content, begin, end);
  size_t brace = find_server_close_brace(content);
  if (brace == std::string::npos) {
    return absl::InternalError(
        "cannot find closing brace for 443 server");
  }
  content.insert(brace, block);
  return content;
}

// Shared: inject @p block just before the closing '}' of the `listen 443`
// server block in site-local-ip.conf.  @p begin/@p end mark a pre-existing
// copy to replace.  @p label is used for log messages.
static absl::Status inject_nginx_block(const std::string& block,
                                       const char* begin,
                                       const char* end,
                                       const char* label) {
  std::string content = read_file(kNginxConf);
  if (content.empty()) {
    return absl::NotFoundError(
        "nginx config not found "
        "-- not running on a Dream Router/NVR?");
  }

  auto new_content_or = inject_nginx_block_into(content, block, begin, end);
  if (!new_content_or.ok()) return new_content_or.status();
  const std::string& new_content = *new_content_or;

  if (new_content == content) {
    LOG(INFO) << "[nginx] " << label << " block already present";
    return absl::OkStatus();
  }

  if (!write_file(kNginxConf, new_content)) {
    return absl::InternalError("failed to write nginx config");
  }
  LOG(INFO) << "[nginx] injected " << label << " block";

  int rc = std::system("nginx -s reload 2>/dev/null");  // NOLINT
  if (rc != 0)
    LOG(WARNING) << "[nginx] reload returned " << rc;
  return absl::OkStatus();
}

// Shared: strip a block and reload nginx.
static absl::Status strip_nginx_block(const char* begin,
                                      const char* end,
                                      const char* label) {
  std::string content = read_file(kNginxConf);
  if (content.empty()) {
    LOG(INFO) << "[nginx] config not found -- nothing to revert";
    return absl::OkStatus();
  }
  if (!strip_block(&content, begin, end)) {
    LOG(INFO) << "[nginx] no " << label
              << " block found -- nothing to revert";
    return absl::OkStatus();
  }
  if (!write_file(kNginxConf, content)) {
    return absl::InternalError("failed to write nginx config");
  }
  LOG(INFO) << "[nginx] removed " << label << " block";

  int rc = std::system("nginx -s reload 2>/dev/null");  // NOLINT
  if (rc != 0)
    LOG(WARNING) << "[nginx] reload returned " << rc;
  return absl::OkStatus();
}

absl::Status patch_nginx_log_proxy(uint16_t port) {
  // Same 401 -> 302-to-root redirect as patch_nginx_admin_proxy: an
  // unauthenticated visitor pasting the log URL gets the UniFi OS login
  // flow instead of a bare "401 Unauthorized".
  std::string block;
  block += kLogMarkerBegin;
  block += "    location /onvif/events/log {\n";
  block += "        include /usr/share/unifi-core/http/auth.conf;\n";
  block += "        include /usr/share/unifi-core/http/proxy.conf;\n";
  block += "        error_page 401 = @onvif_log_unauth;\n";
  block += "        proxy_pass http://127.0.0.1:";
  block += std::to_string(port);
  block += "/;\n";
  block += "    }\n";
  block += "    location @onvif_log_unauth {\n";
  block += "        return 302 /;\n";
  block += "    }\n";
  block += kLogMarkerEnd;
  return inject_nginx_block(block, kLogMarkerBegin, kLogMarkerEnd,
                            "log proxy");
}

absl::Status revert_nginx_log_proxy() {
  return strip_nginx_block(kLogMarkerBegin, kLogMarkerEnd, "log proxy");
}

absl::Status patch_nginx_admin_proxy(uint16_t port) {
  // When auth.conf's auth_request returns 401 (unauthenticated visitor),
  // nginx would otherwise propagate a bare "401 Unauthorized" -- friendly
  // for an XHR but jarring for a user who just pasted the admin URL into
  // a browser.  Override 401 to a 302 redirect at the UniFi OS root,
  // which presents the standard login flow.  On successful login the
  // user can re-navigate to /onvif/admin/.
  std::string block;
  block += kAdminMarkerBegin;
  block += "    location /onvif/admin/ {\n";
  block += "        include /usr/share/unifi-core/http/auth.conf;\n";
  block += "        include /usr/share/unifi-core/http/proxy.conf;\n";
  block += "        error_page 401 = @onvif_admin_unauth;\n";
  block += "        proxy_pass http://127.0.0.1:";
  block += std::to_string(port);
  block += "/;\n";
  block += "    }\n";
  block += "    location @onvif_admin_unauth {\n";
  block += "        return 302 /;\n";
  block += "    }\n";
  block += kAdminMarkerEnd;
  return inject_nginx_block(block, kAdminMarkerBegin, kAdminMarkerEnd,
                            "admin proxy");
}

absl::Status revert_nginx_admin_proxy() {
  return strip_nginx_block(kAdminMarkerBegin, kAdminMarkerEnd,
                           "admin proxy");
}

}  // namespace protect_ui
