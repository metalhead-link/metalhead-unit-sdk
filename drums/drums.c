/*
 * Drums — a basic drum sampler with a fixed, built-in kit.
 *
 * The eleven sounds are rendered once when the unit loads (808-style synthesis: pitch-swept
 * sines, filtered noise and the six-square "metal" oscillator for hats and cymbals) into 16-bit
 * sample buffers at 48 kHz, and played back from there. Each sound has a trigger parameter;
 * map MIDI notes to them in [midimap], and the note velocity sets the level:
 *
 *   [units]
 *   drums Drums
 *   [audioroutes]
 *   drums:1 .Scarlett_2i2_USB:1 1.0
 *   drums:2 .Scarlett_2i2_USB:2 1.0
 *   [midimap]
 *   .device:noteon36 drums:kick          # General MIDI drum notes
 *   .device:noteon38 drums:snare
 *   .device:noteon42 drums:closedhat
 *   .device:noteon46 drums:openhat
 *   .nanoKONTROL2:cc0 drums:volume
 *
 * Outputs 1 and 2 are left and right. Up to 16 sounds play at once (the oldest is cut when
 * more are triggered); the closed hat chokes the open hat.
 */
#include <unit_abi.h>

UNIT_DECLARE("Drums");

#define SAMPLE_RATE         48000
#define MAX_INSTANCES       2
#define MAX_VOICES          16
#define CHOKE_FADE_SAMPLES  96          // 2 ms fade when a voice is choked or stolen
#define PI                  3.14159265F

enum Sound {
    KICK, SNARE, RIM, CLAP, CLOSED_HAT, OPEN_HAT, LOW_TOM, MID_TOM, HIGH_TOM, COWBELL, CRASH,
    NUM_SOUNDS
};

#define PARAM_VOLUME        NUM_SOUNDS  // parameter index after the triggers

struct SoundInfo {
    const char * name;
    float seconds;          // rendered length
    float level;            // mix balance
    float panLeft, panRight;// constant-power pan gains
    int chokeGroup;         // sounds in the same non-zero group cut each other off
};

static const struct SoundInfo sounds[NUM_SOUNDS] = {
    [KICK]       = { "kick",      0.50F, 1.00F, 0.707F, 0.707F, 0 },
    [SNARE]      = { "snare",     0.35F, 0.80F, 0.707F, 0.707F, 0 },
    [RIM]        = { "rim",       0.08F, 0.50F, 0.809F, 0.588F, 0 },
    [CLAP]       = { "clap",      0.45F, 0.70F, 0.649F, 0.760F, 0 },
    [CLOSED_HAT] = { "closedhat", 0.12F, 0.45F, 0.523F, 0.852F, 1 },
    [OPEN_HAT]   = { "openhat",   0.60F, 0.45F, 0.523F, 0.852F, 1 },
    [LOW_TOM]    = { "lowtom",    0.60F, 0.80F, 0.891F, 0.454F, 0 },
    [MID_TOM]    = { "midtom",    0.50F, 0.80F, 0.707F, 0.707F, 0 },
    [HIGH_TOM]   = { "hightom",   0.45F, 0.80F, 0.454F, 0.891F, 0 },
    [COWBELL]    = { "cowbell",   0.40F, 0.45F, 0.852F, 0.523F, 0 },
    [CRASH]      = { "crash",     1.50F, 0.50F, 0.490F, 0.872F, 0 },
};

// Rendered kit: all sounds back to back. Must hold the sum of sounds[].seconds (5.55 s).
#define KIT_SAMPLES         (SAMPLE_RATE * 556 / 100)
static int16_t kit[KIT_SAMPLES];
static const int16_t * sampleData[NUM_SOUNDS];
static uint32_t sampleLength[NUM_SOUNDS];

// --- Synthesis helpers (no libm in units) ---------------------------------------------------

// sin(2*pi*turns) for turns in [0, 1), max error about 0.001
static float sinTurns(float turns) {
    float t = turns < 0.5F ? turns : turns - 1.0F;          // [-0.5, 0.5)
    float x = 2.0F * PI * t;
    float y = (4.0F / PI) * x - (4.0F / (PI * PI)) * x * (x < 0 ? -x : x);
    return 0.225F * (y * (y < 0 ? -y : y) - y) + y;
}

