# Protect event sharing: end-to-end harness

This harness runs **Protect's own event-subscription code** against the
recorder's real PullPoint service. It checks that Protect event sharing
(`--protect_event_proxy_cameras`) behaves as designed with each Protect
version.

The harness does the following:

1. It patches a copy of Protect's `service.js` with `//test:patch_service_js`.
   That binary runs exactly the C++ patch set `patch_alarm_picker()` applies
   on a console: the scope filter, the WS-Addressing patch, and the
   event-sharing route patch.
2. It loads Protect's real modules out of the patched bundle, using the
   npm packages Protect itself uses:
   - the soap transport;
   - the onvif events, poll, reconcile and replace/stop-subscription modules;
   - the motion probe and the constants.

   The store, the message bus, the logger and the analytics/IO probes are
   stubs that record what Protect does. Protect's own 5 s poll timer and 60 s
   reconcile timer are not loaded. The harness drives the same functions on
   the same intervals instead, and the 5 s poll never overlaps itself, just
   as in Protect.
3. It runs `//test:pullpoint_demo` as the recorder. This is the real
   `PullPointServer`, `PullPointHub` and `OnvifListener`, wired as
   `src/main.cpp` wires them. A second `pullpoint_demo --no_recorder`
   process provides the emulated cameras:
   - `SlotLimitedCameraEmulator` replaying the Dahua PTZ capture;
   - one subscription slot by default;
   - motion on demand.

   Because the cameras run in their own process, they stay up while the
   recorder restarts.

The harness contains no Protect code. Protect's modules come from the
bundle you pass when you run it. Never commit a `service.js`, a Protect
`.deb` or a `node_modules` directory to this repository.

## What you need

| Piece | Where to get it |
|---|---|
| Protect `service.js` | **From a console:** copy `/usr/share/unifi-protect/app/service.js`. If onvif-recorder has patched it, `service.js.bak` next to it is the original, but the patched file also works because the patcher reports already-present patches as `already`. **From a Protect package:** extract the `unifi-protect` `.deb`, e.g. `dpkg-deb -x unifi-protect_<ver>_arm64.deb x/`, and use `x/usr/share/unifi-protect/app/service.js`. On a console, `apt-get download unifi-protect` usually fetches the installed version's package. The bundle is plain JavaScript, so an arm64 package works on an x86 machine. |
| npm packages | The packages Protect loads at runtime, at Protect's versions. Protect 7.3.70 ships undici 7.24.6, fast-xml-parser 5.3.8, node-cache 5.1.2 and p-queue 6.6.2. To check another version, look at `/usr/share/unifi-protect/app/node_modules/<package>/package.json` on its console. To install: `mkdir -p ~/protect-npm && cd ~/protect-npm && npm install undici@7.24.6 fast-xml-parser@5.3.8 node-cache@5.1.2 p-queue@6.6.2` |
| Node.js | 20.18 or newer, which undici 7 requires. Protect 7.3.70 itself runs Node 24. |
| `//test:pullpoint_demo`, `//test:patch_service_js` | Built by `run.sh` with `scripts/bz build --config=x86`. |

Port 7892 on 127.0.0.1 must be free, and 7893 for scenario H. The route
patch sends Protect's requests to 7892. `e2e.cjs --port=N` rewrites that one number in the patched
copy, for a machine where 7892 is taken.

## Running

```bash
scripts/protect-e2e/run.sh --nm=$HOME/protect-npm/node_modules \
    7.1.87=/path/to/7.1.87/service.js \
    7.2.105=/path/to/7.2.105/service.js \
    7.3.70=/path/to/7.3.70/service.js
```

- `--fxp=<dir>`: point at a fast-xml-parser package somewhere else. The
  default is `<nm>/fast-xml-parser`.
- `--out=<dir>`: keep the results. The default is a new temporary
  directory.
- `--only=A,D`: run some scenarios only.

Each bundle takes about 9 minutes. You can also run one bundle directly:

```bash
node scripts/protect-e2e/e2e.cjs --bundle=<service.js> --label=7.3.70 \
    --nm=<node_modules> --demo=<bazel-bin>/test/pullpoint_demo \
    --patcher=<bazel-bin>/test/patch_service_js \
    --capture=test/testdata/dahua_dh_sd4a425db_hny_20260219.jsonl \
    [--work=<dir>] [--out=<results.json>] [--only=...]
```

