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
  push-game [--elf FILE] [--title ID] [--flash DIR]
                             Copy what the Game tab needs besides the disc image (which you pick in Files) to the
                             app's Documents: the game's decrypted executable as game/<TITLE_ID>/EBOOT.elf (default
                             inventory/*/decrypt/EBOOT.elf; the title ID from the disc image next to it) and the
                             /dev_flash files the game loads itself (default inventory/firmware/dev_flash, without the
                             reference-only .sprx). Phone connected and unlocked. They stay on your devices.
  pull-coverage              Copy what the phone met that the translation does not cover (D-117) into the game's
                             inventory directory: the SPU programs without lifted code, the addresses lifted code did not
                             reach (a run log for scripts/lift-spu.sh) and the shaders it compiled, from that game's
                             folders only (its title ID: <game>/title-id or the directory's name; D-118). Phone
                             connected and unlocked. Then translate and build again (scripts/lift-spu.sh, build or
                             testflight).
  all                        build + install + run --autorun --exit + report.
  gate                       scripts/gate.sh: the tests, then GTA V's walk on the Mac against games/gta5/gate.json
                             (D-149). build and testflight run it first; --no-gate skips it (say why in the commit).
  pull-log                   Copy the game's log and the one of the launch before (Documents/game.log, game-prev.log)
                             to inventory/phone/logs/. Phone connected and unlocked.
  push-signing-identity      Export the Apple Development identity the app's profile names from the Mac's keychain
                             (a .p12 under a random password, with its chain; macOS asks once) into inventory/signing
                             and copy it to the app's Library/Application Support/pspocket-codegen, for libraries the
                             app signs itself (route B, D-150). The key is the user's own: it goes to their phone only,
                             never into the repository.
  session [FILE] [--since HH:MM]
                             Summarise each app session in a device log (default: the newest
                             inventory/phone/live/syslog-*.log): fps, sound, memory, heat, GPU, threads, faults.
  testflight [--upload-only] gen, then archive the Release build, export it signed for App Store Connect (team
                             M7XJA39ZV6) and upload it for TestFlight: the way onto an iPhone that is not connected to
                             this Mac or on its network. The build number is the commit count, so each upload is
                             higher than the last. App Store Connect needs an app with the bundle ID first (created
                             once, on the website: its API cannot). The upload holds the game's translated code: it
                             stays private to the team (internal testers). --upload-only exports and uploads the last
                             archive. Signs in with an App Store Connect API key when \$PSPOCKET_ASC_KEY_ID is set,
                             else with the Apple ID Xcode is signed in with.

Options:
  --device ID   devicectl device: identifier, UDID or name. Default: \$PSPOCKET_DEVICE, else the first connected iPhone.
  --sim         Use an iPhone simulator instead of the device (\$PSPOCKET_SIMULATOR: name or UDID).
  --debug       Debug configuration instead of Release (build, install, all).
  --no-gate     build, testflight: skip scripts/gate.sh.

Environment:
  PSPOCKET_HOST_BUILD   host CMake build directory, relative to the repo or absolute (default build/host)
  PSPOCKET_GAME_SPU     the game's lifted SPU programs (scripts/lift-spu.sh output) to build in (default: the
                        inventory/*/spu-lifted that holds a game_spu.c, if any)
  PSPOCKET_GAME_PPU     the game's translated PPU code (ppu-lift output, --name game_ppu) to build in (default: the
                        inventory/*/ppu-lifted that holds a game_ppu_reg.c, if any)
  PSPOCKET_GAME_SHADERS the shaders the Mac recorded for the game (pspocket-boot --shader-cache; a directory named
                        shaders) to put in the app (default: the inventory/*/shaders that holds programs, if any)
  PSPOCKET_IOS_BUILD    Xcode output directory: DerivedData, logs, last-run.log, the report (default build/ios)
  PSPOCKET_ASC_KEY_ID, PSPOCKET_ASC_ISSUER_ID, PSPOCKET_ASC_KEY_PATH
                        testflight: an App Store Connect API key (the .p8 file, default
                        ~/.appstoreconnect/private_keys/AuthKey_<ID>.p8, where altool looks for it)
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
GAME_ELF=""
GAME_TITLE=""
GAME_FLASH="$ROOT/inventory/firmware/dev_flash"
while [ $# -gt 0 ]; do
    case "$1" in
        --) shift; APP_ARGS=("$@"); break ;;
        --to) [ $# -ge 2 ] || die "--to needs a directory"; PULL_DIR="$(abspath "$2")"; shift ;;
        --elf) [ $# -ge 2 ] || die "--elf needs a file"; GAME_ELF="$(abspath "$2")"; shift ;;
        --title) [ $# -ge 2 ] || die "--title needs a title ID"; GAME_TITLE="$2"; shift ;;
        --flash) [ $# -ge 2 ] || die "--flash needs a directory"; GAME_FLASH="$(abspath "$2")"; shift ;;
        --sim) SIM=1 ;;
        --debug) CONFIG=Debug; CONFIG_SET=1 ;;
        --device) [ $# -ge 2 ] || die "--device needs an identifier"; DEVICE="$2"; shift ;;
        --device=*) DEVICE="${1#--device=}" ;;
        --autorun) AUTORUN=1 ;;
        --exit) EXIT_AFTER=1 ;;
        --upload-only) UPLOAD_ONLY=1 ;;
        --no-gate) NO_GATE=1 ;;
        --since) [ $# -ge 2 ] || die "--since needs HH:MM"; SINCE="$2"; shift ;;
        *.log) SESSION_LOG="$(abspath "$1")" ;;
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
        [ "${PSPOCKET_ALLOW_HOST_FAIL:-0}" = 1 ] || die "the host build failed (PSPOCKET_ALLOW_HOST_FAIL=1 goes on anyway)"
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
    # The game's lifted SPU programs (scripts/lift-spu.sh, D-076): $PSPOCKET_GAME_SPU, else an inventory/*/spu-lifted
    # that holds a game_spu.c. Without them the interpreter runs the SPUs.
    local game_spu="${PSPOCKET_GAME_SPU:-}" have_game_spu=1
    if [ -z "$game_spu" ]; then
        for d in "$ROOT"/inventory/*/spu-lifted; do
            if [ -f "$d/game_spu.c" ]; then game_spu="$d"; fi
        done
    fi
    if [ -n "$game_spu" ] && [ -f "$game_spu/game_spu.c" ]; then
        say "lifted SPU programs: $game_spu ($(find "$game_spu" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ') programs)"
    else
        say "no lifted SPU programs (scripts/lift-spu.sh): the SPUs are interpreted"
        game_spu="$ROOT/inventory/no-lifted-spu-programs"
        have_game_spu=0
    fi
    # The game's PPU code translated ahead of time (ppu-lift, D-092): $PSPOCKET_GAME_PPU, else an
    # inventory/*/ppu-lifted that holds a game_ppu_reg.c. The app uses it only for the executable it was translated
    # from (D-098); without it the interpreter runs the game's PPU code.
    local game_ppu="${PSPOCKET_GAME_PPU:-}" have_game_ppu=1
    if [ -z "$game_ppu" ]; then
        for d in "$ROOT"/inventory/*/ppu-lifted; do
            if [ -f "$d/game_ppu_reg.c" ]; then game_ppu="$d"; fi
        done
    fi
    if [ -n "$game_ppu" ] && [ -f "$game_ppu/game_ppu_reg.c" ]; then
        say "translated PPU code: $game_ppu ($(find "$game_ppu" -maxdepth 1 -name 'game_ppu_*.c' | wc -l | tr -d ' ') units)"
    else
        say "no translated PPU code (ppu-lift): the PPU is interpreted"
        game_ppu="$ROOT/inventory/no-translated-ppu-code"
        have_game_ppu=0
    fi
    # The shaders the Mac recorded for the game (pspocket-boot --shader-cache, D-114): $PSPOCKET_GAME_SHADERS, else an
    # inventory/*/shaders with programs in it. The app makes them ahead while the game loads.
    local game_shaders="${PSPOCKET_GAME_SHADERS:-}"
    if [ -z "$game_shaders" ]; then
        for d in "$ROOT"/inventory/*/shaders; do
            if [ -d "$d/programs" ]; then game_shaders="$d"; fi
        done
    fi
    if [ -n "$game_shaders" ] && [ -d "$game_shaders/programs" ]; then
        say "recorded shaders: $game_shaders ($(find "$game_shaders/programs" -name '*.txt' | wc -l | tr -d ' ') programs)"
    else
        say "no recorded shaders (pspocket-boot --shader-cache): the app compiles them as the game meets them"
        game_shaders="$ROOT/inventory/no-recorded-shaders/shaders"
    fi
    # Route B's compiler on the phone (D-151): LLVM, clang and lld built for iOS by scripts/build-llvm-ios.sh, and
    # the folder the app compiles against (clang's headers, runtime/codegen/sysroot, the runtime's ABI headers).
    local llvm_ios="$ROOT/build/llvm-ios" llvm_src="" have_codegen=0 codegen_src="$ROOT/runtime/codegen/none"
    local codegen_bundle="$IOS_BUILD/no-codegen/codegen" llvm_ldflags=""
    if [ -f "$llvm_ios/lib/libclangFrontend.a" ] && [ -f "$llvm_ios/lib/liblldMachO.a" ]; then
        llvm_src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:[A-Z]*=//p' "$llvm_ios/CMakeCache.txt")/.." # llvm-project
        have_codegen=1
        codegen_src="$ROOT/runtime/codegen/src"
        codegen_bundle="$IOS_BUILD/codegen-bundle/codegen"
        rm -rf "$IOS_BUILD/codegen-bundle"
        mkdir -p "$codegen_bundle/clang/include" "$codegen_bundle/include/pspocket"
        # Of clang's own headers (15 MB, every target's), the ones arm64 C needs: 2 MB.
        local clang_inc
        clang_inc="$(ls -d "$llvm_ios"/lib/clang/*/include | head -1)"
        (cd "$clang_inc" && cp arm_neon.h arm_fp16.h arm_bf16.h arm_vector_types.h arm_acle.h std*.h __std*.h \
            float.h limits.h iso646.h ptrauth.h "$codegen_bundle/clang/include/")
        cp -R "$ROOT/runtime/codegen/sysroot" "$codegen_bundle/sysroot"
        for d in ppu spu core; do cp -R "$ROOT/runtime/$d/include/pspocket/$d" "$codegen_bundle/include/pspocket/"; done
        llvm_ldflags="$(ls "$llvm_ios"/lib/*.a | tr '\n' ' ')"
        say "compiler on the device: $(ls "$llvm_ios"/lib/*.a | wc -l | tr -d ' ') LLVM libraries ($llvm_ios), headers in $codegen_bundle"
    else
        mkdir -p "$codegen_bundle"
        say "no compiler on the device (scripts/build-llvm-ios.sh builds it)"
    fi
    say "xcodegen: $PROJECT"
    PSPOCKET_HOST_BUILD_DIR="$HOST_BUILD" PSPOCKET_PPU_SRC="$ppu_src" PSPOCKET_GAME_SPU_SRC="$game_spu" \
        PSPOCKET_HAVE_GAME_SPU="$have_game_spu" PSPOCKET_GAME_PPU_SRC="$game_ppu" PSPOCKET_HAVE_GAME_PPU="$have_game_ppu" \
        PSPOCKET_GAME_SHADERS_SRC="$game_shaders" PSPOCKET_HAVE_CODEGEN="$have_codegen" PSPOCKET_CODEGEN_SRC="$codegen_src" \
        PSPOCKET_CODEGEN_BUNDLE_SRC="$codegen_bundle" PSPOCKET_LLVM_SRC_DIR="$llvm_src" PSPOCKET_LLVM_IOS_DIR="$llvm_ios" \
        PSPOCKET_LLVM_LDFLAGS="$llvm_ldflags" "$xcodegen" generate --spec "$APP_DIR/project.yml" --quiet >&2
}

