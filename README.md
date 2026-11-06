# Enable Scale Mode

An addin for the MPC Sample that enables Pad Perform / Scale Mode.

The firmware contains the complete PadPerform system — mode cycling, note
mapping, settings structures — but the UI never exposes it. This addin
patches the firmware at runtime to unlock it, and hooks the note mapping
function to remap pads through configurable scale tables.

## What it does

1. **Patches the PadPerform bool** so the firmware treats it as supported
2. **Patches the notification loop** in `set_mode` so mode changes don't hang
3. **Hooks the note mapping function** with a PACIASP trampoline that remaps
   pad presses through scale intervals when in Pad Perform mode (mode 1)
4. **Provides `padctl`** — a CLI to cycle modes, change scales, and query status

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
octave_offset=0    # shift all notes up/down (-4 to 4)
```

Settings are read on each MPC startup. Changes via `padctl` are saved
immediately and persist across restarts.

## Runtime commands (padctl)

```sh
padctl status       # mode, scale, root, hook state
padctl cycle        # cycle pad mode: Full Level -> 16 Level -> Pad Perform
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
- **No on-screen scale UI.** The Sample's 320x240 non-touch screen has no
  built-in Pad Perform settings dialogue. Use `padctl` over SSH or edit the
  conf file for settings. The pad mode indicator (Full Level / 16 Level /
  Pad Perform) is handled natively by the firmware.
- **Hook requires physical pad press.** The note mapping function is only
  called from the analog pad hardware handler (UART serial path). MIDI
  injection via ALSA or the kernel module does not reach it.

## How it works

1. The `.so` is loaded via `LD_PRELOAD` (standard addin mechanism).
2. A background thread scans MPC's memory for PadPerform markers
   (`padPerformButton`, `PadPerform.Type`).
3. It patches the PadPerform constructor bool (`LDRB` → `MOV #1`) and
   the notification loop (`B.EQ` → unconditional `B`).
4. It hooks the note mapping function (base+0x21472f0) with a PACIASP
   trampoline that intercepts pad events in mode 1 and remaps them
   through scale tables via the note setter (base+0x27e4400).
5. A socket server on `/tmp/padmode.sock` accepts `padctl` commands.

## License

MIT
