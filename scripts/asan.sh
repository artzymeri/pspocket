#!/usr/bin/env bash
# The memory safety sweep (D-604): configures and builds build/asan (-DPSPOCKET_SANITIZE=ON: AddressSanitizer, its
# LeakSanitizer and UBSan), runs each group's tests with every check reporting, and prints one line per group: its
# reports (and their distinct places) and how its tests ended. The parser fuzzing (D-603, the "[fuzz]" tests) runs in
# its groups too. Works with macOS's bash 3.2 (LeakSanitizer is Linux's: on the Mac leaks are not looked for).
#
#   scripts/asan.sh [GROUP...] [--no-build] [--no-fuzz] [-DVAR=VALUE...]
#
#   GROUP       core, common, ppu, spu, lv2, rsx, hle, disc, game, codesign, codegen, boot; all of them when none is
#               named. hle runs test_hle once per test file (Catch2's file tags), so a file's crash or reports stay its
#               own.
#   --no-build  run build/asan as it is
#   --no-fuzz   leave the "[fuzz]" tests out (the ordinary tests alone)
#   -D...       passed to cmake when it configures build/asan (the first time, e.g. -DPSPOCKET_RPCS3_DIR=DIR where
#               external/ is not beside the sources)
#
# Logs: build/asan-logs/<group>/<test>.log, and <group>/reports.txt with the first full report of each distinct place
# (scripts/asan_report.py). PSPOCKET_ASAN_JOBS sets the build's parallel jobs (default: ninja's own). The suppressions
# are scripts/asan.supp (LeakSanitizer's) and scripts/ubsan.supp, each line with its reason.
# Exit status: 0 when no group has a report or a failed test, 1 otherwise. A test that fails on this host without the
# sanitizers too fails here as well (its log says why).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"
BUILD=build/asan
LOGS=build/asan-logs
ALL_GROUPS="core common ppu spu lv2 rsx hle disc game codesign codegen boot"

groups=""
build=1
fuzz=1
cmake_args=()
for a in "$@"; do
    case "$a" in
        --no-build) build=0 ;;
        --no-fuzz) fuzz=0 ;;
        -D*) cmake_args+=("$a") ;;
        core|common|ppu|spu|lv2|rsx|hle|disc|game|codesign|codegen|boot) groups="$groups $a" ;;
        *)
            echo "usage: scripts/asan.sh [GROUP ...] [--no-build] [--no-fuzz] [-DVAR=VALUE ...]" >&2
            exit 64
            ;;
    esac
done
[ -n "$groups" ] || groups="$ALL_GROUPS"

# The test binaries of a group, as tests/<dir>/<name>.
targets_of() {
    case "$1" in
        core) echo "core/test_core core/test_reservation" ;;
        common) echo "common/test_recomp_common" ;;
        ppu) echo "ppu/test_ppu ppu/test_ppu_fuzz ppu/test_ppu_programs" ;;
        spu) echo "spu/test_spu spu/test_spu_fuzz" ;;
        lv2) echo "lv2/test_lv2" ;;
        rsx) echo "rsx/test_rsx" ;;
        hle) echo "hle/test_hle" ;;
        disc) echo "disc/test_disc" ;;
        game) echo "game/test_game_library game/test_system_library" ;;
        codesign) echo "codesign/test_codesign" ;;
        codegen) echo "codegen/test_codegen" ;;
        boot) echo "" ;;
    esac
}

if [ "$build" = 1 ]; then
    if [ "$(uname)" = Linux ]; then
        export CC="${CC:-clang}" CXX="${CXX:-clang++}" # clang only (D-026)
    fi
    echo "==> configure and build $BUILD"
    if ! cmake -S . -B "$BUILD" -G Ninja -DPSPOCKET_SANITIZE=ON -DPSPOCKET_WERROR=ON ${cmake_args[@]+"${cmake_args[@]}"} \
        > "$BUILD.configure.log" 2>&1; then
        tail -20 "$BUILD.configure.log"
        echo "asan.sh: configure failed ($BUILD.configure.log)" >&2
        exit 1
    fi
    jobs=()
    [ -n "${PSPOCKET_ASAN_JOBS:-}" ] && jobs=(-j "$PSPOCKET_ASAN_JOBS")
    if ! ninja -C "$BUILD" ${jobs[@]+"${jobs[@]}"} > "$BUILD.build.log" 2>&1; then
        grep -E "error|FAILED" "$BUILD.build.log" | head -20
        echo "asan.sh: build failed ($BUILD.build.log)" >&2
        exit 1
    fi
