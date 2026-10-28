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

/**
 * test_protect_ui_patch.cpp
 *
 * Tests for the protect_ui_patch module's internal apply/revert logic.
 * Includes the .cpp directly to access file-local (static) functions.
 */

// Include the implementation directly to test file-local (static) functions.
#include "src/protect_ui_patch.cpp"  // NOLINT(build/include)

using namespace protect_ui;  // NOLINT(build/namespaces)

#include <fcntl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>  // NOLINT(build/c++11)
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

static std::string temp_dir() {
  const char* d = std::getenv("TEST_TMPDIR");
  return d ? d : "/tmp";
}

static std::string read_test_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return {std::istreambuf_iterator<char>(f),
          std::istreambuf_iterator<char>()};
}

static bool write_test_file(const std::string& path,
                            const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return f.good();
}

// ---------------------------------------------------------------
// Test 1: Patch strings are consistent
// ---------------------------------------------------------------
static void test_patch_lengths() {
  for (size_t i = 0; i < kUiPatchCount; ++i) {
    const Patch& p = kUiPatches[i];
    check(std::strlen(p.original) == std::strlen(p.replacement),
          "UI patch same byte length");
  }
  for (size_t i = 0; i < kBackendPatchCount; ++i) {
    const Patch& p = kBackendPatches[i];
    check(std::strlen(p.original) == std::strlen(p.replacement),
          "Backend patch same byte length");
  }
  // A replacement that contains its original would be applied again on
  // every pass.
  std::vector<Patch> all(kUiPatches, kUiPatches + kUiPatchCount);
  all.insert(all.end(), kBackendPatches, kBackendPatches + kBackendPatchCount);
  all.push_back(kBackendPatchWsa);
  all.push_back(kBackendPatchRoute);
  for (const Patch& p : all) {
    check(std::strstr(p.replacement, p.original) == nullptr,
          "replacement does not contain its original");
  }
}

// ---------------------------------------------------------------
// Test 2: apply_patches on a file containing the original strings
// ---------------------------------------------------------------
static void test_apply_patches() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_apply.js";
  std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());

  // Build content containing all three UI patch originals.
  std::string content = "prefix_";
  content += kUiPatch1a.original;
  content += "_middle_";
  content += kUiPatch2.original;
  content += "_gap_";
  content += kUiPatch3.original;
  content += "_suffix";

  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 3, "apply_patches returns 3");

  // Verify replacements.
  std::string patched = read_test_file(path);
  check(patched.find(kUiPatch1a.replacement) != std::string::npos,
        "patch1 replacement present");
  check(patched.find(kUiPatch2.replacement) != std::string::npos,
        "patch2 replacement present");
  check(patched.find(kUiPatch3.replacement) != std::string::npos,
        "patch3 replacement present");

  // Original strings should be gone.
  check(patched.find(kUiPatch1a.original) == std::string::npos,
        "patch1 original removed");

  // File size should be unchanged (same-length replacements).
  check(patched.size() == content.size(), "file size unchanged");

  // .bak should have been created with original content.
  std::string backup = read_test_file(bak);
  check(backup == content, ".bak contains original");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// ---------------------------------------------------------------
// Test 3: Already-patched file is a no-op
// ---------------------------------------------------------------
static void test_already_patched() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_noop.js";
  std::remove(path.c_str());

  // Content already has the replacement strings.
  std::string content = "prefix_";
  content += kUiPatch1a.replacement;
  content += "_middle_";
  content += kUiPatch2.replacement;
  content += "_suffix";

  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 0, "already-patched returns 0");

  // No .bak should be created.
  std::string bak = read_test_file(path + ".bak");
  check(bak.empty(), "no .bak created for already-patched");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 4: Missing file returns -1
// ---------------------------------------------------------------
static void test_missing_file() {
  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches("/nonexistent/path.js", kUiPatches, kUiPatchCount,
                        empty_md5);
  check(n == -1, "missing file returns -1");
}

// ---------------------------------------------------------------
// Test 5: Backend patches work
// ---------------------------------------------------------------
static void test_backend_patch() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_service.js";
  std::remove(path.c_str());

  // Protect 7.3.70 uses the same fragment in the ONVIF stream-settings
  // validator *before* the scope predicate.  Only the predicate may change.
  const std::string validator =
      "if((g||f||p||m||h||u)&&!e.isThirdPartyCamera)throw new "
      "i.BadRequestError(\"Camera is not a third-party (ONVIF) camera\");";
  const std::string scope =
      "scope_all_ui_cameras:e=>(0,t.deviceNotDeleted)(e)&&e.isAdopted&&"
      "(0,s.isCamera)(e)&&!e.isThirdPartyCamera,"
      "scope_all_cameras_with_nfcs:e=>e.isAdopted";
  const std::string content =
      validator + "x={scope_all_ui_cameras:_};" + scope;
  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kBackendPatches, kBackendPatchCount, empty_md5);
  check(n == 1, "backend patch returns 1");

  std::string patched = read_test_file(path);
  check(patched.find(validator) == 0,
        "backend: ONVIF stream-settings validator left untouched");
  check(patched.find("(0,s.isCamera)(e)/*isThirdPartyCamera */,"
                     "scope_all_cameras_with_nfcs") != std::string::npos,
        "backend: scope predicate patched");
  check(patched.size() == content.size(), "backend file size unchanged");

  // Re-running is a no-op.
  check(apply_patches(path, kBackendPatches, kBackendPatchCount,
                      empty_md5) == 0,
        "backend: second run is a no-op");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// An anchor that occurs more than once no longer identifies one piece of
// code, so the patch must be skipped and the file left alone.
static void test_ambiguous_anchor_skipped() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_ambiguous.js";
  std::remove(path.c_str());

  std::string content = "a=";
  content += kUiPatch3.original;
  content += ";b=";
  content += kUiPatch3.original;
  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 0, "ambiguous: nothing applied");
  check(read_test_file(path) == content, "ambiguous: file unchanged");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// Patch 2 targets the automation camera-list filter.  Protect 7.2+ reuses
// the bare expression for the microphone capability check, which must be
// left alone; the filter itself is patched wherever it still exists.
static void test_patch2_targets_automation_filter() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_patch2.js";
  std::remove(path.c_str());

  const std::string mic =
      "oxe=e=>e.isThirdPartyCamera&&!e.isPairedWithAiPort?"
      "!!e.thirdPartyCameraInfo?.enableRtspAudio:e.featureFlags.hasMic,";
  const std::string filter =
      "LH=(e,t,n)=>!(t&&e.nvrMac!==n||e.isThirdPartyCamera&&"
      "!e.isPairedWithAiPort),";
  write_test_file(path, mic + filter);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 1, "patch2: one replacement");
  const std::string out = read_test_file(path);
  check(out.find(mic) == 0, "patch2: microphone check untouched");
  check(out.find("e.nvrMac!==n||!1/*ThirdPartyCamera&&!isPairedWithAiPort*/)")
            != std::string::npos,
        "patch2: automation filter patched");

  // On a bundle with only the microphone check (Protect 7.3), nothing
  // changes.
  write_test_file(path, mic);
  check(apply_patches(path, kUiPatches, kUiPatchCount, empty_md5) == 0,
        "patch2: mic-only bundle left alone");
  check(read_test_file(path) == mic, "patch2: mic-only file unchanged");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// A file patched by an older rule set (here: the stream-settings validator
// instead of the scope predicate) must be rebuilt from the pristine .bak
// when dpkg confirms the .bak is the package original.
static void test_rebuild_from_pristine_bak() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_rebuild_service.js";
  std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());

  const std::string original =
      "if((g)&&!e.isThirdPartyCamera)throw 1;"
      "scope_all_ui_cameras:e=>e.isAdopted&&!e.isThirdPartyCamera,scope_x:1";
  std::string mispatched = original;
  mispatched.replace(mispatched.find(kBackendPatch1.original),
                     std::strlen(kBackendPatch1.original),
                     kBackendPatch1.replacement);
  write_test_file(bak, original);
  write_test_file(path, mispatched);

  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);

  int n = apply_patches(path, kBackendPatches, kBackendPatchCount, md5sums);
  check(n == 1, "rebuild: one patch applied to the pristine source");

  std::string expected = original;
  size_t at = expected.find("e.isAdopted&&!e.isThirdPartyCamera");
  expected.replace(at + std::strlen("e.isAdopted"),
                   std::strlen(kBackendPatch1.original),
                   kBackendPatch1.replacement);
  check(read_test_file(path) == expected,
        "rebuild: validator restored and scope predicate patched");
  check(read_test_file(bak) == original, "rebuild: pristine .bak kept");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// Protect 7.1 - 7.3's ONVIF client definition with its neighbours.
static const char kSoapClient[] =
    "return{body:M,rawText:C.text}};"
    "t.connSoapRequest=(e,r,o,i,a,n)=>(0,t.soapRequest)({hostname:e.hostname,"
    "port:e.port,path:r,body:{[o]:{\"@_xmlns\":i,...a}},username:e.username,"
    "password:e.password,timeShift:e.timeShift,extraHeaders:n}),"
    "t.soapRequest=async e=>(await(0,t.soapRequestRaw)(e)).body,";

// The WS-Addressing patch rewrites only the ONVIF client definition, is a
// no-op on the next pass, and is removed again when left out of the set.
static void test_wsa_patch() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_wsa_service.js";
  std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());

  const std::string original = kSoapClient;
  write_test_file(path, original);

  const Patch with_wsa[] = {kBackendPatch1, kBackendPatchWsa};
  std::unordered_map<std::string, std::string> empty_md5;
  check(apply_patches(path, with_wsa, 2, empty_md5) == 1,
        "wsa: one replacement");
  const std::string patched = read_test_file(path);
  check(patched.find("extraHeaders:n})") == std::string::npos,
        "wsa: original definition replaced");
  check(patched.find("extraHeaders:/*onvif-recorder:wsa*/") !=
            std::string::npos,
        "wsa: headers added to extraHeaders");
  check(patched.rfind("return{body:M,rawText:C.text}};"
                      "t.connSoapRequest=(e,r,o,i,a,n)=>", 0) == 0,
        "wsa: preceding code untouched");
  const std::string tail =
      "(n,`http://${e.hostname}${e.port==80?\"\":\":\"+e.port}${r}`,o)}),"
      "t.soapRequest=async e=>(await(0,t.soapRequestRaw)(e)).body,";
  check(patched.size() > tail.size() &&
            patched.compare(patched.size() - tail.size(), tail.size(),
                            tail) == 0,
        "wsa: following code untouched");
  check(read_test_file(bak) == original, "wsa: original backed up");
  check(apply_patches(path, with_wsa, 2, empty_md5) == 0,
        "wsa: second run is a no-op");
  check(read_test_file(path) == patched, "wsa: second run leaves file");

  // Leaving the patch out rebuilds the file from the package original.
  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);
  apply_patches(path, kBackendPatches, kBackendPatchCount, md5sums,
                &kBackendPatchWsa, 1);
  check(read_test_file(path) == original, "wsa: disabling restores original");

  // Without a backup dpkg can vouch for, a disabled patch is taken out of
  // the live file and the existing backup is kept.
  write_test_file(path, patched);
  write_test_file(bak, "older backup");
  apply_patches(path, kBackendPatches, kBackendPatchCount, empty_md5,
                &kBackendPatchWsa, 1);
  check(read_test_file(path) == original,
        "wsa: disabling without a verified backup removes it");
  check(read_test_file(bak) == "older backup",
        "wsa: disabling keeps the existing backup");

  // Two definitions no longer identify one client: leave the file alone.
  const std::string twice = original + original;
  write_test_file(path, twice);
  std::remove(bak.c_str());
  check(apply_patches(path, with_wsa, 2, empty_md5) == 0,
        "wsa: ambiguous anchor skipped");
  check(read_test_file(path) == twice, "wsa: ambiguous file unchanged");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// ---------------------------------------------------------------
// Event sharing route patch (kBackendPatchRoute)
// ---------------------------------------------------------------

