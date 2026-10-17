#!/bin/zsh
# The bench every night (D-1067): a launchd job of the user's runs scripts/bench.py --game all --mode all --nightly
# --accept-clean at 03:00 (runs without a failure or warning join the baseline; bench.py skips a busy Mac: another
# pspocket-boot or ninja, load over 4, under 15 GB free) and keeps the newest 14 nights of inventory/bench/night-*;
# then the waker rule's guard on Assassin's Creed (scripts/shake.sh ac --guard, D-1051).
#
#   scripts/nightly-bench.sh install [HH:MM]   write and load ~/Library/LaunchAgents/com.pspocket.bench.plist
#   scripts/nightly-bench.sh remove            unload and delete it
#   scripts/nightly-bench.sh run               what the job runs (by hand: the same)
#   scripts/nightly-bench.sh status            the job's state and the last night's results
set -u
ROOT=${0:A:h:h}
LABEL=com.pspocket.bench
PLIST=~/Library/LaunchAgents/$LABEL.plist
LOG=$ROOT/inventory/bench/nightly.log

case ${1:-} in
install)
    at=${2:-03:00}
    hh=$((10#${at%%:*})); mm=$((10#${at##*:}))
    mkdir -p ${PLIST:h} $ROOT/inventory/bench
    cat > $PLIST <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key>
    <array><string>/bin/zsh</string><string>-lc</string><string>exec '$ROOT/scripts/nightly-bench.sh' run</string></array>
    <key>StartCalendarInterval</key><dict><key>Hour</key><integer>$hh</integer><key>Minute</key><integer>$mm</integer></dict>
    <key>StandardOutPath</key><string>$LOG</string>
    <key>StandardErrorPath</key><string>$LOG</string>
    <key>LowPriorityIO</key><false/>
</dict>
</plist>
EOF
    launchctl bootout gui/$(id -u)/$LABEL 2>/dev/null
    launchctl bootstrap gui/$(id -u) $PLIST && echo "nightly bench: every day at $at ($PLIST, log $LOG)"
    ;;
remove)
    launchctl bootout gui/$(id -u)/$LABEL 2>/dev/null
    rm -f $PLIST && echo "nightly bench: removed"
    ;;
run)
    cd $ROOT || exit 1
    echo "== $(date '+%Y-%m-%d %H:%M') nightly bench at $(git rev-parse --short HEAD)"
    out=inventory/bench/night-$(date +%Y%m%d)-$(git rev-parse --short HEAD)
    python3 scripts/bench.py --game all --mode all --nightly --accept-clean --out $out
    rc=$?
    # The waker rule's guard (D-1051): eight short boots of Assassin's Creed under D-999's race recipe, when its data
    # and the library the bench just made are there and the bench ran (rc 3: a busy Mac, skipped too).
    if (( rc != 3 )) && [[ -f inventory/BLES00158/disc.iso && -f inventory/BLES00158/bench-library/game.dylib ]]; then
        scripts/shake.sh ac --guard > $out/waker-guard.txt 2>&1
        g=$?
        tail -1 $out/waker-guard.txt
        (( g == 0 )) || rc=1
    fi
    # The newest 14 nights stay; each keeps its worst frames only (bench_check.py).
    nights=(inventory/bench/night-*(N/On))
    for d in ${nights[15,-1]}; do rm -rf $d; done
    echo "== done, exit $rc"
    exit $rc
    ;;
status)
    launchctl print gui/$(id -u)/$LABEL 2>/dev/null | grep -E "state|last exit" || echo "nightly bench: not installed"
    nights=($ROOT/inventory/bench/night-*(N/On)); last=${nights[1]:-}
    [ -n "$last" ] && { echo "last: $last"; cat $last/results.txt 2>/dev/null; }
    ;;
*)
    sed -n '2,10p' $0 | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
