/*
 * VUMeter — level meters on a Novation Launchpad Mini MK3 (programmer mode).
 *
 * Eight audio inputs, one per column of the 8x8 grid (input 1 = left column). Rows light up
 * from the bottom at -48, -36, -24, -18, -12, -6, -3 and -0.5 dBFS, with a 1 second peak
 * hold. Route the unit's MIDI output to the Launchpad:
 *
 *   [units]
 *   vu VUMeter
 *   [audioroutes]
 *   .Scarlett_2i2_USB:1 vu:1
 *   .Scarlett_2i2_USB:2 vu:2
 *   [midiroutes]
 *   vu .Launchpad_Mini_MK3
 *
 * The unit puts the Launchpad in programmer mode and repaints it every 2 seconds, so a
 * Launchpad that is plugged in later (or switched to another mode) recovers by itself.
 */
#include <unit_abi.h>

UNIT_DECLARE("VUMeter");

#define NUM_COLUMNS             8
#define NUM_ROWS                8
#define NUM_PADS                (NUM_COLUMNS * NUM_ROWS)
#define MAX_INSTANCES           4

#define MAX_PADS_PER_BLOCK      6       // host delivers at most 16 MIDI events per block
#define RESYNC_INTERVAL_MS      2000.0F
#define PEAK_HOLD_MS            1000.0F

// Per-sample release factor: 20*log10(0.99994) * 48000 = about -25 dB/s at 48 kHz.
#define RELEASE                 0.99994F

// Launchpad palette indices
#define COLOR_OFF               0
#define COLOR_RED               5
#define COLOR_YELLOW            13
#define COLOR_GREEN             21

#define PAD_UNKNOWN             0xFF

// Row thresholds as linear amplitude: 10^(dB/20) for -48 -36 -24 -18 -12 -6 -3 -0.5 dB
static const float rowThreshold[NUM_ROWS] = {
    0.003981F, 0.015849F, 0.063096F, 0.125893F, 0.251189F, 0.501187F, 0.707946F, 0.944061F,
};

static const uint8_t rowColor[NUM_ROWS] = {
    COLOR_GREEN, COLOR_GREEN, COLOR_GREEN, COLOR_GREEN, COLOR_GREEN, COLOR_YELLOW, COLOR_YELLOW, COLOR_RED,
};

struct Instance {
    int active;
    float level[NUM_COLUMNS];
    float peak[NUM_COLUMNS];
    float peakHoldMs[NUM_COLUMNS];
    uint8_t shown[NUM_PADS];            // colour the Launchpad shows, PAD_UNKNOWN if unsure
    float msSinceResync;
    int nextPad;
};

static struct Instance instances[MAX_INSTANCES];
static const UnitHostApi * api;

static void getName(char * name, size_t maxLength) {
    const char * s = __unit_header.name;
    size_t i = 0;
    for (; s[i] && i + 1 < maxLength; i++) {
        name[i] = s[i];
    }
    if (maxLength) {
        name[i] = 0;
    }
}

static void * createInstance(void) {
    for (int i = 0; i < MAX_INSTANCES; i++) {
        struct Instance * inst = &instances[i];
        if (!inst->active) {
            *inst = (struct Instance) { .active = 1, .msSinceResync = RESYNC_INTERVAL_MS };
            return inst;
        }
    }
    api->log("VUMeter: no free instances (max %d)", MAX_INSTANCES);
    return 0;
}

static uint8_t getNumInputs(void) { return NUM_COLUMNS; }
static uint8_t getNumOutputs(void) { return 0; }
static uint8_t getNumParameters(void) { return 0; }
static uint16_t getParameterId(size_t index) { return 0; }
static void getParameterName(size_t index, char * name, size_t maxLength) { if (maxLength) name[0] = 0; }

static void send(UnitProcessHost * host, uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2) {
    host->sendMIDIEvent(host->hostData, (UnitMidiPacket) { cin, b0, b1, b2 });
}

