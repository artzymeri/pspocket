#!/usr/bin/env bash
# iOS test host (app/ios): generate, build, install, run, and fetch the self-test report. Run from anywhere.
# `scripts/ios.sh --help` lists the commands. Works with macOS's bash 3.2.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
abspath() { case "$1" in /*) printf '%s\n' "$1" ;; *) printf '%s\n' "$ROOT/$1" ;; esac; }

HOST_BUILD="$(abspath "${PSPOCKET_HOST_BUILD:-build/host}")"
IOS_BUILD="$(abspath "${PSPOCKET_IOS_BUILD:-build/ios}")"
APP_DIR="$ROOT/app/ios"
PROJECT="$APP_DIR/PSPocket.xcodeproj"
SCHEME=PSPocket
APP_NAME=PSPocket.app
BUNDLE_ID=com.artzymeri.pspocket.testhost
DERIVED="$IOS_BUILD/DerivedData"
REPORT=selftest-report.txt
# Homebrew's python3 can be broken (pyexpat); the system one only needs to parse JSON.
if [ -x /usr/bin/python3 ]; then PYTHON=/usr/bin/python3; else PYTHON=python3; fi

usage() {
    cat <<EOF
Usage: scripts/ios.sh <command> [options]

Commands:
  gen                        Build the host CMake tree (it generates the lifted self-test sources), then run
                             xcodegen (app/ios/project.yml -> app/ios/PSPocket.xcodeproj).
  build [--sim] [--debug]    gen, then xcodebuild. Default: Release for the device, signed with team M7XJA39ZV6
                             (-allowProvisioningUpdates). --sim builds for an iPhone simulator. Prints the .app path.
  install [--device ID]      Install the last build on the iPhone (devicectl), or with --sim on the simulator.
  run [--autorun] [--exit] [-- ARGS]
                             Launch with the console attached; the output is also saved to <ios build>/last-run.log.
                             --autorun runs every probe at launch, --exit quits once the report is written.
                             ARGS go to the app, e.g. -- -tab metal -passes 32 (Metal load, stats every 5 s).
  report                     Copy Documents/$REPORT from the app's data container to <ios build>/ and print it.
  pull-iso [--to DIR]        Copy the disc image the app holds (the largest .iso in its Documents: Files > On My
                             iPhone > PSPocket) to DIR/disc.iso (default inventory/). Phone connected and unlocked.
  all                        build + install + run --autorun --exit + report.

Options:
  --device ID   devicectl device: identifier, UDID or name. Default: \$PSPOCKET_DEVICE, else the first connected iPhone.
  --sim         Use an iPhone simulator instead of the device (\$PSPOCKET_SIMULATOR: name or UDID).
  --debug       Debug configuration instead of Release (build, install, all).

Environment:
  PSPOCKET_HOST_BUILD   host CMake build directory, relative to the repo or absolute (default build/host)
  PSPOCKET_IOS_BUILD    Xcode output directory: DerivedData, logs, last-run.log, the report (default build/ios)
EOF
}

say() { printf '==> %s\n' "$*" >&2; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---- options ----

COMMAND="${1:-}"
if [ $# -gt 0 ]; then shift; fi
SIM=0
CONFIG=Release
CONFIG_SET=0
DEVICE="${PSPOCKET_DEVICE:-}"
AUTORUN=0
EXIT_AFTER=0
APP_ARGS=()
PULL_DIR="$ROOT/inventory"
while [ $# -gt 0 ]; do
    case "$1" in
        --) shift; APP_ARGS=("$@"); break ;;
        --to) [ $# -ge 2 ] || die "--to needs a directory"; PULL_DIR="$(abspath "$2")"; shift ;;
        --sim) SIM=1 ;;
        --debug) CONFIG=Debug; CONFIG_SET=1 ;;
        --device) [ $# -ge 2 ] || die "--device needs an identifier"; DEVICE="$2"; shift ;;
        --device=*) DEVICE="${1#--device=}" ;;
        --autorun) AUTORUN=1 ;;
        --exit) EXIT_AFTER=1 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (scripts/ios.sh --help)" ;;
    esac
    shift
done

# ---- helpers ----

# Commit for the build info. --no-optional-locks: never touch the index while other tools are committing.
git_hash() {
    local hash
    hash="$(git -C "$ROOT" --no-optional-locks rev-parse --short=10 HEAD 2>/dev/null)" || { echo unknown; return; }
    if [ -n "$(git -C "$ROOT" --no-optional-locks status --porcelain --untracked-files=no 2>/dev/null)" ]; then
        hash="$hash-dirty"
    fi
    echo "$hash"
}

project_version() {
    local v
    v="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$HOST_BUILD/CMakeCache.txt" 2>/dev/null | head -n 1)"
    [ -n "$v" ] || v="$(sed -n 's/^[[:space:]]*VERSION[[:space:]][[:space:]]*\([0-9][0-9.]*\).*/\1/p' \
        "$ROOT/CMakeLists.txt" | head -n 1)"
    echo "${v:-0.0.0}"
}