// The route replacement exactly as specified, kept as an independent copy
// so an accidental edit of the patch table is caught.
static const char kRoute[] =
    R"js(t.soapRequestRaw=async function __onvrSoapRaw(e,__onvrRouted){/*onvif-reco)js"
    R"js(rder:pullpoint*/if(!__onvrRouted&&e&&"string"==typeof e.path){const __onvr)js"
    R"js(P="/onvif-recorder/pullpoint/",__onvrO={...e,hostname:"127.0.0.1",port:789)js"
    R"js(2},__onvrB=e.body||{},__onvrK=__onvrP+encodeURIComponent(e.hostname)+"/"+e)js"
    R"js(ncodeURIComponent(e.port),__onvrW=__onvrSoapRaw.w||(__onvrSoapRaw.w=new Ma)js"
    R"js(p),__onvrS=__onvrSoapRaw.s||(__onvrSoapRaw.s=new Map),__onvrR=(q,u)=>{cons)js"
    R"js(t n=Error("onvif-recorder: no answer"),c=performance.now(),l=u?1e3:4e3;if()js"
    R"js(c<(__onvrW.get(__onvrK)||0)||c<(__onvrW.get("")||0))return Promise.reject()js"
    R"js(n);let h,d=0,g=0;return Promise.race([__onvrSoapRaw(q,1).finally(()=>{d=1})js"
    R"js(),new Promise((_,j)=>{const a=b=>{const k=Math.min(l-g,500);h=setTimeout(()js"
    R"js()=>{const t=performance.now();g+=Math.min(t-b,k);g<3e3||__onvrS.set(__onvr)js"
    R"js(K,t);if(g<l)return a(t);setImmediate(()=>{if(d)return;if(!u){const z=perfo)js"
    R"js(rmance.now();[...__onvrS].some(([x,v])=>x!==__onvrK&&v>z-1e4)&&__onvrW.set)js"
    R"js(("",z+500);__onvrW.set(__onvrK,z+500)}j(n)})},k)};a(c)})]).finally(()=>cle)js"
    R"js(arTimeout(h))};if(e.path.startsWith(__onvrP))return __onvrR(__onvrO,__onvr)js"
    R"js(B.Unsubscribe);if(__onvrB.CreatePullPointSubscription)try{return await __o)js"
    R"js(nvrR({...__onvrO,path:__onvrK+e.path})}catch(x){}if(__onvrB.Renew){let m=0)js"
    R"js(;try{await __onvrR({...__onvrO,path:__onvrK+"/status",body:{GetServiceCapa)js"
    R"js(bilities:{"@_xmlns":"http://www.onvif.org/ver10/events/wsdl"}}});m=1}catch)js"
    R"js((x){}if(m)throw Error("onvif-recorder: moving subscription to proxy")}}con)js"
    R"js(st t=`${e.hostname}:${e.port}`;)js";

// The route replacement onvif-recorder 1.6.32 applied: one fail-fast window
// for all cameras, opened by every timeout.
static const char kRouteRetired1[] =
    R"js(t.soapRequestRaw=async function __onvrSoapRaw(e,__onvrRouted){/*onvif-re)js"
    R"js(corder:pullpoint*/if(!__onvrRouted&&e&&"string"==typeof e.path){const __)js"
    R"js(onvrP="/onvif-recorder/pullpoint/",__onvrO={...e,hostname:"127.0.0.1",po)js"
    R"js(rt:7892},__onvrB=e.body||{},__onvrK=__onvrP+encodeURIComponent(e.hostnam)js"
    R"js(e)+"/"+encodeURIComponent(e.port),__onvrR=(q,l)=>{const n=Error("onvif-r)js"
    R"js(ecorder: no answer");if(performance.now()<(__onvrSoapRaw.d||0))return Pr)js"
    R"js(omise.reject(n);let h;return Promise.race([__onvrSoapRaw(q,1),new Promis)js"
    R"js(e((_,j)=>{h=setTimeout(()=>{__onvrSoapRaw.d=performance.now()+500;j(n)},)js"
    R"js(l||4e3)})]).finally(()=>clearTimeout(h))};if(e.path.startsWith(__onvrP)))js"
    R"js(return __onvrR(__onvrO,__onvrB.Unsubscribe?1e3:0);if(__onvrB.CreatePullP)js"
    R"js(ointSubscription)try{return await __onvrR({...__onvrO,path:__onvrK+e.pat)js"
    R"js(h})}catch(x){}if(__onvrB.Renew){let m=0;try{await __onvrR({...__onvrO,pa)js"
    R"js(th:__onvrK+"/status",body:{GetServiceCapabilities:{"@_xmlns":"http://www)js"
    R"js(.onvif.org/ver10/events/wsdl"}}});m=1}catch(x){}if(m)throw Error("onvif-)js"
    R"js(recorder: moving subscription to proxy")}}const t=`${e.hostname}:${e.por)js"
    R"js(t}`;)js";

// Protect 7.3.70's SOAP transport function, the route patch's target.
static const char kSoapRaw[] =
    "t.soapRequestRaw=async e=>{const t=`${e.hostname}:${e.port}`;"
    "if(l.has(t))try{return await v(S,e)}catch(e){throw l.del(t),e}"
    "try{return await v(y,e)}catch(r){const o=r instanceof Error?r.code:"
    "void 0;if(!(r instanceof m||\"UND_ERR_RES_CONTENT_LENGTH_MISMATCH\"===o||"
    "\"HPE_INVALID_HEADER_TOKEN\"===o||r instanceof Error&&"
    "\"HTTPParserError\"===r.name))throw r;d.warn(`ONVIF SOAP request to "
    "${t}${e.path} failed with undici, retrying with raw socket`);"
    "const i=await v(S,e);return l.set(t,!0),i}};"
    "const y=async(e,t,r)=>{if(/[\\r\\n]/.test(e))throw 1;return{t,r}};";

// A service.js slice with both the WS-Addressing and route anchors.
static std::string soap_fixture() {
  return std::string(kSoapClient) + kSoapRaw;
}

// @p s with the single occurrence of @p p's original replaced.
static std::string with_patch(std::string s, const Patch& p) {
  const size_t at = s.find(p.original);
  if (at != std::string::npos) {
    s.replace(at, std::strlen(p.original), p.replacement);
  }
  return s;
}

// One apply_patches() pass with outcomes for every enabled and removed
// patch.
struct PatchRun {
  int applied = 0;
  std::vector<PatchOutcome> on;
  std::vector<PatchOutcome> off;
};

static PatchRun run_patches(
    const std::string& path, const std::vector<Patch>& on,
    const std::vector<Patch>& off = {},
    const std::unordered_map<std::string, std::string>& md5sums = {}) {
  PatchRun r;
  r.on.assign(on.size(), PatchOutcome::kNotRun);
  r.off.assign(off.size(), PatchOutcome::kNotRun);
  r.applied = apply_patches(path, on.data(), on.size(), md5sums, off.data(),
                            off.size(), r.on.data(), r.off.data());
  return r;
}

static size_t occurrences(const std::string& s, const char* needle) {
  return count_in(s, needle, 0, s.size());
}

// The patch text is the specified one, and it ends with the anchor's own
// first statement so the original body follows unchanged.
static void test_route_patch_text() {
  check(std::string(kBackendPatchRoute.replacement) == kRoute,
        "route: replacement is the specified text");
  check(std::string(kBackendPatchRoute.original) ==
            "t.soapRequestRaw=async e=>{const t=`${e.hostname}:${e.port}`;",
        "route: anchor");
  const std::string tail = "const t=`${e.hostname}:${e.port}`;";
  const std::string rep = kBackendPatchRoute.replacement;
  check(rep.size() > tail.size() &&
            rep.compare(rep.size() - tail.size(), tail.size(), tail) == 0,
        "route: replacement keeps the body's first statement");
  check(rep.find(kInjectedMarker) != std::string::npos,
        "route: replacement carries the injected-code marker");
  check(has_known_patch(rep), "route: patched file is recognised as ours");
  check(rep.find(kBackendPatchRoute.original) == std::string::npos,
        "route: replacement does not contain its anchor");

  // Earlier texts: each exactly as a release applied it, taken out back to
  // the same anchor, and never the current text.
  const char* const retired[] = {kRouteRetired1};
  check(std::size(kRetiredRoutePatches) == std::size(retired),
        "route: every earlier text is retired");
  for (size_t i = 0; i < std::size(kRetiredRoutePatches); ++i) {
    const Patch& p = kRetiredRoutePatches[i];
    const std::string old = p.replacement;
    check(old == retired[i], "route: retired text is the one once applied");
    check(std::string(p.original) == kBackendPatchRoute.original,
          "route: retired text goes back to the same anchor");
    check(old != rep, "route: retired text is not the current one");
    check(old.size() > tail.size() &&
              old.compare(old.size() - tail.size(), tail.size(), tail) == 0,
          "route: retired text keeps the body's first statement");
    check(has_known_patch(old), "route: retired text is recognised as ours");
  }
}

// The patched function still ends where Protect's did and uses neither
// `this` nor `arguments`, and the prologue declares only names that start
// with kRouteNamePrefix in the function's scope.
static void test_route_patch_scan() {
  const std::string original = kSoapRaw;
  const std::string patched = with_patch(original, kBackendPatchRoute);
  const JsBlockScan orig = scan_js_block(original, original.find('{'));
  const JsBlockScan scan = scan_js_block(patched, patched.find('{'));
  const size_t grown = std::strlen(kBackendPatchRoute.replacement) -
                       std::strlen(kBackendPatchRoute.original);
  check(orig.end != std::string::npos && scan.end == orig.end + grown,
        "route scan: patched function ends where the original did");
  check(!orig.uses_this_or_arguments && !scan.uses_this_or_arguments,
        "route scan: injected code uses neither this nor arguments");

  // Declarations directly in the prologue's block (the function's block is
  // depth 1, the prologue's `if` block depth 2), before the original body.
  const std::string rep = kBackendPatchRoute.replacement;
  const size_t body = rep.find("const t=`${e.hostname}");
  std::vector<std::string> names;
  int depth = 0;
  for (size_t i = rep.find('{'); i < body; ++i) {
    if (rep[i] == '{') ++depth;
    if (rep[i] == '}') --depth;
    if (depth != 2 || (rep.compare(i, 6, "const ") != 0 &&
                       rep.compare(i, 4, "let ") != 0)) {
      continue;
    }
    size_t j = rep.find(' ', i) + 1;
    while (j < body) {
      size_t k = j;
      while (k < body && is_ident_char(rep[k])) ++k;
      names.push_back(rep.substr(j, k - j));
      // Skip the initialiser, up to the next declarator or the end.
      int nest = 0;
      while (k < body && (nest > 0 || (rep[k] != ',' && rep[k] != ';'))) {
        if (rep[k] == '(' || rep[k] == '[' || rep[k] == '{') ++nest;
        if (rep[k] == ')' || rep[k] == ']' || rep[k] == '}') --nest;
        ++k;
      }
      if (k >= body || rep[k] == ';') break;
      j = k + 1;
    }
  }
  const std::vector<std::string> expected = {
      "__onvrP", "__onvrO", "__onvrB", "__onvrK", "__onvrW", "__onvrS",
      "__onvrR"};
  check(names == expected, "route scan: prologue declares only __onvr names");
}

