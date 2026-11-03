# Enable Scale Mode

An addin for the Akai MPC that enables the built-in Pad Perform / Scale Mode on
devices where the feature is compiled into the firmware but not exposed in the UI.

The MPC firmware contains the complete PadPerform system — settings, dialogue,
MIDI note filtering, pad lighting, settings persistence — and the pad-mode
cycling function already includes PadPerform as mode 1 (Full Level -> 16 Level
-> Pad Perform -> Full Level). On some models the mode is gated by a
hardware-type generation check that prevents it from activating.

This addin locates those gates at runtime via AArch64 signature scanning and
patches them so the native Pad Perform code path activates.

## Status

**Work in progress.** The addin framework, signature scanner, and generation-gate
detector are in place. Needs hardware testing to confirm:

1. Whether the native pad-mode cycling reaches Pad Perform when the generation
   gate is patched
2. Whether the mode functions correctly once reachable (pad lighting, note
   mapping, settings)

## Install

Build the `.so`, copy the build output to the device, and install via the
shared addin mechanism:

```sh
./build.sh
# then install via the addin installer
```

## Settings

`mpc_enable_scalemode.conf` next to the `.so`:

```
enabled=1    # 0 to load the library but skip the patch
```

Set `MPC_ENABLE_SCALEMODE_DISABLE=1` in the environment to disable without
editing the config.

## How it works

1. The `.so` is loaded into the MPC process via `LD_PRELOAD` (the standard
   addin mechanism).
2. A background thread scans the MPC binary's memory regions for PadPerform
   infrastructure markers (`padPerformButton`, `PadPerform.Type`, `16 Level`).
3. It locates the pad-mode cycling function and verifies the 3-mode switch
   (modes 0, 1, 2) is present.
4. It searches for an inlined hardware-generation gate near the PadPerform
   code — the pattern that compares the hardware type enum against tier
   thresholds (4, 9, 19, 24, 29).
5. If found, the gate's conditional branch is patched to NOP, allowing the
   native PadPerform code to activate regardless of hardware tier.

No reimplementation of scale mode logic — this enables what's already there.

## Technical details

- **ADA2PadMode enum**: 0 = Full Level, 1 = Pad Perform, 2 = 16 Level
- **Button enum**: PAD_PERFORM = 94
- **Cycle order**: 0 -> 2 -> 1 -> 0 (pressing 16 Level button cycles modes)
- **Gate mechanism**: Hardware type enum is compared against generation
  thresholds; PadPerform requires Gen2+ (enum value > 9)

## License

MIT
