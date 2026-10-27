# Loadable units

A **unit** is an audio/MIDI plugin built separately from the firmware, on the laptop, and loaded
by the running device. Upload one with the web UI (`host/configui`, *Units* tab); it is stored as
`/units/<TypeName>.unt` on the SD card next to `config.txt` and loaded at every boot, before
`config.txt` is read. After that it's used like a built-in plugin:

```
[units]
vu VUMeter
```

```sh
cd ccs/units/vumeter && make        # → build/arm32/vumeter.unt   (BeagleBone Black)
                                    #   build/aarch64/vumeter.unt (Raspberry Pi 5)
```

A unit binary is built for one board: upload the one for yours. The other board refuses it
("built for 32-bit ARM (BeagleBone Black), this is 64-bit ARM (Raspberry Pi 5)") instead of
running it. `make arm32` or `make aarch64` builds just one.

| path | |
|---|---|
| `include/unit_abi.h` | the ABI — the only header a unit includes |
| `unit.mk`, `unit.ld` | shared build rules and linker script |
| `common/unitrt.c` | `memcpy`/`memset`/`memmove`/`memcmp` linked into every unit |
| `tools/mkunit.py` | ELF → `.unt` converter |
| `doc/unit-abi-overview.pdf` | printable ABI overview; regenerate with `tools/abi_overview.py` (needs reportlab) |
| `vumeter/` | example: level meters on a Launchpad Mini MK3 |
| `drums/` | example: drum sampler playing WAV files from `/samples/drums` (built-in kit as fallback) |
| `yafr/` | example: plate-style reverb, mono in / stereo out (port of the old built-in YAFR plugin) |
| `clipgrid/` | Ableton-style clip launcher on a Launchpad Mini MK3: 8 tracks × 7 clips, after-the-fact capture; reads the Launchpad through `midiIn` (config in `clipgrid/clipgrid.txt`) |
| `../wormhole/src/unitloader.c` | the firmware side |

## ABI (version 1)

### What a unit is

A unit is one **position-independent image** linked at address 0, plus a list of
**relocations**. The firmware copies the image into a static pool in DDR (at a 4 KiB boundary
on the Pi: its code addresses data with `adrp`, by page), zeroes its bss, adds the load address
to each pointer-sized word the relocation list names (32 bits on the BBB, 64 on the Pi), cleans
the D-cache, invalidates the I-cache and branch predictor, and calls the entry point:

```c
const UnitPlugin * unit_entry(const UnitHostApi * api);
```

There is no dynamic linking. A unit can't call firmware functions or the C library by name. It
gets exactly two things from the host:

- `UnitHostApi`, passed to `unit_entry`: `log()`, `allocate()` (memory for the rest of the
  unit's life, from a shared 64 MB heap) and read-only SD card access (`fileSize`, `readFile`,
  `readDirectory`). It's a versioned table: new services are appended, and units check
  `UNIT_HOST_HAS(api, field)` before using one, so a unit still loads on older firmware. File
  access is only allowed from `unit_entry()` and `createInstance()`; the firmware refuses it
  while units' `process()` functions run.
- `UnitProcessHost`, passed to every `process()` call: transport (`play`/`stop`/`requestTempo`),
  `sendMIDIEvent`, the `UnitTimeInfo` for the block, and `midiIn`/`midiInCount`: the MIDI that
  `[midiroutes]` sends to the unit (`.Launchpad_Mini_MK3 grid`). `midiIn` was appended later:
  check `UNIT_PROCESS_HOST_HAS(api, midiIn)` before reading it.

`mkunit.py` refuses any relocation other than `R_ARM_RELATIVE` / `R_AARCH64_RELATIVE`, so a unit that accidentally
references an outside symbol (`sinf`, `printf`, …) fails at build time, not on the device.

### The plugin interface

`UnitPlugin` is the firmware's `struct AudioPlugin`, spelled out in a standalone header:
`getName`, `createInstance`, `getNumInputs/Outputs/Parameters`, `getParameterId/Name`,
`process` and `releaseInstance`. `unitloader.c` has `_Static_assert`s on the size and every field offset of each ABI
structure against the firmware's own definitions. If someone changes `plugins.h` in an
incompatible way, the firmware stops compiling instead of crashing units. Since the layouts are
identical, loaded units go straight into the plugin table, with no wrapper in the audio
interrupt.

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

### Code generation

ARM (not Thumb) code for Cortex-A8, AAPCS with hard float (`-mcpu=cortex-a8 -mfpu=neon
-mfloat-abi=hard -marm`), built `-fpie -ffreestanding`. Only libgcc (`arm/v5te/hard`, the variant
the firmware uses) is linked, for division and the like. There's no libm: compute tables at
build time, or bring your own approximations.

### File format (`.unt`)

```
UnitFileHeader   64 bytes  magic "UNT1", abiVersion, headerSize, imageSize, bssSize,
                           relocCount, entryOffset, crc32, reserved, name[32]
image            imageSize bytes, starts with UnitImageHeader ("UNIT", abiVersion, name)
relocations      relocCount × uint32 offsets into the image
```

All little endian. The CRC-32 (`zlib.crc32`) covers everything after the file header. The name is
the plugin type name used in `[units]` and must match what `getName()` returns. Names are
`[A-Za-z0-9_]{1,31}`.

### What the loader checks

Magic and header size, an exact ABI version match, consistent sizes, the CRC, the entry point,
the image header against the file header, and that every relocation is in range. After calling
`unit_entry` it also checks that the plugin and all its function pointers lie inside the unit's
own image, and that `getName()` matches. A unit whose name is already loaded isn't loaded twice:
replacing a unit takes a reboot.

### Versioning

- Adding a service at the end of `UnitHostApi`: **same** ABI version (units check `api->size`).
- Changing or reordering any structure, a calling convention, or the meaning of a field: bump
  `UNIT_ABI_VERSION`, and rebuild the units. The loader refuses mismatched units, and the web UI
  checks the version before uploading.

## Writing a unit

```c
#include <unit_abi.h>

UNIT_DECLARE("MyUnit");                 // type name used in [units]

static const UnitHostApi * api;
static const UnitPlugin plugin = { .getName = ..., .process = ..., /* all fields */ };

const UnitPlugin * unit_entry(const UnitHostApi * hostApi) {
    api = hostApi;
    return &plugin;
}
```

Makefile:

```make
NAME := myunit
SRCS := myunit.c
include ../unit.mk
```

`make disasm` shows the code with relocations. `ccs/make-all.sh` builds every unit directory
listed in it.

## Upload protocol

The web UI sends `unitput <bytes>`, the file as base64 lines (768 characters each), then
`unitend`. The device validates and saves the file, then loads it if no unit with that name is
loaded yet. Its reply is `@unitput ok <name> loaded|unchanged|reboot` or
`@unitput error <reason>`. `unitls` lists `/units`, and `unitrm <name>` deletes a file. See
`ccs/wormhole/src/configconsole.c`.

## Limits (for now)

- 2 MB pool for all units' code and static data, 256 KB per file, 16 units, 32 plugins in total;
  64 MB heap for `allocate()`.
- Units are never unloaded, so replacing one, or freeing its instances, takes a reboot.
- Files are read-only, and only while loading. No network or USB access from a unit; only what
  the two tables provide.