// A service.js an earlier version patched gets the current route text, and
// loses the earlier one when sharing is turned off.
static void test_route_patch_upgrade() {
  const std::string path = temp_dir() + "/test_route_upgrade_service.js";
  const std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());
  const std::unordered_map<std::string, std::string> no_md5;
  const Patch& old_route = kRetiredRoutePatches[0];

  const std::string original = soap_fixture();
  const std::string wsa_only = with_patch(original, kBackendPatchWsa);
  const std::string old_both = with_patch(wsa_only, old_route);
  const std::string new_both = with_patch(wsa_only, kBackendPatchRoute);
  check(old_both != wsa_only && new_both != wsa_only && old_both != new_both,
        "upgrade: fixtures differ");

  // Sharing on: the earlier text is replaced and the backup kept.
  write_test_file(path, old_both);
  write_test_file(bak, original);
  PatchReport rep;
  check(patch_service_js(path, PatchOptions{true, true}, no_md5, &rep) == 1,
        "upgrade: one replacement");
  check(rep.event_proxy == PatchOutcome::kApplied &&
            rep.onvif_addressing == PatchOutcome::kAlready,
        "upgrade: outcomes");
  check(read_test_file(path) == new_both, "upgrade: current text in place");
  check(occurrences(read_test_file(path), kRouteNamePrefix) ==
            occurrences(kBackendPatchRoute.replacement, kRouteNamePrefix),
        "upgrade: nothing of the earlier text left");
  check(read_test_file(bak) == original, "upgrade: backup kept");
  check(patch_service_js(path, PatchOptions{true, true}, no_md5, &rep) == 0 &&
            rep.event_proxy == PatchOutcome::kAlready,
        "upgrade: second run is a no-op");
  check(read_test_file(path) == new_both, "upgrade: second run leaves file");

  // With a backup dpkg vouches for, the file is rebuilt from it.
  write_test_file(path, old_both);
  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);
  check(patch_service_js(path, PatchOptions{true, true}, md5sums, &rep) == 2 &&
            rep.event_proxy == PatchOutcome::kApplied,
        "upgrade from .bak: applied");
  check(read_test_file(path) == new_both, "upgrade from .bak: file");

  // Sharing off: the earlier text is removed, and reported as removed.
  write_test_file(path, old_both);
  patch_service_js(path, PatchOptions{true, false}, no_md5, &rep);
  check(rep.event_proxy == PatchOutcome::kRemoved &&
            rep.onvif_addressing == PatchOutcome::kAlready,
        "old route off: outcomes");
  check(read_test_file(path) == wsa_only, "old route off: removed byte-exact");
  patch_service_js(path, PatchOptions{true, false}, no_md5, &rep);
  check(rep.event_proxy == PatchOutcome::kAbsent, "old route off: then absent");

  write_test_file(path, with_patch(original, old_route));
  patch_service_js(path, PatchOptions{false, false}, no_md5, &rep);
  check(rep.event_proxy == PatchOutcome::kRemoved &&
            rep.onvif_addressing == PatchOutcome::kAbsent,
        "old route and wsa off: outcomes");
  check(read_test_file(path) == original, "old route and wsa off: original");

  // Two copies of the earlier text identify no single function: left alone.
  const std::string old_raw = with_patch(kSoapRaw, old_route);
  const std::string twice = old_raw + old_raw;
  write_test_file(path, twice);
  patch_service_js(path, PatchOptions{false, true}, no_md5, &rep);
  check(rep.event_proxy == PatchOutcome::kNotApplicable,
        "old route twice, on: not applicable");
  check(read_test_file(path) == twice, "old route twice, on: unchanged");
  patch_service_js(path, PatchOptions{false, false}, no_md5, &rep);
  check(rep.event_proxy == PatchOutcome::kAmbiguous,
        "old route twice, off: ambiguous");
  check(read_test_file(path) == twice, "old route twice, off: unchanged");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// The route patch applies once, with or without the WS-Addressing patch,
// in either order, is a no-op on the next pass and is removed byte-exact.
static void test_route_patch_apply() {
  const std::string path = temp_dir() + "/test_route_service.js";
  const std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());

  const std::string original = soap_fixture();
  const std::string route_only = with_patch(original, kBackendPatchRoute);
  const std::string wsa_only = with_patch(original, kBackendPatchWsa);
  const std::string both = with_patch(wsa_only, kBackendPatchRoute);
  check(route_only != original && wsa_only != original && both != wsa_only,
        "route: fixture holds both anchors");

  // Route alone.
  write_test_file(path, original);
  PatchRun r = run_patches(path, {kBackendPatch1, kBackendPatchRoute},
                           {kBackendPatchWsa});
  check(r.applied == 1, "route: one replacement");
  check(r.on[1] == PatchOutcome::kApplied, "route: outcome applied");
  check(r.off[0] == PatchOutcome::kAbsent, "route: disabled wsa absent");
  check(read_test_file(path) == route_only, "route: only the anchor changed");
  check(occurrences(read_test_file(path), kBackendPatchRoute.replacement) ==
            1,
        "route: applied exactly once");
  check(read_test_file(bak) == original, "route: original backed up");

  // Idempotent.
  r = run_patches(path, {kBackendPatch1, kBackendPatchRoute},
                  {kBackendPatchWsa});
  check(r.applied == 0, "route: second run is a no-op");
  check(r.on[1] == PatchOutcome::kAlready, "route: outcome already");
  check(read_test_file(path) == route_only, "route: second run leaves file");

  // Adding WS-Addressing to a route-patched file.
  r = run_patches(path, {kBackendPatch1, kBackendPatchWsa, kBackendPatchRoute});
  check(r.applied == 1, "route then wsa: wsa added");
  check(r.on[1] == PatchOutcome::kApplied && r.on[2] == PatchOutcome::kAlready,
        "route then wsa: outcomes");
  check(read_test_file(path) == both, "route then wsa: both present");
  check(read_test_file(bak) == original, "route then wsa: .bak kept");

  // Adding the route to a WS-Addressing-patched file, in both list orders.
  for (int order = 0; order < 2; ++order) {
    write_test_file(path, wsa_only);
    std::remove(bak.c_str());
    const std::vector<Patch> on =
        order == 0
            ? std::vector<Patch>{kBackendPatch1, kBackendPatchWsa,
                                 kBackendPatchRoute}
            : std::vector<Patch>{kBackendPatch1, kBackendPatchRoute,
                                 kBackendPatchWsa};
    r = run_patches(path, on);
    const size_t wsa = order == 0 ? 1 : 2;
    const size_t route = order == 0 ? 2 : 1;
    check(r.applied == 1, "wsa then route: route added");
    check(r.on[wsa] == PatchOutcome::kAlready &&
              r.on[route] == PatchOutcome::kApplied,
          "wsa then route: outcomes");
    check(read_test_file(path) == both, "wsa then route: both present");
    check(occurrences(read_test_file(path), kBackendPatchRoute.replacement) ==
              1,
          "wsa then route: route applied exactly once");
  }

  // Both at once from the original, in both list orders.
  for (int order = 0; order < 2; ++order) {
    write_test_file(path, original);
    std::remove(bak.c_str());
    const std::vector<Patch> on =
        order == 0
            ? std::vector<Patch>{kBackendPatch1, kBackendPatchWsa,
                                 kBackendPatchRoute}
            : std::vector<Patch>{kBackendPatch1, kBackendPatchRoute,
                                 kBackendPatchWsa};
    r = run_patches(path, on);
    check(r.applied == 2, "both: two replacements");
    check(read_test_file(path) == both, "both: same file in either order");
  }

  // Disabled without a backup dpkg vouches for: removed from the live
  // file, byte-exact, leaving WS-Addressing in place.
  write_test_file(bak, "older backup");
  r = run_patches(path, {kBackendPatch1, kBackendPatchWsa},
                  {kBackendPatchRoute});
  check(r.off[0] == PatchOutcome::kRemoved, "route off: outcome removed");
  check(r.on[1] == PatchOutcome::kAlready, "route off: wsa outcome already");
  check(read_test_file(path) == wsa_only, "route off: removed byte-exact");
  check(read_test_file(bak) == "older backup", "route off: backup kept");
  r = run_patches(path, {kBackendPatch1, kBackendPatchWsa},
                  {kBackendPatchRoute});
  check(r.applied == 0 && r.off[0] == PatchOutcome::kAbsent,
        "route off: second run absent");
  check(read_test_file(path) == wsa_only, "route off: second run no change");

  // Disabled with a verified backup: rebuilt from it.
  write_test_file(path, both);
  write_test_file(bak, original);
  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);
  r = run_patches(path, {kBackendPatch1, kBackendPatchWsa},
                  {kBackendPatchRoute}, md5sums);
  check(r.off[0] == PatchOutcome::kRemoved && r.on[1] == PatchOutcome::kAlready,
        "route off from .bak: outcomes");
  check(read_test_file(path) == wsa_only, "route off from .bak: file");
  check(read_test_file(bak) == original, "route off from .bak: .bak kept");

  // Both disabled: back to the original.
  write_test_file(path, both);
  r = run_patches(path, {kBackendPatch1},
                  {kBackendPatchWsa, kBackendPatchRoute});
  check(r.off[0] == PatchOutcome::kRemoved &&
            r.off[1] == PatchOutcome::kRemoved,
        "both off: outcomes");
  check(read_test_file(path) == original, "both off: original restored");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// The route patch is refused where Protect has no such function (7.0),