The output files are:

- `results-<label>.json`: the patch outcomes, plus, for each scenario,
  `pass`, the named checks (with details when one fails) and the measured
  `numbers` (latencies, tick durations, hub counters, request summaries).
- `work-<label>/`: the stderr of every demo process the scenarios started.

The exit status is 0 when every check passed.

## Scenarios

| | Setup | Checks |
|---|---|---|
| A steady | Shared one-slot camera. | Create, Pull and Renew all go to the recorder. The camera sees one subscription (the recorder's) plus Protect's direct GetEventProperties probe. A motion on/off from the camera reaches Protect as one motionStart and one motionStop. No warn logs from Protect. |
| B decline | The camera is not shared (two slots, so the recorder's own subscription leaves room for Protect). | The recorder declines Protect's Create and Protect subscribes at the camera, with no error counted. A direct Renew asks the recorder `/status`, gets no, and renews at the camera. |
| C status probe | Protect starts direct. The recorder is then restarted with the camera shared. | Protect's next Renew moves its subscription to the recorder. The camera drops to one subscription (the recorder's), Protect keeps `isMotionActive`, and the recorder resubscribes about 10 s after the move. Motion still reaches Protect afterwards. |
| D recorder restart | The recorder restarts (SIGTERM, then start with the same `--state_dir`) at four points: between polls, while Protect's pull is held, after a 2.5 s pause, and after a 2.5 s pause with Protect's Renew due. | No "Deactivated" and no circuit breaker. Protect's error count stays below 3. The pull point is adopted. The recorder makes no new Create at the camera (resume) and calls SetSynchronizationPoint once per restart. Motion after the restarts reaches Protect. |
| E recorder down | Nothing listens on 7892. | Create falls back to the camera in well under a second. |
| F hung recorder | A server accepts on 7892 and never answers. | F1: Create falls back after about 4 s. F2: with a proxied camera and a direct camera, no poll tick takes longer than about 6 s, so the direct camera keeps being polled. F3: as F2, with the proxied subscription's Renew due when the recorder hangs. F4: as F2, with a one-slot proxied camera whose slot the recorder's kept subscription holds, and Protect's Renew due right after its second failed pull. |
| G Protect restart | Protect stops its subscription (Unsubscribe) and a fresh Protect runtime starts. | The old pull point faults. The new Create goes through the recorder, and the new subscriber gets the current state (motion on) as Initialized. The camera still has only the recorder's subscription. |
| H recorder stall | A relay on 7892 forwards to the recorder (on 7893) but holds the connections that arrive in one 4.6 s window. | One Protect pull times out (one error), then Protect carries on through the recorder: no Deactivated, still subscribed via the recorder. |

Every scenario also checks everything Protect logged, debug lines
included:

- Protect never falls back to its raw-socket transport. It does that for
  an hour per host after a response undici cannot use.
- Protect accepts every TerminationTime it is sent. It silently replaces
  one that is less than 15 s or more than 600 s after CurrentTime with
  now + 55 s.

## pullpoint_demo on its own

`pullpoint_demo` is also useful by hand:

```bash
scripts/bz build --config=x86 //test:pullpoint_demo
BIN=$(scripts/bz info --config=x86 bazel-bin)/test
$BIN/pullpoint_demo --capture=test/testdata/dahua_dh_sd4a425db_hny_20260219.jsonl \
    --state_dir=/tmp/pp-demo            # serves 127.0.0.1:7892
```

On start it prints one JSON line with the server port and each camera's
address, key and MAC. It then reads one command per line on stdin and
answers each with a JSON line:

| Command | What it does |
|---|---|
| `motion <i> true\|false` | Queues a motion change on camera `<i>`. |
| `stats` | Prints counters for each camera. |
| `requests <i> [from]` | Prints the camera's request log, with each request's sender. |
| `hub` | Prints the hub's snapshot. |
| `quit` | Stops the demo. |

SIGTERM shuts down in `src/main.cpp`'s order and keeps the shared camera
subscriptions for the next run in `--state_dir`.

Options:

- `--no_recorder`: run cameras only.
- `--cameras=host:port,...`: run a recorder for cameras that are already
  running.
- `--share=all|none|0,1`: choose which cameras are shared.
- `--replay_pulls=false`: answer pulls with motion messages only.
- `--slots`, `--second_capture`, `--second_slots`: set up the cameras.

`--help` lists every option.
