/*
 * ClipGrid — an Ableton-style clip launcher on a Novation Launchpad Mini MK3 (programmer mode).
 *
 * Eight tracks, one per column of the 8x8 grid. Track N records audio input N and plays its
 * clips on audio output N (mono). The top seven rows hold the clip slots (row 8 is slot 1);
 * the clips in a column are mutually exclusive: launching one stops the one that was playing.
 *
 *   empty pad          press: arm it (red); one armed slot per column, press again to disarm
 *   armed pad          press a capture button: the last bars of the track's input become a clip
 *   capturing pad      blinks red until the capture's last bar is complete; press to cancel
 *   stopped clip       yellow; press: launches at the next bar (blinks green until then)
 *   playing clip       green, brightness following the track's level; press: stops at the next bar
 *
 * The bottom row holds the capture buttons, recording "after the fact" like the Looper unit:
 * 1, 2, 3, 4, 6, 8, 12 and 16 bars. They capture into every armed slot at once. Pressed in
 * the last beat of a bar, the capture ends at the next bar; otherwise it ends at the start of
 * the current bar and the clip starts playing right away.
 *
 * The round buttons: top row stops a column (at the next bar), the right column launches a
 * scene (the row of clips next to it; empty slots stop their column), and the bottom one
 * (Stop Solo Mute) is a delete modifier: hold it and press a clip to delete it.
 *
 * Every column has a volume parameter (volume1..volume8). Clips follow the transport (Ableton
 * Link / MIDI clock) and are silent while it is stopped. Route the Launchpad to the unit and
 * the unit back to the Launchpad (see clipgrid.txt):
 *
 *   [units]
 *   grid ClipGrid
 *   [midiroutes]
 *   .Launchpad_Mini_MK3 grid
 *   grid .Launchpad_Mini_MK3
 *
 * Memory: every column continuously records its input into a 16-bit history ring made of
 * chunks from a shared pool. A capture doesn't copy audio: the clip takes a reference to the
 * history chunks that hold its bars, and the history swaps in a fresh chunk when it wraps
 * around to a chunk a clip holds. Clips use memory in proportion to their length, and the
 * longest capture is the history length (about 43 seconds; 16 bars of 4/4 down to 90 BPM).
 */
#include <unit_abi.h>

#define MAX_INSTANCES           1       // an instance owns the Launchpad and most of the unit heap

#define NUM_COLUMNS             8
#define NUM_SLOTS               7       // grid rows 8 (slot 0) down to 2; row 1 holds the capture buttons

// Audio memory: 16-bit samples in chunks of 2^CHUNK_SHIFT samples
#define CHUNK_SHIFT             14
#define CHUNK_SAMPLES           (1 << CHUNK_SHIFT)
#define CHUNK_MASK              (CHUNK_SAMPLES - 1)
#define MAX_HISTORY_CHUNKS      128     // per column: 43.3 s at 48 kHz
#define MAX_POOL_CHUNKS         2048
#define NO_CHUNK                (-1)

// Pool sizes to try, largest first: the unit heap (64 MB on older firmware) is shared with other units.
static const uint16_t poolSizesMB[] = { 56, 48, 40, 32, 24, 16, 8 };

#define LOOP_FADE_SECONDS       0.02    // crossfade at the loop end with the audio before its start
#define SWITCH_FADE_SECONDS     0.01    // crossfade when a column switches or stops clips
#define VOLUME_RAMP_SECONDS     0.02    // time for a full-scale volume change

// Loop playback follows the transport clock (the same correction as the built-in Looper)
#define SYNC_FILTER_SECONDS     0.1     // low-pass on the jittery per-block phase error
#define SYNC_CORRECTION_SECONDS 2.0     // time constant for pulling the phase error back to zero
#define SYNC_MAX_CORRECTION     0.001   // max playback rate deviation, +-0.1%
#define SYNC_RESYNC_SECONDS     0.05    // jump instead of glide when further off than this

// Launchpad LEDs
#define NUM_GRID_LEDS           64
#define NUM_LEDS                (NUM_GRID_LEDS + 8 + 8)     // grid, top row, right column
#define LED_MESSAGES_PER_MS     4.0F    // host delivers at most 16 MIDI events per block
#define LED_MAX_CREDIT          6.0F
#define RESYNC_INTERVAL_MS      2000.0F
#define LED_UNKNOWN             0xFFFF
#define LED_RGB                 0x100   // + brightness step: green set with an RGB SysEx (5 packets)

// Playing clips' pads show their track's level: a peak meter, read METER_UPDATE_MS apart
// so a pad changes at most ~33 times a second, in 3 dB steps from -36 dB
#define METER_UPDATE_MS         30.0F
#define METER_RELEASE           0.9998F // per sample: about -80 dB/s at 48 kHz
#define METER_STEPS             12
#define METER_MIN_GREEN         10      // RGB green (0..127) of a playing clip in silence
static const float meterThreshold[METER_STEPS] = {     // 10^(dB/20), -36 dB .. -3 dB
    0.015849F, 0.022387F, 0.031623F, 0.044668F, 0.063096F, 0.089125F,
    0.125893F, 0.177828F, 0.251189F, 0.354813F, 0.501187F, 0.707946F,
};

// Launchpad palette indices
#define COLOR_OFF               0
#define COLOR_DIM_WHITE         1
#define COLOR_RED               5
#define COLOR_DIM_RED           7
#define COLOR_YELLOW            13
#define COLOR_GREEN             21
#define COLOR_DIM_GREEN         23