// where the function body relies on arrow-function semantics, where the
// injected names are already in use, or where the body cannot be
// delimited.  The file is left as it is.
static void test_route_patch_not_applicable() {
  const std::string path = temp_dir() + "/test_route_na_service.js";
  const std::string bak = path + ".bak";
  const std::vector<Patch> on = {kBackendPatch1, kBackendPatchRoute};

  // @p reason: part of route_patch_refusal()'s answer, or nullptr when the
  // anchor is absent.
  auto expect_na = [&](const std::string& content, const char* label,
                       const char* reason) {
    std::remove(bak.c_str());
    write_test_file(path, content);
    const PatchRun r = run_patches(path, on);
    const std::string l = label;
    check(r.applied == 0, (l + ": nothing applied").c_str());
    check(r.on[1] == PatchOutcome::kNotApplicable,
          (l + ": outcome not applicable").c_str());
    check(read_test_file(path) == content, (l + ": file unchanged").c_str());
    const size_t at = content.find(kBackendPatchRoute.original);
    if (reason == nullptr) {
      check(at == std::string::npos, (l + ": no anchor").c_str());
    } else {
      const char* why = at == std::string::npos
                            ? nullptr
                            : route_patch_refusal(content, at);
      check(why != nullptr && std::strstr(why, reason) != nullptr,
            (l + ": refused for the expected reason").c_str());
    }
  };

  // Protect 7.0: SOAP requests through a different client, no anchor.
  expect_na("t.soapRequest=async e=>{const r=await o.default.post(e);"
            "return r.body};",
            "7.0", nullptr);

  const std::string fixture = soap_fixture();
  auto edited = [&](const char* from, const char* to) {
    std::string s = fixture;
    const size_t at = s.find(from);
    check(at != std::string::npos, "route n/a: edit target present");
    s.replace(at, std::strlen(from), to);
    return s;
  };
  const char* kThisOrArguments = "uses this or arguments";
  expect_na(edited("return l.set(t,!0),i", "return l.set(t,!0),this.i"),
            "this", kThisOrArguments);
  expect_na(edited("d.warn(`ONVIF", "d.warn(arguments[0],`ONVIF"),
            "arguments", kThisOrArguments);
  expect_na(edited("d.warn(`ONVIF SOAP request to ${t}",
                   "d.warn(`ONVIF SOAP request to ${this.t}"),
            "this in a template substitution", kThisOrArguments);
  expect_na(edited("const i=await", "const i=e--/2,j=this,k=await"),
            "this after a decrement and a division", kThisOrArguments);
  expect_na(fixture + "const __onvrX=1;", "foreign __onvr",
            "names starting with __onvr");
  expect_na(fixture.substr(0, fixture.find("}};const y=")), "no end",
            "end of soapRequestRaw was not found");

  // `this` and `arguments` as property names, inside strings or as part of
  // longer names do not count.
  std::remove(bak.c_str());
  write_test_file(path,
                  edited("return l.set(t,!0),i",
                         "return l.set(t,!0),e.this,e.arguments,thisArg,"
                         "\"this\",`arguments`,i"));
  const PatchRun r = run_patches(path, on);
  check(r.on[1] == PatchOutcome::kApplied,
        "route: property names and strings named this/arguments allowed");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// Two copies of the anchor no longer identify one function.
static void test_route_patch_ambiguous() {
  const std::string path = temp_dir() + "/test_route_twice_service.js";
  const std::string twice = std::string(kSoapRaw) + kSoapRaw;
  write_test_file(path, twice);
  const PatchRun r = run_patches(path, {kBackendPatch1, kBackendPatchRoute});
  check(r.applied == 0, "route twice: nothing applied");
  check(r.on[1] == PatchOutcome::kAmbiguous, "route twice: outcome ambiguous");
  check(read_test_file(path) == twice, "route twice: file unchanged");
  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// Outcomes of a file that cannot be read stay at their initial value, and
// a disabled patch whose replacement is not unique is reported as such.
static void test_patch_outcomes() {
  const PatchRun missing = run_patches("/nonexistent/service.js",
                                       {kBackendPatchRoute},
                                       {kBackendPatchWsa});
  check(missing.applied == -1 && missing.on[0] == PatchOutcome::kNotRun &&
            missing.off[0] == PatchOutcome::kNotRun,
        "outcomes: missing file not run");

  const std::string path = temp_dir() + "/test_outcomes_service.js";
  const std::string patched = with_patch(kSoapRaw, kBackendPatchRoute);
  const std::string twice = patched + patched;
  write_test_file(path, twice);
  const PatchRun r = run_patches(path, {kBackendPatch1}, {kBackendPatchRoute});
  check(r.off[0] == PatchOutcome::kAmbiguous,
        "outcomes: duplicated patch cannot be removed");
  check(read_test_file(path) == twice, "outcomes: duplicated file unchanged");
  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());

  check(std::string(patch_outcome_name(PatchOutcome::kNotRun)) == "not_run" &&
            std::string(patch_outcome_name(PatchOutcome::kApplied)) ==
                "applied" &&
            std::string(patch_outcome_name(PatchOutcome::kAlready)) ==
                "already" &&
            std::string(patch_outcome_name(PatchOutcome::kRemoved)) ==
                "removed" &&
            std::string(patch_outcome_name(PatchOutcome::kAbsent)) ==
                "absent" &&
            std::string(patch_outcome_name(PatchOutcome::kNotApplicable)) ==
                "not_applicable" &&
            std::string(patch_outcome_name(PatchOutcome::kAmbiguous)) ==
                "ambiguous" &&
            std::string(patch_outcome_name(PatchOutcome::kInvalid)) ==
                "invalid",
        "outcomes: names");
}

// patch_service_js() reports each optional patch: applied when turned on,
// removed when turned off again, not applicable where Protect lacks the
// code, and not run when the file is missing.
static void test_service_js_report() {
  const std::string path = temp_dir() + "/test_report_service.js";
  const std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());
  const std::unordered_map<std::string, std::string> no_md5;
  const std::string original = soap_fixture();
  write_test_file(path, original);

  PatchReport rep;
  check(patch_service_js(path, PatchOptions{true, true}, no_md5, &rep) == 2,
        "report: both patches written");
  check(rep.onvif_addressing == PatchOutcome::kApplied &&
            rep.event_proxy == PatchOutcome::kApplied,
        "report: both applied");
  patch_service_js(path, PatchOptions{true, true}, no_md5, &rep);
  check(rep.onvif_addressing == PatchOutcome::kAlready &&
            rep.event_proxy == PatchOutcome::kAlready,
        "report: both already");
  patch_service_js(path, PatchOptions{true, false}, no_md5, &rep);
  check(rep.onvif_addressing == PatchOutcome::kAlready &&
            rep.event_proxy == PatchOutcome::kRemoved,
        "report: route removed");
  patch_service_js(path, PatchOptions{false, true}, no_md5, &rep);
  check(rep.onvif_addressing == PatchOutcome::kRemoved &&
            rep.event_proxy == PatchOutcome::kApplied,
        "report: wsa removed, route applied");
  patch_service_js(path, PatchOptions{false, false}, no_md5, &rep);
  check(rep.onvif_addressing == PatchOutcome::kAbsent &&
            rep.event_proxy == PatchOutcome::kRemoved,
        "report: route removed, wsa absent");
  check(read_test_file(path) == original, "report: original restored");

  // Defaults: WS-Addressing on, event sharing off.
  patch_service_js(path, PatchOptions{}, no_md5, &rep);
  check(rep.onvif_addressing == PatchOutcome::kApplied &&
            rep.event_proxy == PatchOutcome::kAbsent,
        "report: defaults");

  const std::string old_protect =
      "t.soapRequest=async e=>{const r=await o.default.post(e);"
      "return r.body};";
  write_test_file(path, old_protect);
  patch_service_js(path, PatchOptions{true, true}, no_md5, &rep);
  check(rep.onvif_addressing == PatchOutcome::kNotApplicable &&
            rep.event_proxy == PatchOutcome::kNotApplicable,
        "report: 7.0 not applicable");
  check(read_test_file(path) == old_protect, "report: 7.0 unchanged");

  check(patch_service_js("/nonexistent/service.js", PatchOptions{true, true},
                         no_md5, &rep) == -1 &&
            rep.onvif_addressing == PatchOutcome::kNotRun &&
            rep.event_proxy == PatchOutcome::kNotRun,
        "report: missing file not run");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// ---------------------------------------------------------------
// Local patches (--local_patches)
// ---------------------------------------------------------------

// One local patches entry as JSON.
static std::string local_entry(const std::string& name,
                               const std::string& original,
                               const std::string& replacement,
                               const std::string& file = "service.js") {
  return "{\"name\":" + json_quote(name) + ",\"file\":" + json_quote(file) +
         ",\"original\":" + json_quote(original) +
         ",\"replacement\":" + json_quote(replacement) + "}";
}

// Whether a valid entry @p e has @p name and @p replacement.
static bool valid_entry(const LocalPatch& e, const std::string& name,
                        const std::string& replacement) {
  return e.error.empty() && e.name == name && e.file == "service.js" &&
         e.replacement == replacement;
}

// Whether @p r has no error and its entries are valid entries named as in
// @p names.
static bool all_valid(const LocalPatches& r,
                      const std::vector<std::string>& names) {
  if (!r.error.empty() || r.entries.size() != names.size()) return false;
  for (size_t i = 0; i < names.size(); ++i) {
    if (!r.entries[i].error.empty() || r.entries[i].name != names[i]) {
      return false;
    }
  }
  return true;
}

// Whether @p bad, on line 2 between two valid entries, is the only entry
// skipped: named @p name, with only its name kept and an error containing
// @p part, while the file has no error.
static bool skipped_with(const std::string& bad, const std::string& part,
                         const std::string& name = "entry 2") {
  const std::string text = "[" + local_entry("a", "x=1;", "x=2;") + ",\n" +
                           bad + ",\n" + local_entry("c", "z=1;", "z=2;") +
                           "]";
  const LocalPatches r = parse_local_patches(text);
  const bool ok =
      r.error.empty() && r.entries.size() == 3 &&
      valid_entry(r.entries[0], "a", "x=2;") &&
      valid_entry(r.entries[2], "c", "z=2;") && r.entries[1].name == name &&
      r.entries[1].file.empty() && r.entries[1].original.empty() &&
      r.entries[1].replacement.empty() &&
      r.entries[1].error.find(part) != std::string::npos;
  if (!ok) {
    std::cerr << "  file error: " << r.error
              << "\n  entries: " << r.entries.size() << '\n';
    for (const LocalPatch& e : r.entries) {
      std::cerr << "    " << e.name << ": " << e.error << '\n';
    }
  }
  return ok;
}

// Whether parsing @p text keeps the valid entries named @p names and
// reports the file error @p error exactly.
static bool stops_with(const std::string& text,
                       const std::vector<std::string>& names,
                       const std::string& error) {
  LocalPatches r = parse_local_patches(text);
  const std::string got = r.error;
  r.error.clear();
  const bool ok = all_valid(r, names) && got == error;
  if (!ok) {
    std::cerr << "  file error: " << got << "\n  entries: " << r.entries.size()
              << '\n';
  }
  return ok;
}

// The file format: an array of objects with four string members.
static void test_parse_local_patches() {
  const std::string text =
      "\xEF\xBB\xBF[\n"
      "  {\"name\": \"a\", \"file\": \"service.js\", \"original\": \"x=1;\","
      " \"replacement\": \"x=2;\"},\n"
      "  {\"file\": \"service.js\", \"original\": \"y\\u00e9\\ud83d\\ude00"
      "\\n\\\"\\\\\\/\", \"replacement\": \"z\"}\n"
      "]\n";
  const LocalPatches r = parse_local_patches(text);
  check(all_valid(r, {"a", "entry 2"}), "local parse: two entries");
  if (r.entries.size() == 2) {
    const LocalPatch& a = r.entries[0];
    const LocalPatch& b = r.entries[1];
    check(a.name == "a" && a.file == "service.js" && a.original == "x=1;" &&
              a.replacement == "x=2;",
          "local parse: first entry");
    check(b.original == "y\xC3\xA9\xF0\x9F\x98\x80\n\"\\/",
          "local parse: escapes and surrogate pairs decoded");
  }
  for (const char* empty : {"", " \n\t", "\xEF\xBB\xBF", "[]", " [ ]\n"}) {
    check(all_valid(parse_local_patches(empty), {}),
          "local parse: no entries");
  }
}

// Each entry stands alone: one that cannot be used is skipped with its own
// error (and position) and keeps only its name, and the entries around it
// are still read.
static void test_local_patch_entry_errors() {
  // Valid JSON, not a valid entry.
  check(skipped_with("\"x\"", "line 2, column 1: each entry must be an object"),
        "local skip: string entry");
  check(skipped_with("42", "each entry must be an object"),
        "local skip: number entry");
  check(skipped_with("null", "each entry must be an object"),
        "local skip: null entry");
  check(skipped_with("[1, {\"a\": \"]\"}]", "each entry must be an object"),
        "local skip: array entry");
  check(skipped_with("{\"enabled\":\"false\"}",
                     "line 2, column 2: unknown key \"enabled\" (allowed: "
                     "name, file, original, replacement)"),
        "local skip: unknown key");
  check(skipped_with("{\"name\":\"n\",\"name\":\"m\"}",
                     "line 2, column 13: \"name\" given twice", "n"),
        "local skip: duplicate key, first name kept");
  check(skipped_with("{\"name\":\"n\",\"file\":\"service.js\",\"original\":1,"
                     "\"replacement\":\"b\"}",
                     "the value of \"original\" must be a string", "n"),
        "local skip: non-string value");
  check(skipped_with("{\"name\":\"n\",\"file\":[\"x\",{\"y\":\"]\"}],"
                     "\"original\":\"o\",\"replacement\":\"r\"}",
                     "the value of \"file\" must be a string", "n"),
        "local skip: nested value skipped by its brackets");
  check(skipped_with("{}", "line 2, column 1: \"file\" is missing; use "
                           "\"service.js\""),
        "local skip: empty object");
  check(skipped_with("{\"original\":\"a\",\"replacement\":\"b\"}",
                     "line 2, column 1: \"file\" is missing"),
        "local skip: file missing");
  check(skipped_with(local_entry("ui", "a", "b", "swai.js"),
                     "line 2, column 1: \"file\" is \"swai.js\"; only "
                     "\"service.js\" can be patched",
                     "ui"),
        "local skip: wrong file");
  check(skipped_with("{\"file\":\"service.js\",\"replacement\":\"b\"}",
                     "\"original\" is missing or empty"),
        "local skip: original missing");
  check(skipped_with(local_entry("n", "", "b"),
                     "\"original\" is missing or empty", "n"),
        "local skip: original empty");
  check(skipped_with(local_entry("n", "x", ""),
                     "\"replacement\" is missing or empty", "n"),
        "local skip: replacement empty");
  check(skipped_with(local_entry("n", "x=1", "x=1;y=2"),
                     "\"replacement\" contains \"original\"", "n"),
        "local skip: replacement contains original");
  check(skipped_with("{\"name\":\"n\",\"file\":\"service.js\","
                     "\"original\":\"x\\u0000\",\"replacement\":\"y\"}",
                     "contains a \\u0000 character", "n"),
        "local skip: NUL character");
  check(skipped_with(local_entry("a\nb", "o", "r", "x\"y"),
                     "\"file\" is \"x\\\"y\"", "a\nb"),
        "local skip: values quoted in the error");

  // Malformed JSON inside an entry: skipped up to its closing brace.
  check(skipped_with("{\"name\":\"n\",\"file\":\"service.js\",\"original\":"
                     "\"a\nb\",\"replacement\":\"c\"}",
                     "not valid JSON at line 2, column 46: line break or "
                     "control character inside a string",
                     "n"),
        "local skip: raw line break in a string");
  check(skipped_with("{\"name\":\"\\q\"}", "unknown escape"),
        "local skip: unknown escape, name not read");
  check(skipped_with("{\"name\":\"n\",\"original\":\"\\ud83d\"}", "unpaired",
                     "n"),
        "local skip: unpaired surrogate");
  check(skipped_with("{\"name\":\"\\u12\"}", "four hex digits"),
        "local skip: short \\u escape");
  check(skipped_with("{\"name\" \"b\"}",
                     "not valid JSON at line 2, column 9: expected ':'"),
        "local skip: missing colon, with position");
  check(skipped_with("{\"name\":\"n\" \"file\":\"service.js\"}",
                     "expected ',' or '}'", "n"),
        "local skip: missing comma between members");
  check(skipped_with("{\"name\":\"n\",}", "expected a string", "n"),
        "local skip: trailing comma in an object");
  check(skipped_with("{\"name\":\"n\\\"}{[\",\"oops\":\"x\"}",
                     "unknown key \"oops\"", "n\"}{["),
        "local skip: brackets and escaped quote inside a string");
  check(skipped_with("{'name':'n'}", "expected a string"),
        "local skip: single quotes");
  check(skipped_with("{\"name\":\"n\",\"file\":\"service.js\",\"original\":"
                     "\"[{\",\"replacement\":\"x\" \"y\"}",
                     "expected ',' or '}'", "n"),
        "local skip: brackets inside strings of a broken entry");
  check(skipped_with("{\"name\":\"n\",\"file\":\"service.js\",\"original\":"
                     "\"f({\"a\":1},{\"b\":2})\",\"replacement\":\"r\"}",
                     "expected ',' or '}'", "n"),
        "local skip: unescaped quotes around object literals");

  // Two broken entries next to each other, and one first and last.
  const LocalPatches r = parse_local_patches(
      "[{\"x\":\"1\"},{\"y\" 2},\n" + local_entry("a", "o", "r") +
      ",{\"name\":\"z\"}]");
  check(r.error.empty() && r.entries.size() == 4 &&
            r.entries[0].name == "entry 1" &&
            r.entries[0].error.find("unknown key \"x\"") != std::string::npos &&
            r.entries[1].name == "entry 2" &&
            r.entries[1].error.find("expected ':'") != std::string::npos &&
            valid_entry(r.entries[2], "a", "r") &&
            r.entries[3].name == "z" &&
            r.entries[3].error.find("\"file\" is missing") !=
                std::string::npos,
        "local skip: broken entries first, together and last");
}

// Where the end of a broken entry cannot be found, or text follows an
// entry that is not ',' or ']', the entries before it are kept and the
// rest of the file is one error.
static void test_local_patches_stop() {
  const std::string a = local_entry("a", "x=1;", "x=2;");
  const std::string b = local_entry("b", "y=1;", "y=2;");
  check(stops_with("{\"name\":\"a\"}", {},
                   "not valid JSON at line 1, column 1: expected '[' (the "
                   "file holds an array of entries)"),
        "local stop: not an array");
  check(stops_with("[" + a + " " + b + "]", {"a"},
                   "not valid JSON at line 1, column 74: expected ',' or "
                   "']'; the rest of the file, from line 1, column 74, is "
                   "ignored"),
        "local stop: missing comma between entries");
  check(stops_with("[" + a + ",\n{\"name\":\"b", {"a"},
                   "not valid JSON at line 2, column 11: string not "
                   "terminated; the rest of the file, from line 2, column 1, "
                   "is ignored"),
        "local stop: unterminated string at the end");
  check(stops_with("[" + a + ",\n{\"name\":\"b, \"file\":\"service.js\","
                   "\"original\":\"o\",\"replacement\":\"r\"},\n" + b + "]",
                   {"a"},
                   "not valid JSON at line 2, column 14: expected ',' or "
                   "'}'; the rest of the file, from line 2, column 1, is "
                   "ignored"),
        "local stop: unterminated string in a middle entry");
  // A missing '"' turns round which quotes open strings, and the escaped
  // quote in the next entry turns it back, so the scan ends in that entry:
  // reading stops instead of skipping it.
  const std::string q = local_entry("q", "if(x===\"a\"){y()}", "z");
  check(stops_with("[" + a + ",\n{\"name\":\"b\",\"file\":\"service.js\","
                   "\"original\":\"o,\"replacement\":\"r\"},\n" + q + ",\n" +
                       b + "]",
                   {"a"},
                   "not valid JSON at line 2, column 48: expected ',' or "
                   "'}'; the rest of the file, from line 2, column 1, is "
                   "ignored"),
        "local stop: missing closing quote, escaped quote in the next entry");
  check(stops_with("[" + a + ",\n{\"name\":\"b\",\"file\":\"service.js\","
                   "\"original\":o\",\"replacement\":\"r\"},\n" + q + ",\n" +
                       b + "]",
                   {"a"},
                   "line 2, column 44: the value of \"original\" must be a "
                   "string; the rest of the file, from line 2, column 1, is "
                   "ignored"),
        "local stop: missing opening quote, escaped quote in the next entry");
  check(stops_with("[" + a + ",\n{\"name\":\"b\" \"x\"} y,\n" + b + "]", {"a"},
                   "not valid JSON at line 2, column 13: expected ',' or "
                   "'}'; the rest of the file, from line 2, column 1, is "
                   "ignored"),
        "local stop: text after a broken entry");
  check(stops_with("[" + a + ",\n{\"name\":\"b\",\"file\":[},\n" + b + "]",
                   {"a"},
                   "line 2, column 20: the value of \"file\" must be a "
                   "string; the rest of the file, from line 2, column 1, is "
                   "ignored"),
        "local stop: brackets that do not match");
  check(stops_with("[" + a + ",\n:,\n" + b + "]", {"a"},
                   "not valid JSON at line 2, column 1: expected '{' (each "
                   "entry is an object); the rest of the file, from line 2, "
                   "column 1, is ignored"),
        "local stop: text between entries");
  check(stops_with("[" + a + ",]", {"a"},
                   "not valid JSON at line 1, column 74: expected '{' (each "
                   "entry is an object); the rest of the file, from line 1, "
                   "column 74, is ignored"),
        "local stop: trailing comma");
  check(stops_with("[" + a + "]\nx", {"a"},
                   "not valid JSON at line 2, column 1: unexpected text "
                   "after the array; the rest of the file, from line 2, "
                   "column 1, is ignored"),
        "local stop: text after the array");
  check(stops_with("[" + a + "\n", {"a"},
                   "not valid JSON at line 2, column 1: expected ',' or "
                   "']'; the rest of the file, from line 2, column 1, is "
                   "ignored"),
        "local stop: file ends after an entry");
  check(stops_with("[", {},
                   "not valid JSON at line 1, column 2: expected '{' (each "
                   "entry is an object); the rest of the file, from line 1, "
                   "column 2, is ignored"),
        "local stop: file ends after '['");
}

// Of more than kMaxLocalPatches entries the first ones are used, valid or
// not, and the rest is one error saying how many are ignored.
static void test_local_patches_too_many() {
  auto entries = [](size_t n, size_t invalid) {
    std::string s = "[\n";
    for (size_t i = 0; i < n; ++i) {
      if (i > 0) s += ",\n";
      s += i == invalid ? std::string("{\"name\":\"bad\" 1}")
                        : local_entry("e" + std::to_string(i),
                                      "o" + std::to_string(i),
                                      "r" + std::to_string(i));
    }
    return s;
  };
  const size_t none = static_cast<size_t>(-1);

  LocalPatches r = parse_local_patches(entries(kMaxLocalPatches, none) + "]");
  check(r.error.empty() && r.entries.size() == kMaxLocalPatches,
        "local many: 100 entries are all used");

  r = parse_local_patches(entries(kMaxLocalPatches + 1, none) + "]");
  check(r.entries.size() == kMaxLocalPatches &&
            r.entries.back().name == "e99" &&
            r.error == "more than 100 entries; the one after the first 100, "
                       "from line 102, column 1, is ignored",
        "local many: one ignored");

  r = parse_local_patches(entries(kMaxLocalPatches + 3, 101) + "]");
  check(r.entries.size() == kMaxLocalPatches &&
            r.error == "more than 100 entries; the 3 after the first 100, "
                       "from line 102, column 1, are ignored",
        "local many: invalid entries among the ignored are counted");

  r = parse_local_patches(entries(kMaxLocalPatches + 1, 49) + "]");
  check(r.entries.size() == kMaxLocalPatches && r.entries[49].name == "bad" &&
            !r.entries[49].error.empty() && r.entries[50].error.empty() &&
            r.error.find("the one after the first 100") != std::string::npos,
        "local many: an invalid entry counts towards the 100");

  r = parse_local_patches(entries(kMaxLocalPatches + 2, none) +
                          ",\n{\"name\":\"x");
  check(r.entries.size() == kMaxLocalPatches &&
            r.error == "more than 100 entries; at least 2 after the first "
                       "100, from line 102, column 1, are ignored; not valid "
                       "JSON at line 104, column 11: string not terminated; "
                       "the rest of the file, from line 104, column 1, is "
                       "ignored",
        "local many: malformed JSON among the ignored");

  // Ignored entries are read as the others are: a broken one whose end is
  // not certain stops the count rather than skipping the entry after it.
  r = parse_local_patches(
      entries(kMaxLocalPatches + 1, none) +
      ",\n{\"name\":\"x,\"file\":\"y\"},\n" +
      local_entry("q", "if(x===\"a\"){y()}", "z") + ",\n" +
      local_entry("last", "o", "r") + "]");
  check(r.entries.size() == kMaxLocalPatches &&
            r.error == "more than 100 entries; at least one after the first "
                       "100, from line 102, column 1, is ignored; not valid "
                       "JSON at line 103, column 13: expected ',' or '}'; the "
                       "rest of the file, from line 103, column 1, is ignored",
        "local many: missing quote among the ignored");
}

// A missing file holds no entries and no error; one that cannot be read
// holds no entries and an error.  Of an oversized file the entries that
// end within the size limit are used.
static void test_load_local_patches() {
  const std::string path = temp_dir() + "/test_local_patches_load.json";
  std::remove(path.c_str());
  LocalPatches r = load_local_patches(path);
  check(r.entries.empty() && r.error.empty(),
        "local load: missing file has no entries");

  r = load_local_patches(temp_dir());
  check(r.entries.empty() && r.error == "not a regular file",
        "local load: directory");

  write_test_file(path, std::string(kMaxLocalPatchesBytes + 1, ' '));
  r = load_local_patches(path);
  check(r.entries.empty() &&
            r.error == "larger than 1024 KiB; what follows its first 1024 "
                       "KiB is ignored",
        "local load: oversized file");
  write_test_file(path, std::string(kMaxLocalPatchesBytes, ' '));
  r = load_local_patches(path);
  check(r.entries.empty() && r.error.empty(),
        "local load: file at the size limit");

  write_test_file(path, "[" + local_entry("a", "x=1;", "x=2;") + "]");
  r = load_local_patches(path);
  check(all_valid(r, {"a"}) && r.entries[0].replacement == "x=2;",
        "local load: entries read");
  write_test_file(path, "[" + local_entry("a", "x=1;", "x=2;") + ",");
  r = load_local_patches(path);
  check(r.entries.size() == 1 && r.entries[0].error.empty() &&
            r.error.find("not valid JSON") != std::string::npos,
        "local load: parse error keeps the entries before it");

  // Entries of about 20 KiB, one per line: the limit cuts one of them.
  std::string big = "[\n";
  size_t whole = 0;      // entries that end within the limit
  size_t cut_line = 0;   // line of the entry the limit cuts
  for (size_t i = 0; i < 60; ++i) {
    if (i > 0) big += ",\n";
    const size_t start = big.size();
    big += local_entry("e" + std::to_string(i), "o" + std::to_string(i),
                       std::string(20000, 'r'));
    if (big.size() <= kMaxLocalPatchesBytes) {
      ++whole;
    } else if (cut_line == 0 && start < kMaxLocalPatchesBytes) {
      cut_line = i + 2;
    }
  }
  big += "]\n";
  check(cut_line != 0, "local load: the limit falls inside an entry");
  write_test_file(path, big);
  r = load_local_patches(path);
  check(r.entries.size() == whole && whole > 40 &&
            r.entries.back().error.empty() &&
            r.entries.back().replacement == std::string(20000, 'r') &&
            r.error == "larger than 1024 KiB; the rest of the file, from "
                       "line " + std::to_string(cut_line) +
                       ", column 1, is ignored",
        "local load: entries within the limit used");

  // Malformed JSON before the limit is reported as well.
  write_test_file(path, "[" + local_entry("a", "x=1;", "x=2;") +
                            ",\n{\"name\":\"b\" \"c\"} x,\n" +
                            std::string(kMaxLocalPatchesBytes, ' ') + "]");
  r = load_local_patches(path);
  check(all_valid(LocalPatches{r.entries, ""}, {"a"}) &&
            r.error == "not valid JSON at line 2, column 13: expected ',' or "
                       "'}'; the rest of the file, from line 2, column 1, is "
                       "ignored; the file is also larger than 1024 KiB",
        "local load: oversized file with malformed JSON");

  // A missing '"' early on is reported as it is, not as the size limit,
  // although the scan for the broken entry's end runs up to the limit.
  std::string unquoted = "[" + local_entry("a", "x=1;", "x=2;") +
                         ",\n{\"name\":\"b\",\"file\":\"service.js\","
                         "\"original\":\"o,\"replacement\":\"r\"}";
  while (unquoted.size() <= kMaxLocalPatchesBytes) {
    unquoted += ",\n" + local_entry("e", "o", std::string(20000, 'r'));
  }
  write_test_file(path, unquoted + "]");
  r = load_local_patches(path);
  check(all_valid(LocalPatches{r.entries, ""}, {"a"}) &&
            r.error == "not valid JSON at line 2, column 48: expected ',' or "
                       "'}'; the rest of the file, from line 2, column 1, is "
                       "ignored; the file is also larger than 1024 KiB",
        "local load: oversized file with a missing quote");

  // The limit falling anywhere inside a \u escape pair cuts the entry off;
  // it is not reported as a bad escape.
  const std::string head =
      "[{\"name\":\"a\",\"file\":\"service.js\",\"original\":\"";
  const std::string pair = "\\ud83d\\ude00";
  for (size_t k = 0; k <= pair.size(); ++k) {
    write_test_file(path, head +
                              std::string(kMaxLocalPatchesBytes -
                                              head.size() - k, 'x') +
                              pair + "\",\"replacement\":\"r\"}]");
    r = load_local_patches(path);
    check(r.entries.empty() &&
              r.error == "larger than 1024 KiB; the rest of the file, from "
                         "line 1, column 2, is ignored",
          ("local load: limit " + std::to_string(k) +
           " bytes into a \\u escape pair").c_str());
  }

  // More than kMaxLocalPatches entries and over the size limit: the
  // entries counted are those within the limit, so their number is a
  // lower bound.
  std::string many = "[\n";
  size_t within = 0;     // entries that end within the limit
  size_t many_cut = 0;   // line of the entry the limit cuts
  for (size_t i = 0; i < 200; ++i) {
    if (i > 0) many += ",\n";
    const size_t start = many.size();
    many += local_entry("e" + std::to_string(i), "o" + std::to_string(i),
                        std::string(8000, 'r'));
    if (many.size() <= kMaxLocalPatchesBytes) {
      ++within;
    } else if (many_cut == 0 && start < kMaxLocalPatchesBytes) {
      many_cut = i + 2;
    }
  }
  write_test_file(path, many + "]");
  r = load_local_patches(path);
  check(many_cut != 0 && within > kMaxLocalPatches + 1 &&
            r.entries.size() == kMaxLocalPatches &&
            r.error == "more than 100 entries; at least " +
                           std::to_string(within - kMaxLocalPatches) +
                           " after the first 100, from line 102, column 1, "
                           "are ignored; larger than 1024 KiB; the rest of "
                           "the file, from line " +
                           std::to_string(many_cut) + ", column 1, is ignored",
        "local load: too many entries and oversized");

  // An array closed within the limit is used in full.
  write_test_file(path, "[" + local_entry("a", "x=1;", "x=2;") + "]" +
                            std::string(kMaxLocalPatchesBytes, '\n'));
  r = load_local_patches(path);
  check(all_valid(LocalPatches{r.entries, ""}, {"a"}) &&
            r.error == "larger than 1024 KiB; what follows its first 1024 "
                       "KiB is ignored",
        "local load: oversized after the array");
  std::remove(path.c_str());
}

// /api/status's local_patches object.
static void test_local_patches_json() {
  PatchReport rep;
  rep.local_patches_path = "/data/onvif-recorder/local-patches.json";
  rep.local_patches = {
      {"say \"hi\"\n", PatchOutcome::kApplied},
      {"b", PatchOutcome::kNotApplicable},
      {"entry 3", PatchOutcome::kInvalid,
       "line 4, column 2: unknown key \"a\\\\b\"\t<x>"}};
  check(local_patches_json(rep) ==
            "{\"path\":\"/data/onvif-recorder/local-patches.json\","
            "\"error\":\"\",\"entries\":[{\"name\":\"say \\\"hi\\\"\\u000a\","
            "\"outcome\":\"applied\",\"error\":\"\"},{\"name\":\"b\","
            "\"outcome\":\"not_applicable\",\"error\":\"\"},{\"name\":"
            "\"entry 3\",\"outcome\":\"invalid\",\"error\":\"line 4, column "
            "2: unknown key \\\"a\\\\\\\\b\\\"\\u0009<x>\"}]}",
        "local json: entries");
  // The output reads back as JSON: the error string decodes to the
  // original text.
  const std::string json = local_patches_json(rep);
  std::string decoded;
  JsonCursor c{json};
  c.pos = json.find("\"line 4");
  check(c.pos != std::string::npos &&
            parse_json_string(&c, &decoded).ok() &&
            decoded == rep.local_patches[2].error,
        "local json: error string round-trips");

  PatchReport broken;
  broken.local_patches_path = "p";
  broken.local_patches_error = "not valid JSON at line 1, column 1: x\\y";
  check(local_patches_json(broken) ==
            "{\"path\":\"p\",\"error\":\"not valid JSON at line 1, column 1: "
            "x\\\\y\",\"entries\":[]}",
        "local json: error");
}

// Each local patch not applied and the file's error are logged, a line
// again only when the previous pass did not log it.
static void test_local_patch_log_lines() {
  PatchReport rep;
  rep.local_patches_path = "/p.json";
  rep.local_patches_error = "more than 100 entries; ...";
  rep.local_patches = {{"ok", PatchOutcome::kApplied},
                       {"same", PatchOutcome::kAlready},
                       {"gone", PatchOutcome::kNotApplicable},
                       {"twice", PatchOutcome::kAmbiguous},
                       {"q\"\n", PatchOutcome::kInvalid, "line 1: bad"},
                       {"later", PatchOutcome::kNotRun}};
  const std::vector<std::string> lines = local_patch_errors(rep);
  check(lines.size() == 4 &&
            lines[0] == "local patches /p.json: more than 100 entries; ..." &&
            lines[1].rfind("local patch \"gone\" not applied: its original "
                           "text is not in service.js", 0) == 0 &&
            lines[2] == "local patch \"twice\" not applied: its original text "
                        "occurs more than once in service.js" &&
            lines[3] == "local patch \"q\\\"\\u000a\" not applied: line 1: bad",
        "local log: one line per error, names quoted");

  PatchReport unreadable;
  unreadable.local_patches_path = "/p.json";
  unreadable.local_patches_error = "not a regular file";
  check(local_patch_errors(unreadable) ==
            std::vector<std::string>{"local patches /p.json: not a regular "
                                     "file -- none of its entries is applied"},
        "local log: unreadable file");
  check(local_patch_errors(PatchReport{}).empty(), "local log: nothing");

  std::set<std::string> last;
  check(new_lines({"a", "b"}, &last) == std::vector<std::string>{"a", "b"},
        "local log: first pass logs all");
  check(new_lines({"a", "b"}, &last).empty(),
        "local log: unchanged pass logs nothing");
  check(new_lines({"a", "c"}, &last) == std::vector<std::string>{"c"},
        "local log: changed line logged");
  check(new_lines({"c"}, &last).empty() &&
            new_lines({"a", "c"}, &last) == std::vector<std::string>{"a"},
        "local log: line back after a pass without it logged again");
}

// The text of the ERROR lines logged while it is registered.
class ErrorLines : public absl::LogSink {
 public:
  void Send(const absl::LogEntry& entry) override {
    if (entry.log_severity() != absl::LogSeverity::kError) return;
    std::lock_guard<std::mutex> lock(mu_);
    lines_.emplace_back(entry.text_message());
  }
  // The lines logged since the last call.
  std::vector<std::string> take() {
    std::lock_guard<std::mutex> lock(mu_);
    return std::exchange(lines_, {});
  }

 private:
  std::mutex mu_;
  std::vector<std::string> lines_;
};

// Repeated passes over unchanged local patches log their errors once.
static void test_log_local_patches_once() {
  ErrorLines sink;
  absl::AddLogSink(&sink);
  PatchReport rep;
  rep.local_patches_path = "/p.json";
  rep.local_patches = {{"ok", PatchOutcome::kApplied},
                       {"gone", PatchOutcome::kNotApplicable},
                       {"bad", PatchOutcome::kInvalid, "line 2, column 1: x"}};
  log_local_patches(rep);
  const std::vector<std::string> first = sink.take();
  log_local_patches(rep);
  const std::vector<std::string> second = sink.take();
  rep.local_patches[2].error = "line 2, column 1: y";
  log_local_patches(rep);
  const std::vector<std::string> third = sink.take();
  log_local_patches(PatchReport{});
  absl::RemoveLogSink(&sink);
  check(first.size() == 2 && second.empty(),
        "local log once: logged on the first pass only");
  check(third == std::vector<std::string>{"[ui_patch] local patch \"bad\" not "
                                          "applied: line 2, column 1: y"},
        "local log once: changed error logged again");
}

// Local patches follow ours in patch_service_js(): each is reported, the
// valid ones apply while invalid ones are skipped, ours apply whatever the
// file holds, and with a verified .bak removing an entry restores the
// original text.
static void test_local_patches_service_js() {
  const std::string path = temp_dir() + "/test_local_service.js";
  const std::string bak = path + ".bak";
  const std::string lp = temp_dir() + "/test_local_patches.json";
  std::remove(path.c_str());
  std::remove(bak.c_str());
  std::remove(lp.c_str());
  const std::string original =
      soap_fixture() + "var A=1;var B=2;var C=3;C=3;";
  write_test_file(path, original);
  auto replaced = [](std::string s, const std::string& from,
                     const std::string& to) {
    s.replace(s.find(from), from.size(), to);
    return s;
  };
  const std::string wsa_only = with_patch(original, kBackendPatchWsa);
  const std::string wsa_a = replaced(wsa_only, "var A=1;", "var A=10;");
  const std::string wsa_ab = replaced(wsa_a, "var B=2;", "var B=20;");
  const PatchOptions opts{true, false, lp};

  // Applied, after ours, in one write.
  write_test_file(lp, "[" + local_entry("a", "var A=1;", "var A=10;") + "," +
                          local_entry("b", "var B=2;", "var B=20;") + "]");
  PatchReport rep;
  check(patch_service_js(path, opts, {}, &rep) == 3,
        "local: ours and both entries written");
  check(read_test_file(path) == wsa_ab, "local: file patched");
  check(read_test_file(bak) == original, "local: original backed up");
  check(rep.onvif_addressing == PatchOutcome::kApplied &&
            rep.event_proxy == PatchOutcome::kAbsent,
        "local: our outcomes");
  check(rep.local_patches_path == lp && rep.local_patches_error.empty() &&
            rep.local_patches.size() == 2 && rep.local_patches[0].name == "a" &&
            rep.local_patches[0].outcome == PatchOutcome::kApplied &&
            rep.local_patches[1].name == "b" &&
            rep.local_patches[1].outcome == PatchOutcome::kApplied,
        "local: entries applied");

  // Already present, with and without a verified .bak.
  check(patch_service_js(path, opts, {}, &rep) == 0,
        "local: second pass writes nothing");
  check(rep.local_patches.size() == 2 &&
            rep.local_patches[0].outcome == PatchOutcome::kAlready &&
            rep.local_patches[1].outcome == PatchOutcome::kAlready,
        "local: entries already present");
  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);
  patch_service_js(path, opts, md5sums, &rep);
  check(read_test_file(path) == wsa_ab &&
            rep.local_patches[0].outcome == PatchOutcome::kAlready &&
            rep.local_patches[1].outcome == PatchOutcome::kAlready,
        "local: rebuilt from .bak, entries already present");

  // Removing "b" restores its original text; an entry whose text is gone
  // is not applicable, one whose text occurs twice is ambiguous.
  write_test_file(lp, "[" + local_entry("a", "var A=1;", "var A=10;") + "," +
                          local_entry("gone", "var Z=9;", "var Z=90;") + "," +
                          local_entry("twice", "C=3;", "C=30;") + "]");
  patch_service_js(path, opts, md5sums, &rep);
  check(read_test_file(path) == wsa_a, "local: removed entry undone");
  check(read_test_file(bak) == original, "local: .bak kept");
  check(rep.local_patches.size() == 3 &&
            rep.local_patches[0].outcome == PatchOutcome::kAlready &&
            rep.local_patches[1].outcome == PatchOutcome::kNotApplicable &&
            rep.local_patches[2].outcome == PatchOutcome::kAmbiguous &&
            rep.onvif_addressing == PatchOutcome::kAlready,
        "local: already, not applicable, ambiguous");

  // Invalid entries are skipped with their errors; the valid ones around
  // them, and ours, still apply.
  write_test_file(
      lp, "[" + local_entry("a", "var A=1;", "var A=10;") + ",\n" +
              local_entry("ui", "x", "y", "swai.js") + ",\n" +
              "{\"name\":\"raw\",\"file\":\"service.js\",\"original\":\"var\n"
              "C=3;\",\"replacement\":\"x\"},\n" +
              local_entry("b", "var B=2;", "var B=20;") + ",\n" +
              local_entry("gone", "var Z=9;", "var Z=90;") + "]");
  write_test_file(path, original);
  check(patch_service_js(path, opts, md5sums, &rep) == 3,
        "local mixed: ours and the two valid entries written");
  check(read_test_file(path) == wsa_ab, "local mixed: valid entries applied");
  check(rep.local_patches_error.empty() && rep.local_patches.size() == 5 &&
            rep.local_patches[0].outcome == PatchOutcome::kApplied &&
            rep.local_patches[0].error.empty() &&
            rep.local_patches[1].name == "ui" &&
            rep.local_patches[1].outcome == PatchOutcome::kInvalid &&
            rep.local_patches[1].error ==
                "line 2, column 1: \"file\" is \"swai.js\"; only "
                "\"service.js\" can be patched" &&
            rep.local_patches[2].name == "raw" &&
            rep.local_patches[2].outcome == PatchOutcome::kInvalid &&
            rep.local_patches[2].error.find("control character") !=
                std::string::npos &&
            rep.local_patches[3].name == "b" &&
            rep.local_patches[3].outcome == PatchOutcome::kApplied &&
            rep.local_patches[4].outcome == PatchOutcome::kNotApplicable &&
            rep.local_patches[4].error.empty() &&
            rep.onvif_addressing == PatchOutcome::kApplied,
        "local mixed: outcomes and errors");

  // Where the file cannot be read further, the entries before that still
  // apply and the rest is the file's error.
  write_test_file(lp, "[" + local_entry("a", "var A=1;", "var A=10;") +
                          ",\n{\"name\":\"b\",\"original\":\"var B=2;");
  patch_service_js(path, opts, md5sums, &rep);
  check(read_test_file(path) == wsa_a, "local cut: earlier entry applied");
  check(rep.local_patches.size() == 1 &&
            rep.local_patches[0].outcome == PatchOutcome::kAlready &&
            rep.local_patches_error ==
                "not valid JSON at line 2, column 33: string not terminated; "
                "the rest of the file, from line 2, column 1, is ignored",
        "local cut: error reported");

  // A file that is not an array applies none; ours still apply.
  write_test_file(lp, local_entry("a", "var A=1;", "var A=10;"));
  patch_service_js(path, opts, md5sums, &rep);
  check(read_test_file(path) == wsa_only, "local not array: none applied");
  check(rep.local_patches.empty() &&
            rep.local_patches_error.find("expected '['") !=
                std::string::npos &&
            rep.onvif_addressing == PatchOutcome::kAlready,
        "local not array: error reported, ours applied");

  // Without service.js the valid entries are not run; invalid ones are
  // still reported.
  write_test_file(lp, "[" + local_entry("a", "var A=1;", "var A=10;") + "," +
                          local_entry("ui", "x", "y", "swai.js") + "]");
  check(patch_service_js("/nonexistent/service.js", opts, md5sums, &rep) ==
                -1 &&
            rep.local_patches.size() == 2 &&
            rep.local_patches[0].outcome == PatchOutcome::kNotRun &&
            rep.local_patches[1].outcome == PatchOutcome::kInvalid,
        "local: missing service.js");

  // No file, then no path: no entries and no error.
  std::remove(lp.c_str());
  write_test_file(path, wsa_a);
  patch_service_js(path, opts, md5sums, &rep);
  check(read_test_file(path) == wsa_only && rep.local_patches.empty() &&
            rep.local_patches_error.empty() && rep.local_patches_path == lp,
        "local: missing file holds no entries");
  patch_service_js(path, PatchOptions{true, false, ""}, md5sums, &rep);
  check(rep.local_patches_path.empty() && rep.local_patches.empty() &&
            rep.local_patches_error.empty(),
        "local: no path");

  // With everything off the original comes back byte for byte.
  write_test_file(lp, "[" + local_entry("a", "var A=1;", "var A=10;") + "]");
  patch_service_js(path, opts, md5sums, &rep);
  check(read_test_file(path) == wsa_a, "local: entry applied again");
  std::remove(lp.c_str());
  patch_service_js(path, PatchOptions{false, false, lp}, md5sums, &rep);
  check(read_test_file(path) == original, "local: original restored");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// An entry whose replacement forms its original again with the code around
// it ("%~" -> "~~%%" in "%%~" leaves "%~" at the front) is not applied:
// each pass over a file without a verified .bak would otherwise apply it
// once more and grow the file without end.
static void test_local_patch_forms_original_again() {
  const std::string path = temp_dir() + "/test_local_again_service.js";
  const std::string bak = path + ".bak";
  const std::string lp = temp_dir() + "/test_local_again_patches.json";
  std::remove(path.c_str());
  std::remove(bak.c_str());
  const std::string original = soap_fixture() + "var s=\"%%~\";";
  write_test_file(path, original);
  write_test_file(lp, "[" + local_entry("grows", "%~", "~~%%") + "," +
                          local_entry("ok", "var s=", "let s=") + "]");
  std::string expected = with_patch(original, kBackendPatchWsa);
  expected.replace(expected.find("var s="), 6, "let s=");
  const PatchOptions opts{true, false, lp};

  for (int pass = 0; pass < 3; ++pass) {
    PatchReport rep;
    patch_service_js(path, opts, {}, &rep);
    check(read_test_file(path) == expected,
          "local again: entry not applied on a pass over the live file");
    check(rep.local_patches.size() == 2 &&
              rep.local_patches[0].outcome == PatchOutcome::kNotApplicable &&
              rep.local_patches[1].outcome == (pass == 0
                                                   ? PatchOutcome::kApplied
                                                   : PatchOutcome::kAlready),
          "local again: reported not applicable, the other entry applied");
  }

  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);
  check(read_test_file(bak) == original, "local again: original backed up");
  PatchReport rep;
  patch_service_js(path, opts, md5sums, &rep);
  check(read_test_file(path) == expected && rep.local_patches.size() == 2 &&
            rep.local_patches[0].outcome == PatchOutcome::kNotApplicable,
        "local again: not applied when rebuilt from .bak either");

  std::remove(path.c_str());
  std::remove(bak.c_str());
  std::remove(lp.c_str());
}

