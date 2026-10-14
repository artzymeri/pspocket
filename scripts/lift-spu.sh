#!/usr/bin/env bash
# Lifts the SPU programs a run saved (pspocket-boot --spu-dump DUMP) into C for -DPSPOCKET_GAME_SPU=OUT (D-076).
#
#   scripts/lift-spu.sh DUMP OUT [LOG...]
#
# DUMP holds spu_<hash>_<lsaddr>.bin (the code, from the user's game) and .txt (kind, guest address, LS base, size,
# entry, and for modules "seed" lines: addresses execution entered them at). Every image is lifted at its LS base with
# its entry and seeds; the logs of earlier runs with lifted programs add the addresses where execution left lifted code
# ("no lifted code at ...") as seeds too, so the next lift covers them, and the seeds the programs already in OUT were
# lifted with (OUT/NAME/NAME.seeds) stay seeds (the seeds accumulate). OUT gets one directory per program and game_spu.c, whose pspocket_game_spu_register() registers them all. DUMP and OUT hold
# code derived from the game: keep them in the gitignored inventory/ or build/ (CLAUDE.md rule 1).
#
# Job binaries and modules are position independent: they are lifted relocatable (spu-lift --relocatable, D-081), once
# per hash. Copies of one saved at several LS addresses (older dumps) become one program, lifted at the first copy's
# base, with the seeds of every copy moved there.
#
# Works with macOS's bash 3.2.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
LIFT="${PSPOCKET_SPU_LIFT:-$ROOT/build/host/recomp/spu/spu-lift}"

