#!/usr/bin/env bash
# Copyright 2026 Daniel W
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Runs the Protect event-sharing end-to-end scenarios (e2e.cjs) against
# each Protect service.js given, one after another (they share port 7892).
# Builds //test:pullpoint_demo and //test:patch_service_js first.
#
# usage: scripts/protect-e2e/run.sh --nm=<node_modules> [--fxp=<fast-xml-parser dir>]
#          [--out=<results dir>] [--only=A,B,...] <label>=<service.js> ...
#
#   scripts/protect-e2e/run.sh --nm=$HOME/protect-npm/node_modules \
#       7.3.70=$HOME/protect/7.3.70/service.js 7.2.105=$HOME/protect/7.2.105/service.js
#
# Results: <out>/results-<label>.json (default out: a new temporary
# directory, printed at the end).  Exit status 0 when every scenario of
# every bundle passed.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

NM=""
FXP=""
OUT=""
ONLY=""
BUNDLES=()
for arg in "$@"; do
  case "$arg" in
    --nm=*) NM="${arg#--nm=}" ;;
    --fxp=*) FXP="${arg#--fxp=}" ;;
    --out=*) OUT="${arg#--out=}" ;;
    --only=*) ONLY="${arg#--only=}" ;;
    *=*) BUNDLES+=("$arg") ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done
if [ -z "$NM" ] || [ ${#BUNDLES[@]} -eq 0 ]; then
  sed -n '/^# usage/,/^# every bundle/p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi
[ -n "$FXP" ] || FXP="$NM/fast-xml-parser"
[ -n "$OUT" ] || OUT=$(mktemp -d "${TMPDIR:-/tmp}/protect-e2e.XXXXXX")
mkdir -p "$OUT"

"$REPO/scripts/bz" build --config=x86 //test:pullpoint_demo //test:patch_service_js
BIN="$("$REPO/scripts/bz" info --config=x86 bazel-bin 2>/dev/null)/test"
CAPTURE="$REPO/test/testdata/dahua_dh_sd4a425db_hny_20260219.jsonl"

status=0
for b in "${BUNDLES[@]}"; do
  label="${b%%=*}"
  bundle="${b#*=}"
  extra=()
  [ -n "$ONLY" ] && extra+=("--only=$ONLY")
  node "$HERE/e2e.cjs" --bundle="$bundle" --label="$label" --nm="$NM" --fxp="$FXP" \
    --demo="$BIN/pullpoint_demo" --patcher="$BIN/patch_service_js" --capture="$CAPTURE" \
    --work="$OUT/work-$label" --out="$OUT/results-$label.json" "${extra[@]}" || status=1
done
echo "results: $OUT"
exit $status
