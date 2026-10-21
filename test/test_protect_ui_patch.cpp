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
  test_route_patch_apply();
  test_route_patch_not_applicable();
  test_route_patch_ambiguous();
  test_patch_outcomes();
  test_service_js_report();
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