find_xcodegen() {
    if command -v xcodegen >/dev/null 2>&1; then command -v xcodegen
    elif [ -x /opt/homebrew/bin/xcodegen ]; then echo /opt/homebrew/bin/xcodegen
    else die "xcodegen not found (brew install xcodegen)"
    fi
}

# Simulator to use: $PSPOCKET_SIMULATOR (name or UDID), else a booted iPhone, else the first iPhone of the newest
# runtime. Prints the UDID.
simulator_udid() {
    xcrun simctl list devices available -j | "$PYTHON" -c '
import json, re, sys
want = sys.argv[1]
runtimes = json.load(sys.stdin)["devices"]
def version(key):
    return [int(n) for n in re.findall(r"\d+", key.rsplit(".", 1)[-1])]
devices = [d for key in sorted(runtimes, key=version, reverse=True) if ".iOS-" in key for d in runtimes[key]]
if want:
    picks = [d for d in devices if want in (d["udid"], d["name"])]
else:
    phones = [d for d in devices if d["name"].startswith("iPhone")]
    picks = [d for d in phones if d["state"] == "Booted"] + phones
if not picks:
    sys.exit("no matching iPhone simulator (xcrun simctl list devices available)")
print(picks[0]["udid"])
' "${PSPOCKET_SIMULATOR:-}"
}

# Device to use: --device / $PSPOCKET_DEVICE, else the first connected iPhone (then the first paired one).
device_id() {
    if [ -n "$DEVICE" ]; then echo "$DEVICE"; return; fi
    mkdir -p "$IOS_BUILD"
    local json="$IOS_BUILD/devices.json"
    rm -f "$json"
    xcrun devicectl list devices --quiet --json-output "$json" >/dev/null 2>&1 || true
    [ -s "$json" ] || die "devicectl could not list devices"
    "$PYTHON" - "$json" <<'EOF'
import json, sys
devices = json.load(open(sys.argv[1]))["result"]["devices"]
phones = [d for d in devices
          if d.get("hardwareProperties", {}).get("deviceType") == "iPhone"
          and d.get("hardwareProperties", {}).get("reality", "physical") == "physical"]
connected = [d for d in phones if d.get("connectionProperties", {}).get("tunnelState") == "connected"]
paired = [d for d in phones if d.get("connectionProperties", {}).get("pairingState") == "paired"]
picks = connected + paired
if not picks:
    sys.exit("error: no iPhone found: connect it with a cable, unlock it and trust this Mac")
d = picks[0]
print("==> device: %s (%s, %s)" % (d["deviceProperties"].get("name", "?"),
      d["hardwareProperties"].get("marketingName", "?"), d["identifier"]), file=sys.stderr)
print(d["identifier"])
EOF
}

