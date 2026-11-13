/*
 * unit_abi.h — binary interface between the metalhead firmware and loadable units.
 *
 * A unit file is a bundle of audio/MIDI plugins built separately from the firmware (see
 * README.md of metalhead-unit-sdk) and loaded at runtime from /units/<name>.unt on the SD card. This
 * header is the whole contract: a unit includes nothing from the firmware, and the firmware
 * checks at compile time (unitloader.c) that these structures match its own plugin structures.
 *
 * Plugins and their parameters are modelled on CLAP (https://github.com/free-audio/clap):
 * UnitFactory is clap_plugin_factory, UnitParamInfo and the parameter functions of UnitPlugin
 * are the params extension (clap/ext/params.h), UnitParamEvent is clap_event_param_value.
 * Where this ABI is simpler, the comments say so.
 *
 * Rules for changing this file:
 *   - Never change the layout of an existing structure or the meaning of a field.
 *   - New host services are appended to UnitHostApi (units check api->size before use), new
 *     plugin functions to UnitPlugin (the firmware checks plugin->size).
 *   - Anything else is a new UNIT_ABI_VERSION; the loader refuses units built for another one.
 *
 * Calling convention: AAPCS with hard float (VFP/NEON registers), ARM (not Thumb) code for
 * Cortex-A8. process() runs in the audio interrupt; units must not block, and may use
 * floating point and NEON freely (the IRQ handler saves the full VFP/NEON state).
 */
#ifndef UNIT_ABI_H_
#define UNIT_ABI_H_

#include <stddef.h>
#include <stdint.h>

// 1: one plugin per file, parameters were a name and a MIDI value
// 2: bundles (UnitFactory), CLAP-style parameters
#define UNIT_ABI_VERSION        2

#define UNIT_NAME_LENGTH        32      // including the terminating NUL

// --- MIDI -----------------------------------------------------------------------------

// One USB-MIDI event packet (USB Device Class Definition for MIDI Devices 1.0, section 4).
// header = (cable number << 4) | code index number. Cable 0 means "the default jack of the
// device this unit's MIDI output is routed to"; SysEx is sent as CIN 0x4 / 0x5-0x7 packets.
typedef struct UnitMidiPacket {
    uint8_t header;
    uint8_t midi0;
    uint8_t midi1;
    uint8_t midi2;
} __attribute__((packed)) UnitMidiPacket;

#define UNIT_MIDI_CIN_SYSEX_START       0x4     // also: SysEx continues
#define UNIT_MIDI_CIN_SYSEX_END_1BYTE   0x5
#define UNIT_MIDI_CIN_SYSEX_END_2BYTE   0x6
#define UNIT_MIDI_CIN_SYSEX_END_3BYTE   0x7
#define UNIT_MIDI_CIN_NOTE_OFF          0x8
#define UNIT_MIDI_CIN_NOTE_ON           0x9
#define UNIT_MIDI_CIN_CONTROL_CHANGE    0xB

// --- Parameters -----------------------------------------------------------------------
//
// A plugin describes its parameters with getParamInfo(); the firmware sends them to process()
// as UnitParamEvents with plain values in minValue..maxValue (a tempo in BPM, a time in ms).
// Values from MIDI controllers and [presets] (0..127) are mapped linearly onto that range,
// and rounded for stepped parameters: 0 is minValue, 127 is maxValue.
//
// Unlike CLAP, parameters belong to the plugin, not to an instance: every instance has the
// same ones, and they are fixed once unit_entry() has returned (config.txt and the web UI
// refer to them before any instance exists).

// Flags: the values of CLAP_PARAM_IS_*
#define UNIT_PARAM_IS_STEPPED       (1u << 0)   // integer values only; with IS_ENUM, a choice
#define UNIT_PARAM_IS_PERIODIC      (1u << 1)   // wraps around, like a phase
#define UNIT_PARAM_IS_HIDDEN        (1u << 2)   // not shown in user interfaces
#define UNIT_PARAM_IS_READONLY      (1u << 3)   // the plugin's output, the host doesn't set it
#define UNIT_PARAM_IS_BYPASS        (1u << 4)   // the plugin's bypass switch
#define UNIT_PARAM_IS_AUTOMATABLE   (1u << 5)
#define UNIT_PARAM_IS_ENUM          (1u << 16)  // stepped, and paramValueToText names the values
// Not in CLAP: every event is an action (a button press, a drum hit with the value as its
// velocity) rather than a new state. A user interface shows a button and nothing is saved.
#define UNIT_PARAM_IS_TRIGGER       (1u << 24)

#define UNIT_PARAM_NAME_LENGTH      32  // including the terminating NUL
#define UNIT_PARAM_MODULE_LENGTH    32

#define UNIT_INVALID_ID             0xFFFFFFFFu