// Our patches are applied and reported the same with local patches
// present: ours go first, so an entry cannot take an anchor of ours or
// make a safety scan of ours refuse, and turning ours off keeps the
// entries.
static void test_local_patches_after_ours() {
  const std::string path = temp_dir() + "/test_local_order_service.js";
  const std::string bak = path + ".bak";
  const std::string lp = temp_dir() + "/test_local_order_patches.json";
  std::remove(path.c_str());
  std::remove(bak.c_str());
  const std::string original = soap_fixture();
  write_test_file(path, original);
  write_test_file(bak, original);
  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);

  // "this" in soapRequestRaw would make the route patch refuse, and the
  // WS-Addressing anchor is ours.
  const std::string uses_this =
      local_entry("this", "return l.set(t,!0),i", "return l.set(t,!0),this&&i");
  const std::string takes_anchor =
      local_entry("anchor", kBackendPatchWsa.original, "t.connSoapRequest=0");
  write_test_file(lp, "[" + uses_this + "," + takes_anchor + "]");

  const std::string ours = with_patch(with_patch(original, kBackendPatchWsa),
                                      kBackendPatchRoute);
  std::string expected = ours;
  expected.replace(expected.find("return l.set(t,!0),i"),
                   std::strlen("return l.set(t,!0),i"),
                   "return l.set(t,!0),this&&i");
  for (int pass = 0; pass < 2; ++pass) {
    PatchReport rep;
    patch_service_js(path, PatchOptions{true, true, lp}, md5sums, &rep);
    const PatchOutcome want =
        pass == 0 ? PatchOutcome::kApplied : PatchOutcome::kAlready;
    check(rep.onvif_addressing == want && rep.event_proxy == want,
          "local order: ours applied as without local patches");
    check(rep.local_patches.size() == 2 &&
              rep.local_patches[0].outcome == want &&
              rep.local_patches[1].outcome == PatchOutcome::kNotApplicable,
          "local order: entry applied, our anchor not taken");
    check(read_test_file(path) == expected, "local order: file");
  }

  // Without a verified .bak the next pass starts from the live file.
  PatchReport rep;
  patch_service_js(path, PatchOptions{true, true, lp}, {}, &rep);
  check(rep.onvif_addressing == PatchOutcome::kAlready &&
            rep.event_proxy == PatchOutcome::kAlready &&
            rep.local_patches[0].outcome == PatchOutcome::kAlready,
        "local order: live file pass");

  // Ours off: removed, the entry stays, and the entry on our former anchor
  // now applies.
  patch_service_js(path, PatchOptions{false, false, lp}, md5sums, &rep);
  check(rep.onvif_addressing == PatchOutcome::kRemoved &&
            rep.event_proxy == PatchOutcome::kRemoved &&
            rep.local_patches[0].outcome == PatchOutcome::kAlready &&
            rep.local_patches[1].outcome == PatchOutcome::kApplied,
        "local order: ours removed, entries kept or applied");
  std::string entries_only = original;
  entries_only.replace(entries_only.find("return l.set(t,!0),i"),
                       std::strlen("return l.set(t,!0),i"),
                       "return l.set(t,!0),this&&i");
  entries_only = with_patch(
      entries_only, Patch{kBackendPatchWsa.original, "t.connSoapRequest=0"});
  check(read_test_file(path) == entries_only, "local order: entries only");

  std::remove(path.c_str());
  std::remove(bak.c_str());
  std::remove(lp.c_str());
}