fi

# Every check on and reporting: ASan's (stack use after return, initialization order, string functions), its leak
# check at exit (Linux), UBSan's with stacks and without stopping (each place reports once per process). An ASan error
# ends its process (the build does not use -fsanitize-recover), so a group runs its tests apart (hle by file).
leaks=1
[ "$(uname)" = Linux ] || leaks=0
ASAN_OPTIONS="detect_leaks=$leaks detect_stack_use_after_return=1 check_initialization_order=1 strict_init_order=1"
ASAN_OPTIONS="$ASAN_OPTIONS strict_string_checks=1 detect_container_overflow=1 print_stacktrace=1 symbolize=1"
ASAN_OPTIONS="$ASAN_OPTIONS fast_unwind_on_malloc=0 malloc_context_size=30 handle_abort=1"
UBSAN_OPTIONS="print_stacktrace=1 halt_on_error=0 report_error_type=1 suppressions=$ROOT/scripts/ubsan.supp"
LSAN_OPTIONS="suppressions=$ROOT/scripts/asan.supp print_suppressions=0"
export ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS

filter=()
[ "$fuzz" = 1 ] || filter=("~[fuzz]")

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

summarize() {
    local group="$1" outcome="$2"
    local logs
    logs=$(find "$LOGS/$group" -name '*.log' | sort)
    # shellcheck disable=SC2086
    python3 "$ROOT/scripts/asan_report.py" $logs --examples "$LOGS/$group/reports.txt" > "$LOGS/$group/places.txt"
    local count distinct
    count=$(sed -n 's/^.*: \([0-9][0-9]*\) reports, .*$/\1/p' "$LOGS/$group/places.txt" | awk '{s += $1} END {print s + 0}')
    distinct=$(grep -cE '^ *[0-9]+  ' "$LOGS/$group/places.txt" || true)
    if [ "$count" = 0 ]; then
        printf '%-8s 0 reports; %s\n' "$group" "$outcome"
        rm -f "$LOGS/$group/reports.txt"
        return
    fi
    printf '%-8s %s reports, %s distinct; %s\n' "$group" "$count" "$distinct" "$outcome"
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
                if [ "$fuzz" = 0 ]; then
                    run_test hle "$f" "$BUILD/tests/hle/test_hle" -# "[#$f]~[fuzz]"
                else
                    run_test hle "$f" "$BUILD/tests/hle/test_hle" -# "[#$f]"
                fi
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
            # pspocket-boot's fault and thread reports, through ctest for its checks of their output.
            run_test boot ctest.raw ctest --test-dir "$BUILD" -R '^pspocket_boot_' -V
            sed 's/^[0-9][0-9]*: //' "$LOGS/boot/ctest.raw.log" > "$LOGS/boot/pspocket_boot.log"
            rm -f "$LOGS/boot/ctest.raw.log"
            [ "$result" = passed ] || status=1
            summarize boot "ctest pspocket_boot_*: $result"
            ;;
        *)
            outcome=""
            for t in $(targets_of "$g"); do
                bin="$BUILD/tests/$t"
                name=$(basename "$t")
                if [ ! -x "$bin" ]; then
                    outcome="$outcome $name: not built;"
                    continue
                fi
                run_test "$g" "$name" "$bin" ${filter[@]+"${filter[@]}"}
                case "$result" in passed|skipped) ;; *) status=1 ;; esac
                outcome="$outcome $name $result;"
            done
            outcome="${outcome# }"
            summarize "$g" "${outcome%;}"
            ;;
    esac
done
exit "$status"