// clap_param_info without the cookie, with shorter strings.
typedef struct UnitParamInfo {
    uint32_t id;                // stable across versions of the plugin; not UNIT_INVALID_ID
    uint32_t flags;             // UNIT_PARAM_IS_*
    // Used in config.txt (<unit>:<name>) as well as shown: [A-Za-z0-9_.-]+, unique in the plugin
    char name[UNIT_PARAM_NAME_LENGTH];
    // Group for user interfaces, '/' separated ("tank/damping"), or "" for none
    char module[UNIT_PARAM_MODULE_LENGTH];
    double minValue;
    double maxValue;
    double defaultValue;        // what a new instance starts with
} UnitParamInfo;

// A parameter change, delivered to process(). clap_event_param_value without note, port,
// channel and key: parameters here aren't per note.
typedef struct UnitParamEvent {
    uint32_t time;              // sample offset in the block; the firmware sends 0 for now
    uint32_t id;                // UnitParamInfo.id
    double value;               // plain value in minValue..maxValue
} UnitParamEvent;

// --- Host -----------------------------------------------------------------------------

typedef struct UnitTimeInfo {
    uint64_t microSeconds;      // device clock at the start of this block
    float sampleRate;
    int isPlaying;              // transport running (Ableton Link / MIDI clock timeline)
    double barStartPos;         // in quarter notes
    double ppqPos;              // position in quarter notes
    float tempo;                // BPM
    int timeSigNumerator;       // quantum
} UnitTimeInfo;

struct UnitHostData;            // opaque

// Passed to process(); valid only during that call.
typedef struct UnitProcessHost {
    void (*play)(void);
    void (*stop)(void);
    void (*requestTempo)(float tempoBpm);
    // Queue a MIDI event on this unit's MIDI output (routed by [midiroutes]). At most 16
    // events are delivered per block for all units together; extra events are dropped.
    void (*sendMIDIEvent)(struct UnitHostData * hostData, UnitMidiPacket packet);
    UnitTimeInfo timeInfo;
    struct UnitHostData * hostData;
    // MIDI routed to this unit by [midiroutes] (e.g. ".Launchpad_Mini_MK3 grid") that arrived
    // since the last block, in order. Shares the 16 events per block limit of incoming MIDI.
    const UnitMidiPacket * midiIn;
    size_t midiInCount;

    // Fields appended later go here: check UNIT_PROCESS_HOST_HAS(api, field) before use.
} UnitProcessHost;

// --- A plugin -----------------------------------------------------------------------------
//
// The plugin is the type ("YAFR" in "[units] verb YAFR"); its instances are the units of the
// configuration. Functions that take no instance describe the type and are called from the
// main loop only.

typedef struct UnitPlugin {
    uint32_t size;              // sizeof(UnitPlugin) the unit was built with

    // clap_plugin_descriptor
    const char * name;          // the type name used in [units]: [A-Za-z0-9_]{1,31}
    const char * vendor;        // may be NULL
    const char * version;       // may be NULL, e.g. "1.2.0"
    const char * description;   // may be NULL; one line

    void * (*createInstance)(void);                         // NULL if no instance is available
    // Gives an instance back when a config reload drops its unit; createInstance() may hand
    // it out again later. NULL if instances are never given back (they leak on reloads).
    void (*releaseInstance)(void * instance);

    uint32_t (*getNumInputs)(void);                         // audio channels, unit:1 .. unit:N
    uint32_t (*getNumOutputs)(void);

    // clap_plugin_params
    uint32_t (*getParamCount)(void);
    int (*getParamInfo)(uint32_t index, UnitParamInfo * info);     // 1 if index is valid
    // The instance's current value. Called from the main loop, which process() may interrupt:
    // read a value process() keeps up to date (a float or an aligned 32-bit word, so it can't
    // be torn); don't compute it. Return 0 for a trigger or an unknown id.
    int (*getParamValue)(void * instance, uint32_t id, double * value);
    // Optional (may be NULL, or return 0 to let the firmware print/parse a plain number).
    int (*paramValueToText)(uint32_t id, double value, char * text, uint32_t size);   // "-6.0 dB"
    int (*paramTextToValue)(uint32_t id, const char * text, double * value);

    // Called once per audio block (one USB frame, frameCount samples per channel).
    // Unconnected inputs contain silence; outputs start zeroed. Parameter events come first,
    // in the order they happened; apply them before processing the block.
    void (*process)(void * instance, UnitProcessHost * host,
                    size_t nrInChannels, float ** inSamples,
                    size_t nrOutChannels, float ** outSamples,
                    size_t frameCount,
                    const UnitParamEvent * paramEvents, size_t paramEventCount);

    // Functions appended later go here; the firmware checks size before calling them.
} UnitPlugin;

