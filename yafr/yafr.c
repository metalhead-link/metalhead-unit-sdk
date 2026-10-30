/*
 * YAFR — Yet Another Freaking Reverb: a plate-style reverb with mono in and stereo out.
 *
 * Ported from the built-in plugin that used to live in wormhole/src/yafr.c (commit e5cdcb6,
 * branch debug_thumb_abort). Same structure: pre-delay, early reflections from a tapped delay
 * line, four input-diffusion allpasses, six damped parallel combs, four output-diffusion
 * allpasses. Differences from the original:
 *
 *   - Real stereo: the right channel runs its own tank with every delay lengthened by
 *     STEREO_SPREAD samples (as in Freeverb), and `width` mixes the two the way Freeverb does
 *     (0 = mono, 1 = fully separated). The original scaled the right channel's wet level.
 *   - Fixed gains on the comb input and the early-reflection sum, so wet = 1 is about unity
 *     instead of clipping (the original summed six combs and twelve taps unscaled).
 *   - Comb feedback always follows `roomsize` (the original used unscaled gains until the
 *     parameter was first moved). Wet, dry and width ramp over a block to avoid zipper noise.
 *   - No modulo or libm in the audio path; the unused LFO is gone. Delay memory comes from
 *     the unit heap instead of 750 KB of static arrays.
 *
 * Delay times are in samples at 48 kHz.
 *
 *   [units]
 *   verb YAFR
 *   [audioroutes]
 *   looper:1 verb:1 1.0
 *   verb:1 .Scarlett_2i2_USB:1 1.0
 *   verb:2 .Scarlett_2i2_USB:2 1.0
 *   [midimap]
 *   .nanoKONTROL2:cc16 verb:roomsize
 *   .nanoKONTROL2:cc17 verb:dampening
 *   .nanoKONTROL2:cc0 verb:wetlevel
 *   .nanoKONTROL2:cc1 verb:drylevel
 *   .nanoKONTROL2:cc18 verb:width
 *   .nanoKONTROL2:cc19 verb:predelay
 */
#include <unit_abi.h>

#define MAX_INSTANCES           2
#define NUM_ALLPASS             8
#define NUM_INPUT_ALLPASS       4       // the first four diffuse the input, the rest the output
#define NUM_COMB                6
#define NUM_EARLY_REFLECTIONS   12
#define STEREO_SPREAD           23      // extra samples on every right-channel delay

#define EARLY_SIZE              2048    // power of two > longest early tap + STEREO_SPREAD
#define PREDELAY_SIZE           4096    // power of two > MAX_PREDELAY
#define MAX_PREDELAY_MS         50
#define MAX_PREDELAY            (MAX_PREDELAY_MS * 48)

#define COMB_INPUT_GAIN         0.04F
#define EARLY_GAIN              (0.6F / 4.285F)     // 0.6 of the original mix, over the sum of earlyGains
#define LATE_GAIN               0.4F

static const int allpassDelays[NUM_ALLPASS] = {347, 113, 37, 59, 53, 43, 37, 29};
static const float allpassGains[NUM_ALLPASS] = {0.7F, 0.7F, 0.7F, 0.7F, 0.5F, 0.5F, 0.5F, 0.5F};

static const int combDelays[NUM_COMB] = {1687, 1601, 2053, 2251, 1493, 1277};
static const float combGains[NUM_COMB] = {0.773F, 0.802F, 0.753F, 0.733F, 0.792F, 0.812F};

static const int earlyDelays[NUM_EARLY_REFLECTIONS] = {
    192, 256, 354, 496, 612, 784, 912, 1108, 1284, 1456, 1632, 1808
};
static const float earlyGains[NUM_EARLY_REFLECTIONS] = {
    0.841F, 0.504F, 0.491F, 0.379F, 0.380F, 0.346F,
    0.289F, 0.272F, 0.192F, 0.193F, 0.217F, 0.181F
};

_Static_assert(1808 + STEREO_SPREAD < EARLY_SIZE, "EARLY_SIZE too small");

// A fixed delay of `size` samples: read the oldest sample, then overwrite it.
struct DelayLine {
    float * buffer;
    int size;
    int index;
};

struct Comb {
    struct DelayLine delay;
    float feedback;
    float state;                        // one-pole lowpass in the feedback path
};

