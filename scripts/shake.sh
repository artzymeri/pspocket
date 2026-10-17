#!/bin/zsh
# Shake mode's runs (D-1045, runtime/core chaos.hpp): a game's scripted walk under seeded perturbations, many seeds,
# each run's summary sorted into buckets (scripts/triage.py), so races show on the Mac in a few runs.
#
#   scripts/shake.sh gta5|ac [--seeds N] [--seed S] [--from S] [--spec SPEC] [--timeout S] [--keep]
#   scripts/shake.sh gta5|ac --rerun S [--times K]        the same seed K times (3): how often it fails
#   scripts/shake.sh gta5|ac --ddmin S [--times K]        delta debugging: the fewest points that still fail with S
#   scripts/shake.sh ac --case training ...                Assassin's Creed's training from the installed game and save
#   scripts/shake.sh ac --guard [--times K]               the waker rule's guard (D-1051): K boots (8) to 8 s, each
#                                                         waker delayed up to 2 ms after half its wakes (S3), must all
#                                                         pass; --guard-control does the same with the rule off and
#                                                         passes when a boot crashes (the guard can see the race)
#
# SPEC is PSPOCKET_SHAKE's text without the seed: "legal" (the default: every PS3-legal point), "all", "adversarial",
# or points ("S1;S3=200000:1000;S8"). A run fails when triage finds a fault, a fatal error, a deadlock, a freeze, a
# livelock or no summary; the table prints each run's seed, ending, buckets and median fps. --ddmin starts from SPEC's
# points and keeps halving them while a run with the half still fails (each test up to K runs: a race need not show
# every time), then tries each point alone out: what is left is a set where every point matters.
#
# gta5: the gate's prologue walk (games/gta5/walk-prologue.txt, 128 s). ac: Assassin's Creed (BLES00158) from a fresh
# copy of its hdd0, its game folder and save emptied (a first install), through the install into the garden
# (games/ac1/walk-garden.txt, 330 s), with the game library PSPOCKET_AC_LIBRARY (default <data>/bench-library/
# game.dylib, data PSPOCKET_AC_DATA, default inventory/BLES00158) and the installed firmware's dev_flash; each run's
# hdd0 copy (1.3 GB after the install) is removed after it unless --keep, and none is made under 11 GB free.
# Runs go to inventory/shake/<stamp>/ (game output: never committed, CLAUDE.md rule 1). Exit 0: no run failed.
set -u
cd "$(dirname "$0")/.."
game=${1:-}
[[ $game == gta5 || $game == ac ]] || { echo "usage: scripts/shake.sh gta5|ac [options] (see the top of the file)" >&2; exit 64; }
shift
seeds=4 seed_from=1 one_seed="" spec=legal timeout_s="" keep=0 rerun="" ddmin="" times=3 guard="" ac_case=install
while (( $# )); do
  case $1 in
    --seeds) shift; seeds=$1 ;;
    --from) shift; seed_from=$1 ;;
    --seed) shift; one_seed=$1 ;;
    --spec) shift; spec=$1 ;;
    --timeout) shift; timeout_s=$1 ;;
    --keep) keep=1 ;;
    --rerun) shift; rerun=$1 ;;
    --ddmin) shift; ddmin=$1 ;;
    --times) shift; times=$1 ;;
    --guard) guard=on ;;
    --case) shift; ac_case=$1 ;;
    --guard-control) guard=control ;;
    *) echo "shake: unknown option $1" >&2; exit 64 ;;
  esac
  shift
done
build=${PSPOCKET_SHAKE_BUILD:-build/host}
boot=$build/runtime/loader/pspocket-boot
[[ -x $boot ]] || { echo "shake: no $boot" >&2; exit 2; }
out=inventory/shake/$(date +%Y%m%d-%H%M%S)-$game
mkdir -p $out