# scripts/gate.sh (D-149): nothing reaches the phone that does worse on the Mac than what is there. Exit 2 (no game
# inputs here) runs only the tests.
cmd_gate() {
    if [ "${NO_GATE:-0}" = 1 ]; then
        warn "the gate is skipped (--no-gate)"
        return 0
    fi
    local status=0
    "$ROOT/scripts/gate.sh" quick || die "the gate failed: the tests (scripts/gate.sh quick)"
    "$ROOT/scripts/gate.sh" game || status=$?
    case "$status" in
        0) ;;
        2) warn "the game step of the gate cannot run here (no game inputs in inventory/)" ;;
        *) die "the gate failed: the game walk (scripts/gate.sh game; the run is in inventory/gate/)" ;;
    esac
}

cmd_pull_log() {
    local id dir stamp f
    id="$(device_id)"
    require_unlocked "$id"
    dir="$ROOT/inventory/phone/logs"
    stamp="$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$dir"
    for f in game.log game-prev.log; do
        if xcrun devicectl device copy from --device "$id" --domain-type appDataContainer \
            --domain-identifier "$BUNDLE_ID" --source "Documents/$f" --destination "$dir/$stamp-$f" >/dev/null 2>&1; then
            say "$dir/$stamp-$f ($(wc -l <"$dir/$stamp-$f" | tr -d ' ') lines)"
        else
            warn "no Documents/$f on the phone"
        fi
    done
}