// One channel's tank.
struct Tank {
    struct DelayLine allpass[NUM_ALLPASS];
    struct Comb comb[NUM_COMB];
    int earlyTaps[NUM_EARLY_REFLECTIONS];
};

struct Instance {
    int active;
    struct Tank tank[2];
    float * early;                      // EARLY_SIZE, shared by both channels
    float * predelay;                   // PREDELAY_SIZE
    int earlyIndex;
    int predelayIndex;
    int predelayLength;

    float roomSize, dampening, wetLevel, dryLevel, width, preDelay;   // the parameters; preDelay in ms
    float damp;                         // comb lowpass coefficient
    float wet1, wet2, dry;              // current output gains, ramped towards the targets
};

static struct Instance instances[MAX_INSTANCES];
static const UnitHostApi * api;

// Decaying feedback ends in denormals, which the VFP handles very slowly.
static inline float flushDenormal(float x) {
    union { float f; uint32_t u; } v = { .f = x };
    return (v.u & 0x7F800000u) == 0 ? 0.0F : x;
}

// =============================================================================================
// DSP
// =============================================================================================

static inline float delayTick(struct DelayLine * d, float input) {
    float out = d->buffer[d->index];
    d->buffer[d->index] = input;
    if (++d->index == d->size) {
        d->index = 0;
    }
    return out;
}

static inline float allpassTick(struct DelayLine * d, float gain, float input) {
    float delayed = d->buffer[d->index];
    d->buffer[d->index] = input + gain * delayed;
    if (++d->index == d->size) {
        d->index = 0;
    }
    return delayed - gain * input;
}

static inline float combTick(struct Comb * c, float damp, float input) {
    float out = c->delay.buffer[c->delay.index];
    c->state = flushDenormal(out + damp * (c->state - out));
    delayTick(&c->delay, input + c->feedback * c->state);
    return out;
}

static float tankTick(struct Instance * inst, struct Tank * t, float input) {
    float earlySum = 0.0F;
    for (int i = 0; i < NUM_EARLY_REFLECTIONS; i++) {
        earlySum += inst->early[(inst->earlyIndex - t->earlyTaps[i]) & (EARLY_SIZE - 1)] * earlyGains[i];
    }

    float diffused = input * COMB_INPUT_GAIN;
    for (int i = 0; i < NUM_INPUT_ALLPASS; i++) {
        diffused = allpassTick(&t->allpass[i], allpassGains[i], diffused);
    }
    float late = 0.0F;
    for (int i = 0; i < NUM_COMB; i++) {
        late += combTick(&t->comb[i], inst->damp, diffused);
    }
    for (int i = NUM_INPUT_ALLPASS; i < NUM_ALLPASS; i++) {
        late = allpassTick(&t->allpass[i], allpassGains[i], late);
    }
    return earlySum * EARLY_GAIN + late * LATE_GAIN;
}

static void updateRoomSize(struct Instance * inst) {
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < NUM_COMB; i++) {
            inst->tank[ch].comb[i].feedback = combGains[i] * (0.28F + 0.7F * inst->roomSize);
        }
    }
}

enum { PARAM_ROOM_SIZE = 1001, PARAM_DAMPENING, PARAM_WET, PARAM_DRY, PARAM_WIDTH, PARAM_PREDELAY };

// The firmware keeps values in range
static void setParameter(struct Instance * inst, uint32_t id, float value) {
    switch (id) {
    case PARAM_ROOM_SIZE:
        inst->roomSize = value;
        updateRoomSize(inst);
        break;
    case PARAM_DAMPENING:
        inst->dampening = value;
        inst->damp = 0.4F * value;
        break;
    case PARAM_WET: inst->wetLevel = value; break;
    case PARAM_DRY: inst->dryLevel = value; break;
    case PARAM_WIDTH: inst->width = value; break;
    case PARAM_PREDELAY:
        inst->preDelay = value;
        inst->predelayLength = (int)(value * (MAX_PREDELAY / MAX_PREDELAY_MS));
        break;
    }
}

