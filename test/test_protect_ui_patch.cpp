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
#include <string>
#include <unordered_map>
#include <vector>

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
                "ambiguous",
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

// Whether parsing @p text fails with a message containing @p part.
static bool parse_fails_with(const std::string& text, const std::string& part) {
  const auto r = parse_local_patches(text);
  if (r.ok()) return false;
  const std::string m(r.status().message());
  if (m.find(part) != std::string::npos) return true;
  std::cerr << "  message was: " << m << '\n';
  return false;
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
  const auto r = parse_local_patches(text);
  check(r.ok() && r->size() == 2, "local parse: two entries");
  if (r.ok() && r->size() == 2) {
    const LocalPatch& a = (*r)[0];
    const LocalPatch& b = (*r)[1];
    check(a.name == "a" && a.file == "service.js" && a.original == "x=1;" &&
              a.replacement == "x=2;",
          "local parse: first entry");
    check(b.name == "entry 2", "local parse: default name");
    check(b.original == "y\xC3\xA9\xF0\x9F\x98\x80\n\"\\/",
          "local parse: escapes and surrogate pairs decoded");
  }
  for (const char* empty : {"", " \n\t", "\xEF\xBB\xBF", "[]", " [ ]\n"}) {
    const auto e = parse_local_patches(empty);
    check(e.ok() && e->empty(), "local parse: no entries");
  }

  const std::string ok_entry = local_entry("a", "x=1;", "x=2;");
  check(parse_fails_with("{\"name\":\"a\"}", "expected '['"),
        "local parse: not an array");
  check(parse_fails_with("[" + ok_entry + " " + ok_entry + "]",
                         "line 1, column 74: expected ',' or ']'"),
        "local parse: missing comma, with position");
  check(parse_fails_with("[\n" + ok_entry + ",\n\n  {\"name\" \"b\"}]",
                         "line 4, column 11: expected ':'"),
        "local parse: position on a later line");
  check(parse_fails_with("[\"x\"]", "expected '{'"),
        "local parse: entry not an object");
  check(parse_fails_with("[" + ok_entry + ",]", "expected '{'"),
        "local parse: trailing comma");
  check(parse_fails_with("[{\"file\":\"service.js\",\"original\":1,"
                         "\"replacement\":\"b\"}]",
                         "the value of \"original\" must be a string"),
        "local parse: non-string value");
  check(parse_fails_with("[{\"enabled\":\"false\"}]",
                         "unknown key \"enabled\""),
        "local parse: unknown key");
  check(parse_fails_with("[{\"name\":\"a\",\"name\":\"b\"}]",
                         "\"name\" given twice"),
        "local parse: duplicate key");
  check(parse_fails_with("[{\"original\":\"a\",\"replacement\":\"b\"}]",
                         "entry 1 (line 1, column 2): \"file\" is missing"),
        "local parse: file missing");
  check(parse_fails_with("[" + local_entry("ui", "a", "b", "swai.js") + "]",
                         "entry 1 \"ui\" (line 1, column 2): \"file\" is "
                         "\"swai.js\"; only \"service.js\" can be patched"),
        "local parse: wrong file");
  check(parse_fails_with("[{\"file\":\"service.js\",\"replacement\":\"b\"}]",
                         "\"original\" is missing or empty"),
        "local parse: original missing");
  check(parse_fails_with("[" + local_entry("a", "", "b") + "]",
                         "\"original\" is missing or empty"),
        "local parse: original empty");
  check(parse_fails_with("[" + local_entry("a", "x", "") + "]",
                         "\"replacement\" is missing or empty"),
        "local parse: replacement empty");
  check(parse_fails_with("[" + local_entry("a", "x=1", "x=1;y=2") + "]",
                         "\"replacement\" contains \"original\""),
        "local parse: replacement contains original");
  check(parse_fails_with("[{\"name\":\"a\",\"file\":\"service.js\","
                         "\"original\":\"x\\u0000\",\"replacement\":\"y\"}]",
                         "\\u0000"),
        "local parse: NUL character");
  check(parse_fails_with("[{\"name\":\"a", "string not terminated"),
        "local parse: unterminated string");
  check(parse_fails_with("[{\"name\":\"a\nb\"}]", "control character"),
        "local parse: raw line break in a string");
  check(parse_fails_with("[{\"name\":\"\\q\"}]", "unknown escape"),
        "local parse: unknown escape");
  check(parse_fails_with("[{\"name\":\"\\ud83d\"}]", "unpaired"),
        "local parse: unpaired surrogate");
  check(parse_fails_with("[{\"name\":\"\\u12\"}]", "four hex digits"),
        "local parse: short \\u escape");
  check(parse_fails_with("[] x", "unexpected text after the array"),
        "local parse: trailing text");

  std::string many = "[";
  for (size_t i = 0; i <= kMaxLocalPatches; ++i) {
    if (i > 0) many += ',';
    many += local_entry("e" + std::to_string(i), "o" + std::to_string(i),
                        "r" + std::to_string(i));
  }
  many += "]";
  check(parse_fails_with(many, "more than 100 entries"),
        "local parse: too many entries");
}

