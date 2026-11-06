# Enable Scale Mode

An addin for the MPC Sample that adds **Scale** as a 16 Levels Type.

On the Sample, 16 Levels spreads one sound over 16 slots (4 pads x 4 banks)
and Shift+16 Levels cycles its Type (Tune, Velocity, Filter). This addin
adds Scale to that cycle: with Scale selected, the 16 slots play the notes
of a configurable scale instead of chromatic steps.

## Use

1. Press **16 Levels** to turn it on.
2. Press **Shift+16 Levels** until Scale is selected (one press after the
   Type it follows, `scale_slot`). The next press returns to the native cycle.
3. Play the pads; switch banks for the higher notes.

Scale shows no popup of its own yet; `padctl status` shows `scale_on=1`.

## What it does

1. **Hooks the 16 Levels Type cycle** (Shift+16 Levels) to insert Scale
2. **Hooks the note mapping function** with a PACIASP trampoline that remaps
   the 16 slots through scale intervals while Scale is selected
3. **Patches the notification loop** in `set_mode` so `padctl` Type changes don't hang
4. **Provides `padctl`** — a CLI to change scales and query status
5. Optionally (`padperform_patch=1`) forces the PadPerform bool; off by
   default because it blocks 16 Levels on the Sample

## Scales

8 built-in scales: Chromatic, Major, Minor, Harmonic Minor, Dorian, Blues,
Phrygian, Mixolydian. Default: Blues, root C3.

With 4 pads in Blues/C3: pad 0 = C3, pad 1 = Eb3, pad 2 = F3, pad 3 = F#3.

## Install

```sh
make build         # cross-compile for aarch64 (needs Docker)
make release       # package the dist zip
# copy to device and run install.sh, or:
make deploy MPC_HOST=root@<device-ip>
```

## Settings

`mpc_enable_scalemode.conf` next to the `.so`:

```
enabled=1          # 0 to load but skip all patches
scale=Blues        # Chromatic, Major, Minor, Harm Minor, Dorian, Blues, Phrygian, Mixolydian
root=C3            # note name with octave, or MIDI number (0-127)
octave_offset=2    # shift all notes up/down (-4 to 4); 2 centers bank A at root
scale_slot=1       # native Type value Scale follows in the Shift+16 Levels cycle
padperform_patch=0 # 1 forces the PadPerform bool (blocks 16 Levels)
```

Settings are read on each MPC startup. Changes via `padctl` are saved
immediately and persist across restarts.

## Runtime commands (padctl)

```sh
padctl status       # mode, scale, root, hook state
padctl cycle        # step the native 16 Levels Type (debug)
padctl scale Blues   # change scale (prefix match, case-insensitive)
padctl root A2      # change root note
padctl octave -1    # shift octave offset
padctl scaleinfo    # current scale config
padctl testmap 0    # dry-run: show what MIDI note pad 0 would produce
padctl hookhits     # how many times the hook has fired
```

## Known limitations

- **Pads need samples.** The firmware skips the note mapping function entirely
  for pads that have no sample assigned. Scale Mode repitches whatever sample
  is already on the pad — it doesn't create sound from nothing. Load a drum
  program where every pad in the active bank has a sample, or duplicate a
  single sample across all pads for a melodic layout.
- **No on-screen scale settings.** The firmware's Pad Perform settings
  dialogue needs a touchscreen, which the Sample doesn't have. Use `padctl`
  over SSH or edit the conf file.
- **Type names not mapped yet.** `padctl` prints the native Type as a number
  (0-2); which number is Tune, Velocity or Filter is still being checked on
  hardware, and so is the default `scale_slot`.
- **Hook requires physical pad press.** The note mapping function is only
  called from the analog pad hardware handler (UART serial path). MIDI
  injection via ALSA or the kernel module does not reach it.

## How it works

1. The `.so` is loaded via `LD_PRELOAD` (standard addin mechanism).
2. A background thread scans MPC's memory for the 16 Levels Type controller
   (`padPerformButton`, `PadPerform.Type`) and patches the notification loop
   in `set_mode` (`B.EQ` → unconditional `B`).
3. It hooks the Type cycle (base+0x1542030): after Type `scale_slot`, one
   more press selects Scale without advancing the native Type.
4. It hooks the note mapping function (base+0x21472f0) with a PACIASP
   trampoline that, while Scale is selected, repitches each slot through
   the scale tables via the note setter (base+0x27e4400).
5. A socket server on `/tmp/padmode.sock` accepts `padctl` commands.

## License

MIT