static void process(void * instance, UnitProcessHost * host,
                    size_t nrInChannels, float ** inSamples,
                    size_t nrOutChannels, float ** outSamples,
                    size_t frameCount,
                    const UnitParamEvent * paramEvents, size_t paramEventCount) {
    struct Instance * inst = instance;
    if (!inst || nrInChannels < 1 || nrOutChannels < 2 || frameCount == 0) {
        return;
    }
    for (size_t c = 0; c < paramEventCount; c++) {
        setParameter(inst, paramEvents[c].id, paramEvents[c].value);
    }

    // Freeverb-style width: wet1 to its own side, wet2 crossed over.
    float wet1 = inst->wetLevel * (0.5F + 0.5F * inst->width);
    float wet2 = inst->wetLevel * (0.5F - 0.5F * inst->width);
    float dry = inst->dryLevel;
    float step = 1.0F / frameCount;
    float dWet1 = (wet1 - inst->wet1) * step;
    float dWet2 = (wet2 - inst->wet2) * step;
    float dDry = (dry - inst->dry) * step;

    const float * in = inSamples[0];
    float * outLeft = outSamples[0];
    float * outRight = outSamples[1];
    for (size_t i = 0; i < frameCount; i++) {
        float input = in[i];

        float delayed = input;
        if (inst->predelayLength > 0) {
            delayed = inst->predelay[(inst->predelayIndex - inst->predelayLength) & (PREDELAY_SIZE - 1)];
        }
        inst->predelay[inst->predelayIndex] = input;
        inst->predelayIndex = (inst->predelayIndex + 1) & (PREDELAY_SIZE - 1);

        inst->early[inst->earlyIndex] = delayed;
        float left = tankTick(inst, &inst->tank[0], delayed);
        float right = tankTick(inst, &inst->tank[1], delayed);
        inst->earlyIndex = (inst->earlyIndex + 1) & (EARLY_SIZE - 1);

        inst->wet1 += dWet1;
        inst->wet2 += dWet2;
        inst->dry += dDry;
        outLeft[i] += left * inst->wet1 + right * inst->wet2 + input * inst->dry;
        outRight[i] += right * inst->wet1 + left * inst->wet2 + input * inst->dry;
    }
    inst->wet1 = wet1;
    inst->wet2 = wet2;
    inst->dry = dry;
}

// =============================================================================================
// Setup
// =============================================================================================

static float * allocateZeroed(int samples) {
    float * buffer = api->allocate(samples * sizeof(float));
    if (buffer) {
        for (int i = 0; i < samples; i++) {
            buffer[i] = 0.0F;
        }
    }
    return buffer;
}

// Reuses `buffer` (a released instance's memory, the same size) instead of allocating again.
static float * zeroedBuffer(float * buffer, int samples) {
    if (!buffer) {
        return allocateZeroed(samples);
    }
    for (int i = 0; i < samples; i++) {
        buffer[i] = 0.0F;
    }
    return buffer;
}

static int initDelay(struct DelayLine * d, float * oldBuffer, int size) {
    d->buffer = zeroedBuffer(oldBuffer, size);
    d->size = size;
    d->index = 0;
    return d->buffer != 0;
}

// Buffers survive a release: the unit heap never frees, so a reused slot keeps its memory.
static int initInstance(struct Instance * inst) {
    struct Instance old = *inst;
    *inst = (struct Instance) {
        .roomSize = 0.5F,
        .dampening = 0.5F,
        .wetLevel = 0.3F,
        .dryLevel = 0.7F,
        .width = 1.0F,
        .preDelay = 0.0F,
    };
    inst->damp = 0.4F * inst->dampening;
    inst->early = zeroedBuffer(old.early, EARLY_SIZE);
    inst->predelay = zeroedBuffer(old.predelay, PREDELAY_SIZE);
    if (!inst->early || !inst->predelay) {
        return 0;
    }
    for (int ch = 0; ch < 2; ch++) {
        struct Tank * t = &inst->tank[ch];
        int spread = ch * STEREO_SPREAD;
        for (int i = 0; i < NUM_ALLPASS; i++) {
            if (!initDelay(&t->allpass[i], old.tank[ch].allpass[i].buffer, allpassDelays[i] + spread)) {
                return 0;
            }
        }
        for (int i = 0; i < NUM_COMB; i++) {
            if (!initDelay(&t->comb[i].delay, old.tank[ch].comb[i].delay.buffer, combDelays[i] + spread)) {
                return 0;
            }
        }
        for (int i = 0; i < NUM_EARLY_REFLECTIONS; i++) {
            t->earlyTaps[i] = earlyDelays[i] + spread;
        }
    }
    updateRoomSize(inst);
    // Start at the target gains so there's no fade-in on the first block.
    inst->wet1 = inst->wetLevel * (0.5F + 0.5F * inst->width);
    inst->wet2 = inst->wetLevel * (0.5F - 0.5F * inst->width);
    inst->dry = inst->dryLevel;
    return 1;
}

