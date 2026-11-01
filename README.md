# Metalhead unit SDK

Everything needed to build plugins for the metalhead firmware outside it: the ABI header, the
build rules, the tools, and example units. The firmware,
[`metalhead-firmware`](https://github.com/metalhead-link/metalhead-firmware), includes this
repository as a submodule at `ccs/units` and compiles its loader against `include/unit_abi.h`,
so an ABI change is made here and the submodule moved forward in `metalhead-firmware`.

# Loadable units

A **unit** is a file of audio/MIDI plugins built separately from the firmware, on the laptop,
and loaded by the running device. Upload one with the web UI (`host/configui`, *Units* tab); it
is stored as `/units/<Name>.unt` on the SD card next to `config.txt` and loaded at every boot,
before `config.txt` is read. After that its plugins are used like the built-in ones:

```
[units]
vu VUMeter
```

```sh
cd vumeter && make                  # → build/arm32/vumeter.unt   (BeagleBone Black)
                                    #   build/aarch64/vumeter.unt (Raspberry Pi 5)
make test                           # runs the plugins on the Mac (tools/unittest.c)
```

A unit binary is built for one board: upload the one for yours. The other board refuses it
("built for 32-bit ARM (BeagleBone Black), this is 64-bit ARM (Raspberry Pi 5)") instead of
running it. `make arm32` or `make aarch64` builds just one.

Both boards are supported, and a unit is meant to work on both. The BeagleBone is much slower (a
1 GHz Cortex-A8 with single-precision NEON, against the Pi's 2.4 GHz Cortex-A76), so check a new
unit's CPU use per USB microframe in the live view on a BBB before releasing it. A plugin too heavy
for the BBB goes on the Pi's list only (see *Writing a unit*); say so in the unit's README.

One file can hold several plugins (a **bundle**, like a CLAP file): `stock/` builds all the
examples below into one `Stock.unt`. Each example directory also builds its plugin on its own;
upload either the bundle or the separate files, not both (a plugin name can only be loaded once).

| path | |
|---|---|
| `include/unit_abi.h` | the ABI — the only header a unit includes |
| `unit.mk`, `unit.ld` | shared build rules and linker script |
| `common/unitrt.c` | `memcpy`/`memset`/`memmove`/`memcmp` linked into every unit |
| `tools/mkunit.py` | ELF → `.unt` converter |
| `tools/upload_units.py` | rebuilds every unit and uploads them over the USB serial port (`--bbb`, `--reboot`, unit names; `-h`) |
| `tools/unittest.c` | `make test`: the plugins compiled for the Mac and exercised through the ABI |
| `doc/unit-abi-overview.pdf` | printable ABI overview; regenerate with `tools/abi_overview.py` (needs reportlab) |
| `vumeter/` | example: level meters on a Launchpad Mini MK3 |
| `drums/` | example: drum sampler playing WAV files from `/samples/drums` (built-in kit as fallback) |
| `yafr/` | example: plate-style reverb, mono in / stereo out (port of the old built-in YAFR plugin) |
| `clipgrid/` | Ableton-style clip launcher on a Launchpad Mini MK3: 8 tracks × 7 clips, after-the-fact capture; reads the Launchpad through `midiIn` (config in `clipgrid/clipgrid.txt`) |
| `stock/` | all four examples in one unit file |
| `tools/metalhead_serial.py` | finds the device's USB serial port and talks to its console (macOS) |
| metalhead-firmware's `ccs/wormhole/src/unitloader.c` | the firmware side |

Mutable Instruments modules (Plaits, Rings, Clouds, ...) are a C++ bundle in their own
repository, [`metalhead-mutable-units`](https://github.com/metalhead-link/metalhead-mutable-units) (see *Units in their own repository* below).

## ABI (version 2)

Plugins and parameters follow [CLAP](https://github.com/free-audio/clap): the unit's entry
point returns a factory (`clap_plugin_factory`), a plugin describes itself with a descriptor and
its parameters with `UnitParamInfo` (`clap_param_info`), and parameter changes arrive as
`UnitParamEvent`s (`clap_event_param_value`) with plain values. Where it's simpler than CLAP,
`unit_abi.h` says so. Version 1 (one plugin per file, parameters as a name and a MIDI value) is
refused by this firmware: rebuild the units.

### What a unit is

A unit is one **position-independent image** linked at address 0, plus a list of
**relocations**. The firmware copies the image into a static pool in DDR (at a 4 KiB boundary
on the Pi: its code addresses data with `adrp`, by page), zeroes its bss, adds the load address
to each pointer-sized word the relocation list names (32 bits on the BBB, 64 on the Pi), cleans
the D-cache, invalidates the I-cache and branch predictor, and calls the entry point:

```c
const UnitFactory * unit_entry(const UnitHostApi * api);

typedef struct UnitFactory {
    uint32_t size;                                      // sizeof(UnitFactory)
    uint32_t (*getPluginCount)(void);
    const UnitPlugin * (*getPlugin)(uint32_t index);
} UnitFactory;
```

`UNIT_BUNDLE("Name", a_plugin, b_plugin)` defines `unit_entry` and the factory: it calls each
listed `UnitPluginEntry` function (one-time setup, then return the plugin, or NULL to leave it
out) and names the file. `unit.mk` generates that line from the Makefile, so a plugin's source
only defines its entry function and can go into any bundle.

There is no dynamic linking. A unit can't call firmware functions or the C library by name. It
gets exactly two things from the host:

- `UnitHostApi`, passed to `unit_entry` (and on to the plugin entry functions): `log()`,
  `allocate()` (memory for the rest of the unit's life, from a shared 64 MB heap) and read-only
  SD card access (`fileSize`, `readFile`, `readDirectory`). It's a versioned table: new services
  are appended, and units check `UNIT_HOST_HAS(api, field)` before using one, so a unit still
  loads on older firmware. File access is only allowed from `unit_entry()` and
  `createInstance()`; the firmware refuses it while units' `process()` functions run.
- `UnitProcessHost`, passed to every `process()` call: transport (`play`/`stop`/`requestTempo`),
  `sendMIDIEvent`, the `UnitTimeInfo` for the block, and `midiIn`/`midiInCount`: the MIDI that
  `[midiroutes]` sends to the unit (`.Launchpad_Mini_MK3 grid`). Fields appended later are
  checked with `UNIT_PROCESS_HOST_HAS(api, field)`.

`mkunit.py` refuses any relocation other than `R_ARM_RELATIVE` / `R_AARCH64_RELATIVE`, so a unit that accidentally
references an outside symbol (`sinf`, `printf`, …) fails at build time, not on the device.

### The plugin interface

`UnitPlugin` is the firmware's `struct AudioPlugin`, spelled out in a standalone header.
`unitloader.c` has `_Static_assert`s on the size and every field offset of each ABI structure
against the firmware's own definitions. If someone changes `plugins.h` in an incompatible way,
the firmware stops compiling instead of crashing units. Since the layouts are identical, loaded
plugins go straight into the plugin table, with no wrapper in the audio interrupt.

| field | |
|---|---|
| `size` | `sizeof(UnitPlugin)` the unit was built with; functions appended later are only called if it's big enough |
| `name`, `vendor`, `version`, `description` | the descriptor; `name` is the type used in `[units]`, `[A-Za-z0-9_]{1,31}`, the others may be NULL |
| `createInstance`, `releaseInstance` | instances (below) |
| `getNumInputs`, `getNumOutputs` | audio channels, `unit:1 .. unit:N` |
| `getParamCount`, `getParamInfo` | the parameters (below) |
| `getParamValue` | an instance's current value of a parameter |
| `paramValueToText`, `paramTextToValue` | optional: `"12.5 ms"`, `"Hall"`; NULL prints/parses a plain number |
| `process` | one audio block, with the parameter events that came in since the last one |

- `process()` runs in the **audio interrupt**, once per audio block (`frameCount` samples per
  channel; use `timeInfo.sampleRate` rather than assuming a block size). Don't block or spin. Floating point and NEON are fine: the IRQ
  handler saves the full VFP/NEON state.
- Unconnected inputs contain silence; outputs start zeroed.
- At most **16 MIDI events per block** are delivered, for all units together. That holds for
  the units' MIDI output, and for incoming MIDI (parameter changes and `midiIn` alike).
- Instances come from `createInstance()`; units keep them in their own static memory (their bss),
  so plan for a fixed maximum, the same as the built-in plugins.
- `releaseInstance()` gives an instance back when a config reload (`cfgapply`) drops its unit;
  `createInstance()` may hand that slot out again. A unit that keeps its name and type across a
  reload keeps its instance and isn't released. `releaseInstance` may be NULL, but then every
  reload that drops the unit loses an instance. Memory from `allocate()` is never freed, so keep
  it with the slot and reuse it (see `yafr/`) rather than allocating again.
- Everything `unit_entry` returns has to stay valid forever: units are never unloaded.

### Parameters

```c
typedef struct UnitParamInfo {
    uint32_t id;            // stable; anything but UNIT_INVALID_ID
    uint32_t flags;         // UNIT_PARAM_IS_*: CLAP's values, plus IS_TRIGGER
    char name[32];          // used in config.txt as <unit>:<name>: [A-Za-z0-9_.-]+
    char module[32];        // group for user interfaces, "tank/damping", or ""
    double minValue, maxValue, defaultValue;
} UnitParamInfo;

typedef struct UnitParamEvent {
    uint32_t time;          // sample offset in the block (0 for now)
    uint32_t id;
    double value;           // plain: minValue..maxValue
} UnitParamEvent;
```

- Values are **plain**, in the parameter's own unit: ClickTrack's `tempo` is 40..240 BPM, YAFR's
  `predelay` 0..50 ms. The firmware keeps them in range and rounds stepped (`IS_STEPPED`,
  `IS_ENUM`) parameters.
- MIDI controllers (`[midimap]`) and `[presets]` still use 0..127: the firmware maps 0 to
  `minValue` and 127 to `maxValue`, linearly. Configs written for ABI 1 keep working, as long as
  a parameter's range covers what 0..127 used to mean.
- `UNIT_PARAM_IS_TRIGGER` (not in CLAP) marks an action rather than a state: a button
  (ClickTrack's `play`, Looper's `record`) or a drum hit whose value is the velocity. It has no
  value for `getParamValue`; a UI shows a button. The built-in buttons act on a full press,
  `value >= 1.0` (cc value 127), as they did on `midiValue == 127`.
- Unlike CLAP, parameters belong to the plugin, not to an instance, and don't change after
  `unit_entry()`: `config.txt` and the web UI refer to them before any instance exists.
- `getParamValue` is called from the main loop and can be interrupted by `process()`: return a
  value `process()` keeps up to date (a `float` can't be torn), don't compute it there.
- A new instance starts at the defaults; the firmware doesn't send them.
- The firmware checks every parameter when the plugin is loaded: a valid name, unique names and
  ids, finite values, `minValue <= defaultValue <= maxValue`. One bad parameter refuses the
  unit file.

The device console exposes them for the web UI (`ccs/wormhole/src/configconsole.c`):
`cfgplugins` prints an `@param` line per parameter, `paramget <unit>` the current values and
`paramset <unit>:<parameter> <value>` sets one in the running config (`120`, or `120 BPM` as
`paramValueToText` writes it).

### Code generation

ARM (not Thumb) code for Cortex-A8, AAPCS with hard float (`-mcpu=cortex-a8 -mfpu=neon
-mfloat-abi=hard -marm`), built `-fpie -ffreestanding`. Only libgcc (`arm/v5te/hard`, the variant
the firmware uses) is linked, for division and the like.

`LIBM := 1` in a unit's Makefile also links newlib's libm (`sinf`, `powf`, ...). It isn't built
position independent, but it needs no relocations mkunit.py refuses: the `arm/v5te` variant has
no `movw`/`movt`, and on the Pi its code reaches data PC-relative. Its errno goes to a stub in
`common/unitrt.c`; build with `-fno-math-errno` so `sqrtf` and friends become instructions.

C++ sources (`.cc`, `.cpp`) are built with the same flags minus `-ffreestanding` (the C++
headers need it off), and with `-fno-rtti -fno-exceptions -fno-threadsafe-statics
-fno-use-cxa-atexit`: no runtime for any of those. Nothing runs static constructors, so mkunit.py
refuses a unit with a non-empty `.init_array`: set up globals in `unit_entry()` or
`createInstance()`. See `metalhead-mutable-units` for a C++ bundle.

### File format (`.unt`)

```
UnitFileHeader   64 bytes  magic "UNT1", abiVersion, headerSize, imageSize, bssSize,
                           relocCount, entryOffset, crc32, machine, name[32]
image            imageSize bytes, starts with UnitImageHeader ("UNIT", abiVersion, name)
relocations      relocCount × uint32 offsets into the image
```

All little endian. The CRC-32 (`zlib.crc32`) covers everything after the file header. The name is
the unit file's (`BUNDLE` in the Makefile): the device stores it as `/units/<name>.unt`. Its
plugins have their own names. Names are `[A-Za-z0-9_]{1,31}`.

### What the loader checks

Magic and header size, an exact ABI version match, consistent sizes, the CRC, the entry point,
the image header against the file header, and that every relocation is in range. After calling
`unit_entry` it checks that the factory, every plugin, their strings and their function pointers
lie inside the unit's own image, and the parameters (above). A plugin whose name is already
loaded (built in, or from another file) fails the file. A file's plugins are loaded all or none.
A file whose name is already loaded isn't loaded twice: replacing a unit takes a reboot.

### Versioning

- Adding a service at the end of `UnitHostApi`, a field at the end of `UnitProcessHost`, or a
  function at the end of `UnitPlugin` or `UnitFactory`: **same** ABI version (units check
  `api->size`, the firmware checks `plugin->size`).
- Changing or reordering any structure, a calling convention, or the meaning of a field: bump
  `UNIT_ABI_VERSION`, and rebuild the units. The loader refuses mismatched units, and the web UI
  checks the version before uploading.
- Both boards share one ABI (the loader is `ccs/wormhole/src/unitloader.c`, compiled for each).
  A new version ships in a firmware image for the BBB and one for the Pi at the same time; a board
  left behind refuses every unit built after it.

## Writing a unit

```c
#include <unit_abi.h>

static const UnitHostApi * api;

enum { PARAM_GAIN = 1 };
static const UnitParamInfo parameters[] = {
    { PARAM_GAIN, UNIT_PARAM_IS_AUTOMATABLE, "gain", "", -60, 12, 0 },     // dB
};

static int getParamInfo(uint32_t index, UnitParamInfo * info) {
    if (index >= 1) return 0;
    *info = parameters[index];
    return 1;
}
// ... createInstance, getParamValue, process (reads paramEvents[i].id / .value)

static const UnitPlugin plugin = {
    .size = sizeof(UnitPlugin),
    .name = "MyGain",                   // type name used in [units]
    .getParamInfo = getParamInfo,
    /* ... every function that isn't optional ... */
};

const UnitPlugin * mygain_plugin(const UnitHostApi * hostApi) {
    api = hostApi;
    return &plugin;
}
```

Makefile:

```make
NAME := mygain                  # build/<machine>/mygain.unt
BUNDLE := MyGain                # /units/MyGain.unt on the device
PLUGINS := mygain_plugin        # the plugins' entry functions, in order
SRCS := mygain.c                # sources may come from other unit directories
include ../unit.mk
```

A plugin for one board only goes in `PLUGINS_arm32` or `PLUGINS_aarch64`, with its sources in
`SRCS_arm32` or `SRCS_aarch64` (`metalhead-mutable-units` keeps the heavy modules off the BeagleBone this way).
`make test` runs the plugins of both lists.

`make disasm-arm32` shows the code with relocations. `make test` builds the sources for the Mac
with the address and undefined-behaviour sanitizers and runs `tools/unittest.c` on them: it
checks the factory, the descriptors and parameters, sets every parameter to its minimum, maximum
and default and expects `getParamValue` to report them back, presses the triggers, and releases
and recreates an instance (`TEST_ARGS="-v -r <folder>"` lists the parameters and serves files
from a folder; `-w <folder>` renders each plugin to a WAV there, playing notes into `midiIn` and a
plucked sawtooth into its inputs, in blocks of 1 to 64 frames). metalhead-firmware's `ccs/make-all.sh` builds every unit directory listed in it.

### Units in their own repository

A unit doesn't have to live in this SDK: its Makefile only has to include `unit.mk` from a
checkout of it (`unit.mk` finds `include/`, `unit.ld` and `tools/` next to itself), for example
with the SDK as a submodule:

```make
SDK ?= sdk                      # git submodule add <metalhead-unit-sdk url> sdk
NAME := mine
BUNDLE := Mine
PLUGINS := mine_plugin
SRCS := mine.c
include $(SDK)/unit.mk
```

`tools/upload_units.py --dir <path>` (or `METALHEAD_UNIT_DIRS=<path>:<path>`) builds and uploads
such units together with the SDK's; the path is the unit directory or a directory of them.

## Upload protocol

The web UI sends `unitput <bytes>`, the file as base64 lines (768 characters each), then
`unitend`. The device validates and saves the file, then loads it if no unit with that name is
loaded yet. Its reply is `@unitput ok <name> loaded|unchanged|reboot` or
`@unitput error <reason>`. `unitls` lists `/units` with the plugins of each loaded file, and
`unitrm <name>` deletes a file. See metalhead-firmware's `ccs/wormhole/src/configconsole.c`.

## Limits (for now)

- 8 MB pool for all units' code and static data, 2 MB per file, 16 unit files, 32 plugins and
  1024 parameters in total; 64 MB heap for `allocate()`.
- Units are never unloaded, so replacing one, or freeing its instances, takes a reboot.
- Files are read-only, and only while loading. No network or USB access from a unit; only what
  the two tables provide.
- Parameter changes arrive at the start of the block (`time` is 0), and units can't report
  parameter changes of their own (CLAP's output events) yet.
