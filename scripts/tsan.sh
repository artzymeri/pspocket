#!/usr/bin/env bash
# The ThreadSanitizer sweep of the threaded runtime (D-571, D-585): configures and builds build/tsan
# (-DPSPOCKET_TSAN=ON), runs each group's tests under ThreadSanitizer with scripts/tsan.supp and prints one line per
# group: its reports (and their distinct places) and how its tests ended. A race does not show in an ordinary build,
# so this is the regression check for the threaded code. Works with macOS's bash 3.2.
#
#   scripts/tsan.sh [GROUP...] [--no-build] [--raw] [-DVAR=VALUE...]
#
#   GROUP       core, ppu, spu, lv2, rsx, hle, boot; all of them when none is named. hle runs test_hle once per test
#               file (Catch2's file tags), so a file's crash or reports stay its own.
#   --no-build  run build/tsan as it is
#   --raw       without the suppressions: every report, the guest-level ones too
#   -D...       passed to cmake when it configures build/tsan (the first time, e.g. -DPSPOCKET_RPCS3_DIR=DIR where
#               external/ is not beside the sources)
#
# Logs: build/tsan-logs/<group>/<test>.log, and <group>/reports.txt with the first full report of each distinct place
# (scripts/tsan_report.py). PSPOCKET_TSAN_JOBS sets the build's parallel jobs (default: ninja's own).
# Exit status: 0 when no group has a report or a failed test, 1 otherwise. A test that fails on this host without
# ThreadSanitizer too (one that needs FFmpeg, say) fails here as well; its log says why.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"
BUILD=build/tsan
LOGS=build/tsan-logs
ALL_GROUPS="core ppu spu lv2 rsx hle boot"

groups=""
build=1
raw=0
cmake_args=()
for a in "$@"; do
    case "$a" in
        --no-build) build=0 ;;
        --raw) raw=1 ;;
        -D*) cmake_args+=("$a") ;;
        core|ppu|spu|lv2|rsx|hle|boot) groups="$groups $a" ;;
        *)
            echo "usage: scripts/tsan.sh [core|ppu|spu|lv2|rsx|hle|boot ...] [--no-build] [--raw] [-DVAR=VALUE ...]" >&2
            exit 64
            ;;
    esac
done
[ -n "$groups" ] || groups="$ALL_GROUPS"

targets_of() {
    case "$1" in
        core) echo "test_core test_reservation" ;;
        ppu) echo "test_ppu test_ppu_programs" ;;
        spu) echo "test_spu" ;;
        lv2) echo "test_lv2" ;;
        rsx) echo "test_rsx" ;;
        hle) echo "test_hle" ;;
        boot) echo "pspocket-boot boot_programs boot_report_programs" ;;
    esac
}

if [ "$build" = 1 ]; then
    if [ "$(uname)" = Linux ]; then
        export CC="${CC:-clang}" CXX="${CXX:-clang++}" # clang only (D-026)
    fi
    echo "==> configure and build $BUILD"
    if ! cmake -S . -B "$BUILD" -G Ninja -DPSPOCKET_TSAN=ON -DPSPOCKET_WERROR=ON ${cmake_args[@]+"${cmake_args[@]}"} \
        > "$BUILD.configure.log" 2>&1; then
        tail -20 "$BUILD.configure.log"
        echo "tsan.sh: configure failed ($BUILD.configure.log)" >&2
        exit 1
    fi
    targets=""
    for g in $groups; do targets="$targets $(targets_of "$g")"; done
    jobs=()
    [ -n "${PSPOCKET_TSAN_JOBS:-}" ] && jobs=(-j "$PSPOCKET_TSAN_JOBS")
    # shellcheck disable=SC2086 # the target list splits on spaces
    if ! ninja -C "$BUILD" ${jobs[@]+"${jobs[@]}"} $targets > "$BUILD.build.log" 2>&1; then
        grep -E "error|FAILED" "$BUILD.build.log" | head -20
        echo "tsan.sh: build failed ($BUILD.build.log)" >&2
        exit 1
    fi
fi