// =============================================================================================
// Plugin interface
// =============================================================================================

// The defaults are initInstance()'s
static const UnitParamInfo parameters[] = {
    { PARAM_ROOM_SIZE, UNIT_PARAM_IS_AUTOMATABLE, "roomsize", "tank", 0, 1, 0.5 },
    { PARAM_DAMPENING, UNIT_PARAM_IS_AUTOMATABLE, "dampening", "tank", 0, 1, 0.5 },
    { PARAM_WET, UNIT_PARAM_IS_AUTOMATABLE, "wetlevel", "mix", 0, 1, 0.3 },
    { PARAM_DRY, UNIT_PARAM_IS_AUTOMATABLE, "drylevel", "mix", 0, 1, 0.7 },
    { PARAM_WIDTH, UNIT_PARAM_IS_AUTOMATABLE, "width", "mix", 0, 1, 1 },
    { PARAM_PREDELAY, UNIT_PARAM_IS_AUTOMATABLE, "predelay", "tank", 0, MAX_PREDELAY_MS, 0 },
};
#define NUM_PARAMETERS (sizeof(parameters) / sizeof(parameters[0]))

static void * createInstance(void) {
    if (!UNIT_HOST_HAS(api, allocate)) {
        api->log("YAFR: this firmware has no unit heap; update the firmware");
        return 0;
    }
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (!instances[i].active) {
            if (!initInstance(&instances[i])) {
                api->log("YAFR: out of unit heap");
                return 0;               // what was allocated stays allocated: the heap never frees
            }
            instances[i].active = 1;
            return &instances[i];
        }
    }
    api->log("YAFR: no free instances (max %d)", MAX_INSTANCES);
    return 0;
}

static void releaseInstance(void * instance) {
    ((struct Instance *)instance)->active = 0;
}

static uint32_t getNumInputs(void) { return 1; }
static uint32_t getNumOutputs(void) { return 2; }
static uint32_t getParamCount(void) { return NUM_PARAMETERS; }

static int getParamInfo(uint32_t index, UnitParamInfo * info) {
    if (index >= NUM_PARAMETERS) {
        return 0;
    }
    *info = parameters[index];
    return 1;
}

static int getParamValue(void * instance, uint32_t id, double * value) {
    struct Instance * inst = instance;
    switch (id) {
    case PARAM_ROOM_SIZE: *value = inst->roomSize; return 1;
    case PARAM_DAMPENING: *value = inst->dampening; return 1;
    case PARAM_WET: *value = inst->wetLevel; return 1;
    case PARAM_DRY: *value = inst->dryLevel; return 1;
    case PARAM_WIDTH: *value = inst->width; return 1;
    case PARAM_PREDELAY: *value = inst->preDelay; return 1;
    }
    return 0;
}

// "12.5 ms": tenths, without a C library
static int paramValueToText(uint32_t id, double value, char * text, uint32_t size) {
    if (id != PARAM_PREDELAY || size < 10 || value < 0 || value > MAX_PREDELAY_MS) {
        return 0;
    }
    int tenths = (int)(value * 10 + 0.5);
    char * p = text;
    if (tenths >= 100) {
        *p++ = '0' + tenths / 100;
    }
    *p++ = '0' + tenths / 10 % 10;
    *p++ = '.';
    *p++ = '0' + tenths % 10;
    *p++ = ' ';
    *p++ = 'm';
    *p++ = 's';
    *p = 0;
    return 1;
}

static const UnitPlugin plugin = {
    .size = sizeof(UnitPlugin),
    .name = "YAFR",
    .vendor = "metalhead",
    .version = "1.1",
    .description = "Plate-style reverb, mono in, stereo out",
    .createInstance = createInstance,
    .releaseInstance = releaseInstance,
    .getNumInputs = getNumInputs,
    .getNumOutputs = getNumOutputs,
    .getParamCount = getParamCount,
    .getParamInfo = getParamInfo,
    .getParamValue = getParamValue,
    .paramValueToText = paramValueToText,
    .process = process,
};

const UnitPlugin * yafr_plugin(const UnitHostApi * hostApi) {
    api = hostApi;
    return &plugin;
}