static float advance(float * phase, float hz) {
    *phase += hz / SAMPLE_RATE;
    if (*phase >= 1.0F) {
        *phase -= 1.0F;
    }
    return *phase;
}

// Per-sample factor for an exponential decay with time constant tau (seconds).
static float decay(float tau) {
    return 1.0F - 1.0F / (tau * SAMPLE_RATE);
}

static float onePoleCoefficient(float hz) {
    float w = 2.0F * PI * hz / SAMPLE_RATE;
    return w / (1.0F + w);
}

struct Filter {
    float low, band;
};

// Chamberlin state-variable bandpass; fine up to ~6 kHz at 48 kHz.
static float bandpass(struct Filter * f, float x, float hz, float damping) {
    float k = 2.0F * sinTurns(hz / (2.0F * SAMPLE_RATE));
    f->low += k * f->band;
    float high = x - f->low - damping * f->band;
    f->band += k * high;
    return f->band;
}

struct Synth {
    uint32_t rng;
    float phase[6];
    float env, env2, pitchEnv;
    float lp1, lp2, lp3;
    struct Filter bp;
};

static float noise(struct Synth * s) {
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return (float)(int32_t)s->rng * (1.0F / 2147483648.0F);
}

static float square(float phase) {
    return phase < 0.5F ? 1.0F : -1.0F;
}

// The TR-808 "metal" source: six detuned square waves.
static float metal(struct Synth * s, float scale) {
    static const float hz[6] = { 205.3F, 304.4F, 369.6F, 522.7F, 540.0F, 800.0F };
    float sum = 0;
    for (int i = 0; i < 6; i++) {
        sum += square(advance(&s->phase[i], hz[i] * scale));
    }
    return sum * (1.0F / 6.0F);
}

// Two cascaded one-pole highpasses (state in lp1/lp2).
static float highpass2(struct Synth * s, float x, float hz) {
    float a = onePoleCoefficient(hz);
    s->lp1 += a * (x - s->lp1);
    x -= s->lp1;
    s->lp2 += a * (x - s->lp2);
    return x - s->lp2;
}

static float tom(struct Synth * s, float hz, float tau, uint32_t i) {
    if (i == 0) { s->env = 1; s->pitchEnv = 1; s->env2 = 1; }
    s->pitchEnv *= decay(0.08F);
    s->env *= decay(tau);
    s->env2 *= decay(0.003F);
    return sinTurns(advance(&s->phase[0], hz * (1.0F + 0.4F * s->pitchEnv))) * s->env + noise(s) * s->env2 * 0.2F;
}

