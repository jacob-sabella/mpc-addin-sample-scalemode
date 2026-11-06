#!/usr/bin/env bash
# Device test on an MPC Sample, all remote: buttons and pads go in through `padctl on|off|note` (frames injected into
# MPC's control-surface stream, filtered like real ones), the screen comes back through the draw handler's /snap.
# Needs the addin and mpc-addin-sample-draw-handler installed and MPC running. Leaves Scale off and 16 Levels as found.
#   MPC_HOST=root@<device> [SSH_KEY=~/.ssh/key] tests/device_test.sh [out_dir]
# AUDIO_SRC=<PulseAudio/PipeWire source of the Sample's USB audio> also records the 16 slots in Blues and checks each one's
# pitch (tests/pitch_check.py). Load a pitched sample on the 16 Levels pad first (a saw at C3 with a short fade works best;
# PITCH_MODE=clicks instead for a drum sample with two or more transients), and checks that the SCALE page and Scale add no
# delay to a pad (padctl lat, tests/latency_check.py).
# RESTART=1 also checks that a scale changed on the page survives an MPC restart (restarts MPC once).
# out_dir (default build/device-test) gets the page frames while pads play, and page.gif when ffmpeg is installed.
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=${MPC_HOST:?set MPC_HOST=root@<device>}
SSH=(ssh -o BatchMode=yes ${SSH_KEY:+-i "$SSH_KEY"} "$HOST")
SCP=(scp -q -o BatchMode=yes ${SSH_KEY:+-i "$SSH_KEY"})
OUT=${1:-build/device-test}
P=/data/mpc-addins/sample-scalemode/padctl
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
if [ "$(pc status | sed -n 's/.*sixteen=\([-0-9]*\).*/\1/p')" = 1 ]; then short16; fi   # start with 16 Levels off, as after MPC starts

t=$(mark); sleep 1; short16; short16; sleep 0.5
l=$(log_from "$t")
if ! grep -q '16 Levels held' <<<"$l" && [ "$(scale_on)" = 0 ]; then
    ok "short 16 Levels presses pass through, no toggle"
else bad "short 16 Levels press"; fi

t=$(mark); sleep 1; pc on 1 49 >/dev/null; pc note 1 40 127 80 >/dev/null; pc off 1 49 >/dev/null; sleep 0.5
grep -q 'type-cycle' <<<"$(log_from "$t")" && ok "Shift+16 Levels runs the Type cycle" || bad "Shift+16 Levels"

hold
[ "$(scale_on)" = 1 ] && ok "hold 16 Levels: Scale on" || bad "hold did not turn Scale on"
[ "$(pc status | sed -n 's/.*sixteen=\([-0-9]*\).*/\1/p')" = 1 ] && ok "hold turns 16 Levels on with Scale" || bad "16 Levels not on after the hold"
o=$(owner); [ "${o:-0}" != 0 ] && ok "SCALE page has the screen (client $o)" || bad "page did not open"

h0=$(hooks); pc note 10 37 100 40 >/dev/null; sleep 0.3
[ "$(hooks)" != "$h0" ] && ok "pads reach the scale hook" || bad "pads do not reach the hook"
sr=$(pc status | sed -n 's/.*sample_root=\([-0-9]*\).*/\1/p')
[ -n "$sr" ] && [ "$sr" -ge 0 ] && ok "reads the sample's root note ($sr)" || bad "sample root note not read (${sr:-none})"
hr=$(pc hear)
grep -q 'heard=-\{0,1\}[0-9]' <<<"$hr" && ok "reads the sample's audio for its pitch ($hr)" || bad "sample audio not read ($hr)"
d0=$(pc detect | sed 's/.*=//')
pc detect always >/dev/null; ha=$(pc hear); pc detect off >/dev/null; ho=$(pc hear); pc detect "$d0" >/dev/null
if grep -q 'heard=-1' <<<"$ha" || { grep -q 'used=heard' <<<"$ha" && grep -q 'used=mpc' <<<"$ho"; }; then
    ok "padctl detect always/off switch the note heard on and off (back to $(pc detect | sed 's/.*=//'))"
else bad "padctl detect ($ha / $ho)"; fi