// Bar counts of the capture buttons, bottom row left to right
static const uint8_t captureBars[NUM_COLUMNS] = { 1, 2, 3, 4, 6, 8, 12, 16 };

// Launchpad programmer mode: pads send note on (row * 10 + column, row 1 at the bottom), the
// round buttons send control changes (127 pressed, 0 released)
#define CC_STOP_FIRST           91      // top row, 91..98 left to right
#define CC_DELETE               19      // right column, bottom (Stop Solo Mute); x9 above it are scenes

#define PARAM_VOLUME            500     // + column (1..8)
#define NUM_PARAMETERS          NUM_COLUMNS

enum ClipState {
    CLIP_EMPTY,
    CLIP_ARMED,
    CLIP_CAPTURING,     // waiting for the end of the capture's last bar
    CLIP_FILLED,
    CLIP_DELETING,      // deleted while fading out; its chunks are released after the fade
};

#define QUEUE_NONE              (-1)
#define QUEUE_STOP              (-2)

struct Clip {
    uint8_t state;
    uint32_t length;            // loop length in samples
    uint32_t fade;              // samples before the loop start, for the crossfade at its end
    uint32_t regionOffset;      // offset of the clip's first sample (fade included) in chunks[0]
    uint16_t chunkCount;
    int16_t chunks[MAX_HISTORY_CHUNKS];
    uint64_t endSample;         // capturing: sample count at which the loop ends
    double lengthPpq;
    double startPpq;            // a timeline position where the loop starts
    double position;            // playback position in samples, 0 <= position < length
    double phaseErrorFiltered;  // in samples
};

struct Column {
    struct Clip clips[NUM_SLOTS];
    int8_t current;             // playing slot, -1 if none
    int8_t fading;              // slot fading out after a switch, -1 if none
    int8_t queued;              // slot to launch at queuedAtPpq, QUEUE_STOP or QUEUE_NONE
    double queuedAtPpq;
    uint32_t fadePos;           // progress of the switch crossfade
    uint32_t fadeLength;
    float volume;
    float volumeTarget;
    int16_t history[MAX_HISTORY_CHUNKS];                // chunk holding history chunk number n % historyChunks
    uint32_t historyChunkNumber[MAX_HISTORY_CHUNKS];    // which chunk number (sample >> CHUNK_SHIFT) it holds
};

struct Instance {
    int active;
    int16_t * pool;
    uint32_t poolChunks;
    uint32_t historyChunks;     // per column
    uint8_t refCount[MAX_POOL_CHUNKS];
    int16_t freeChunks[MAX_POOL_CHUNKS];
    uint32_t freeCount;
    int outOfMemory;            // logged; cleared when a chunk is available again
    int hasMidiIn;              // the firmware passes routed MIDI to process()

    uint64_t now;               // samples recorded since the instance was created
    int wasPlaying;
    int deleteHeld;
    struct Column columns[NUM_COLUMNS];

    uint16_t shown[NUM_LEDS];   // colour the Launchpad shows (palette or LED_RGB), LED_UNKNOWN if unsure
    float meter[NUM_COLUMNS];   // peak level of each column's output
    uint8_t meterStep[NUM_COLUMNS];
    float msSinceMeter;
    float msSinceResync;
    float midiCredit;
    int nextLed;
};

static struct Instance instances[MAX_INSTANCES];
static const UnitHostApi * api;

// --- Math (units have no libm) ------------------------------------------------------------

static inline double absd(double x) {
    return x < 0 ? -x : x;
}

static inline double floord(double x) {
    double t = (double)(int64_t)x;
    return t > x ? t - 1.0 : t;
}

// x modulo m, in [0, m)
static inline double wrap(double x, double m) {
    double r = x - m * floord(x / m);
    return r >= m ? r - m : r;
}

static inline int64_t roundToInt(double x) {
    return (int64_t)floord(x + 0.5);
}

// --- Chunk pool -----------------------------------------------------------------------------

static int16_t allocateChunk(struct Instance * inst) {
    if (!inst->freeCount) {
        if (!inst->outOfMemory) {
            inst->outOfMemory = 1;
            api->log("ClipGrid: out of memory, inputs are not recorded; delete some clips");
        }
        return NO_CHUNK;
    }
    inst->outOfMemory = 0;
    int16_t chunk = inst->freeChunks[--inst->freeCount];
    inst->refCount[chunk] = 1;
    return chunk;
}

static void releaseChunk(struct Instance * inst, int16_t chunk) {
    if (chunk != NO_CHUNK && --inst->refCount[chunk] == 0) {
        inst->freeChunks[inst->freeCount++] = chunk;
    }
}

static inline int16_t * chunkData(struct Instance * inst, int16_t chunk) {
    return inst->pool + ((uint32_t)chunk << CHUNK_SHIFT);
}

// --- Recording ------------------------------------------------------------------------------

// Makes the history slot for chunk number `number` hold a chunk only the history uses.
static int16_t historyChunkFor(struct Instance * inst, struct Column * col, uint32_t number) {
    uint32_t k = number % inst->historyChunks;
    if (col->historyChunkNumber[k] != number) {
        int16_t chunk = col->history[k];
        if (chunk == NO_CHUNK || inst->refCount[chunk] > 1) {
            // A clip holds the old chunk (or there was none): keep it for the clip
            releaseChunk(inst, chunk);
            chunk = allocateChunk(inst);
        }
        col->history[k] = chunk;
        col->historyChunkNumber[k] = number;
    }
    return col->history[k];
}