// One sample of a sound; i counts samples from the start.
static float synthesize(enum Sound sound, struct Synth * s, uint32_t i) {
    float t = (float)i / SAMPLE_RATE;
    switch (sound) {
    case KICK: {
        if (i == 0) { s->env = 1; s->pitchEnv = 1; s->env2 = 1; }
        s->pitchEnv *= decay(0.04F);
        s->env *= decay(0.16F);
        s->env2 *= decay(0.002F);
        float body = sinTurns(advance(&s->phase[0], 45.0F + 110.0F * s->pitchEnv)) * s->env;
        s->lp1 += onePoleCoefficient(4000) * (noise(s) - s->lp1);
        float x = 1.6F * (body + 0.3F * s->lp1 * s->env2);
        return x / (1.0F + (x < 0 ? -x : x));               // soft saturation
    }
    case SNARE: {
        if (i == 0) { s->env = 1; s->env2 = 1; }
        s->env *= decay(0.045F);
        s->env2 *= decay(0.075F);
        float tone = 0.5F * sinTurns(advance(&s->phase[0], 185)) + 0.3F * sinTurns(advance(&s->phase[1], 330));
        float rattle = bandpass(&s->bp, noise(s), 3000, 0.7F) + 0.3F * highpass2(s, noise(s), 5000);
        return 0.6F * tone * s->env + rattle * s->env2;
    }
    case RIM: {
        if (i == 0) { s->env = 1; s->env2 = 1; s->pitchEnv = 1; }
        s->env *= decay(0.006F);
        s->env2 *= decay(0.010F);
        s->pitchEnv *= decay(0.004F);
        return 0.6F * sinTurns(advance(&s->phase[0], 1700)) * s->env +
               0.4F * sinTurns(advance(&s->phase[1], 500)) * s->env2 +
               0.5F * bandpass(&s->bp, noise(s), 3000, 0.5F) * s->pitchEnv;
    }
    case CLAP: {
        // Three short bursts 11 ms apart, then the reverberant tail.
        static const float burstTimes[] = { 0.0F, 0.011F, 0.022F, 0.033F };
        for (int b = 0; b < 4; b++) {
            if (i == (uint32_t)(burstTimes[b] * SAMPLE_RATE)) {
                s->env = 1;
            }
        }
        s->env *= t < 0.033F ? decay(0.004F) : decay(0.09F);
        return 1.5F * bandpass(&s->bp, noise(s), 1200, 0.5F) * s->env;
    }
    case CLOSED_HAT:
    case OPEN_HAT: {
        if (i == 0) { s->env = 1; }
        s->env *= decay(sound == CLOSED_HAT ? 0.012F : 0.13F);
        float x = highpass2(s, 0.8F * metal(s, 1.0F) + 0.3F * noise(s), 7000);
        s->lp3 += onePoleCoefficient(14000) * (x - s->lp3);
        return 2.0F * s->lp3 * s->env;
    }
    case LOW_TOM:
        return tom(s, 80, 0.18F, i);
    case MID_TOM:
        return tom(s, 120, 0.15F, i);
    case HIGH_TOM:
        return tom(s, 170, 0.13F, i);
    case COWBELL: {
        if (i == 0) { s->env = 1; s->env2 = 1; }
        s->env *= decay(0.012F);
        s->env2 *= decay(0.13F);
        float x = 0.5F * (square(advance(&s->phase[0], 540)) + square(advance(&s->phase[1], 800)));
        return 2.0F * bandpass(&s->bp, x, 2640, 0.6F) * (0.6F * s->env + 0.4F * s->env2);
    }
    case CRASH: {
        if (i == 0) { s->env = 1; }
        s->env *= decay(0.45F);
        float x = highpass2(s, 0.6F * metal(s, 1.5F) + 0.6F * noise(s), 5000);
        return 1.5F * x * s->env;
    }
    default:
        return 0;
    }
}

// Render a sound twice: first to find its peak, then normalised into 16-bit samples.
static void renderSound(enum Sound sound, int16_t * out, uint32_t length) {
    struct Synth s;
    float peak = 0;
    for (int pass = 0; pass < 2; pass++) {
        s = (struct Synth) { .rng = 0x9E3779B9u + sound };
        float scale = peak > 0 ? 0.9F * 32767.0F / peak : 0;
        for (uint32_t i = 0; i < length; i++) {
            float x = synthesize(sound, &s, i);
            if (pass == 0) {
                float a = x < 0 ? -x : x;
                if (a > peak) {
                    peak = a;
                }
            } else {
                // Fade out the last 5 ms so truncated tails don't click.
                uint32_t remaining = length - i;
                if (remaining < SAMPLE_RATE / 200) {
                    x *= (float)remaining / (SAMPLE_RATE / 200);
                }
                out[i] = (int16_t)(x * scale);
            }
        }
    }
}

static void renderKit(void) {
    uint32_t offset = 0;
    for (int sound = 0; sound < NUM_SOUNDS; sound++) {
        uint32_t length = (uint32_t)(sounds[sound].seconds * SAMPLE_RATE);
        if (offset + length > KIT_SAMPLES) {
            length = KIT_SAMPLES - offset;
        }
        renderSound(sound, kit + offset, length);
        sampleData[sound] = kit + offset;
        sampleLength[sound] = length;
        offset += length;
    }
}

// --- Playback -----------------------------------------------------------------------------

struct Voice {
    int active;
    enum Sound sound;
    uint32_t position;
    uint32_t age;           // trigger order, for stealing the oldest voice
    float gain;
    int fade;               // > 0: fading out over this many more samples
};

struct Instance {
    int active;
    struct Voice voices[MAX_VOICES];
    uint32_t triggerCount;
    float volume;
};

static struct Instance instances[MAX_INSTANCES];
static const UnitHostApi * api;

static void stopVoice(struct Voice * v) {
    if (v->active && !v->fade) {
        v->fade = CHOKE_FADE_SAMPLES;
    }
}