# Fails with the message the lead asked for when the phone is locked (launching needs it unlocked).
require_unlocked() {
    local json="$IOS_BUILD/lockstate.json"
    rm -f "$json"
    xcrun devicectl device info lockState --device "$1" --quiet --json-output "$json" >/dev/null 2>&1 || return 0
    if "$PYTHON" -c 'import json, sys; sys.exit(0 if json.load(open(sys.argv[1]))["result"].get("passcodeRequired") else 1)' \
        "$json" 2>/dev/null; then
        die "Unlock the iPhone and run again"
    fi
}

boot_simulator() {
    say "simulator: $1"
    xcrun simctl bootstatus "$1" -b >/dev/null
}

app_path() { # <platform: iphoneos|iphonesimulator>
    local recorded="$IOS_BUILD/last-build-$1"
    if [ "$CONFIG_SET" = 0 ] && [ -s "$recorded" ]; then cat "$recorded"
    else echo "$DERIVED/Build/Products/$CONFIG-$1/$APP_NAME"
    fi
}

# ---- commands ----

cmd_gen() {
    local xcodegen generated count
    xcodegen="$(find_xcodegen)"
    say "host build: $HOST_BUILD"
    local host_ok=1 generator=()
    # Configure every time (about a second): a configure that failed earlier leaves a cache but no build files.
    if [ ! -f "$HOST_BUILD/CMakeCache.txt" ]; then generator=(-G Ninja); fi
    cmake -S "$ROOT" -B "$HOST_BUILD" ${generator[@]+"${generator[@]}"} --log-level=WARNING >&2 || host_ok=0
    if [ "$host_ok" = 1 ]; then
        cmake --build "$HOST_BUILD" >&2 || host_ok=0
    fi
    if [ "$host_ok" = 0 ]; then
        warn "the host build failed; continuing with whatever generated sources exist" \
             "(Xcode compiles the runtime sources itself)"
    fi
    generated="$HOST_BUILD/generated/selftest"
    count=0
    if [ -d "$generated" ]; then
        count="$(find "$generated" -type f \( -name '*.c' -o -name '*.cpp' \) | wc -l | tr -d ' ')"
    fi
    say "generated self-test sources: $count in $generated"
    # A component joins the iOS build when it joins the host build (pspocket_add_component): once its
    # CMakeLists.txt exists. Until then its sources may be half-written.
    local ppu_src="$ROOT/runtime/ppu/src"
    if [ ! -f "$ROOT/runtime/ppu/CMakeLists.txt" ]; then
        if [ -d "$ppu_src" ]; then say "runtime/ppu/src: not compiled until runtime/ppu/CMakeLists.txt exists"; fi
        ppu_src="$ROOT/runtime/ppu/not-in-the-host-build-yet"
    fi
    say "xcodegen: $PROJECT"
    PSPOCKET_HOST_BUILD_DIR="$HOST_BUILD" PSPOCKET_PPU_SRC="$ppu_src" \
        "$xcodegen" generate --spec "$APP_DIR/project.yml" --quiet >&2
}

cmd_build() {
    cmd_gen
    local hash version platform destination log status
    local extra=()
    hash="$(git_hash)"
    version="$(project_version)"
    mkdir -p "$IOS_BUILD"
    if [ "$SIM" = 1 ]; then
        platform=iphonesimulator
        destination="platform=iOS Simulator,id=$(simulator_udid)"
    else
        platform=iphoneos
        destination="generic/platform=iOS"
        extra=(-allowProvisioningUpdates)
    fi
    log="$IOS_BUILD/xcodebuild-$platform.log"
    say "xcodebuild $CONFIG, $destination, pspocket $version ($hash); full log: $log"
    # Show diagnostics as they come; the full output goes to the log.
    set +e +o pipefail
    xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration "$CONFIG" -destination "$destination" \
        -derivedDataPath "$DERIVED" ${extra[@]+"${extra[@]}"} \
        PSPOCKET_GIT_HASH="$hash" PSPOCKET_VERSION="$version" build 2>&1 \
        | tee "$log" \
        | grep --line-buffered -E '(^|: )(error|warning): |^\*\* BUILD ' \
        | awk '!seen[$0]++ { print; fflush() }' >&2
    status=${PIPESTATUS[0]}
    set -e -o pipefail
    if [ "$status" != 0 ]; then
        if grep -q 'No Accounts' "$log"; then
            warn "Xcode has no Apple ID signed in, so it cannot create the provisioning profile for $BUNDLE_ID" \
                 "with the two memory entitlements. Open Xcode > Settings > Accounts, add the Apple ID of team" \
                 "M7XJA39ZV6, then run again."
        fi
        die "xcodebuild failed (status $status); full log: $log"
    fi

    local app="$DERIVED/Build/Products/$CONFIG-$platform/$APP_NAME"
    [ -d "$app" ] || die "build succeeded but $app is missing"
    echo "$app" >"$IOS_BUILD/last-build-$platform"
    say "app: $app"
    echo "$app"
}