// scan_js_block() finds the closing brace past braces inside strings,
// template literals, comments and regular expressions.
static void test_scan_js_block() {
  auto end_of = [](const std::string& s) { return scan_js_block(s, 0).end; };
  auto closes_at_end = [&](const std::string& s) {
    return end_of(s + "tail") == s.size() - 1;
  };
  check(closes_at_end("{a={b:{c:1}};}"), "scan: nested braces");
  check(closes_at_end("{a=\"}\\\"}\";b='}\\'}';}"), "scan: strings");
  check(closes_at_end("{a=`}${ {x:\"}\"}.x }}`;}"),
        "scan: template with substitution");
  check(closes_at_end("{a=`${`${\"}\"}`}`+`$`+`\\${`;}"),
        "scan: nested and escaped templates");
  check(closes_at_end("{/*}*/a=1//}\n;}"), "scan: comments");
  check(closes_at_end("{e=/}[/}]\\/}/g.test(x);}"),
        "scan: regex literal containing }");
  check(closes_at_end("{if(x)return/}/.test(y);}"), "scan: regex after return");
  check(closes_at_end("{a=(b)/2/c;d=e[0]/f/g;}"), "scan: division");
  check(closes_at_end("{a=x.return/2/y;}"), "scan: property named return");
  check(closes_at_end("{a=/x/g/2;}"), "scan: division after a regex");
  check(closes_at_end("{a=i++/2;b='/';c=j--/2;d='/';}"),
        "scan: division after ++ and --");
  check(closes_at_end("{a=b+ +/}/.source.length;c=d- -/}/.lastIndex;}"),
        "scan: regex after spaced unary operators");
  check(end_of("{a=\"}") == std::string::npos, "scan: unterminated string");
  check(end_of("{a=`${b}") == std::string::npos,
        "scan: unterminated template");
  check(end_of("{a=1") == std::string::npos, "scan: unterminated block");
  check(end_of("x{}") == std::string::npos, "scan: must start at a brace");
  std::string big = "{";
  big.append(kMaxScanBytes, ';');
  big += "}";
  check(end_of(big) == std::string::npos, "scan: gives up past the limit");

  auto uses = [](const std::string& s) {
    return scan_js_block(s, 0).uses_this_or_arguments;
  };
  check(uses("{return this}"), "scan: this");
  check(uses("{f(arguments[0])}"), "scan: arguments");
  check(uses("{f(...arguments)}"), "scan: spread arguments");
  check(uses("{a=`${this.x}`}"), "scan: this in a substitution");
  check(uses("{a=i--/2;b=this;c=x/3}"), "scan: this between divisions");
  check(!uses("{a.this;b?.arguments;thisx;xthis;$this;arguments_}"),
        "scan: property and longer names");
  check(!uses("{a=\"this\";b='arguments';c=`this`;/*this*/d=/this/}"),
        "scan: strings, comments and regexes");
  check(!uses("{}this"), "scan: after the block");
}

