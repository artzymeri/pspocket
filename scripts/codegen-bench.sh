#!/bin/zsh
# Route B, stage 2 (D-151): how fast the phone compiles real translated code. Copies one of GTA V's PPU units and two
# of its SPU programs (a typical one and the biggest) with their headers into the app's
# Library/Application Support/pspocket-codegen/bench/, runs the self-test and prints the codegen.compile probe:
# seconds and peak memory per file at -O1 and -O2, then the PPU unit on 2, 4 and 6 threads at once.
#
#   scripts/codegen-bench.sh [--mac]
#
# --mac runs the same probe on this Mac (pspocket-selftest from build/host) for the numbers to compare against. The
# translated code comes from inventory/gta5 (scripts/translate-game.sh) and goes only to the phone or to
# ~/Library/Application Support/pspocket-codegen/bench, which is deleted afterwards (never committed: rule 1).
set -eu
cd "$(dirname "$0")/.."
bundle=com.artzymeri.pspocket.testhost
game=${PSPOCKET_BENCH_GAME:-inventory/gta5}
out=inventory/codegen-bench
rm -rf $out && mkdir -p $out/stage
# name, source file, header
pick=(
  "1-ppu $game/ppu-lifted/game_ppu_100.c $game/ppu-lifted/game_ppu.h"
  "2-spu $game/spu-lifted/spu_2607cab907577500_04c00/spu_2607cab907577500_04c00_0.c
         $game/spu-lifted/spu_2607cab907577500_04c00/spu_2607cab907577500_04c00.h"
  "3-spu-biggest $game/spu-lifted/spu_bb7fe62f5ca7873a_04c00/spu_bb7fe62f5ca7873a_04c00_0.c
                 $game/spu-lifted/spu_bb7fe62f5ca7873a_04c00/spu_bb7fe62f5ca7873a_04c00.h"
)
for p in $pick; do
  parts=(${=p})
  [[ -f ${parts[2]} && -f ${parts[3]} ]] || { echo "missing ${parts[2]} (scripts/translate-game.sh first)" >&2; exit 1; }
  mkdir -p $out/stage/${parts[1]}
  cp ${parts[2]} ${parts[3]} $out/stage/${parts[1]}/
done

if [[ ${1:-} == --mac ]]; then
  dest="$HOME/Library/Application Support/pspocket-codegen/bench"
  rm -rf $dest && mkdir -p ${dest:h} && cp -R $out/stage $dest
  build/host/runtime/platform/macos/pspocket-selftest codegen.compile | tee $out/report-mac.txt
  rm -rf $dest
  exit 0
fi

dev=$(xcrun devicectl list devices 2>/dev/null | awk '/iPhone/ && /connected|available \(paired\)/ {print $3; exit}')
[[ -n $dev ]] || { echo "the iPhone is not connected: plug it in and unlock it" >&2; exit 2; }
for f in $out/stage/*/*; do
  rel=${f#$out/stage/}
  for try in 1 2 3 4; do # copies over Wi-Fi drop now and then
    xcrun devicectl device copy to --device $dev --domain-type appDataContainer --domain-identifier $bundle \
      --source $f --destination "Library/Application Support/pspocket-codegen/bench/$rel" > $out/copy.log 2>&1 && break
    [[ $try == 4 ]] && { cat $out/copy.log >&2; exit 1; }
    echo "copy of $rel failed, again ($try)" >&2
  done
done
echo "==> translated code on the phone; running the self-test (a few minutes)"
scripts/ios.sh run --autorun --exit > $out/run.log 2>&1 || true
scripts/ios.sh report > $out/report.txt 2>&1 || true
grep -A 16 "codegen.compile" $out/report.txt || { echo "no codegen.compile in the report ($out/report.txt)"; exit 1; }