cmd_push_signing_identity() {
    local id dir f
    id="$(device_id)"
    require_unlocked "$id"
    dir="$ROOT/inventory/signing"
    mkdir -p "$dir"
    chmod 700 "$dir"
    rm -f "$dir"/identity.p12 "$dir"/identity.pass "$dir"/chain-*.cer
    say "exporting the Apple Development identity (macOS asks for the keychain password once)"
    "$HOST_BUILD/runtime/codesign/pspocket-codesign" export-identity "Apple Development" "$dir" >&2 \
        || die "the identity was not exported"
    for f in "$dir"/identity.p12 "$dir"/identity.pass "$dir"/chain-*.cer; do
        [ -f "$f" ] || continue
        xcrun devicectl device copy to --device "$id" --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
            --source "$f" --destination "Library/Application Support/pspocket-codegen/$(basename "$f")" >/dev/null \
            || die "could not copy $(basename "$f") to the phone"
    done
    say "the signing identity is on the phone (Library/Application Support/pspocket-codegen)"
}

cmd_session() {
    local log="${SESSION_LOG:-}"
    if [ -z "$log" ]; then
        log="$(ls -t "$ROOT"/inventory/phone/live/syslog-*.log 2>/dev/null | head -1)"
        [ -n "$log" ] || die "no device log in inventory/phone/live/ (idevicesyslog -p PSPocket > ...)"
    fi
    say "sessions in $log"
    "$PYTHON" "$ROOT/scripts/phone_session.py" "$log" ${SINCE:+--since "$SINCE"}
}