static void recordColumn(struct Instance * inst, struct Column * col, const float * in, size_t frameCount) {
    uint64_t sample = inst->now;
    size_t i = 0;
    while (i < frameCount) {
        uint32_t offset = sample & CHUNK_MASK;
        size_t run = CHUNK_SAMPLES - offset;
        if (run > frameCount - i) {
            run = frameCount - i;
        }
        int16_t chunk = historyChunkFor(inst, col, (uint32_t)(sample >> CHUNK_SHIFT));
        if (chunk != NO_CHUNK) {
            int16_t * out = chunkData(inst, chunk) + offset;
            for (size_t j = 0; j < run; j++) {
                float x = in ? in[i + j] * 32767.0F : 0.0F;
                if (x > 32767.0F) {
                    x = 32767.0F;
                } else if (x < -32767.0F) {
                    x = -32767.0F;
                }
                out[j] = (int16_t)x;
            }
        }
        i += run;
        sample += run;
    }
}

// --- Playback -------------------------------------------------------------------------------

static inline double samplesPerPpq(const UnitTimeInfo * ti) {
    return (60.0 / ti->tempo) * ti->sampleRate;
}

static inline double beatsPerBar(const UnitTimeInfo * ti) {
    return ti->timeSigNumerator > 0 ? ti->timeSigNumerator : 4;
}

static inline float regionSample(struct Instance * inst, struct Clip * clip, uint32_t index) {
    uint32_t r = clip->regionOffset + index;
    return chunkData(inst, clip->chunks[r >> CHUNK_SHIFT])[r & CHUNK_MASK] * (1.0F / 32768.0F);
}

// Sample x (0 <= x < length) of the loop. Over the last `fade` samples it crossfades into the
// audio from before the loop start, so the end runs smoothly into the start.
static inline float loopSample(struct Instance * inst, struct Clip * clip, uint32_t x) {
    float s = regionSample(inst, clip, clip->fade + x);
    if (x + clip->fade >= clip->length) {
        uint32_t intoFade = x + clip->fade - clip->length;
        float t = ((float)intoFade + 0.5F) / (float)clip->fade;
        float f = t * t * (3.0F - 2.0F * t);
        s += f * (regionSample(inst, clip, intoFade) - s);
    }
    return s;
}

// Puts the playback position where the transport says it should be.
static void syncClip(struct Clip * clip, const UnitTimeInfo * ti) {
    double phasePpq = wrap(ti->ppqPos - clip->startPpq, clip->lengthPpq);
    clip->position = wrap(phasePpq / clip->lengthPpq * clip->length, clip->length);
    clip->phaseErrorFiltered = 0.0;
}

static void playClip(struct Instance * inst, struct Clip * clip, const UnitTimeInfo * ti,
                     float * out, size_t frameCount, float gainStart, float gainEnd) {
    double spp = samplesPerPpq(ti);
    double length = clip->length;
    double position = clip->position;
    double sampleIncrement = length / (clip->lengthPpq * spp);

    // Compare the phase within the loop, the shortest way around
    double expectedPhasePpq = wrap(ti->ppqPos - clip->startPpq, clip->lengthPpq);
    double delta = expectedPhasePpq - clip->lengthPpq * (position / length);
    if (delta > 0.5 * clip->lengthPpq) {
        delta -= clip->lengthPpq;
    } else if (delta < -0.5 * clip->lengthPpq) {
        delta += clip->lengthPpq;
    }

    // ppqPos jitters from block to block: smooth the error, then nudge the playback rate in
    // proportion to it instead of jumping, which would be audible.
    double sampleRate = ti->sampleRate;
    double alpha = (double)frameCount / (SYNC_FILTER_SECONDS * sampleRate);
    if (alpha > 1.0) {
        alpha = 1.0;
    }
    clip->phaseErrorFiltered += alpha * (delta * spp - clip->phaseErrorFiltered);
    double phaseError = clip->phaseErrorFiltered;
    if (absd(phaseError) > SYNC_RESYNC_SECONDS * sampleRate) {
        position = wrap(position + phaseError * sampleIncrement, length);
        clip->phaseErrorFiltered = 0.0;
    } else {
        double correction = phaseError / (SYNC_CORRECTION_SECONDS * sampleRate);
        if (correction > SYNC_MAX_CORRECTION) {
            correction = SYNC_MAX_CORRECTION;
        } else if (correction < -SYNC_MAX_CORRECTION) {
            correction = -SYNC_MAX_CORRECTION;
        }
        sampleIncrement *= 1.0 + correction;
    }

    float gain = gainStart;
    float gainStep = (gainEnd - gainStart) / (float)frameCount;
    for (size_t i = 0; i < frameCount; i++) {
        uint32_t x = (uint32_t)position;
        if (x >= clip->length) {
            x = clip->length - 1;
        }
        uint32_t next = x + 1 < clip->length ? x + 1 : 0;
        float frac = (float)(position - x);
        float a = loopSample(inst, clip, x);
        float b = loopSample(inst, clip, next);
        out[i] += gain * (a + frac * (b - a));
        gain += gainStep;
        position += sampleIncrement;
        if (position >= length) {
            position -= length;
        }
    }
    clip->position = position;
}

// --- Clips and columns ------------------------------------------------------------------