# symbolize_inline_frames=0: a frame is the function an access is made in, with what is inlined into it (the
# big-endian accessors), which is what scripts/tsan.supp's race_top entries name. exitcode=0: a test's exit status is
# its own, the reports are counted from its log. second_deadlock_stack: both stacks of a lock-order inversion.
TSAN_OPTIONS="halt_on_error=0 exitcode=0 second_deadlock_stack=1 symbolize_inline_frames=0"
[ "$raw" = 1 ] || TSAN_OPTIONS="$TSAN_OPTIONS suppressions=$ROOT/scripts/tsan.supp"
export TSAN_OPTIONS

status=0

# run_test GROUP NAME COMMAND...: one test binary's run, its log at $LOGS/GROUP/NAME.log; sets result to how it ended.
run_test() {
    local group="$1" name="$2"
    shift 2
    local log="$LOGS/$group/$name.log"
    "$@" > "$log" 2>&1
    local rc=$?
    # Catch2: 0 passed, 4 every test skipped (they need the user's firmware), 2 no test ran.
    case "$rc" in
        0) result=passed ;;
        4) result=skipped ;;
        2) result="no tests" ;;
        *) result="FAILED (exit $rc)" ;;
    esac
}

# A group's line: its reports and distinct places, its tests' results, then each place (when there are reports).
summarize() {
    local group="$1" outcome="$2"
    local logs
    logs=$(find "$LOGS/$group" -name '*.log' | sort)
    local count distinct
    # shellcheck disable=SC2086
    count=$(cat $logs /dev/null | grep -c '^WARNING: ThreadSanitizer' || true)
    if [ "$count" = 0 ]; then
        printf '%-5s 0 reports; %s\n' "$group" "$outcome"
        rm -f "$LOGS/$group/reports.txt"
        return
    fi
    # shellcheck disable=SC2086
    python3 "$ROOT/scripts/tsan_report.py" $logs --examples "$LOGS/$group/reports.txt" > "$LOGS/$group/places.txt"
    distinct=$(grep -cE '^ *[0-9]+  ' "$LOGS/$group/places.txt" || true)
    printf '%-5s %s reports, %s distinct; %s\n' "$group" "$count" "$distinct" "$outcome"
    sed -n '/^ *[0-9][0-9]*  /,$p' "$LOGS/$group/places.txt" | sed 's/^/      /'
    status=1
}

for g in $groups; do
    rm -rf "${LOGS:?}/$g"
    mkdir -p "$LOGS/$g"
    case "$g" in
        hle)
            passed=0 skipped=0 none=0 failed=""
            for src in tests/hle/test_*.cpp; do
                f=$(basename "$src" .cpp)
                run_test hle "$f" "$BUILD/tests/hle/test_hle" -# "[#$f]"
                case "$result" in
                    passed) passed=$((passed + 1)) ;;
                    skipped) skipped=$((skipped + 1)) ;;
                    "no tests") none=$((none + 1)) ;;
                    *) failed="$failed $f" ;;
                esac
            done
            outcome="test_hle by file: $passed passed, $skipped skipped, $none without a test here"
            if [ -n "$failed" ]; then
                outcome="$outcome; FAILED:$failed"
                status=1
            fi
            summarize hle "$outcome"
            ;;
        boot)
            # pspocket-boot's own threads and its reports, through ctest for its checks of their output
            # (tests/hle/CMakeLists.txt). ctest -V puts the test's number before each line of its output.
            run_test boot ctest.raw ctest --test-dir "$BUILD" -R '^pspocket_boot_' -V
            sed 's/^[0-9][0-9]*: //' "$LOGS/boot/ctest.raw.log" > "$LOGS/boot/pspocket_boot.log"
            rm -f "$LOGS/boot/ctest.raw.log"
            [ "$result" = passed ] || status=1
            summarize boot "ctest pspocket_boot_*: $result"
            ;;
        *)
            outcome=""
            for t in $(targets_of "$g"); do
                bin=$(find "$BUILD/tests/$g" -maxdepth 1 -name "$t" -type f | head -1)
                if [ -z "$bin" ]; then
                    outcome="$outcome $t: not built;"
                    status=1
                    continue
                fi
                run_test "$g" "$t" "$bin"
                case "$result" in passed|skipped) ;; *) status=1 ;; esac
                outcome="$outcome $t $result;"
            done
            outcome="${outcome# }"
            summarize "$g" "${outcome%;}"
            ;;
    esac
done
exit "$status"
