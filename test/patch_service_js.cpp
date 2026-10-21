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
 * patch_service_js.cpp
 *
 * Applies onvif-recorder's service.js patch set to a copy of a Protect
 * service.js bundle, with the same code patch_alarm_picker() runs on a
 * router: the backend scope filter always, plus the WS-Addressing patch
 * and the event-sharing route patch as selected.  The input file is never
 * modified.  Used by scripts/protect-e2e to load Protect's own modules
 * exactly as the recorder would leave them.
 *
 * Usage:
 *   patch_service_js --in=<service.js> --out=<patched.js>
 *                    [--onvif_addressing=true|false] [--event_proxy=true|false]
 *
 * Prints one JSON line: {"onvif_addressing":"<outcome>","event_proxy":
 * "<outcome>","applied":<n>,"out":"<path>"} with outcomes as named by
 * patch_outcome_name().  Exit status 0 when the file was patched as asked
 * (every enabled patch applied or already present), 1 otherwise.
 */

// Include the implementation directly to reach the file-local
// patch_service_js().
#include "src/protect_ui_patch.cpp"  // NOLINT(build/include)

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

bool flag_value(const std::string& arg, const std::string& name, std::string* out) {
  const std::string prefix = "--" + name + "=";
  if (arg.compare(0, prefix.size(), prefix) != 0) return false;
  *out = arg.substr(prefix.size());
  return true;
}

bool parse_bool(const std::string& s, bool* out) {
  if (s == "true" || s == "1") {
    *out = true;
    return true;
  }
  if (s == "false" || s == "0") {
    *out = false;
    return true;
  }
  return false;
}

bool path_exists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

int usage(const char* argv0) {
  std::cerr << "usage: " << argv0 << " --in=<service.js> --out=<patched.js>"
            << " [--onvif_addressing=true|false] [--event_proxy=true|false]\n";
  return 2;
}

bool wanted(protect_ui::PatchOutcome o) {
  return o == protect_ui::PatchOutcome::kApplied || o == protect_ui::PatchOutcome::kAlready;
}

bool unwanted(protect_ui::PatchOutcome o) {
  return o == protect_ui::PatchOutcome::kRemoved || o == protect_ui::PatchOutcome::kAbsent;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string in;
  std::string out;
  protect_ui::PatchOptions opts;
  opts.onvif_addressing = true;
  opts.event_proxy = true;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    std::string v;
    if (flag_value(arg, "in", &v)) {
      in = v;
    } else if (flag_value(arg, "out", &v)) {
      out = v;
    } else if (flag_value(arg, "onvif_addressing", &v)) {
      if (!parse_bool(v, &opts.onvif_addressing)) return usage(argv[0]);
    } else if (flag_value(arg, "event_proxy", &v)) {
      if (!parse_bool(v, &opts.event_proxy)) return usage(argv[0]);
    } else {
      return usage(argv[0]);
    }
  }
  if (in.empty() || out.empty() || in == out) return usage(argv[0]);

  std::ifstream src(in, std::ios::binary);
  if (!src) {
    std::cerr << "cannot read " << in << '\n';
    return 2;
  }
  std::ostringstream text;
  text << src.rdbuf();
  {
    std::ofstream dst(out, std::ios::binary | std::ios::trunc);
    dst << text.str();
    if (!dst) {
      std::cerr << "cannot write " << out << '\n';
      return 2;
    }
  }

  // patch_service_js() keeps a backup of the unpatched file next to it,
  // as on a router; the copy needs none.
  const std::string bak = out + ".bak";
  const bool bak_existed = path_exists(bak);
  protect_ui::PatchReport report;
  const std::unordered_map<std::string, std::string> no_md5sums;
  const int applied = protect_ui::patch_service_js(out, opts, no_md5sums, &report);
  if (!bak_existed) std::remove(bak.c_str());

  std::cout << "{\"onvif_addressing\":\""
            << protect_ui::patch_outcome_name(report.onvif_addressing)
            << "\",\"event_proxy\":\"" << protect_ui::patch_outcome_name(report.event_proxy)
            << "\",\"applied\":" << applied << ",\"out\":\"" << out << "\"}" << std::endl;

  const bool ok = (opts.onvif_addressing ? wanted(report.onvif_addressing)
                                         : unwanted(report.onvif_addressing)) &&
                  (opts.event_proxy ? wanted(report.event_proxy)
                                    : unwanted(report.event_proxy));
  return ok ? 0 : 1;
}
