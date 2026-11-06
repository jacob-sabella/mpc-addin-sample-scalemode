#!/usr/bin/env bash
# Device test on an MPC Sample, all remote: buttons and pads go in through `padctl on|off|note` (frames injected into
# MPC's control-surface stream, filtered like real ones), the screen comes back through the draw handler's /snap.
# Needs the addin and mpc-addin-sample-draw-handler installed and MPC running. Leaves Scale off and 16 Levels as found.
#   MPC_HOST=root@<device> [SSH_KEY=~/.ssh/key] tests/device_test.sh [out_dir]
# out_dir (default build/device-test) gets the page frames while pads play, and page.gif when ffmpeg is installed.
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=${MPC_HOST:?set MPC_HOST=root@<device>}
SSH=(ssh -o BatchMode=yes ${SSH_KEY:+-i "$SSH_KEY"} "$HOST")
SCP=(scp -q -o BatchMode=yes ${SSH_KEY:+-i "$SSH_KEY"})
OUT=${1:-build/device-test}
P=/data/mpc-addins/enable-scalemode/padctl
DH=http://127.0.0.1:6740
PADS="37 36 42 82"   # bank A's four pads on channel 10

fails=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
dev() { "${SSH[@]}" "$@"; }
pc() { dev "$P $*"; }
scale_on() { pc status | sed -n 's/.*scale_on=\([0-9]\).*/\1/p'; }
owner() { dev "wget -qO- $DH/status" | sed -n 's/.*"owner":\([0-9]*\).*/\1/p'; }
hooks() { pc hookhits | sed 's/.*=//'; }
mark() { dev date +%s; }
log_from() { dev "pid=\$(pgrep 'MPC Sample' | tail -n 1); journalctl -u ac50 --no-pager --since @$1 | grep \"\$pid\" | sed 's/.*\]: //'"; }
hold() { pc note 1 40 127 1100 >/dev/null; sleep 0.3; }
short16() { pc note 1 40 127 80 >/dev/null; sleep 0.4; }

pc status | grep -q 'func=' && ok "padctl socket answers" || { bad "padctl socket"; exit 1; }
[ "$(scale_on)" = 1 ] && hold
[ "$(scale_on)" = 0 ] && ok "start with Scale off" || bad "Scale still on"

t=$(mark); sleep 1; short16; short16; sleep 0.5
l=$(log_from "$t")
if ! grep -q '16 Levels held' <<<"$l" && [ "$(scale_on)" = 0 ]; then
    ok "short 16 Levels presses pass through, no toggle"
else bad "short 16 Levels press"; fi

t=$(mark); sleep 1; pc on 1 49 >/dev/null; pc note 1 40 127 80 >/dev/null; pc off 1 49 >/dev/null; sleep 0.5
grep -q 'type-cycle' <<<"$(log_from "$t")" && ok "Shift+16 Levels runs the Type cycle" || bad "Shift+16 Levels"

hold
[ "$(scale_on)" = 1 ] && ok "hold 16 Levels: Scale on" || bad "hold did not turn Scale on"
o=$(owner); [ "${o:-0}" != 0 ] && ok "SCALE page has the screen (client $o)" || bad "page did not open"

h0=$(hooks); pc note 10 37 100 40 >/dev/null; sleep 0.3
if [ "$(hooks)" = "$h0" ]; then short16; h0=$(hooks); pc note 10 37 100 40 >/dev/null; sleep 0.3; fi   # 16 Levels was off
[ "$(hooks)" != "$h0" ] && ok "pads reach the scale hook" || bad "pads do not reach the hook"

dev "rm -rf /tmp/scaletest; mkdir -p /tmp/scaletest; cd /tmp/scaletest
     ( for i in \$(seq 1 6); do for n in $PADS; do $P note 10 \$n 110 60 >/dev/null; sleep 0.2; done; done ) &
     i=0; while [ \$i -lt 40 ]; do wget -qO f\$(printf %02d \$i).raw $DH/snap; i=\$((i+1)); sleep 0.1; done; wait"
o=$(owner); [ "${o:-0}" != 0 ] && ok "page stays up while pads play" || bad "page closed while pads played"
sleep 5
[ "$(owner)" = 0 ] && ok "page gives the screen back after idle" || bad "page still up after idle"

hold
[ "$(scale_on)" = 0 ] && ok "hold again: Scale off" || bad "second hold did not turn Scale off"
sleep 5

mkdir -p "$OUT"
"${SCP[@]}" "$HOST:/tmp/scaletest/f*.raw" "$OUT/"
dev "rm -rf /tmp/scaletest"
if command -v ffmpeg >/dev/null; then
    cat "$OUT"/f*.raw | ffmpeg -loglevel error -y -f rawvideo -pixel_format bgr0 -video_size 320x240 -framerate 8 -i - \
        -vf "split[a][b];[a]palettegen[p];[b][p]paletteuse" "$OUT/page.gif" && ok "frames: $OUT/page.gif"
fi

echo "$([ $fails = 0 ] && echo PASS || echo "FAIL ($fails)")"
exit $((fails != 0))