static void releaseClip(struct Instance * inst, struct Clip * clip) {
    for (int i = 0; i < clip->chunkCount; i++) {
        releaseChunk(inst, clip->chunks[i]);
    }
    clip->chunkCount = 0;
    clip->state = CLIP_EMPTY;
}

static void finishFade(struct Instance * inst, struct Column * col) {
    if (col->fading >= 0) {
        struct Clip * clip = &col->clips[col->fading];
        if (clip->state == CLIP_DELETING) {
            releaseClip(inst, clip);
        }
        col->fading = -1;
    }
    col->fadePos = col->fadeLength;
}

// Switches the column to `slot` (-1: stop) right away, crossfading.
static void switchTo(struct Instance * inst, struct Column * col, int slot, const UnitTimeInfo * ti) {
    if (col->current == slot) {
        return;
    }
    finishFade(inst, col);
    col->fading = col->current;
    col->current = slot;
    col->fadePos = 0;
    col->fadeLength = (uint32_t)(SWITCH_FADE_SECONDS * ti->sampleRate);
    if (slot >= 0) {
        syncClip(&col->clips[slot], ti);
    }
}

static void queue(struct Column * col, int slotOrStop, const UnitTimeInfo * ti) {
    col->queued = slotOrStop;
    col->queuedAtPpq = ti->barStartPos + beatsPerBar(ti);
}

static void deleteClip(struct Instance * inst, struct Column * col, int slot, const UnitTimeInfo * ti) {
    struct Clip * clip = &col->clips[slot];
    if (col->queued == slot) {
        col->queued = QUEUE_NONE;
    }
    if (clip->state != CLIP_FILLED) {
        if (clip->state != CLIP_DELETING) {
            clip->state = CLIP_EMPTY;
        }
        return;
    }
    if (col->current == slot) {
        switchTo(inst, col, -1, ti);
    }
    if (col->fading == slot) {
        clip->state = CLIP_DELETING;    // released when the fade ends
    } else {
        releaseClip(inst, clip);
    }
}

static void arm(struct Column * col, int slot) {
    for (int s = 0; s < NUM_SLOTS; s++) {
        if (col->clips[s].state == CLIP_ARMED) {
            col->clips[s].state = CLIP_EMPTY;
        }
    }
    col->clips[slot].state = CLIP_ARMED;
}

// Starts capturing the last `bars` bars into the column's armed slot, ending at a bar line.
static void startCapture(struct Instance * inst, struct Column * col, int slot, int bars, const UnitTimeInfo * ti, uint64_t blockStart) {
    struct Clip * clip = &col->clips[slot];
    double spp = samplesPerPpq(ti);
    double barLength = beatsPerBar(ti);
    double nextBarPpq = ti->barStartPos + barLength;
    // In the last beat of a bar the capture ends at the next bar, otherwise at this bar's start
    double endPpq = nextBarPpq - ti->ppqPos < 1.0 ? nextBarPpq : ti->barStartPos;
    int64_t endSample = (int64_t)blockStart + roundToInt((endPpq - ti->ppqPos) * spp);
    uint32_t length = (uint32_t)roundToInt(bars * barLength * spp);
    uint32_t fade = (uint32_t)(LOOP_FADE_SECONDS * ti->sampleRate);

    uint64_t maxLength = (uint64_t)(inst->historyChunks - 1) * CHUNK_SAMPLES - CHUNK_SAMPLES;
    if (length + fade > maxLength || endSample < 0) {
        api->log("ClipGrid: %d bars is longer than the %d s of history", bars, (int)(maxLength / ti->sampleRate));
        return;
    }
    clip->state = CLIP_CAPTURING;
    clip->length = length;
    clip->fade = fade;
    clip->endSample = (uint64_t)endSample;
    clip->lengthPpq = bars * barLength;
    clip->startPpq = endPpq;
}

// Turns a capture whose audio has been recorded into a clip and launches it.
static void finishCapture(struct Instance * inst, struct Column * col, int slot, const UnitTimeInfo * ti) {
    struct Clip * clip = &col->clips[slot];
    int64_t regionStart = (int64_t)clip->endSample - clip->length - clip->fade;
    uint32_t first = (uint32_t)(regionStart >> CHUNK_SHIFT);
    uint32_t last = (uint32_t)((clip->endSample - 1) >> CHUNK_SHIFT);
    if (regionStart < 0 || last - first + 1 > MAX_HISTORY_CHUNKS) {
        clip->state = CLIP_ARMED;
        return;
    }
    for (uint32_t n = first; n <= last; n++) {
        uint32_t k = n % inst->historyChunks;
        if (col->historyChunkNumber[k] != n || col->history[k] == NO_CHUNK) {
            api->log("ClipGrid: the audio for this capture is no longer there");
            clip->state = CLIP_ARMED;
            return;
        }
    }
    clip->chunkCount = 0;
    for (uint32_t n = first; n <= last; n++) {
        int16_t chunk = col->history[n % inst->historyChunks];
        inst->refCount[chunk]++;
        clip->chunks[clip->chunkCount++] = chunk;
    }
    clip->regionOffset = (uint32_t)regionStart & CHUNK_MASK;
    clip->state = CLIP_FILLED;
    col->queued = QUEUE_NONE;
    switchTo(inst, col, slot, ti);
}

// --- Buttons --------------------------------------------------------------------------------