cmd_build() {
    cmd_gate
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

# The Game tab's files besides the disc image: the decrypted executable and the user's /dev_flash files.
cmd_push_game() {
    local dev elf title iso boot f rel
    if [ -n "$GAME_ELF" ]; then elf="$GAME_ELF"
    else elf="$(find "$ROOT/inventory" -name EBOOT.elf -path '*/decrypt/*' 2>/dev/null | head -1)"
    fi
    [ -n "$elf" ] && [ -f "$elf" ] || die "no decrypted EBOOT.elf (inventory/<game>/decrypt/EBOOT.elf, or --elf FILE)"
    title="$GAME_TITLE"
    if [ -z "$title" ]; then
        # The disc image next to the executable's game folder names the title (pspocket-boot --probe-disc).
        iso="$(find "$(dirname "$(dirname "$elf")")" -maxdepth 2 -type f -iname '*.iso' 2>/dev/null | head -1)"
        boot="$HOST_BUILD/runtime/loader/pspocket-boot"
        if [ -n "$iso" ] && [ -x "$boot" ]; then title="$("$boot" --probe-disc "$iso" 2>/dev/null | awk '{ print $1 }')"; fi
    fi
    dev="$(device_id)"
    require_unlocked "$dev"
    local dest="Documents/game/EBOOT.elf"
    if [ -n "$title" ]; then dest="Documents/game/$title/EBOOT.elf"; fi
    say "copying $elf to $dest on the iPhone"
    xcrun devicectl device copy to --device "$dev" --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
        --source "$elf" --destination "$dest" >&2 || die "copying the executable failed: is PSPocket installed?"
    if [ -d "$GAME_FLASH" ]; then
        find "$GAME_FLASH" -type f ! -name '*.sprx' | while IFS= read -r f; do
            rel="${f#"$GAME_FLASH"/}"
            say "copying dev_flash/$rel"
            xcrun devicectl device copy to --device "$dev" --domain-type appDataContainer \
                --domain-identifier "$BUNDLE_ID" --source "$f" --destination "Documents/dev_flash/$rel" >&2 \
                || die "copying dev_flash/$rel failed"
        done
    else
        warn "no $GAME_FLASH: the game's MP3 codec will be missing (inventory/firmware/tools/fw_unself.py --raw flashMP3.pic)"
    fi
    say "done: in PSPocket's Game tab, choose the game's .iso and press Boot"
}

cmd_testflight() {
    local hash version build_number archive log options status ipa
    local auth=()
    if [ -n "${PSPOCKET_ASC_KEY_ID:-}" ]; then
        [ -n "${PSPOCKET_ASC_ISSUER_ID:-}" ] || die "PSPOCKET_ASC_KEY_ID needs PSPOCKET_ASC_ISSUER_ID"
        local key="${PSPOCKET_ASC_KEY_PATH:-$HOME/.appstoreconnect/private_keys/AuthKey_$PSPOCKET_ASC_KEY_ID.p8}"
        [ -f "$key" ] || die "no API key at $key"
        auth=(-authenticationKeyPath "$key" -authenticationKeyID "$PSPOCKET_ASC_KEY_ID"
            -authenticationKeyIssuerID "$PSPOCKET_ASC_ISSUER_ID")
    fi
    archive="$IOS_BUILD/PSPocket.xcarchive"
    if [ "${UPLOAD_ONLY:-0}" != 1 ]; then
        cmd_gate
        cmd_gen
        hash="$(git_hash)"
        version="$(project_version)"
        build_number="$(git -C "$ROOT" rev-list --count HEAD)"
        log="$IOS_BUILD/xcodebuild-archive.log"
        mkdir -p "$IOS_BUILD"
        rm -rf "$archive"
        say "xcodebuild archive, pspocket $version build $build_number ($hash); full log: $log"
        set +e +o pipefail
        xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration Release -destination generic/platform=iOS \
            -derivedDataPath "$DERIVED" -archivePath "$archive" -allowProvisioningUpdates \
            PSPOCKET_GIT_HASH="$hash" PSPOCKET_VERSION="$version" CURRENT_PROJECT_VERSION="$build_number" archive 2>&1 \
            | tee "$log" \
            | grep --line-buffered -E '(^|: )(error|warning): |^\*\* ARCHIVE ' \
            | awk '!seen[$0]++ { print; fflush() }' >&2
        status=${PIPESTATUS[0]}
        set -e -o pipefail
        [ "$status" = 0 ] || die "xcodebuild archive failed (status $status); full log: $log"
    fi
    [ -d "$archive" ] || die "no archive at $archive (scripts/ios.sh testflight)"

    # Signed for App Store Connect into an .ipa here first (symbols stay here: the translated code makes them large,
    # and crash logs are symbolicated with the archive's dSYM), then uploaded. With an API key: the Apple Distribution
    # identity in this Mac's keychain and an App Store profile made through the API with the app's capabilities
    # (scripts/asc.mjs; such keys have no cloud signing). Without one: automatic signing with Xcode's Apple ID.
    local signing='<key>signingStyle</key><string>automatic</string>'
    if [ ${#auth[@]} -gt 0 ]; then
        local serial profile_name="PSPocket App Store"
        serial="$(security find-certificate -c "Apple Distribution" -p 2>/dev/null | openssl x509 -noout -serial \
            | sed 's/^serial=//')" || true
        [ -n "$serial" ] || die "no Apple Distribution certificate in the keychain"
        node "$ROOT/scripts/asc.mjs" profile "$BUNDLE_ID" "$profile_name" "$serial" \
            EXTENDED_VIRTUAL_ADDRESSING INCREASED_MEMORY_LIMIT >&2 || die "no App Store profile (scripts/asc.mjs)"
        signing="<key>signingStyle</key><string>manual</string>
    <key>signingCertificate</key><string>Apple Distribution</string>
    <key>provisioningProfiles</key><dict><key>$BUNDLE_ID</key><string>$profile_name</string></dict>"
    fi
    options="$IOS_BUILD/ExportOptions.plist"
    cat > "$options" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key><string>app-store-connect</string>
    <key>destination</key><string>export</string>
    <key>teamID</key><string>M7XJA39ZV6</string>
    $signing
    <key>uploadSymbols</key><false/>
    <key>manageAppVersionAndBuildNumber</key><false/>
</dict>
</plist>
PLIST
    log="$IOS_BUILD/xcodebuild-export.log"
    rm -rf "$IOS_BUILD/export"
    say "exporting for App Store Connect; full log: $log"
    set +e +o pipefail
    if [ ${#auth[@]} -gt 0 ]; then
        xcodebuild -exportArchive -archivePath "$archive" -exportOptionsPlist "$options" \
            -exportPath "$IOS_BUILD/export" 2>&1 | tee "$log" | grep --line-buffered -E 'error|EXPORT' >&2
    else
        xcodebuild -exportArchive -archivePath "$archive" -exportOptionsPlist "$options" \
            -exportPath "$IOS_BUILD/export" -allowProvisioningUpdates 2>&1 | tee "$log" \
            | grep --line-buffered -E 'error|EXPORT' >&2
    fi
    status=${PIPESTATUS[0]}
    set -e -o pipefail
    [ "$status" = 0 ] || die "export failed (status $status); full log: $log"
    ipa="$(find "$IOS_BUILD/export" -maxdepth 1 -name '*.ipa' | head -1)"
    [ -n "$ipa" ] || die "the export made no .ipa; full log: $log"

    log="$IOS_BUILD/upload.log"
    say "uploading $(basename "$ipa") ($(du -h "$ipa" | cut -f1)); full log: $log"
    set +e +o pipefail
    if [ ${#auth[@]} -gt 0 ]; then
        xcrun altool --upload-app -f "$ipa" -t ios --apiKey "$PSPOCKET_ASC_KEY_ID" --apiIssuer "$PSPOCKET_ASC_ISSUER_ID" \
            2>&1 | tee "$log" >&2
    else
        xcrun altool --upload-app -f "$ipa" -t ios 2>&1 | tee "$log" >&2
    fi
    status=${PIPESTATUS[0]}
    set -e -o pipefail
    # altool can print an error and still exit 0.
    if [ "$status" != 0 ] || grep -qiE '^ERROR|error:|Failed to upload' "$log"; then
        if grep -qi 'no suitable application records\|cannot be found\|app record' "$log"; then
            warn "App Store Connect has no app for $BUNDLE_ID yet: create it once at https://appstoreconnect.apple.com" \
                 "(Apps > + > New App, bundle ID $BUNDLE_ID), then run scripts/ios.sh testflight --upload-only."
        fi
        die "upload failed (status $status); full log: $log"
    fi
    say "uploaded: it shows in TestFlight once App Store Connect has processed it (minutes)"
    if [ ${#auth[@]} -gt 0 ]; then
        node "$ROOT/scripts/asc.mjs" testers "$BUNDLE_ID" >&2 || warn "the TestFlight testers were not set up"
    fi
}

# The game's inventory directory: $PSPOCKET_GAME_DIR, else the one whose lifted SPU programs a build would use.
game_inventory_dir() {
    if [ -n "${PSPOCKET_GAME_DIR:-}" ]; then echo "$PSPOCKET_GAME_DIR"; return; fi
    local found=""
    for d in "$ROOT"/inventory/*/spu-lifted; do
        if [ -f "$d/game_spu.c" ]; then found="$(dirname "$d")"; fi
    done
    [ -n "$found" ] || die "no inventory/*/spu-lifted: set PSPOCKET_GAME_DIR to the game's inventory directory"
    echo "$found"
}

# The game's title ID (BLES01807): $GAME/title-id, else the directory's name when it is one (translate-game.sh names
# them so).
game_title_id() {
    local game="$1" name
    if [ -s "$game/title-id" ]; then tr -d '[:space:]' < "$game/title-id"; return; fi
    name="$(basename "$game")"
    if [[ "$name" =~ ^[A-Z]{4}[0-9]{5}$ ]]; then echo "$name"; return; fi
    die "which game is $game? Write its title ID (as BLES01807) into $game/title-id"
}

cmd_pull_coverage() {
    local dev game title tmp n_spu=0 n_seeds=0 n_shaders=0 n_pipes=0 stamp
    game="$(game_inventory_dir)"
    title="$(game_title_id "$game")"
    tmp="$(mktemp -d)"
    if [ -n "${PSPOCKET_PULL_FROM:-}" ]; then
        # An app data container on this Mac instead (a simulator's: xcrun simctl get_app_container ... data; tests).
        say "pulling the coverage in $PSPOCKET_PULL_FROM into $game"
        cp -R "$PSPOCKET_PULL_FROM/Library/Application Support/pspocket-coverage" "$tmp/coverage" \
            || die "no Library/Application Support/pspocket-coverage in $PSPOCKET_PULL_FROM"
        if [ -d "$PSPOCKET_PULL_FROM/Library/Caches/pspocket-shaders" ]; then
            cp -R "$PSPOCKET_PULL_FROM/Library/Caches/pspocket-shaders" "$tmp/shaders"
        fi
    else
        dev="$(device_id)"
        say "pulling the phone's coverage into $game"
        xcrun devicectl device copy from --device "$dev" --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
            --source "Library/Application Support/pspocket-coverage" --destination "$tmp/coverage" --quiet >&2 \
            || die "nothing to pull: the app has run no game since it records coverage (D-117), or the phone is locked"
        xcrun devicectl device copy from --device "$dev" --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
            --source "Library/Caches/pspocket-shaders" --destination "$tmp/shaders" --quiet >&2 \
            || warn "no shader cache on the phone"
    fi
    # Each game keeps its own folders (D-118): only this game's are taken. Builds up to 131 kept one folder for every
    # game the phone ran, which may hold another game's programs: taken only on request.
    for kind in coverage shaders; do
        if [ -d "$tmp/$kind/$title" ]; then
            mv "$tmp/$kind" "$tmp/$kind-all"
            mv "$tmp/$kind-all/$title" "$tmp/$kind"
        elif [ -d "$tmp/$kind/spu" ] || [ -d "$tmp/$kind/programs" ]; then
            if [ "${PSPOCKET_PULL_SHARED:-0}" = 1 ]; then
                warn "$kind: the phone's folder from before D-118, shared by every game it ran (PSPOCKET_PULL_SHARED=1)"
            else
                warn "$kind: only the folder from before D-118 (build 131 or earlier), shared by every game the phone" \
                     "ran: skipped (PSPOCKET_PULL_SHARED=1 takes it)"
                rm -rf "$tmp/$kind"
            fi
        else
            rm -rf "$tmp/$kind"
        fi
    done
    say "game $title"
    # SPU programs: an image is the same wherever it was saved; its seed lines (where execution entered it) add up.
    mkdir -p "$game/spu"
    for f in "$tmp"/coverage/spu/spu_*; do
        [ -e "$f" ] || continue
        local name dest
        name="$(basename "$f")"
        dest="$game/spu/$name"
        if [ ! -e "$dest" ]; then
            cp "$f" "$dest"
            case "$name" in *.bin) n_spu=$((n_spu + 1)) ;; esac
        elif [ "${name##*.}" = txt ]; then
            while IFS= read -r line; do
                case "$line" in seed\ *) grep -qxF "$line" "$dest" || { printf '%s\n' "$line" >> "$dest"; n_seeds=$((n_seeds + 1)); } ;; esac
            done < "$f"
        fi
    done
    # The addresses lifted code did not reach: a run log, which scripts/lift-spu.sh and translate-game.sh read.
    stamp="$(date +%Y%m%d-%H%M%S)"
    if [ -s "$tmp/coverage/spu-misses.log" ]; then
        cp "$tmp/coverage/spu-misses.log" "$game/run-phone-$stamp.log"
        n_seeds=$((n_seeds + $(grep -c 'no lifted code at' "$tmp/coverage/spu-misses.log" || true)))
    fi
    # Shaders: new programs, new pipeline lines.
    if [ -d "$tmp/shaders/programs" ]; then
        mkdir -p "$game/shaders/programs"
        for f in "$tmp"/shaders/programs/*.txt; do
            [ -e "$f" ] || continue
            [ -e "$game/shaders/programs/$(basename "$f")" ] && continue
            cp "$f" "$game/shaders/programs/"
            n_shaders=$((n_shaders + 1))
        done
        if [ -f "$tmp/shaders/pipelines.txt" ]; then
            touch "$game/shaders/pipelines.txt"
            while IFS= read -r line; do
                grep -qxF "$line" "$game/shaders/pipelines.txt" || { printf '%s\n' "$line" >> "$game/shaders/pipelines.txt"; n_pipes=$((n_pipes + 1)); }
            done < "$tmp/shaders/pipelines.txt"
        fi
    fi
    rm -rf "$tmp"
    say "new: $n_spu SPU programs, $n_seeds entry points, $n_shaders shader programs, $n_pipes pipelines"
    say "next: scripts/lift-spu.sh $game/spu $game/spu-lifted $game/run-*.log, then scripts/ios.sh build or testflight"
}

case "$COMMAND" in
    gen) cmd_gen ;;
    build) cmd_build ;;
    install) cmd_install ;;
    run) cmd_run; exit "$RUN_STATUS" ;;
    report) cmd_report ;;
    pull-iso) cmd_pull_iso ;;
    push-game) cmd_push_game ;;
    testflight) cmd_testflight ;;
    pull-coverage) cmd_pull_coverage ;;
    gate) cmd_gate ;;
    pull-log) cmd_pull_log ;;
    session) cmd_session ;;
    push-signing-identity) cmd_push_signing_identity ;;
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