[ $# -ge 2 ] || { echo "usage: scripts/lift-spu.sh DUMP OUT [LOG...]" >&2; exit 64; }
DUMP="$1"; OUT="$2"; shift 2
[ -x "$LIFT" ] || { echo "error: no spu-lift at $LIFT (ninja -C build/host spu-lift, or set PSPOCKET_SPU_LIFT)" >&2; exit 1; }
[ -d "$DUMP" ] || { echo "error: no dump directory $DUMP" >&2; exit 1; }
mkdir -p "$OUT"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
value() { sed -n "s/^$1 //p" "$2"; }

# The images: name kind base size entry, one per line.
for meta in "$DUMP"/spu_*.txt; do
    [ -e "$meta" ] || continue
    name="$(basename "$meta" .txt)"
    [ -f "$DUMP/$name.bin" ] || { echo "warning: $name has no $name.bin, skipped" >&2; continue; }
    echo "$name $(value kind "$meta") $(value base "$meta") $(value size "$meta") $(value entry "$meta" | head -1)"
done | sort > "$TMP/images"

# Seeds, "name address": from the logs ("no lifted code at 0xADDR in program NAME") and the images' "seed" lines.
{
    if [ $# -gt 0 ]; then
        sed -n 's/.*no lifted code at \(0x[0-9a-fA-F]*\) in program \(spu_[0-9a-f]*_[0-9a-f]*\).*/\2 \1/p' "$@" || true
    fi
    for meta in "$DUMP"/spu_*.txt; do
        [ -e "$meta" ] || continue
        name="$(basename "$meta" .txt)"
        value seed "$meta" | sed "s/^/$name /"
    done
    # The seeds an earlier lift used (NAME.seeds, as runtime/prepare keeps them): a seed stays once found, though the
    # logs that gave it are not kept. A program lifted before the script kept them carries its entries over instead,
    # which are every label: those of code only discovery or a constant found, and block starts a newer lifter no
    # longer makes (D-958), which as seeds would come back as labels.
    for reg in "$OUT"/spu_*/spu_*_reg.c; do
        [ -e "$reg" ] || continue
        dir="$(dirname "$reg")"
        name="$(basename "$dir")"
        if [ -f "$dir/$name.seeds" ]; then
            sed "s/^/$name /" "$dir/$name.seeds"
        else
            sed -n 's/^    {\(0x[0-9A-Fa-f]*\)u, .*/\1/p' "$reg" | sed "s/^/$name /"
        fi
    done
} > "$TMP/raw-seeds"

# Relocatable images (jobs, modules) are lifted once per hash, at the first copy's base: "name canonical" for each,
# and the seeds of the other copies moved by the difference of the bases.
awk '
    function hex(s,   v, i) { s = tolower(s); sub(/^0x/, "", s); v = 0
        for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
        return v }
    FNR == NR {
        base[$1] = hex($3)
        if ($2 == "job" || $2 == "module") {
            h = substr($1, 1, 20)
            if (!(h in first)) first[h] = $1
            canon[$1] = first[h]
            print $1, first[h] > "'"$TMP/canon"'"
        }
        next
    }
    {
        name = $1; at = hex($2)
        if (name in canon && canon[name] != name) {
            at = (at - base[name] + base[canon[name]]) % 262144
            if (at < 0) at += 262144
            name = canon[name]
        }
        printf "%s 0x%x\n", name, at
    }
' "$TMP/images" "$TMP/raw-seeds" | sort -u > "$TMP/seeds"
touch "$TMP/canon"

count=0; failed=0; merged=0
DECLS=""; CALLS=""
while read -r name kind base size entry; do
    canonical="$(awk -v n="$name" '$1 == n { print $2 }' "$TMP/canon")"
    if [ -n "$canonical" ] && [ "$canonical" != "$name" ]; then merged=$((merged + 1)); continue; fi
    args=(--image "$DUMP/$name.bin" --base "$base" --entry "$entry" --name "$name" --out "$OUT/$name" --quiet)
    # More spu-lift options for every program (comparisons: PSPOCKET_SPU_LIFT_ARGS=--whole).
    extra=(${PSPOCKET_SPU_LIFT_ARGS:-})
    args+=(${extra[@]+"${extra[@]}"})
    case "$kind" in job|module) args+=(--relocatable) ;; esac
    # Seeds inside the image's code only: execution also leaves lifted code for code outside it (the SPURS syscall
    # stubs, modules a program loads), which spu-lift refuses as seeds.
    while read -r prog seed; do
        [ "$prog" = "$name" ] || continue
        if [ $((seed)) -ge $((base)) ] && [ $((seed)) -lt $((base + size)) ]; then args+=(--seed "$seed"); fi
    done < "$TMP/seeds"
    if "$LIFT" "${args[@]}" > "$OUT/$name.log" 2>&1; then
        printf '%s\n' "${args[@]}" | awk 'prev == "--seed" { print } { prev = $0 }' > "$OUT/$name/$name.seeds"
        DECLS="${DECLS}void ${name}_register(void);"$'\n'
        CALLS="${CALLS}    ${name}_register();"$'\n'
        count=$((count + 1))
    else
        echo "warning: spu-lift failed for $name (see $OUT/$name.log); it stays interpreted" >&2
        rm -rf "${OUT:?}/$name"
        failed=$((failed + 1))
    fi
done < "$TMP/images"

# Programs of earlier lifts that are no longer lifted (merged copies) must not be built.
for dir in "$OUT"/spu_*/; do
    [ -d "$dir" ] || continue
    name="$(basename "$dir")"
    case "$CALLS" in *" ${name}_register();"*) ;; *) rm -rf "${OUT:?}/$name" "$OUT/$name.log" ;; esac
done

{
    echo "/* The game's lifted SPU programs (scripts/lift-spu.sh, D-076). Generated: do not edit or commit. */"
    echo
    printf '%s\n' "$DECLS"
    echo "void pspocket_game_spu_register(void);"
    echo "void pspocket_game_spu_register(void)"
    echo "{"
    printf '%s' "$CALLS"
    echo "}"
} > "$OUT/game_spu.c.tmp"
mv "$OUT/game_spu.c.tmp" "$OUT/game_spu.c"
echo "lifted $count SPU programs into $OUT ($failed failed, $merged copies merged); seeds: $(wc -l < "$TMP/seeds" | tr -d ' ')"
