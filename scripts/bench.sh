#!/bin/zsh
# The open-world bench (D-1060): each case of games/<title>/bench/cases.json run from a save on a fresh copy of the
# game's hdd0, as the phone runs it (shaders compiled in the background, SPU programs translated in the session), its
# frames and 5 s lines checked by scripts/bench_check.py; an index page of the results.
#
#   scripts/bench.sh [CASE ...] [--title gta5] [--mode edge-off|edge-on|all] [--accept] [--keep-frames N] [--out DIR]
#                    [--nightly]
#
# No CASE: every case. --mode: the case file's modes (default edge-off). --accept: the runs that pass become the
# baseline (median of the last 5) and their message shapes known. --nightly: refuses to start on a busy Mac (another
# pspocket-boot or ninja, load average over 4, under 15 GB free) and keeps the Mac awake.
# Outputs (game data, gitignored): inventory/bench/<stamp>/<case>-<mode>/{run.log,summary.json,result.json,frames/},
# inventory/bench/<stamp>/index.html. Saves: inventory/<title>/saves/<name>/ (savedata directories).
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
title=gta5 modes=edge-off accept= keep=6 out= nightly= cases=()
while (( $# )); do
  case $1 in
    --title) shift; title=$1 ;;
    --mode) shift; modes=$1 ;;
    --accept) accept=--accept ;;
    --keep-frames) shift; keep=$1 ;;
    --out) shift; out=$1 ;;
    --nightly) nightly=1 ;;
    -*) echo "usage: scripts/bench.sh [CASE ...] [--title T] [--mode M|all] [--accept] [--keep-frames N] [--out DIR] [--nightly]" >&2; exit 64 ;;
    *) cases+=($1) ;;
  esac
  shift
done
G=inventory/$title CF=games/$title/bench/cases.json
[[ -f $CF ]] || { echo "bench: no $CF" >&2; exit 2; }
elf=$G/decrypt/EBOOT.elf iso=$G/disc.iso flash=inventory/firmware/dev_flash boot=build/host/runtime/loader/pspocket-boot
for f in $elf $iso $G/hdd0 $G/shaders $flash $boot; do [[ -e $f ]] || { echo "bench: needs $f" >&2; exit 2; }; done
if [[ -n $nightly ]]; then
  if pgrep -x pspocket-boot >/dev/null || pgrep -x ninja >/dev/null; then echo "bench: the Mac is busy (pspocket-boot or ninja)"; exit 3; fi
  load=$(sysctl -n vm.loadavg | awk '{print int($2)}'); (( load > 4 )) && { echo "bench: load average $load"; exit 3; }
  free=$(df -g . | awk 'NR==2{print $4}'); (( free < 15 )) && { echo "bench: $free GB free"; exit 3; }
fi
stamp=$(date +%Y%m%d-%H%M%S)-$(git rev-parse --short HEAD)
out=${out:-inventory/bench/$stamp}
mkdir -p $out inventory/bench/spu-translate
[[ $modes == all ]] && modes=$(python3 -c "import json;print(' '.join(json.load(open('$CF'))['modes']))")
names=$(python3 -c "import json;print(' '.join(c['name'] for c in json.load(open('$CF'))['cases']))")
(( ${#cases} )) || cases=(${=names})
[[ -n $nightly ]] && { caffeinate -dimsu -w $$ & }
for c in $cases; do
  spec=$(python3 -c "
import json,sys
cs={c['name']:c for c in json.load(open('$CF'))['cases']}
c=cs.get('$c') or sys.exit('bench: no case $c')
print(c['save'], c['route'], c['timeout'], c.get('from_s', 0))") || exit 2
  read save route timeout from_s <<< "$spec"
  for m in ${=modes}; do
    R=$out/$c-$m
    mkdir -p $R/frames
    envs=$(python3 -c "import json;print(' '.join(f'{k}={v}' for k,v in json.load(open('$CF'))['modes']['$m'].items()))")
    # A fresh hdd0 with the save (APFS clones: no space until written), a frozen copy of the shader cache.
    cp -c -R $G/hdd0 $R/hdd0
    mkdir -p $R/hdd0/home/00000001/savedata
    cp -R $G/saves/$save/* $R/hdd0/home/00000001/savedata/
    cp -c -R $G/shaders $R/shaders
    python3 scripts/route.py games/$title/bench/$route > $R/press.txt
    echo "==> $c ($m): $timeout s"
    env ${=envs} perl -e "alarm $((timeout + 300)); exec @ARGV" $boot $elf --gpu metal --timeout $timeout --hdd0 $R/hdd0 \
      --bdvd $iso --flash $flash --shader-cache $R/shaders --async-shaders 4 --spu-translate inventory/bench/spu-translate \
      --press-file $R/press.txt --frames $R/frames --frame-every 60 --frame-jumps --summary $R/summary.json > $R/run.log 2>&1
    rm -rf $R/hdd0 $R/hdd1 $R/shaders
    python3 scripts/bench_check.py $R --case $c --mode $m --from-s $from_s --keep $keep $accept | tee -a $out/results.txt
  done
done
python3 scripts/bench_report.py $out > /dev/null && echo "bench: $out/index.html"
grep -q "^FAIL" $out/results.txt && exit 1
exit 0
