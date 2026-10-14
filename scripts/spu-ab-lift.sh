#!/usr/bin/env bash
# Copies a lift of the game's SPU programs (scripts/lift-spu.sh output) with its symbols renamed, to build in as a second
# translation (D-962): -DPSPOCKET_GAME_SPU_B=OUT, then PSPOCKET_SPU_AB=S runs it in every other S seconds of a session,
# beside the default lift (-DPSPOCKET_GAME_SPU), so one session measures both in the same scenes (the SPURS time lines
# say each window's half).
#
#   scripts/spu-ab-lift.sh LIFT OUT
#
# spu_<hash>_ becomes spb_<hash>_ (functions, programs), each program registers as a B twin
# (ps_spu_register_program_b), and game_spu.c's pspocket_game_spu_register becomes pspocket_game_spu_b_register. LIFT
# and OUT hold code derived from the game: keep them in the gitignored inventory/ (CLAUDE.md rule 1).
#
# Works with macOS's bash 3.2.
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: scripts/spu-ab-lift.sh LIFT OUT" >&2; exit 64; }
LIFT="$1"; OUT="$2"
[ -f "$LIFT/game_spu.c" ] || { echo "error: $LIFT holds no game_spu.c (scripts/lift-spu.sh output)" >&2; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT"
for dir in "$LIFT"/spu_*/; do
    name="$(basename "$dir")"
    mkdir -p "$OUT/$name"
    for f in "$dir"*.c "$dir"*.h; do
        [ -e "$f" ] || continue
        # Not the #include lines: the files keep their names.
        sed -E '/^#include/!s/spu_([0-9a-f]{16})_/spb_\1_/g; s/ps_spu_register_program\(/ps_spu_register_program_b(/g' "$f" \
            > "$OUT/$name/$(basename "$f")"
    done
    reg="$OUT/$name/${name}_reg.c"
    { echo "void ps_spu_register_program_b(ps_spu_program* program);"; cat "$reg"; } > "$reg.tmp"
    # The declaration after the includes.
    awk 'NR == 1 { decl = $0; next } { print } /^#include/ && !done { print decl; done = 1 }' "$reg.tmp" > "$reg"
    rm "$reg.tmp"
done
sed -E 's/spu_([0-9a-f]{16})_/spb_\1_/g; s/pspocket_game_spu_register/pspocket_game_spu_b_register/g' "$LIFT/game_spu.c" \
    > "$OUT/game_spu.c"
echo "renamed $(ls -d "$OUT"/spu_*/ | wc -l | tr -d ' ') programs into $OUT"