static void padPressed(struct Instance * inst, struct Column * col, int slot, const UnitTimeInfo * ti) {
    struct Clip * clip = &col->clips[slot];
    if (inst->deleteHeld) {
        deleteClip(inst, col, slot, ti);
        return;
    }
    switch (clip->state) {
    case CLIP_EMPTY:
        arm(col, slot);
        break;
    case CLIP_ARMED:
    case CLIP_CAPTURING:
        clip->state = CLIP_EMPTY;
        break;
    case CLIP_FILLED:
        if (col->queued == slot) {
            col->queued = QUEUE_NONE;
        } else if (col->current == slot) {
            col->queued = col->queued == QUEUE_STOP ? QUEUE_NONE : QUEUE_STOP;
            if (col->queued == QUEUE_STOP) {
                queue(col, QUEUE_STOP, ti);
            }
        } else {
            queue(col, slot, ti);
        }
        break;
    }
}

static void captureButtonPressed(struct Instance * inst, int bars, const UnitTimeInfo * ti, uint64_t blockStart) {
    if (!ti->isPlaying) {
        api->log("ClipGrid: start the transport to capture");
        return;
    }
    for (int c = 0; c < NUM_COLUMNS; c++) {
        struct Column * col = &inst->columns[c];
        for (int s = 0; s < NUM_SLOTS; s++) {
            if (col->clips[s].state == CLIP_ARMED) {
                startCapture(inst, col, s, bars, ti, blockStart);
            }
        }
    }
}

static void sceneButtonPressed(struct Instance * inst, int slot, const UnitTimeInfo * ti) {
    for (int c = 0; c < NUM_COLUMNS; c++) {
        struct Column * col = &inst->columns[c];
        if (col->clips[slot].state == CLIP_FILLED) {
            if (col->current == slot) {
                col->queued = QUEUE_NONE;
            } else {
                queue(col, slot, ti);
            }
        } else if (col->current >= 0) {
            queue(col, QUEUE_STOP, ti);
        }
    }
}

static void processParameters(struct Instance * inst, const UnitParamEvent * events, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t id = events[i].id;
        if (id >= PARAM_VOLUME + 1 && id <= PARAM_VOLUME + NUM_COLUMNS) {
            inst->columns[id - PARAM_VOLUME - 1].volumeTarget = events[i].value;
        }
    }
}

static void stopButtonPressed(struct Column * col, const UnitTimeInfo * ti) {
    if (col->current >= 0) {
        queue(col, QUEUE_STOP, ti);
    } else {
        col->queued = QUEUE_NONE;
    }
}

static void processMidi(struct Instance * inst, const UnitProcessHost * host, const UnitTimeInfo * ti, uint64_t blockStart) {
    if (!inst->hasMidiIn) {
        return;
    }
    for (size_t i = 0; i < host->midiInCount; i++) {
        UnitMidiPacket p = host->midiIn[i];
        int cin = p.header & 0x0F;
        int row = p.midi1 / 10;
        int column = p.midi1 % 10;
        if (cin == UNIT_MIDI_CIN_NOTE_ON && p.midi2 > 0) {
            if (row < 1 || row > 8 || column < 1 || column > NUM_COLUMNS) {
                continue;
            }
            if (row == 1) {
                captureButtonPressed(inst, captureBars[column - 1], ti, blockStart);
            } else {
                padPressed(inst, &inst->columns[column - 1], 8 - row, ti);
            }
        } else if (cin == UNIT_MIDI_CIN_CONTROL_CHANGE) {
            int pressed = p.midi2 > 0;
            if (p.midi1 == CC_DELETE) {
                inst->deleteHeld = pressed;
            } else if (!pressed) {
                continue;
            } else if (p.midi1 >= CC_STOP_FIRST && p.midi1 < CC_STOP_FIRST + NUM_COLUMNS) {
                stopButtonPressed(&inst->columns[p.midi1 - CC_STOP_FIRST], ti);
            } else if (column == 9 && row >= 2 && row <= 8) {
                sceneButtonPressed(inst, 8 - row, ti);
            }
        }
    }
}

// --- LEDs -----------------------------------------------------------------------------------

static void send(UnitProcessHost * host, uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2) {
    host->sendMIDIEvent(host->hostData, (UnitMidiPacket) { cin, b0, b1, b2 });
}

static void enterProgrammerMode(UnitProcessHost * host) {
    // F0h 00h 20h 29h 02h 0Dh 0Eh 01h F7h
    send(host, UNIT_MIDI_CIN_SYSEX_START, 0xF0, 0x00, 0x20);
    send(host, UNIT_MIDI_CIN_SYSEX_START, 0x29, 0x02, 0x0D);
    send(host, UNIT_MIDI_CIN_SYSEX_END_3BYTE, 0x0E, 0x01, 0xF7);
}

static uint16_t clipColor(struct Column * col, int slot, int blink, uint8_t meterStep) {
    struct Clip * clip = &col->clips[slot];
    switch (clip->state) {
    case CLIP_ARMED:
        return COLOR_RED;
    case CLIP_CAPTURING:
        return blink ? COLOR_RED : COLOR_OFF;
    case CLIP_FILLED:
        if (col->queued == slot) {
            return blink ? COLOR_GREEN : COLOR_OFF;
        }
        if (col->current == slot) {
            return col->queued == QUEUE_STOP && !blink ? COLOR_OFF : LED_RGB + meterStep;
        }
        return COLOR_YELLOW;
    default:
        return COLOR_OFF;
    }
}