static void enterProgrammerMode(UnitProcessHost * host) {
    // F0h 00h 20h 29h 02h 0Dh 0Eh 01h F7h
    send(host, UNIT_MIDI_CIN_SYSEX_START, 0xF0, 0x00, 0x20);
    send(host, UNIT_MIDI_CIN_SYSEX_START, 0x29, 0x02, 0x0D);
    send(host, UNIT_MIDI_CIN_SYSEX_END_3BYTE, 0x0E, 0x01, 0xF7);
}

static uint8_t padColor(struct Instance * inst, int column, int row) {
    float threshold = rowThreshold[row];
    if (inst->level[column] >= threshold) {
        return rowColor[row];
    }
    // Peak hold: only the highest row the peak reaches stays lit.
    if (inst->peak[column] >= threshold && (row == NUM_ROWS - 1 || inst->peak[column] < rowThreshold[row + 1])) {
        return rowColor[row];
    }
    return COLOR_OFF;
}

static void process(void * instance, UnitProcessHost * host,
                    size_t nrInChannels, float ** inSamples,
                    size_t nrOutChannels, float ** outSamples,
                    size_t frameCount,
                    UnitParameterChange * parameterChanges, size_t parameterChangeCount) {
    struct Instance * inst = instance;
    if (!inst) {
        return;
    }
    float sampleRate = host->timeInfo.sampleRate > 0 ? host->timeInfo.sampleRate : 48000.0F;
    float blockMs = frameCount * 1000.0F / sampleRate;

    // Metering
    for (size_t c = 0; c < NUM_COLUMNS && c < nrInChannels; c++) {
        float level = inst->level[c];
        const float * in = inSamples[c];
        for (size_t i = 0; i < frameCount; i++) {
            float x = in[i] < 0 ? -in[i] : in[i];
            level *= RELEASE;
            if (x > level) {
                level = x;
            }
        }
        inst->level[c] = level;

        if (level >= inst->peak[c]) {
            inst->peak[c] = level;
            inst->peakHoldMs[c] = PEAK_HOLD_MS;
        } else if ((inst->peakHoldMs[c] -= blockMs) <= 0) {
            inst->peak[c] = level;
        }
    }

    // Periodically (re)enter programmer mode and repaint everything.
    int budget = MAX_PADS_PER_BLOCK;
    inst->msSinceResync += blockMs;
    if (inst->msSinceResync >= RESYNC_INTERVAL_MS) {
        inst->msSinceResync = 0;
        enterProgrammerMode(host);
        for (int p = 0; p < NUM_PADS; p++) {
            inst->shown[p] = PAD_UNKNOWN;
        }
        budget -= 3;
    }

    // Send the pads that changed, scanning round-robin so no pad starves.
    for (int scanned = 0; scanned < NUM_PADS && budget > 0; scanned++) {
        int p = inst->nextPad;
        inst->nextPad = (p + 1) % NUM_PADS;
        int column = p % NUM_COLUMNS;
        int row = p / NUM_COLUMNS;
        uint8_t color = padColor(inst, column, row);
        if (inst->shown[p] != color) {
            // Programmer mode pad numbers: row 1 is the bottom row, pad = row * 10 + column
            uint8_t note = (row + 1) * 10 + (column + 1);
            send(host, UNIT_MIDI_CIN_NOTE_ON, 0x90, note, color);
            inst->shown[p] = color;
            budget--;
        }
    }
}

static const UnitPlugin plugin = {
    .vendorId = 0,
    .getName = getName,
    .createInstance = createInstance,
    .getNumInputs = getNumInputs,
    .getNumOutputs = getNumOutputs,
    .getNumParameters = getNumParameters,
    .getParameterId = getParameterId,
    .getParameterName = getParameterName,
    .process = process,
};

const UnitPlugin * unit_entry(const UnitHostApi * hostApi) {
    api = hostApi;
    api->log("VUMeter unit ready (%d inputs)", NUM_COLUMNS);
    return &plugin;
}
