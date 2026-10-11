/*
 * unit_abi.h — binary interface between the wormhole firmware and loadable units.
 *
 * A unit is an audio/MIDI plugin built separately from the firmware (see ccs/units/README.md)
 * and loaded at runtime from /units/<name>.unt on the SD card. This header is the whole
 * contract: a unit includes nothing from the firmware, and the firmware checks at compile
 * time (unitloader.c) that these structures match its own plugin structures.
 *
 * Rules for changing this file:
 *   - Never change the layout of an existing structure or the meaning of a field.
 *   - New host services are appended to UnitHostApi (units check api->size before use).
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

#define UNIT_ABI_VERSION        1

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

// A parameter change, produced from the [midimap] section of config.txt.
typedef union UnitParameterChange {
    struct {
        uint8_t unitIndex;
        uint16_t parameterId;   // as returned by getParameterId()
        uint8_t midiValue;      // raw 0..127 value of the mapped MIDI message
        float value;            // midiValue / 127
    };
    uint64_t _mem;
} __attribute__((packed)) UnitParameterChange;

// --- Host -----------------------------------------------------------------------------

typedef struct UnitTimeInfo {
    uint64_t microSeconds;      // device clock at the start of this block
    float sampleRate;
    int isPlaying;              // transport running (Ableton Link / MIDI clock timeline)
    float barStartPos;          // in quarter notes
    float ppqPos;               // position in quarter notes
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
} UnitProcessHost;

// --- The plugin a unit provides ---------------------------------------------------------

typedef struct UnitPlugin {
    uint32_t vendorId;
    void (*getName)(char * name, size_t maxLength);         // the type name used in [units]
    void * (*createInstance)(void);                         // NULL if no instance is available

    uint8_t (*getNumInputs)(void);                          // audio channels, unit:1 .. unit:N
    uint8_t (*getNumOutputs)(void);
    uint8_t (*getNumParameters)(void);
    uint16_t (*getParameterId)(size_t index);
    void (*getParameterName)(size_t index, char * name, size_t maxLength);

    // Called once per audio block (one USB frame, frameCount samples per channel).
    // Unconnected inputs contain silence; outputs start zeroed.
    void (*process)(void * instance, UnitProcessHost * host,
                    size_t nrInChannels, float ** inSamples,
                    size_t nrOutChannels, float ** outSamples,
                    size_t frameCount,
                    UnitParameterChange * parameterChanges, size_t parameterChangeCount);
} UnitPlugin;

// --- Services the firmware offers to units ----------------------------------------------

typedef struct UnitHostApi {
    uint32_t abiVersion;        // UNIT_ABI_VERSION of the firmware
    uint32_t size;              // sizeof(UnitHostApi) in the firmware; fields beyond it are absent

    // printf-style logging to the device console (newlib vsnprintf, so %f works). Safe from
    // process(): messages are queued and printed from the main loop; longer than 255
    // characters is truncated, and messages are dropped when the queue (64) is full.
    void (*log)(const char * format, ...);
} UnitHostApi;

// --- Entry point ------------------------------------------------------------------------

// Every unit defines this function. It is called once, after the unit has been loaded and
// relocated; the returned plugin (and everything it points to) must stay valid forever.
// The api pointer stays valid too and may be kept.
const UnitPlugin * unit_entry(const UnitHostApi * api);

// --- Image header -------------------------------------------------------------------------

#define UNIT_IMAGE_MAGIC        0x54494E55u     // "UNIT"

// First bytes of every unit image; put there by UNIT_DECLARE and the unit linker script.
typedef struct UnitImageHeader {
    uint32_t magic;
    uint32_t abiVersion;
    char name[UNIT_NAME_LENGTH];
} UnitImageHeader;

#define UNIT_DECLARE(typeName) \
    const UnitImageHeader __unit_header __attribute__((section(".unit_header"), used)) = { \
        UNIT_IMAGE_MAGIC, UNIT_ABI_VERSION, typeName }

// --- File format (.unt) -------------------------------------------------------------------
//
//   UnitFileHeader      64 bytes
//   image               imageSize bytes, linked at address 0, starts with a UnitImageHeader
//   relocations         relocCount little-endian uint32 offsets into the image; the loader
//                       adds the load address to the 32-bit word at each offset
//
// After loading, bssSize zero bytes follow the image. crc32 (IEEE 802.3, as zlib.crc32) covers
// everything after the file header.

#define UNIT_FILE_MAGIC         0x31544E55u     // "UNT1"

typedef struct UnitFileHeader {
    uint32_t magic;
    uint16_t abiVersion;
    uint16_t headerSize;        // sizeof(UnitFileHeader) == 64
    uint32_t imageSize;
    uint32_t bssSize;
    uint32_t relocCount;
    uint32_t entryOffset;       // offset of unit_entry in the image
    uint32_t crc32;
    uint32_t reserved;
    char name[UNIT_NAME_LENGTH];
} UnitFileHeader;

#endif /* UNIT_ABI_H_ */