// LEDs 0..63: grid, bottom row first; 64..71: top row; 72..79: right column, bottom first.
static uint16_t ledColor(struct Instance * inst, int led, int blink) {
    if (led < NUM_GRID_LEDS) {
        int row = led / 8 + 1;
        int column = led % 8;
        if (row == 1) {
            for (int c = 0; c < NUM_COLUMNS; c++) {
                for (int s = 0; s < NUM_SLOTS; s++) {
                    if (inst->columns[c].clips[s].state == CLIP_ARMED) {
                        return COLOR_DIM_RED;
                    }
                }
            }
            return COLOR_DIM_WHITE;
        }
        return clipColor(&inst->columns[column], 8 - row, blink, inst->meterStep[column]);
    }
    if (led < NUM_GRID_LEDS + 8) {
        struct Column * col = &inst->columns[led - NUM_GRID_LEDS];
        if (col->current < 0) {
            return COLOR_OFF;
        }
        return col->queued == QUEUE_STOP && blink ? COLOR_RED : COLOR_DIM_RED;
    }
    int row = led - NUM_GRID_LEDS - 8 + 1;
    if (row == 1) {
        return inst->deleteHeld ? COLOR_RED : COLOR_DIM_WHITE;
    }
    for (int c = 0; c < NUM_COLUMNS; c++) {
        if (inst->columns[c].clips[8 - row].state == CLIP_FILLED) {
            return COLOR_DIM_GREEN;
        }
    }
    return COLOR_OFF;
}

static void updateLeds(struct Instance * inst, UnitProcessHost * host, const UnitTimeInfo * ti, float blockMs) {
    int blink = ti->isPlaying ? wrap(ti->ppqPos, 1.0) < 0.5 : (ti->microSeconds / 250000) & 1;

    inst->midiCredit += blockMs * LED_MESSAGES_PER_MS;
    if (inst->midiCredit > LED_MAX_CREDIT) {
        inst->midiCredit = LED_MAX_CREDIT;
    }

    // Periodically (re)enter programmer mode and repaint everything, so a Launchpad that is
    // plugged in later (or switched to another mode) recovers by itself.
    inst->msSinceResync += blockMs;
    if (inst->msSinceResync >= RESYNC_INTERVAL_MS && inst->midiCredit >= 3.0F) {
        inst->msSinceResync = 0;
        enterProgrammerMode(host);
        for (int i = 0; i < NUM_LEDS; i++) {
            inst->shown[i] = LED_UNKNOWN;
        }
        inst->midiCredit -= 3.0F;
    }

    inst->msSinceMeter += blockMs;
    if (inst->msSinceMeter >= METER_UPDATE_MS) {
        inst->msSinceMeter = 0;
        for (int c = 0; c < NUM_COLUMNS; c++) {
            int step = 0;
            while (step < METER_STEPS && inst->meter[c] >= meterThreshold[step]) {
                step++;
            }
            inst->meterStep[c] = step;
        }
    }

    // Send the LEDs that changed, scanning round-robin so none starves. An LED that needs
    // more credit than there is stops the scan, so it goes first once the credit is there.
    for (int scanned = 0; scanned < NUM_LEDS; scanned++) {
        int led = inst->nextLed;
        uint16_t color = ledColor(inst, led, blink);
        if (inst->shown[led] == color) {
            inst->nextLed = (led + 1) % NUM_LEDS;
            continue;
        }
        float cost = color & LED_RGB ? 5.0F : 1.0F;
        if (inst->midiCredit < cost) {
            break;
        }
        inst->nextLed = (led + 1) % NUM_LEDS;
        if (color & LED_RGB) {
            // LED lighting SysEx: F0h 00h 20h 29h 02h 0Dh 03h, then type 3 (RGB), LED, R, G, B, F7h
            uint8_t note = (led / 8 + 1) * 10 + led % 8 + 1;
            int step = color & 0xFF;
            uint8_t green = METER_MIN_GREEN + step * (127 - METER_MIN_GREEN) / METER_STEPS;
            send(host, UNIT_MIDI_CIN_SYSEX_START, 0xF0, 0x00, 0x20);
            send(host, UNIT_MIDI_CIN_SYSEX_START, 0x29, 0x02, 0x0D);
            send(host, UNIT_MIDI_CIN_SYSEX_START, 0x03, 0x03, note);
            send(host, UNIT_MIDI_CIN_SYSEX_START, 0x00, green, 0x00);
            send(host, UNIT_MIDI_CIN_SYSEX_END_1BYTE, 0xF7, 0x00, 0x00);
        } else if (led < NUM_GRID_LEDS) {
            // Programmer mode: row 1 is the bottom row, pad = row * 10 + column
            uint8_t note = (led / 8 + 1) * 10 + led % 8 + 1;
            send(host, UNIT_MIDI_CIN_NOTE_ON, 0x90, note, color);
        } else if (led < NUM_GRID_LEDS + 8) {
            send(host, UNIT_MIDI_CIN_CONTROL_CHANGE, 0xB0, 91 + led - NUM_GRID_LEDS, color);
        } else {
            send(host, UNIT_MIDI_CIN_CONTROL_CHANGE, 0xB0, (led - NUM_GRID_LEDS - 8 + 1) * 10 + 9, color);
        }
        inst->shown[led] = color;
        inst->midiCredit -= cost;
    }
}