cmd_install() {
    if [ "$SIM" = 1 ]; then
        local udid app
        udid="$(simulator_udid)"
        app="$(app_path iphonesimulator)"
        [ -d "$app" ] || die "no simulator build at $app (scripts/ios.sh build --sim)"
        boot_simulator "$udid"
        say "install $app"
        xcrun simctl install "$udid" "$app"
    else
        local dev app
        dev="$(device_id)"
        app="$(app_path iphoneos)"
        [ -d "$app" ] || die "no device build at $app (scripts/ios.sh build)"
        say "install $app"
        xcrun devicectl device install app --device "$dev" "$app" >&2
    fi
}

# Sets RUN_STATUS (the launcher's exit status; the app exits 1 when a probe failed) instead of failing, so `all`
# still fetches the report.
RUN_STATUS=0
cmd_run() {
    local args=() log="$IOS_BUILD/last-run.log" status
    if [ "$AUTORUN" = 1 ]; then args+=(-autorun); fi
    if [ "$EXIT_AFTER" = 1 ]; then args+=(-autorun-exit); fi
    args+=(${APP_ARGS[@]+"${APP_ARGS[@]}"})
    mkdir -p "$IOS_BUILD"
    if [ "$SIM" = 1 ]; then
        local udid
        udid="$(simulator_udid)"
        boot_simulator "$udid"
        say "launch $BUNDLE_ID ${args[*]+"${args[*]}"} (console; Ctrl-C to stop)"
        set +e +o pipefail
        xcrun simctl launch --console-pty --terminate-running-process "$udid" "$BUNDLE_ID" \
            ${args[@]+"${args[@]}"} 2>&1 | tee "$log"
        status=${PIPESTATUS[0]}
        set -e -o pipefail
    else
        local dev
        dev="$(device_id)"
        require_unlocked "$dev"
        say "launch $BUNDLE_ID ${args[*]+"${args[*]}"} (console; Ctrl-C to stop)"
        set +e +o pipefail
        # `--` keeps devicectl from parsing the app's arguments: it would read -autorun as bundled short flags.
        xcrun devicectl device process launch --device "$dev" --console --terminate-existing -- "$BUNDLE_ID" \
            ${args[@]+"${args[@]}"} 2>&1 | tee "$log"
        status=${PIPESTATUS[0]}
        set -e -o pipefail
        if [ "$status" != 0 ] && grep -qi 'locked' "$log"; then
            die "Unlock the iPhone and run again"
        fi
    fi
    say "console log: $log"
    if [ "$status" != 0 ]; then warn "the app or the launch failed (status $status); see the log above"; fi
    RUN_STATUS=$status
}

cmd_report() {
    local out="$IOS_BUILD/$REPORT"
    mkdir -p "$IOS_BUILD"
    rm -f "$out"
    if [ "$SIM" = 1 ]; then
        local udid container
        udid="$(simulator_udid)"
        container="$(xcrun simctl get_app_container "$udid" "$BUNDLE_ID" data)" \
            || die "the app is not installed on the simulator"
        cp "$container/Documents/$REPORT" "$out" || die "no report yet (scripts/ios.sh run --sim --autorun --exit)"
    else
        local dev
        dev="$(device_id)"
        xcrun devicectl device copy from --device "$dev" --domain-type appDataContainer \
            --domain-identifier "$BUNDLE_ID" --source "Documents/$REPORT" --destination "$out" >&2 \
            || die "could not copy the report: is the app installed and has the self-test run? (scripts/ios.sh all)"
    fi
    [ -s "$out" ] || die "no report at $out"
    cat "$out"
    say "report: $out"
}