# One run: run_one DIR SEED SPEC. Writes DIR/{run.log,summary.json,shake.log,triage.json}; returns 1 when it failed.
run_one() {
  setopt localoptions nullglob
  local dir=$1 seed=$2 sp=$3
  mkdir -p $dir
  local -a cmd
  if [[ $game == gta5 ]]; then
    cmd=($boot inventory/gta5/decrypt/EBOOT.elf --gpu metal --timeout ${timeout_s:-128} --hdd0 inventory/gta5/hdd0
      --bdvd inventory/gta5/disc.iso --flash inventory/firmware/dev_flash --shader-cache inventory/gta5/shaders
      --press-file games/gta5/walk-prologue.txt)
  else
    local data=${PSPOCKET_AC_DATA:-inventory/BLES00158}
    local lib=${PSPOCKET_AC_LIBRARY:-$data/bench-library/game.dylib}
    [[ -f $lib ]] || { echo "shake: no AC game library $lib" >&2; return 2; }
    local free=$(df -g . | awk 'NR==2{print $4}')
    (( free >= 11 )) || { echo "shake: ${free} GB free: no hdd0 copy under 11 GB" >&2; return 2; }
    mkdir -p $dir/hdd
    cp -c -R $data/hdd0 $dir/hdd/hdd0 || return 2
    local press=games/ac1/walk-garden.txt t=330
    if [[ $ac_case == training ]]; then
      # The bench's training: the installed game and its save, the Animus training (walk-training.txt).
      press=games/ac1/walk-training.txt t=570
    else
      # A first install, as the bench's install-garden (games/ac1/bench/cases.json): the game's folder and save emptied.
      rm -rf $dir/hdd/hdd0/game/BLES00158/USRDIR/* $dir/hdd/hdd0/home/00000001/savedata/BLES00158*
    fi
    cmd=($boot $data/decrypt/EBOOT.elf --gpu metal --timeout ${timeout_s:-$t} --hdd0 $dir/hdd/hdd0
      --bdvd $data/disc.iso --flash inventory/firmware/installed/dev_flash --game-library $lib
      --press-file $press)
  fi
  PSPOCKET_SHAKE="seed=$seed;$sp" PSPOCKET_SHAKE_LOG=$dir/shake.log $cmd --summary $dir/summary.json \
    --stop-on-stall --livelock 60 --max-footprint 6 > $dir/run.log 2>&1
  echo $? > $dir/exit
  [[ $game == ac && $keep == 0 ]] && rm -rf $dir/hdd
  python3 scripts/triage.py --summary $dir/summary.json $dir/run.log --json > $dir/triage.json 2>/dev/null
  python3 - $dir <<'PY'
import json, sys, os
d = sys.argv[1]
t = json.load(open(os.path.join(d, "triage.json")))
try:
    s = json.load(open(os.path.join(d, "summary.json")))
except Exception:
    s = {}
fps = sorted(s.get("fps", [])) or [0]
bad = [b["bucket"] for b in t if b["severity"] <= 2 and b["bucket"] != "unimplemented"]
print(f'{os.path.basename(d):<14} {s.get("exit", "?"):<9} {fps[len(fps)//2]:5.1f} fps  '
      f'{",".join(b["bucket"] for b in t):<40} {"FAIL" if bad else "ok"}')
open(os.path.join(d, "failed"), "w").write("1" if bad else "0")
PY
  [[ $(cat $dir/failed 2>/dev/null) == 0 ]]
}

# Whether SPEC fails with SEED within K runs (stops at the first failure).
fails() {
  local tag=$1 seed=$2 sp=$3 k
  for (( k = 1; k <= times; k++ )); do
    run_one $out/$tag-$k $seed "$sp" || return 0
  done
  return 1
}

echo "==> shake $game: spec \"$spec\" ($out)"
# The waker rule's guard (D-1051): D-999's race recipe (the waker held up to 2 ms after half its wakes) on Assassin's
# Creed's first boot, where the thread registry crashed 0.8 s in when the woken thread overtook its waker (D-1050: with
# S16 a quarter of the boots crash). With the rule (D-999, D-1041) none may; --guard-control turns the rule off
# (PSPOCKET_LV2_SENDER_FIRST=0) and expects a crash, which shows the guard still reaches the race.
if [[ -n $guard ]]; then
  [[ $game == ac ]] || { echo "shake: --guard is Assassin's Creed's" >&2; exit 64; }
  [[ $times == 3 ]] && times=8
  timeout_s=${timeout_s:-8}
  [[ $guard == control ]] && export PSPOCKET_LV2_SENDER_FIRST=0
  failed=0
  for (( k = 1; k <= times; k++ )); do run_one $out/guard-$k $((100 + k)) "S3=500000:2000" || (( failed++ )); done
  if [[ $guard == control ]]; then
    echo "waker guard control (rule off): $failed of $times boots crashed$( (( failed )) && echo ': the guard sees the race' || echo ': NOT reproduced, the guard may be blind')"
    exit $(( failed == 0 ))
  fi
  echo "waker guard: $failed of $times boots failed$( (( failed )) && echo ': the waker rule regressed (D-999, D-1041)' )"
  exit $(( failed > 0 ))
fi

if [[ -n $rerun ]]; then
  failed=0
  for (( k = 1; k <= times; k++ )); do run_one $out/seed$rerun-$k $rerun "$spec" || (( failed++ )); done
  echo "seed $rerun: $failed of $times runs failed"
  exit $(( failed > 0 ))
fi

if [[ -n $ddmin ]]; then
  # The points: SPEC's own, or every point of its family.
  points=(${(s:;:)spec})
  if [[ $spec == legal || $spec == all || $spec == adversarial ]]; then
    points=($(python3 - $spec <<'PY'
import sys
legal = ["S1","S2","S3","S4","S5","S6","S8","S9","S10","S11","S12","S13","S14"]
adv = ["S7","S15","S16"]
f = sys.argv[1]
print(" ".join(legal if f == "legal" else adv if f == "adversarial" else legal + adv))
PY
))
  fi
  echo "ddmin over ${#points} points: ${points[*]}"
  fails base $ddmin "${(j:;:)points}" || { echo "seed $ddmin does not fail with all of them in $times runs"; exit 0; }
  step=0
  n=2
  while (( ${#points} >= 2 )); do
    (( size = (${#points} + n - 1) / n ))
    reduced=0
    for (( i = 0; i < n; i++ )); do
      chunk=(${points[$(( i * size + 1 )),$(( (i + 1) * size ))]})
      (( ${#chunk} )) || continue
      (( step++ ))
      if fails step$step $ddmin "${(j:;:)chunk}"; then
        points=($chunk); n=2; reduced=1; echo "  still fails with ${points[*]}"; break
      fi
      rest=(${points:|chunk})
      (( ${#rest} )) || continue
      (( step++ ))
      if fails step$step $ddmin "${(j:;:)rest}"; then
        points=($rest); (( n = n > 2 ? n - 1 : 2 )); reduced=1; echo "  still fails with ${points[*]}"; break
      fi
    done
    if (( ! reduced )); then
      (( n >= ${#points} )) && break
      (( n = n * 2 > ${#points} ? ${#points} : n * 2 ))
    fi
  done
  echo "seed $ddmin: the smallest failing set found: ${points[*]} (PSPOCKET_SHAKE=\"seed=$ddmin;${(j:;:)points}\")"
  exit 1
fi

list=()
if [[ -n $one_seed ]]; then list=($one_seed); else for (( s = seed_from; s < seed_from + seeds; s++ )); do list+=($s); done; fi
failed=0
printf '%-14s %-9s %9s  %-40s %s\n' run exit median buckets result
for s in $list; do
  run_one $out/seed$s $s "$spec" || (( failed++ ))
done
echo "shake $game: $failed of ${#list} runs failed ($out)"
exit $(( failed > 0 ))