// A missing file holds no entries; a directory or an oversized file is an
// error.
static void test_load_local_patches() {
  const std::string path = temp_dir() + "/test_local_patches_load.json";
  std::remove(path.c_str());
  auto r = load_local_patches(path);
  check(r.ok() && r->empty(), "local load: missing file has no entries");

  check(!load_local_patches(temp_dir()).ok(), "local load: directory");

  write_test_file(path, std::string(kMaxLocalPatchesBytes + 1, ' '));
  r = load_local_patches(path);
  check(!r.ok() && std::string(r.status().message()).find("larger than") !=
                       std::string::npos,
        "local load: oversized file");
  write_test_file(path, std::string(kMaxLocalPatchesBytes, ' '));
  r = load_local_patches(path);
  check(r.ok() && r->empty(), "local load: file at the size limit");

  write_test_file(path, "[" + local_entry("a", "x=1;", "x=2;") + "]");
  r = load_local_patches(path);
  check(r.ok() && r->size() == 1 && (*r)[0].replacement == "x=2;",
        "local load: entries read");
  write_test_file(path, "[");
  r = load_local_patches(path);
  check(!r.ok() && std::string(r.status().message()).find("not valid JSON") !=
                       std::string::npos,
        "local load: parse error");
  std::remove(path.c_str());
}

// /api/status's local_patches object.
static void test_local_patches_json() {
  PatchReport rep;
  rep.local_patches_path = "/data/onvif-recorder/local-patches.json";
  rep.local_patches = {{"say \"hi\"\n", PatchOutcome::kApplied},
                       {"b", PatchOutcome::kNotApplicable}};
  check(local_patches_json(rep) ==
            "{\"path\":\"/data/onvif-recorder/local-patches.json\","
            "\"error\":\"\",\"entries\":[{\"name\":\"say \\\"hi\\\"\\u000a\","
            "\"outcome\":\"applied\"},{\"name\":\"b\",\"outcome\":"
            "\"not_applicable\"}]}",
        "local json: entries");
  PatchReport broken;
  broken.local_patches_path = "p";
  broken.local_patches_error = "not valid JSON at line 1, column 1: x\\y";
  check(local_patches_json(broken) ==
            "{\"path\":\"p\",\"error\":\"not valid JSON at line 1, column 1: "
            "x\\\\y\",\"entries\":[]}",
        "local json: error");
}

// Local patches follow ours in patch_service_js(): each is reported, a
// broken file applies none of them while ours still apply, and with a
// verified .bak removing an entry restores the original text.
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

  // A file that is not valid applies none of its entries; ours still
  // apply.
  write_test_file(lp, "[" + local_entry("a", "var A=1;", "var A=10;"));
  patch_service_js(path, opts, md5sums, &rep);
  check(read_test_file(path) == wsa_only, "local invalid: no entry applied");
  check(rep.local_patches.empty() &&
            rep.local_patches_error.find("not valid JSON") !=
                std::string::npos &&
            rep.onvif_addressing == PatchOutcome::kAlready,
        "local invalid: error reported, ours applied");

  // So does one with an entry for another file.
  write_test_file(lp, "[" + local_entry("a", "var A=1;", "var A=10;") + "," +
                          local_entry("ui", "x", "y", "swai.js") + "]");
  write_test_file(path, original);
  check(patch_service_js(path, opts, md5sums, &rep) == 1,
        "local wrong file: only ours written");
  check(read_test_file(path) == wsa_only, "local wrong file: none applied");
  check(rep.local_patches.empty() &&
            rep.local_patches_error.find("only \"service.js\"") !=
                std::string::npos,
        "local wrong file: error reported");

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
  test_load_local_patches();
  test_local_patches_json();
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