// --- Plugin ---------------------------------------------------------------------------------

static void process(void * instance, UnitProcessHost * host,
                    size_t nrInChannels, float ** inSamples,
                    size_t nrOutChannels, float ** outSamples,
                    size_t frameCount,
                    const UnitParamEvent * paramEvents, size_t paramEventCount) {
    struct Instance * inst = instance;
    if (!inst || !frameCount) {
        return;
    }
    UnitTimeInfo timeInfo = host->timeInfo;     // host's copy is shared with other units
    const UnitTimeInfo * ti = &timeInfo;
    if (timeInfo.sampleRate <= 0) {
        timeInfo.sampleRate = 48000.0F;
    }
    int playing = ti->isPlaying && ti->tempo > 0;
    uint64_t blockStart = inst->now;
    double blockPpq = playing ? frameCount / samplesPerPpq(ti) : 0.0;

    processParameters(inst, paramEvents, paramEventCount);
    processMidi(inst, host, ti, blockStart);

    for (int c = 0; c < NUM_COLUMNS; c++) {
        recordColumn(inst, &inst->columns[c], c < (int)nrInChannels ? inSamples[c] : 0, frameCount);
    }
    inst->now += frameCount;

    for (int c = 0; c < NUM_COLUMNS; c++) {
        struct Column * col = &inst->columns[c];

        if (playing != inst->wasPlaying) {
            for (int s = 0; s < NUM_SLOTS; s++) {
                if (col->clips[s].state == CLIP_CAPTURING) {
                    col->clips[s].state = CLIP_ARMED;
                }
            }
            finishFade(inst, col);
            if (playing && col->current >= 0) {
                // Transport started: fade the clips in, in phase with the new timeline
                syncClip(&col->clips[col->current], ti);
                col->fadePos = 0;
                col->fadeLength = (uint32_t)(SWITCH_FADE_SECONDS * ti->sampleRate);
            }
        }

        for (int s = 0; s < NUM_SLOTS; s++) {
            if (col->clips[s].state == CLIP_CAPTURING && col->clips[s].endSample <= inst->now) {
                finishCapture(inst, col, s, ti);
            }
        }

        if (col->queued != QUEUE_NONE) {
            if (playing && col->queuedAtPpq - ti->ppqPos > beatsPerBar(ti) + 1.0) {
                // The timeline jumped back (transport restarted): wait for its next bar
                col->queuedAtPpq = ti->barStartPos + beatsPerBar(ti);
            }
            if (!playing || ti->ppqPos + 0.5 * blockPpq >= col->queuedAtPpq) {
                int slot = col->queued == QUEUE_STOP ? -1 : col->queued;
                col->queued = QUEUE_NONE;
                if (slot < 0 || col->clips[slot].state == CLIP_FILLED) {
                    switchTo(inst, col, slot, ti);
                }
            }
        }

        float volumeStart = col->volume;
        float maxStep = frameCount / (VOLUME_RAMP_SECONDS * ti->sampleRate);
        float volumeDelta = col->volumeTarget - col->volume;
        col->volume += volumeDelta > maxStep ? maxStep : volumeDelta < -maxStep ? -maxStep : volumeDelta;

        float * out = c < (int)nrOutChannels ? outSamples[c] : 0;
        if (!playing || !out) {
            finishFade(inst, col);
            inst->meter[c] = 0.0F;
            continue;
        }

        float fadeStart = (float)col->fadePos / (float)col->fadeLength;
        uint32_t fadeEndPos = col->fadePos + frameCount < col->fadeLength ? col->fadePos + frameCount : col->fadeLength;
        float fadeEnd = (float)fadeEndPos / (float)col->fadeLength;
        if (col->current >= 0) {
            playClip(inst, &col->clips[col->current], ti, out, frameCount,
                     fadeStart * volumeStart, fadeEnd * col->volume);
        }
        if (col->fading >= 0) {
            playClip(inst, &col->clips[col->fading], ti, out, frameCount,
                     (1.0F - fadeStart) * volumeStart, (1.0F - fadeEnd) * col->volume);
        }
        col->fadePos = fadeEndPos;
        if (col->fadePos >= col->fadeLength) {
            finishFade(inst, col);
        }

        float level = inst->meter[c];
        for (size_t i = 0; i < frameCount; i++) {
            float x = out[i] < 0 ? -out[i] : out[i];
            level *= METER_RELEASE;
            if (x > level) {
                level = x;
            }
        }
        inst->meter[c] = level;
    }
    inst->wasPlaying = playing;

    updateLeds(inst, host, ti, frameCount * 1000.0F / ti->sampleRate);
}

static void resetInstance(struct Instance * inst) {
    inst->now = 0;
    inst->wasPlaying = 0;
    inst->deleteHeld = 0;
    inst->outOfMemory = 0;
    inst->freeCount = 0;
    for (uint32_t i = inst->poolChunks; i > 0; i--) {
        inst->refCount[i - 1] = 0;
        inst->freeChunks[inst->freeCount++] = (int16_t)(i - 1);
    }
    for (int c = 0; c < NUM_COLUMNS; c++) {
        struct Column * col = &inst->columns[c];
        for (int s = 0; s < NUM_SLOTS; s++) {
            col->clips[s].state = CLIP_EMPTY;
            col->clips[s].chunkCount = 0;
        }
        col->current = -1;
        col->fading = -1;
        col->queued = QUEUE_NONE;
        col->fadePos = col->fadeLength = 1;
        col->volume = col->volumeTarget = 1.0F;
        for (int k = 0; k < MAX_HISTORY_CHUNKS; k++) {
            col->history[k] = NO_CHUNK;
            col->historyChunkNumber[k] = 0xFFFFFFFFu;
        }
    }
    for (int i = 0; i < NUM_LEDS; i++) {
        inst->shown[i] = LED_UNKNOWN;
    }
    inst->msSinceResync = RESYNC_INTERVAL_MS;
    inst->midiCredit = 0.0F;
    inst->msSinceMeter = 0.0F;
    for (int c = 0; c < NUM_COLUMNS; c++) {
        inst->meter[c] = 0.0F;
        inst->meterStep[c] = 0;
    }
    inst->nextLed = 0;
}