// Protect needs a restart only when service.js was written after the
// running process started.
static void test_file_newer_than_process() {
  const std::string path = temp_dir() + "/test_newer_than_process.js";
  // Let this process age past the clock-tick resolution of its start time.
  usleep(100 * 1000);
  write_test_file(path, "x");
  check(file_newer_than_process(path, getpid()),
        "newer: file written after the process started");

  const struct timespec y2k[2] = {{946684800, 0}, {946684800, 0}};
  check(utimensat(AT_FDCWD, path.c_str(), y2k, 0) == 0, "newer: set mtime");
  check(!file_newer_than_process(path, getpid()),
        "newer: file older than the process");
  check(!file_newer_than_process(path + ".missing", getpid()),
        "newer: missing file");
  check(!file_newer_than_process(path, 0), "newer: no such process");
  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 6: backup logic
//
// An unpatched live file is the package's file and becomes the backup,
// whatever dpkg's md5sums say (they lag behind during an upgrade).  A
// patched live file never replaces the backup.
// ---------------------------------------------------------------
static void test_dpkg_backup_logic() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_dpkg.js";
  std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());

  std::string content = "prefix_";
  content += kUiPatch1a.original;
  content += "_suffix";
  write_test_file(path, content);
  write_test_file(bak, "old backup content");

  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = "0000000000000000aaaaaaaaaaaaaaaa";  // stale
  int n = apply_patches(path, kUiPatches, kUiPatchCount, md5sums);
  check(n == 1, "dpkg: patch applied");
  check(read_test_file(bak) == content, "dpkg: unpatched file backed up");

  // A later pass over the patched file (no md5sums, as on a dev machine)
  // with an extra patch keeps the backup of the unpatched file.
  const std::string patched = read_test_file(path);
  write_test_file(path, patched + ";" + kUiPatch3.original);
  std::unordered_map<std::string, std::string> empty_md5;
  check(apply_patches(path, kUiPatches, kUiPatchCount, empty_md5) == 1,
        "dpkg: extra patch applied");
  check(read_test_file(bak) == content, "dpkg: patched file not backed up");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// A live file that differs from the package original without carrying our
