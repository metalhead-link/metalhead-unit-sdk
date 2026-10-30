# Units as DAW plugins

The plugins of a unit file, built for the Mac as **CLAP**, **VST3** and **AudioUnit (v2)**
plugins, so the same code that runs on the device runs in Live, Logic or Bitwig. Nothing in the
units changes: the ABI is modelled on CLAP (`../include/unit_abi.h`), so `unitclap.c`
translates it to CLAP, and [clap-wrapper](https://github.com/free-audio/clap-wrapper) turns
that CLAP into a VST3 and an AUv2.

```sh
cd ccs/units/daw
make UNIT=yafr              # → build/yafr/plugins/metalhead YAFR.{clap,vst3,component}
make install UNIT=stock     # all four examples, copied to ~/Library/Audio/Plug-Ins/{CLAP,VST3,Components}
```

`UNIT` is a unit directory. Its `Makefile` (`BUNDLE`, `PLUGINS`, `SRCS`) says what goes in,
the same as for the device. One file per unit file: `stock` gives one `metalhead Stock.vst3`
holding VUMeter, Drums, YAFR and ClipGrid. As on the device, install either the bundle or the
separate files, not both: the plugin IDs (`metalhead.unit.<Name>`) are the same.

Needs CMake ≥ 3.21 and Ninja (`port install cmake ninja`, or `pip3 install cmake ninja`). The
first configure downloads clap-wrapper (pinned in `CMakeLists.txt`) and, through it, the CLAP,
VST3 (MIT licensed since 3.8) and AudioUnit SDKs. The plugins are universal (arm64 + x86_64),
macOS 11 and later, and unsigned: fine on the machine that built them.

| file | |
|---|---|
| `unitclap.c` | the unit ABI as CLAP: factory, parameters, state, audio and note ports, process |
| `entry.cpp` | the exported `clap_entry`, and the AudioUnit names (clap-wrapper's AUv2 factory) |
| `CMakeLists.txt` | reads the unit's Makefile, generates `unit_entry()`, builds through clap-wrapper |
| `unitvars.mk` | prints a unit Makefile's variables for `CMakeLists.txt` |

## What the DAW sees

| unit | DAW |
|---|---|
| plugin name, vendor, version, description | the plugin's descriptor; ID `metalhead.unit.<Name>` |
| no inputs, outputs | an instrument (AU `aumu`); with trigger parameters also a drum machine |
| inputs and outputs | an effect with a MIDI input (AU `aumf`) |
| no outputs (VUMeter) | an analyzer; CLAP and VST3 only, an AudioUnit needs an output |
| audio channels | ports of two: `In 1-2`, `In 3-4`, …, mono for an odd last one; the first is the main port |
| `UnitParamInfo` | parameters, with the same id, name, module, range, default and flags (CLAP's values) |
| `paramValueToText`, `paramTextToValue` | the DAW's text for a value; without them a plain number |
| read-only parameters | reported to the DAW as they change |
| trigger parameters | parameters that fire when set above their minimum, and MIDI notes: the n-th trigger is note 36 + n (Drums: C1 is the first sound), velocity is the value |
| MIDI from the DAW | `midiIn` (notes, CCs, SysEx), as `[midiroutes]` sends it on the device |
| `sendMIDIEvent` | the plugin's MIDI output, SysEx included |
| `UnitTimeInfo` | the DAW's transport: tempo, position, bar start, time signature, playing |
| `play`, `stop`, `requestTempo` | ignored: the DAW owns the transport |

The state the DAW saves with a project is the parameter values as text, one
`<id> <value> <name>` per line; triggers and read-only parameters aren't saved.

## Like the device

- `process()` gets blocks of at most one millisecond (48 frames at 48 kHz), however big the
  DAW's buffer is. Events land at the start of the block they fall in, so their timing is
  within a millisecond, as on the device.
- Outputs start zeroed; inputs the DAW doesn't connect are silence.
- File access: the SD card is a folder, `$METALHEAD_SD_ROOT` or else `~/Music/metalhead`
  (`/samples/drums` is `~/Music/metalhead/samples/drums`). It fails inside `process()`, as on
  the device. Copy the card's folders there to get the same samples.
- `log()` goes to the unified log, subsystem `metalhead.unit`:
  `log stream --predicate 'subsystem == "metalhead.unit"'`, or Console.app.
- `allocate()` is `malloc`, never freed; there's no 64 MB limit.
- Units are loaded once per DAW process and never unloaded.

## Unlike the device

- **Instances are limited by the unit.** A unit keeps its instances in static memory
  (`MAX_INSTANCES`: 2 for YAFR and Drums, 1 for ClipGrid), so the third YAFR in a project fails
  to load. Raise it in the unit if a DAW needs more.
- **Sample rate.** Units get the DAW's rate in `timeInfo.sampleRate`. A unit that assumes
  48 kHz (Drums resamples its WAVs to a fixed 48000) plays at the wrong pitch at other rates:
  run the project at 48 kHz.
- **Raw MIDI.** VST3 and AU hosts pass notes and controllers to a plugin, but no SysEx, and
  MIDI from a plugin hardly anywhere. The Launchpad units (VUMeter, ClipGrid) need a CLAP host
  that routes plugin MIDI to a controller (Bitwig, Reaper). Live and Logic don't load CLAP.
- **No editor.** The DAW shows its generic parameter view.
