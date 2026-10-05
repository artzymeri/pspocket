#!/bin/zsh
# The gate every change passes before it reaches the phone (D-149, plan "only forward"):
#
#   scripts/gate.sh quick    builds the tests and runs them (ctest; about 15 s once built)
#   scripts/gate.sh game     GTA V's prologue walk with --summary, checked against games/gta5/gate.json (2.5 min)
#   scripts/gate.sh [all]    both
#   scripts/gate.sh spu      spu-check over the game's SPU programs (2 trials, 40 entries each: about 27 s a program,
#                            two hours for GTA V's): for changes to the SPU translation, run before the gate
#
# Options: --accept-messages (game) adds the run's new warning and error shapes to the baseline instead of failing.
# --library FILE (game) runs the walk with the game's code from a game library (pspocket-prepare, D-152) instead of the
# translation built into pspocket-boot; --builtin-spu adds the built-in SPU programs, registered first, as the app does
# (D-155).
# The runs stay in inventory/gate/ (game output: never committed, CLAUDE.md rule 1). Exit 0: passed; 1: failed;
# 2: the game step cannot run here (no game inputs in inventory/).
set -u
cd "$(dirname "$0")/.."
mode=all accept="" library=()
while (( $# )); do
  case $1 in
    quick|game|all|spu) mode=$1 ;;
    --accept-messages) accept=--accept-messages ;;
    --library) shift; [[ -f ${1:-} ]] || { echo "gate: no game library ${1:-}" >&2; exit 64; }; library+=(--game-library $1) ;;
    --builtin-spu) library+=(--builtin-spu) ;;
    *) echo "usage: scripts/gate.sh [quick|game|all|spu] [--accept-messages] [--library FILE [--builtin-spu]]" >&2; exit 64 ;;
  esac
  shift
done
build=${PSPOCKET_GATE_BUILD:-build/host}
stamp=$(date +%Y%m%d-%H%M%S)-$(git rev-parse --short HEAD)$(git diff --quiet HEAD -- . ':!docs' 2>/dev/null || echo +)
out=inventory/gate/$stamp
mkdir -p $out

if [[ $mode == quick || $mode == all ]]; then
  echo "==> gate quick: build, then ctest"
  if ! ninja -C $build > $out/build.log 2>&1; then
    tail -30 $out/build.log; echo "gate: FAIL (build, $out/build.log)"; exit 1
  fi
  if ! ctest --test-dir $build -j 8 --timeout 300 --output-on-failure > $out/ctest.log 2>&1; then
    grep -E "Failed|FAILED|\*\*\*" $out/ctest.log | head -20; echo "gate: FAIL (tests, $out/ctest.log)"; exit 1
  fi
  tail -3 $out/ctest.log | head -1
fi

if [[ $mode == spu ]]; then
  spu=inventory/gta5/spu
  [[ -x $build/runtime/loader/spu-check && -d $spu ]] || { echo "gate: spu-check needs $spu and a game SPU build"; exit 2; }
  echo "==> gate spu: spu-check $spu (2 trials, 40 entries a program)"
  if ! $build/runtime/loader/spu-check $spu --trials 2 --entries 40 > $out/spu-check.log 2>&1; then
    grep -v " 0 differences" $out/spu-check.log | head -20
    echo "gate: FAIL (translated SPU code differs from the interpreter, $out/spu-check.log)"; exit 1
  fi
  echo "    $(grep -c ' 0 differences' $out/spu-check.log) programs, no difference"
fi

if [[ $mode == game || $mode == all ]]; then
  elf=inventory/gta5/decrypt/EBOOT.elf iso=inventory/gta5/disc.iso hdd0=inventory/gta5/hdd0
  flash=inventory/firmware/dev_flash shaders=inventory/gta5/shaders
  for f in $elf $iso $hdd0 $flash $shaders; do
    [[ -e $f ]] || { echo "gate: the game step needs $f"; exit 2; }
  done
  gate=games/gta5/gate.json
  timeout_s=$(python3 -c "import json; print(json.load(open('$gate'))['timeout_s'])")
  frame_every=$(python3 -c "import json; print(json.load(open('$gate'))['frame_every'])")
  frame_from=$(python3 -c "import json; print(json.load(open('$gate'))['frame_from'])")
  echo "==> gate game: GTA V prologue walk, ${timeout_s} s${library:+, code from ${library[*]}}"
  $build/runtime/loader/pspocket-boot $elf --gpu metal --timeout $timeout_s --hdd0 $hdd0 --bdvd $iso --flash $flash \
    --shader-cache $shaders --press-file games/gta5/walk-prologue.txt --frame-jumps --frame-every $frame_every \
    --frame-from $frame_from --summary $out/summary.json $library > $out/run.log 2>&1
  rc=$?
  echo "    pspocket-boot exit $rc ($out/run.log)"
  if [[ ! -f $out/summary.json ]]; then
    grep -E "\[fatal\]|malformed|desync" $out/run.log | head -5; echo "gate: FAIL (no summary: the run died)"; exit 1
  fi
  python3 scripts/gate_check.py $out/summary.json $gate --messages inventory/gate/baseline-messages.json $accept \
    | tee $out/gate.txt
  [[ ${pipestatus[1]} == 0 ]] || exit 1
fi
echo "gate: PASS ($out)"