# The game's disc image from the app's Documents, for Mac runs (scripts/boot.sh calls this when the Mac has none).
cmd_pull_iso() {
    local dev json pick name size dest free_kb
    dev="$(device_id)"
    require_unlocked "$dev"
    mkdir -p "$IOS_BUILD" "$PULL_DIR"
    json="$IOS_BUILD/documents.json"
    rm -f "$json"
    xcrun devicectl device info files --device "$dev" --domain-type appDataContainer \
        --domain-identifier "$BUNDLE_ID" --subdirectory Documents --quiet --json-output "$json" >/dev/null 2>&1 \
        || xcrun devicectl device info files --device "$dev" --domain-type appDataContainer \
            --domain-identifier "$BUNDLE_ID" --recurse --quiet --json-output "$json" >/dev/null 2>&1 \
        || die "could not list the app's files on the iPhone: is PSPocket installed? (scripts/ios.sh all)"
    # The listing's layout isn't documented: take every object that names a .iso, with its size when it has one.
    pick="$("$PYTHON" - "$json" <<'PY'
import json, sys
found = []
def walk(o):
    if isinstance(o, dict):
        name = next((v for k, v in o.items() if isinstance(v, str) and k.lower() in ("name", "relativepath", "path")
                     and v.lower().endswith(".iso")), None)
        if name:
            # devicectl keeps the size in "metadata": {"metadata": {"size": N, ...}, "name": ..., "relativePath": ...}
            fields = dict(o, **o["metadata"]) if isinstance(o.get("metadata"), dict) else o
            size = next((v for k, v in fields.items() if k.lower() in ("size", "filesize") and isinstance(v, int)), 0)
            found.append((size, name.rsplit("/", 1)[-1]))
        for v in o.values(): walk(v)
    elif isinstance(o, list):
        for v in o: walk(v)
walk(json.load(open(sys.argv[1])))
if found:
    size, name = max(found)
    print("%d\t%s" % (size, name))
PY
)"
    [ -n "$pick" ] || die "no .iso in the app's Documents on the iPhone (Files > On My iPhone > PSPocket)"
    size="${pick%%$'\t'*}"
    name="${pick#*$'\t'}"
    dest="$PULL_DIR/disc.iso"
    free_kb="$(df -Pk "$PULL_DIR" | awk 'NR == 2 { print $4 }')"
    if [ "$size" -gt 0 ] && [ $((free_kb / 1024)) -lt $((size / 1048576 + 1024)) ]; then
        die "not enough space on this Mac for $name: it needs $((size / 1073741824 + 1)) GB free in $PULL_DIR"
    fi
    say "copying \"$name\" ($((size / 1048576)) MB) from the iPhone to $dest; only the first time, a few minutes"
    xcrun devicectl device copy from --device "$dev" --domain-type appDataContainer \
        --domain-identifier "$BUNDLE_ID" --source "Documents/$name" --destination "$dest.part" >&2 \
        || { rm -f "$dest.part"; die "copying the disc image failed (keep the iPhone connected and unlocked)"; }
    mv "$dest.part" "$dest"
    say "disc image: $dest"
}

case "$COMMAND" in
    gen) cmd_gen ;;
    build) cmd_build ;;
    install) cmd_install ;;
    run) cmd_run; exit "$RUN_STATUS" ;;
    report) cmd_report ;;
    pull-iso) cmd_pull_iso ;;
    all)
        cmd_build >/dev/null
        cmd_install
        AUTORUN=1
        EXIT_AFTER=1
        cmd_run
        cmd_report
        exit "$RUN_STATUS"
        ;;
    -h|--help|help|"") usage ;;
    *) usage >&2; die "unknown command: $COMMAND" ;;
esac