dev "rm -rf /tmp/scaletest; mkdir -p /tmp/scaletest; cd /tmp/scaletest
     ( for i in \$(seq 1 6); do for n in $PADS; do $P note 10 \$n 110 60 >/dev/null; sleep 0.2; done; done ) &
     i=0; while [ \$i -lt 40 ]; do wget -qO f\$(printf %02d \$i).raw $DH/snap; i=\$((i+1)); sleep 0.1; done; wait"
o=$(owner); [ "${o:-0}" != 0 ] && ok "page stays up while pads play" || bad "page closed while pads played"
sleep 6
o=$(owner); [ "${o:-0}" != 0 ] && ok "page stays up with Scale on, no input for 6 s" || bad "page closed with Scale on"

if [ -n "${AUDIO_SRC:-}" ]; then
    mkdir -p "$OUT"; sleep 2; was=$(pc status | sed -n 's/.* scale=\([^ ]*\) .*/\1/p'); pc scale Blues >/dev/null
    timeout 36 parecord -d "$AUDIO_SRC" --file-format=wav --format=s32le --rate=44100 --channels=2 "$OUT/slots.wav" & rec=$!
    sleep 0.5
    dev "for n in 37 36 42 82 40 38 46 44 48 47 45 43 49 55 51 53; do $P note 10 \$n 110 1600 >/dev/null; sleep 0.2; done"
    wait $rec || true; pc scale "$was" >/dev/null
    BLUES=0,3,5,6,7,10,12,15,17,18,19,22,24,27,29,30
    if [ "${PITCH_MODE:-spectrum}" = clicks ]; then args=(clicks "$OUT/slots.wav" 16 $BLUES); else args=(spectrum "$OUT/slots.wav" auto 1.8 16 $BLUES); fi
    python3 tests/pitch_check.py "${args[@]}" > "$OUT/pitch.txt" && ok "the 16 slots sound in Blues (audio, $OUT/pitch.txt)" \
        || { bad "slot pitches (audio)"; cat "$OUT/pitch.txt"; }
    for what in page scale; do   # what the SCALE page and Scale add to a pad's delay: every other hit flipped, on an exact grid
        timeout 16 parecord -d "$AUDIO_SRC" --file-format=wav --format=s32le --rate=44100 --channels=2 "$OUT/lat-$what.wav" & rec=$!
        sleep 1; pc lat $what 24 500 >/dev/null; wait $rec || true
        python3 tests/latency_check.py "$OUT/lat-$what.wav" 0.5 10 > "$OUT/lat-$what.txt" \
            && ok "the $what adds no pad delay ($(tail -n 1 "$OUT/lat-$what.txt"))" || { bad "$what adds pad delay"; cat "$OUT/lat-$what.txt"; }
    done
fi

hold
[ "$(scale_on)" = 0 ] && ok "hold again: Scale off" || bad "second hold did not turn Scale off"
sleep 5
[ "$(owner)" = 0 ] && ok "with Scale off the page gives the screen back after idle" || bad "page still up after Scale off"

if [ "${RESTART:-0}" = 1 ]; then
    before=$(pc status | sed -n 's/.* scale=\([^ ]*\) .*/\1/p')
    hold; dev "wget -qO- '$DH/input?key=0&value=1' >/dev/null; wget -qO- '$DH/input?dial=1&value=1' >/dev/null"; sleep 1.5
    hold; sleep 5
    changed=$(pc status | sed -n 's/.* scale=\([^ ]*\) .*/\1/p')
    dev "systemctl restart ac50"; sleep 25
    after=$(pc status | sed -n 's/.* scale=\([^ ]*\) .*/\1/p')
    [ "$changed" != "$before" ] && [ "$after" = "$changed" ] && ok "page change survives a restart ($before -> $after)" \
        || bad "persistence: $before -> $changed -> $after after restart"
    pc scale "$before" >/dev/null
fi

mkdir -p "$OUT"
"${SCP[@]}" "$HOST:/tmp/scaletest/f*.raw" "$OUT/"
dev "rm -rf /tmp/scaletest"
if command -v ffmpeg >/dev/null; then
    cat "$OUT"/f*.raw | ffmpeg -loglevel error -y -f rawvideo -pixel_format bgr0 -video_size 320x240 -framerate 8 -i - \
        -vf "split[a][b];[a]palettegen[p];[b][p]paletteuse" "$OUT/page.gif" && ok "frames: $OUT/page.gif"
fi

echo "$([ $fails = 0 ] && echo PASS || echo "FAIL ($fails)")"
exit $((fails != 0))