// --- Services the firmware offers to units ----------------------------------------------

typedef struct UnitDirectoryEntry {
    char name[64];
    uint32_t size;
    uint8_t isDirectory;
} UnitDirectoryEntry;

typedef struct UnitHostApi {
    uint32_t abiVersion;        // UNIT_ABI_VERSION of the firmware
    uint32_t size;              // sizeof(UnitHostApi) in the firmware; fields beyond it are absent

    // printf-style logging to the device console (newlib vsnprintf, so %f works). Safe from
    // process(): messages are queued and printed from the main loop; longer than 255
    // characters is truncated, and messages are dropped when the queue (64) is full.
    void (*log)(const char * format, ...);

    // Memory for the rest of the unit's life (never freed), 8-byte aligned and not zeroed.
    // Returns NULL when the unit heap (shared by all units: 256 MB on the Raspberry Pi 5, 192 MB
    // on the BeagleBone Black, 64 MB in firmware before 2026-11-13) is exhausted.
    void * (*allocate)(size_t size);

    // Files on the SD card, by absolute path ("/samples/drums/kick.wav"). Only call these
    // from unit_entry() or createInstance(), never from process(); they block on the SD card
    // and refuse to run in interrupt context. Negative return values are errors.
    int32_t (*fileSize)(const char * path);
    int32_t (*readFile)(const char * path, uint32_t offset, void * buffer, uint32_t length);  // bytes read
    // Directory entry number `index` (0, 1, 2, ...): returns 1 and fills `entry`, 0 after the
    // last entry. Order is the order on disk; names longer than 63 characters are skipped.
    int32_t (*readDirectory)(const char * path, uint32_t index, UnitDirectoryEntry * entry);

    // sizeof(UnitProcessHost) in the firmware: fields of UnitProcessHost beyond it are absent.
    uint32_t processHostSize;

    // Services appended later go here: check UNIT_HOST_HAS(api, field) before use.
} UnitHostApi;

// True if the firmware's UnitHostApi includes `field`.
#define UNIT_HOST_HAS(api, field) \
    ((api)->size >= offsetof(UnitHostApi, field) + sizeof(((UnitHostApi *)0)->field))

// True if the UnitProcessHost the firmware passes to process() includes `field`.
#define UNIT_PROCESS_HOST_HAS(api, field) \
    (UNIT_HOST_HAS(api, processHostSize) && \
     (api)->processHostSize >= offsetof(UnitProcessHost, field) + sizeof(((UnitProcessHost *)0)->field))

// --- The bundle: entry point and factory ----------------------------------------------------

// clap_plugin_factory: the plugins in this unit file. The plugins (and everything they point
// to) must stay valid forever; the count and order must not change after unit_entry().
typedef struct UnitFactory {
    uint32_t size;              // sizeof(UnitFactory) the unit was built with
    uint32_t (*getPluginCount)(void);
    const UnitPlugin * (*getPlugin)(uint32_t index);
} UnitFactory;

// Every unit file defines this function (UNIT_BUNDLE below does). It is called once, after
// the unit has been loaded and relocated. The api pointer stays valid and may be kept.
const UnitFactory * unit_entry(const UnitHostApi * api);

// How a plugin's source file hands its plugin to the bundle: one-time setup (keep the api,
// load samples), then the plugin, or NULL to leave it out of the bundle.
typedef const UnitPlugin * UnitPluginEntry(const UnitHostApi * api);

// Defines unit_entry() and the factory for the plugins whose UnitPluginEntry functions are
// listed, in that order, and names the unit file. In one source file of the unit:
//
//   const UnitPlugin * yafr_plugin(const UnitHostApi * api);
//   UNIT_BUNDLE("Effects", yafr_plugin, chorus_plugin);
#define UNIT_BUNDLE(bundleName, ...) \
    UNIT_DECLARE(bundleName); \
    static UnitPluginEntry * const unit_bundle_entries[] = { __VA_ARGS__ }; \
    static const UnitPlugin * unit_bundle_plugins[sizeof(unit_bundle_entries) / sizeof(unit_bundle_entries[0])]; \
    static uint32_t unit_bundle_count; \
    static uint32_t unit_bundle_get_plugin_count(void) { return unit_bundle_count; } \
    static const UnitPlugin * unit_bundle_get_plugin(uint32_t index) { \
        return index < unit_bundle_count ? unit_bundle_plugins[index] : 0; \
    } \
    static const UnitFactory unit_bundle_factory = { \
        sizeof(UnitFactory), unit_bundle_get_plugin_count, unit_bundle_get_plugin }; \
    const UnitFactory * unit_entry(const UnitHostApi * api) { \
        for (size_t i = 0; i < sizeof(unit_bundle_entries) / sizeof(unit_bundle_entries[0]); i++) { \
            const UnitPlugin * plugin = unit_bundle_entries[i](api); \
            if (plugin) { \
                unit_bundle_plugins[unit_bundle_count++] = plugin; \
            } \
        } \
        return &unit_bundle_factory; \
    }