static int allocatePool(struct Instance * inst) {
    for (size_t i = 0; i < sizeof(poolSizesMB) / sizeof(poolSizesMB[0]); i++) {
        uint32_t chunks = (uint32_t)poolSizesMB[i] * 1024 * 1024 / (CHUNK_SAMPLES * sizeof(int16_t));
        if (chunks > MAX_POOL_CHUNKS) {
            chunks = MAX_POOL_CHUNKS;
        }
        int16_t * pool = api->allocate((size_t)chunks * CHUNK_SAMPLES * sizeof(int16_t));
        if (pool) {
            inst->pool = pool;
            inst->poolChunks = chunks;
            return 1;
        }
    }
    return 0;
}

static void * createInstance(void) {
    if (!UNIT_HOST_HAS(api, allocate)) {
        api->log("ClipGrid: this firmware has no unit heap; update the firmware");
        return 0;
    }
    for (int i = 0; i < MAX_INSTANCES; i++) {
        struct Instance * inst = &instances[i];
        if (inst->active) {
            continue;
        }
        // The pool stays with the slot: the heap never frees
        if (!inst->pool && !allocatePool(inst)) {
            api->log("ClipGrid: out of unit heap");
            return 0;
        }
        // 5/8 of the pool for the columns' history, at most MAX_HISTORY_CHUNKS each
        inst->historyChunks = inst->poolChunks * 5 / (8 * NUM_COLUMNS);
        if (inst->historyChunks > MAX_HISTORY_CHUNKS) {
            inst->historyChunks = MAX_HISTORY_CHUNKS;
        }
        resetInstance(inst);
        inst->hasMidiIn = UNIT_PROCESS_HOST_HAS(api, midiInCount);
        if (!inst->hasMidiIn) {
            api->log("ClipGrid: this firmware doesn't pass MIDI to units; update the firmware");
        }
        inst->active = 1;
        api->log("ClipGrid: %d MB, %d s of history per column",
                 (int)(inst->poolChunks * CHUNK_SAMPLES * sizeof(int16_t) / (1024 * 1024)),
                 (int)((inst->historyChunks - 2) * CHUNK_SAMPLES / 48000));
        return inst;
    }
    api->log("ClipGrid: no free instances (max %d)", MAX_INSTANCES);
    return 0;
}

static void releaseInstance(void * instance) {
    ((struct Instance *)instance)->active = 0;
}

static void copyString(char * dest, const char * src, size_t maxLength) {
    size_t i = 0;
    for (; src[i] && i + 1 < maxLength; i++) {
        dest[i] = src[i];
    }
    if (maxLength) {
        dest[i] = 0;
    }
}

static uint32_t getNumInputs(void) { return NUM_COLUMNS; }
static uint32_t getNumOutputs(void) { return NUM_COLUMNS; }
static uint32_t getParamCount(void) { return NUM_PARAMETERS; }

// volume1 .. volume8
static int getParamInfo(uint32_t index, UnitParamInfo * info) {
    if (index >= NUM_PARAMETERS) {
        return 0;
    }
    *info = (UnitParamInfo) {
        .id = PARAM_VOLUME + 1 + index,
        .flags = UNIT_PARAM_IS_AUTOMATABLE,
        .module = "tracks",
        .minValue = 0,
        .maxValue = 1,
        .defaultValue = 1,
    };
    copyString(info->name, "volume", sizeof(info->name));
    info->name[6] = '1' + index;
    info->name[7] = 0;
    return 1;
}

static int getParamValue(void * instance, uint32_t id, double * value) {
    if (id < PARAM_VOLUME + 1 || id > PARAM_VOLUME + NUM_COLUMNS) {
        return 0;
    }
    *value = ((struct Instance *)instance)->columns[id - PARAM_VOLUME - 1].volumeTarget;
    return 1;
}

static const UnitPlugin plugin = {
    .size = sizeof(UnitPlugin),
    .name = "ClipGrid",
    .vendor = "metalhead",
    .version = "1.1",
    .description = "Clip launcher on a Launchpad Mini MK3: 8 tracks x 7 clips",
    .createInstance = createInstance,
    .releaseInstance = releaseInstance,
    .getNumInputs = getNumInputs,
    .getNumOutputs = getNumOutputs,
    .getParamCount = getParamCount,
    .getParamInfo = getParamInfo,
    .getParamValue = getParamValue,
    .process = process,
};

const UnitPlugin * clipgrid_plugin(const UnitHostApi * hostApi) {
    api = hostApi;
    api->log("ClipGrid unit ready (%d tracks x %d clips)", NUM_COLUMNS, NUM_SLOTS);
    return &plugin;
}