static void trigger(struct Instance * inst, enum Sound sound, uint8_t velocity) {
    if (velocity == 0) {
        return;             // note-on with velocity 0 is a note-off
    }
    if (sounds[sound].chokeGroup) {
        for (int v = 0; v < MAX_VOICES; v++) {
            struct Voice * voice = &inst->voices[v];
            if (voice->active && sounds[voice->sound].chokeGroup == sounds[sound].chokeGroup) {
                stopVoice(voice);
            }
        }
    }
    struct Voice * target = 0;
    for (int v = 0; v < MAX_VOICES && !target; v++) {
        if (!inst->voices[v].active) {
            target = &inst->voices[v];
        }
    }
    if (!target) {
        // Steal the oldest voice; it gets cut without a fade (rare with 16 voices).
        target = &inst->voices[0];
        for (int v = 1; v < MAX_VOICES; v++) {
            if (inst->voices[v].age < target->age) {
                target = &inst->voices[v];
            }
        }
    }
    float v = velocity / 127.0F;
    *target = (struct Voice) {
        .active = 1,
        .sound = sound,
        .age = ++inst->triggerCount,
        .gain = sounds[sound].level * v * (0.5F + 0.5F * v),
    };
}

static void process(void * instance, UnitProcessHost * host,
                    size_t nrInChannels, float ** inSamples,
                    size_t nrOutChannels, float ** outSamples,
                    size_t frameCount,
                    UnitParameterChange * parameterChanges, size_t parameterChangeCount) {
    struct Instance * inst = instance;
    if (!inst || nrOutChannels < 2) {
        return;
    }
    for (size_t c = 0; c < parameterChangeCount; c++) {
        int index = parameterChanges[c].parameterId - 1;
        if (index >= 0 && index < NUM_SOUNDS) {
            trigger(inst, index, parameterChanges[c].midiValue);
        } else if (index == PARAM_VOLUME) {
            inst->volume = parameterChanges[c].midiValue / 127.0F;
        }
    }

    float * left = outSamples[0];
    float * right = outSamples[1];
    const float toFloat = inst->volume / 32768.0F;
    for (int v = 0; v < MAX_VOICES; v++) {
        struct Voice * voice = &inst->voices[v];
        if (!voice->active) {
            continue;
        }
        const struct SoundInfo * info = &sounds[voice->sound];
        const int16_t * data = sampleData[voice->sound];
        uint32_t length = sampleLength[voice->sound];
        float gainLeft = voice->gain * info->panLeft * toFloat;
        float gainRight = voice->gain * info->panRight * toFloat;
        for (size_t i = 0; i < frameCount; i++) {
            if (voice->position >= length) {
                voice->active = 0;
                break;
            }
            float x = data[voice->position++];
            if (voice->fade) {
                x *= (float)voice->fade / CHOKE_FADE_SAMPLES;
                if (--voice->fade == 0) {
                    voice->active = 0;
                    break;
                }
            }
            left[i] += x * gainLeft;
            right[i] += x * gainRight;
        }
    }
}

// --- Plugin interface -----------------------------------------------------------------------

static void copyString(char * dest, const char * src, size_t maxLength) {
    size_t i = 0;
    for (; src[i] && i + 1 < maxLength; i++) {
        dest[i] = src[i];
    }
    if (maxLength) {
        dest[i] = 0;
    }
}

static void getName(char * name, size_t maxLength) {
    copyString(name, __unit_header.name, maxLength);
}

static void * createInstance(void) {
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (!instances[i].active) {
            instances[i] = (struct Instance) { .active = 1, .volume = 0.8F };
            return &instances[i];
        }
    }
    api->log("Drums: no free instances (max %d)", MAX_INSTANCES);
    return 0;
}

static uint8_t getNumInputs(void) { return 0; }
static uint8_t getNumOutputs(void) { return 2; }
static uint8_t getNumParameters(void) { return NUM_SOUNDS + 1; }

// Parameter ids must be non-zero (the firmware treats 0 as "no such parameter").
static uint16_t getParameterId(size_t index) {
    return index <= PARAM_VOLUME ? index + 1 : 0;
}

static void getParameterName(size_t index, char * name, size_t maxLength) {
    copyString(name, index < NUM_SOUNDS ? sounds[index].name : index == PARAM_VOLUME ? "volume" : "", maxLength);
}

static const UnitPlugin plugin = {
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
    renderKit();
    api->log("Drums unit ready: %d sounds, %d KB of samples", NUM_SOUNDS, (int)(sizeof(kit) / 1024));
    return &plugin;
}