// patches (damaged, or edited by hand) is rebuilt from the verified .bak,
// and that .bak is never replaced by it.
static void test_unpatched_damage_rebuilt_from_bak() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_damaged_service.js";
  std::string bak = path + ".bak";
  const std::string pristine =
      "v1;scope_all_ui_cameras:e=>e.isAdopted&&!e.isThirdPartyCamera,scope_x";
  const std::string damaged = pristine.substr(0, 40);
  write_test_file(bak, pristine);
  write_test_file(path, damaged);
  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);

  check(apply_patches(path, kBackendPatches, kBackendPatchCount, md5sums) ==
            1,
        "damaged: rebuilt and patched");
  const std::string out = read_test_file(path);
  check(out.rfind("v1;", 0) == 0 && out.size() == pristine.size() &&
            out.find(kBackendPatch1.replacement) != std::string::npos,
        "damaged: original restored and patched");
  check(read_test_file(bak) == pristine, "damaged: verified .bak kept");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// dpkg marks a package half-installed while it unpacks it.
static void test_half_installed_status() {
  check(status_is_half_installed("install ok half-installed"),
        "status: unpacking");
  check(status_is_half_installed("install reinstreq half-installed"),
        "status: failed unpack");
  check(!status_is_half_installed("install ok installed"), "status: done");
  check(!status_is_half_installed("install ok unpacked"), "status: unpacked");
  check(!status_is_half_installed(""), "status: unknown package");
}

// Files are replaced whole, keeping their mode.
static void test_replace_file() {
  const std::string path = temp_dir() + "/test_replace.js";
  write_test_file(path, "old");
  chmod(path.c_str(), 0755);
  check(replace_file(path, "new content"), "replace: succeeds");
  check(read_test_file(path) == "new content", "replace: content written");
  struct stat st;
  check(stat(path.c_str(), &st) == 0 && (st.st_mode & 07777) == 0755,
        "replace: mode kept");
  check(read_test_file(path + ".onvif-recorder.tmp").empty(),
        "replace: no temporary file left");
  std::remove(path.c_str());

  check(replace_file(path, "fresh"), "replace: creates a missing file");
  check(read_test_file(path) == "fresh", "replace: new file written");
  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 7: Partial patch (only some patterns present)
// ---------------------------------------------------------------
static void test_partial_patch() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_partial.js";
  std::remove(path.c_str());

  // Only include patch 1, not patches 2 or 3.
  std::string content = "header_";
  content += kUiPatch1a.original;
  content += "_footer";

  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 1, "partial: only 1 patch applied");

  std::string patched = read_test_file(path);
  check(patched.find(kUiPatch1a.replacement) != std::string::npos,
        "partial: patch1 applied");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// ---------------------------------------------------------------
// Test: revert_alarm_picker_in restores files byte-for-byte from .bak.
// Simulates the production sequence: write original -> apply (creates
// .bak + modifies live) -> revert -> live must equal original.
// ---------------------------------------------------------------
static void test_revert_byte_for_byte() {
  const std::string ui_dir = temp_dir() + "/ui_revert_test";
  std::system(("rm -rf " + ui_dir + " && mkdir -p " + ui_dir).c_str());

  // Build a minimal swai.js containing the v1.4.3+ patch1a pattern.
  std::string original;
  original += "/* head */\n";
  original += kUiPatch1a.original;
  original += "\n/* tail */\n";
  // Pad to 1 KiB to make any drift byte-detectable.
  original.resize(1024, ' ');

  const std::string live_path = ui_dir + "/swai.js";
  check(write_test_file(live_path, original), "write swai.js original");

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(live_path, kUiPatches, kUiPatchCount, empty_md5);
  check(n >= 1, "apply produced at least 1 replacement");

  // Sanity: live file is now patched, .bak holds the original.
  std::string patched = read_test_file(live_path);
  check(patched != original, "live file modified by apply");
  check(read_test_file(live_path + ".bak") == original,
        ".bak holds the original byte-for-byte");

  // Revert from the test-only directory, no service.js path.
  auto s = revert_alarm_picker_in(ui_dir, /*service_path=*/"");
  const std::string revert_msg =
      std::string("revert ok: ") + std::string(s.message());
  check(s.ok(), revert_msg.c_str());

  std::string restored = read_test_file(live_path);
  check(restored == original, "live file restored byte-for-byte");

  std::system(("rm -rf " + ui_dir).c_str());
}

// ---------------------------------------------------------------
// Test: nginx block injection places the block inside the server
// block, not inside a previously-injected location block.
// Regression for the v1.4.3/v1.4.4 bug where the admin-proxy block
// got nested inside the log-viewer location, causing
//   "location /onvif/admin/ is outside location /onvif/events/log"
// on nginx -t.
// ---------------------------------------------------------------
static void test_nginx_inject_not_nested() {
  static const char kBaseConf[] =
    "server {\n"
    "    listen 443 ssl http2 default_server;\n"
    "    server_name _;\n"
    "    include /usr/share/unifi-core/http/shared-post-setup-server.conf;\n"
    "}\n";
  static const char kLogBegin[] = "    # --- log begin ---\n";
  static const char kLogEnd[]   = "    # --- log end ---\n";
  static const char kAdminBegin[] = "    # --- admin begin ---\n";
  static const char kAdminEnd[]   = "    # --- admin end ---\n";

  const std::string log_block =
    std::string(kLogBegin) +
    "    location /onvif/events/log {\n"
    "        proxy_pass http://127.0.0.1:7890/;\n"
    "    }\n" +
    kLogEnd;
  const std::string admin_block =
    std::string(kAdminBegin) +
    "    location /onvif/admin/ {\n"
    "        proxy_pass http://127.0.0.1:7891/;\n"
    "    }\n" +
    kAdminEnd;

  // 1. Inject log block into a clean config.
  auto c1 = protect_ui::inject_nginx_block_into(kBaseConf, log_block,
                                                kLogBegin, kLogEnd);
  check(c1.ok(), "1st inject ok");
  check(c1->find("/onvif/events/log") != std::string::npos,
        "1st inject placed log location");

  // 2. Inject admin block into the already-patched config.  Previously,
  //    find('}', s443) found the log location's '}' first, nesting admin
  //    inside log.  The fixed finder should locate the server's '}'.
  auto c2 = protect_ui::inject_nginx_block_into(*c1, admin_block,
                                                kAdminBegin, kAdminEnd);
  check(c2.ok(), "2nd inject ok");

  const std::string& out = *c2;
  size_t log_loc   = out.find("location /onvif/events/log {");
  size_t log_close = out.find("    }\n", log_loc);
  size_t admin_loc = out.find("location /onvif/admin/ {");
  check(log_loc   != std::string::npos, "log location present");
  check(log_close != std::string::npos, "log location closed");
  check(admin_loc != std::string::npos, "admin location present");
  check(admin_loc > log_close,
        "admin location must be OUTSIDE (after) the log location's close");

  // Overall brace balance must be consistent with the pre-inject
  // brace balance (kBaseConf has depth 0 at the end).
  int depth = 0;
  for (char c : out) {
    if (c == '{') ++depth;
    else if (c == '}') --depth;
  }
  check(depth == 0, "config brace-balanced after inject");
}

// ---------------------------------------------------------------
// Test: injection into a config without `listen 443` returns an error.
// ---------------------------------------------------------------
static void test_nginx_inject_no_server() {
  static const char kNoServer[] = "# no server here\n";
  auto r = protect_ui::inject_nginx_block_into(kNoServer, "block",
                                               "# begin\n", "# end\n");
  check(!r.ok(), "no listen 443 => error");
}

int main() {
  test_patch_lengths();
  test_apply_patches();
  test_already_patched();
  test_missing_file();
  test_backend_patch();
  test_ambiguous_anchor_skipped();
  test_patch2_targets_automation_filter();
  test_rebuild_from_pristine_bak();
  test_wsa_patch();
  test_route_patch_text();
  test_route_patch_scan();
  test_route_patch_upgrade();
  test_route_patch_apply();
  test_route_patch_not_applicable();
  test_route_patch_ambiguous();
  test_patch_outcomes();
  test_service_js_report();
  test_parse_local_patches();
  test_local_patch_entry_errors();
  test_local_patches_stop();
  test_local_patches_too_many();
  test_load_local_patches();
  test_local_patches_json();
  test_local_patch_log_lines();
  test_log_local_patches_once();
  test_local_patches_service_js();
  test_local_patch_forms_original_again();
  test_local_patches_after_ours();
  test_scan_js_block();
  test_file_newer_than_process();
  test_dpkg_backup_logic();
  test_unpatched_damage_rebuilt_from_bak();
  test_half_installed_status();
  test_replace_file();
  test_partial_patch();
  test_revert_byte_for_byte();
  test_nginx_inject_not_nested();
  test_nginx_inject_no_server();

  std::cout << "test_protect_ui_patch: "
            << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