// --- Image header -------------------------------------------------------------------------

#define UNIT_IMAGE_MAGIC        0x54494E55u     // "UNIT"

// First bytes of every unit image; put there by UNIT_DECLARE (part of UNIT_BUNDLE) and the
// unit linker script. The name is the unit file's: /units/<name>.unt.
typedef struct UnitImageHeader {
    uint32_t magic;
    uint32_t abiVersion;
    char name[UNIT_NAME_LENGTH];
} UnitImageHeader;

#if defined(__APPLE__)
#define UNIT_HEADER_SECTION     "__DATA,__unit_header"  // a test build on the Mac (make test)
#else
#define UNIT_HEADER_SECTION     ".unit_header"
#endif

#define UNIT_DECLARE(bundleName) \
    const UnitImageHeader __unit_header __attribute__((section(UNIT_HEADER_SECTION), used)) = { \
        UNIT_IMAGE_MAGIC, UNIT_ABI_VERSION, bundleName }

// --- File format (.unt) -------------------------------------------------------------------
//
//   UnitFileHeader      64 bytes
//   image               imageSize bytes, linked at address 0, starts with a UnitImageHeader
//   relocations         relocCount little-endian uint32 offsets into the image; the loader
//                       adds the load address to the pointer-sized word at each offset
//                       (32 bits for UNIT_MACHINE_ARM32, 64 bits for UNIT_MACHINE_AARCH64)
//   resources           optional, to the end of the file: a UnitResourceTable (below). Never
//                       loaded into memory; the firmware reads them from the file when asked.
//                       Firmware from before resources refuses a file that has them.
//
// A unit is built for one machine; the loader refuses the other's (the ABI structures hold
// pointers, so their layout differs). unit.mk builds both.
// After loading, bssSize zero bytes follow the image. crc32 (IEEE 802.3, as zlib.crc32) covers
// everything after the file header.

#define UNIT_FILE_MAGIC         0x31544E55u     // "UNT1"

#define UNIT_MACHINE_ARM32      0       // BeagleBone Black: Cortex-A8, ARM state, hard float
#define UNIT_MACHINE_AARCH64    1       // Raspberry Pi 5: Cortex-A76, 64-bit
#if defined(__aarch64__)
#define UNIT_MACHINE            UNIT_MACHINE_AARCH64
#else
#define UNIT_MACHINE            UNIT_MACHINE_ARM32
#endif

typedef struct UnitFileHeader {
    uint32_t magic;
    uint16_t abiVersion;
    uint16_t headerSize;        // sizeof(UnitFileHeader) == 64
    uint32_t imageSize;
    uint32_t bssSize;
    uint32_t relocCount;
    uint32_t entryOffset;       // offset of unit_entry in the image
    uint32_t crc32;
    uint32_t machine;           // UNIT_MACHINE_*; 0 in files from before there was a choice
    char name[UNIT_NAME_LENGTH];
} UnitFileHeader;

// --- Resources (.unt) ---------------------------------------------------------------------
//
// What a unit file brings besides its code, after the relocations:
//
//   UnitResourceTable   8 bytes
//   UnitResourceEntry   48 bytes each, count of them
//   data                each entry's bytes, at its offset from the start of the table
//
// Every entry lies inside the file. Kinds the firmware doesn't know are kept and ignored.
// tools/unitfile.py lists, adds and extracts them; mkunit.py --faceplate adds faceplates.

#define UNIT_RESOURCES_MAGIC    0x53455255u     // "URES"
#define UNIT_RESOURCES_MAX      64              // entries in one file
#define UNIT_RESOURCE_MAX_SIZE  (256 * 1024)    // bytes in one entry

// A plugin's faceplate (doc/faceplates.md): its HTML page, gzip compressed (RFC 1952). The
// firmware hands it out as it is (console `faceplate`), and the page unpacks it.
#define UNIT_RESOURCE_FACEPLATE 1

typedef struct UnitResourceTable {
    uint32_t magic;             // UNIT_RESOURCES_MAGIC
    uint32_t count;
} UnitResourceTable;

typedef struct UnitResourceEntry {
    uint32_t kind;              // UNIT_RESOURCE_*
    uint32_t offset;            // from the start of the UnitResourceTable
    uint32_t size;
    uint32_t reserved;          // 0
    char plugin[UNIT_NAME_LENGTH];  // the plugin of this file it belongs to, NUL terminated
} UnitResourceEntry;

#endif /* UNIT_ABI_H_ */
