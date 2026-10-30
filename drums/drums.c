/*
 * Drums — a drum sampler that plays WAV files from the SD card.
 *
 * Put WAV files in /samples/drums on the SD card; each file becomes a trigger parameter named
 * after it (lower case, other characters become '_'). One level of subfolders is allowed, and
 * each subfolder is a choke group: its sounds cut each other off, like closed and open hats.
 *
 *   /samples/drums/kick.wav            -> drums:kick
 *   /samples/drums/Snare 01.wav        -> drums:snare_01
 *   /samples/drums/hats/closed.wav     -> drums:hats_closed   } choke group "hats"
 *   /samples/drums/hats/open.wav       -> drums:hats_open     }
 *
 * WAV: PCM 8/16/24/32-bit or 32-bit float, mono or stereo (extra channels are ignored), any
 * sample rate (resampled to 48 kHz on load), up to 20 seconds and 32 sounds. Samples are read
 * when the unit loads, so reboot after changing them. Without usable WAV files the unit
 * falls back to a built-in synthesised kit (kick, snare, rim, clap, closedhat, openhat,
 * lowtom, midtom, hightom, cowbell, crash).
 *
 * Map MIDI notes to the sounds in [midimap]; the velocity sets the level:
 *
 *   [units]
 *   drums Drums
 *   [audioroutes]
 *   drums:1 .Scarlett_2i2_USB:1 1.0
 *   drums:2 .Scarlett_2i2_USB:2 1.0
 *   [midimap]
 *   .device:noteon36 drums:kick
 *   .device:noteon38 drums:snare_01
 *   .nanoKONTROL2:cc0 drums:volume
 *
 * Outputs 1 and 2 are left and right. Up to 16 sounds play at once (the oldest is cut when
 * more are triggered).
 */
#include <unit_abi.h>

#define SAMPLE_FOLDER       "/samples/drums"
#define SAMPLE_RATE         48000
#define MAX_SOUNDS          32
#define MAX_GROUPS          16
#define MAX_SECONDS         20
#define MAX_INSTANCES       2
#define MAX_VOICES          16
#define CHOKE_FADE_SAMPLES  96          // 2 ms fade when a voice is choked
#define PI                  3.14159265F

// A loaded sound: 16-bit samples at 48 kHz, interleaved when stereo.
struct Sound {
    char name[UNIT_NAME_LENGTH];        // parameter name
    const int16_t * data;
    uint32_t frames;
    uint8_t channels;                   // 1 or 2
    uint8_t chokeGroup;                 // 0 = none
    float gainLeft, gainRight;
};

static struct Sound sounds[MAX_SOUNDS];
static int soundCount;
static const UnitHostApi * api;

static void copyString(char * dest, const char * src, size_t maxLength) {
    size_t i = 0;
    for (; src[i] && i + 1 < maxLength; i++) {
        dest[i] = src[i];
    }
    if (maxLength) {
        dest[i] = 0;
    }
}

static int stringsEqual(const char * a, const char * b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static char lowerCase(char c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

// =============================================================================================
// Built-in kit (used when there are no WAV files)
// =============================================================================================

enum KitSound {
    KICK, SNARE, RIM, CLAP, CLOSED_HAT, OPEN_HAT, LOW_TOM, MID_TOM, HIGH_TOM, COWBELL, CRASH,
    NUM_KIT_SOUNDS
};

static const struct {
    const char * name;
    float seconds;          // rendered length
    float level;            // mix balance
    float panLeft, panRight;// constant-power pan gains
    int chokeGroup;
} kitInfo[NUM_KIT_SOUNDS] = {
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
static float synthesize(enum KitSound sound, struct Synth * s, uint32_t i) {
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
static void renderSound(enum KitSound sound, int16_t * out, uint32_t length) {
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


static void loadBuiltinKit(void) {
    for (int k = 0; k < NUM_KIT_SOUNDS; k++) {
        uint32_t frames = (uint32_t)(kitInfo[k].seconds * SAMPLE_RATE);
        int16_t * data = api->allocate(frames * sizeof(int16_t));
        if (!data) {
            return;
        }
        renderSound(k, data, frames);
        struct Sound * s = &sounds[soundCount++];
        copyString(s->name, kitInfo[k].name, sizeof(s->name));
        s->data = data;
        s->frames = frames;
        s->channels = 1;
        s->chokeGroup = kitInfo[k].chokeGroup;
        s->gainLeft = kitInfo[k].level * kitInfo[k].panLeft;
        s->gainRight = kitInfo[k].level * kitInfo[k].panRight;
    }
}

// =============================================================================================
// WAV files
// =============================================================================================

#define WAVE_FORMAT_PCM         1
#define WAVE_FORMAT_IEEE_FLOAT  3
#define WAVE_FORMAT_EXTENSIBLE  0xFFFE

struct Wav {
    uint16_t format;
    uint16_t channels;
    uint32_t sampleRate;
    uint16_t bitsPerSample;
    uint16_t blockAlign;
    uint32_t dataOffset;
    uint32_t frames;
};

static uint16_t le16(const uint8_t * p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t * p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int fourcc(const uint8_t * p, const char * id) {
    return p[0] == id[0] && p[1] == id[1] && p[2] == id[2] && p[3] == id[3];
}

// Walk the RIFF chunks for "fmt " and "data". Returns an error message or 0.
static const char * parseWav(const char * path, int32_t fileSize, struct Wav * wav) {
    uint8_t buf[40];
    if (fileSize < 44 || api->readFile(path, 0, buf, 12) != 12 || !fourcc(buf, "RIFF") || !fourcc(buf + 8, "WAVE")) {
        return "not a WAV file";
    }
    int haveFormat = 0;
    uint32_t offset = 12;
    while (offset + 8 <= (uint32_t)fileSize) {
        if (api->readFile(path, offset, buf, 8) != 8) {
            return "read error";
        }
        uint32_t size = le32(buf + 4);
        if (fourcc(buf, "fmt ")) {
            uint32_t n = size < sizeof(buf) ? size : sizeof(buf);
            if (n < 16 || api->readFile(path, offset + 8, buf, n) != (int32_t)n) {
                return "bad fmt chunk";
            }
            wav->format = le16(buf);
            wav->channels = le16(buf + 2);
            wav->sampleRate = le32(buf + 4);
            wav->blockAlign = le16(buf + 12);
            wav->bitsPerSample = le16(buf + 14);
            if (wav->format == WAVE_FORMAT_EXTENSIBLE && n >= 26) {
                wav->format = le16(buf + 24);       // first two bytes of the SubFormat GUID
            }
            haveFormat = 1;
        } else if (fourcc(buf, "data")) {
            if (!haveFormat) {
                return "data before fmt";
            }
            wav->dataOffset = offset + 8;
            uint32_t available = (uint32_t)fileSize - wav->dataOffset;
            if (size > available) {
                size = available;                   // truncated file, or size left at 0xFFFFFFFF
            }
            if (wav->blockAlign == 0) {
                return "bad block size";
            }
            wav->frames = size / wav->blockAlign;
            break;
        }
        offset += 8 + size + (size & 1);           // chunks are padded to even sizes
    }
    if (!wav->dataOffset) {
        return "no audio data";
    }
    int bits = wav->bitsPerSample;
    if (!((wav->format == WAVE_FORMAT_PCM && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (wav->format == WAVE_FORMAT_IEEE_FLOAT && bits == 32))) {
        return "unsupported sample format (use PCM or 32-bit float)";
    }
    if (wav->channels < 1 || wav->blockAlign != wav->channels * bits / 8) {
        return "bad channel layout";
    }
    if (wav->sampleRate < 4000 || wav->sampleRate > 384000) {
        return "unsupported sample rate";
    }
    if (wav->frames == 0) {
        return "empty";
    }
    return 0;
}

// Sequential reader with a small buffer; frames are read in increasing order.
#define READ_BUFFER_SIZE 8192
static uint8_t readBuffer[READ_BUFFER_SIZE];

struct FrameReader {
    const char * path;
    const struct Wav * wav;
    uint32_t first, count;              // frames currently in readBuffer
};

static float decodeSample(const struct Wav * wav, const uint8_t * p) {
    switch (wav->bitsPerSample) {
    case 8:  return ((int)p[0] - 128) * (1.0F / 128.0F);
    case 16: return (int16_t)le16(p) * (1.0F / 32768.0F);
    case 24: return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) * (1.0F / 2147483648.0F);
    default:
        if (wav->format == WAVE_FORMAT_IEEE_FLOAT) {
            union { uint32_t u; float f; } v = { .u = le32(p) };
            return v.f;
        }
        return (int32_t)le32(p) * (1.0F / 2147483648.0F);
    }
}

// Left and right of frame `index` (mono files give the same value twice).
static int readFrame(struct FrameReader * r, uint32_t index, float * left, float * right) {
    if (index >= r->wav->frames) {
        index = r->wav->frames - 1;
    }
    if (index < r->first || index >= r->first + r->count) {
        uint32_t perBuffer = READ_BUFFER_SIZE / r->wav->blockAlign;
        uint32_t count = r->wav->frames - index < perBuffer ? r->wav->frames - index : perBuffer;
        int32_t got = api->readFile(r->path, r->wav->dataOffset + index * r->wav->blockAlign, readBuffer, count * r->wav->blockAlign);
        if (got < (int32_t)r->wav->blockAlign) {
            return 0;
        }
        r->first = index;
        r->count = got / r->wav->blockAlign;
    }
    const uint8_t * frame = readBuffer + (index - r->first) * r->wav->blockAlign;
    *left = decodeSample(r->wav, frame);
    *right = r->wav->channels > 1 ? decodeSample(r->wav, frame + r->wav->bitsPerSample / 8) : *left;
    return 1;
}

static int16_t toInt16(float x) {
    x *= 32768.0F;
    return x >= 32767.0F ? 32767 : x <= -32768.0F ? -32768 : (int16_t)x;
}

static const char * loadWav(const char * path, int32_t fileSize, struct Sound * sound) {
    struct Wav wav = {0};
    const char * error = parseWav(path, fileSize, &wav);
    if (error) {
        return error;
    }
    int channels = wav.channels > 1 ? 2 : 1;
    // Resample to 48 kHz by linear interpolation, stepping through the source in 32.32 fixed point.
    uint64_t step = ((uint64_t)wav.sampleRate << 32) / SAMPLE_RATE;
    uint64_t frames64 = (((uint64_t)(wav.frames - 1) << 32) / step) + 1;
    uint32_t frames = frames64 > (uint64_t)MAX_SECONDS * SAMPLE_RATE ? MAX_SECONDS * SAMPLE_RATE : (uint32_t)frames64;
    int16_t * data = api->allocate(frames * channels * sizeof(int16_t));
    if (!data) {
        return "out of memory";
    }
    struct FrameReader reader = { .path = path, .wav = &wav };
    uint64_t position = 0;
    for (uint32_t i = 0; i < frames; i++, position += step) {
        uint32_t index = (uint32_t)(position >> 32);
        float frac = (uint32_t)position * (1.0F / 4294967296.0F);
        float l0, r0, l1 = 0, r1 = 0;
        if (!readFrame(&reader, index, &l0, &r0) || (frac > 0 && !readFrame(&reader, index + 1, &l1, &r1))) {
            return "read error";
        }
        float l = frac > 0 ? l0 + (l1 - l0) * frac : l0;
        float r = frac > 0 ? r0 + (r1 - r0) * frac : r0;
        if (channels == 2) {
            data[2 * i] = toInt16(l);
            data[2 * i + 1] = toInt16(r);
        } else {
            data[i] = toInt16(l);
        }
    }
    sound->data = data;
    sound->frames = frames;
    sound->channels = channels;
    sound->gainLeft = sound->gainRight = 1.0F;
    api->log("Drums: %s (%d Hz, %d-bit, %s, %d ms)", sound->name, (int)wav.sampleRate, wav.bitsPerSample,
             wav.channels == 1 ? "mono" : "stereo", (int)(frames / (SAMPLE_RATE / 1000)));
    return 0;
}

// =============================================================================================
// Scanning the sample folder
// =============================================================================================

struct Candidate {
    char path[160];
    char name[UNIT_NAME_LENGTH];
    int32_t size;
    uint8_t group;
};

static struct Candidate candidates[MAX_SOUNDS];
static int candidateCount;
static char groupNames[MAX_GROUPS][64];
static int groupCount;

static int isWav(const char * name) {
    size_t n = 0;
    while (name[n]) {
        n++;
    }
    return n > 4 && name[n - 4] == '.' && lowerCase(name[n - 3]) == 'w' && lowerCase(name[n - 2]) == 'a' && lowerCase(name[n - 1]) == 'v';
}

// "Snare 01.wav" in group "hats" -> "hats_snare_01": lower case, runs of other characters -> '_'
static void parameterName(char * out, const char * group, const char * file) {
    size_t n = 0;
    const char * parts[2] = { group, file };
    for (int p = 0; p < 2; p++) {
        if (!parts[p]) {
            continue;
        }
        if (n > 0 && n + 1 < UNIT_NAME_LENGTH && out[n - 1] != '_') {
            out[n++] = '_';
        }
        const char * end = parts[p];
        while (*end) {
            end++;
        }
        if (p == 1) {
            end -= 4;                   // drop ".wav"
        }
        for (const char * c = parts[p]; c < end && n + 1 < UNIT_NAME_LENGTH; c++) {
            char ch = lowerCase(*c);
            int ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
            if (ok) {
                out[n++] = ch;
            } else if (n > 0 && out[n - 1] != '_') {
                out[n++] = '_';
            }
        }
    }
    while (n > 0 && out[n - 1] == '_') {
        n--;
    }
    if (n == 0) {
        out[n++] = 's';
    }
    out[n] = 0;
}

static int nameTaken(const char * name, int upTo) {
    if (stringsEqual(name, "volume")) {
        return 1;
    }
    for (int i = 0; i < upTo; i++) {
        if (stringsEqual(candidates[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

static void addCandidate(const char * directory, const UnitDirectoryEntry * entry, int group) {
    if (candidateCount == MAX_SOUNDS) {
        api->log("Drums: more than %d sounds, ignoring %s/%s", MAX_SOUNDS, directory, entry->name);
        return;
    }
    struct Candidate * c = &candidates[candidateCount];
    char * p = c->path;
    for (const char * s = directory; *s; s++) *p++ = *s;
    *p++ = '/';
    for (const char * s = entry->name; *s && p < c->path + sizeof(c->path) - 1; s++) *p++ = *s;
    *p = 0;
    parameterName(c->name, group ? groupNames[group - 1] : 0, entry->name);
    // Make the name unique: snare, snare_2, snare_3, ...
    if (nameTaken(c->name, candidateCount)) {
        char base[UNIT_NAME_LENGTH];
        copyString(base, c->name, sizeof(base) - 3);
        for (int n = 2; nameTaken(c->name, candidateCount) && n < 100; n++) {
            size_t len = 0;
            while (base[len]) {
                c->name[len] = base[len];
                len++;
            }
            c->name[len++] = '_';
            if (n >= 10) {
                c->name[len++] = '0' + n / 10;
            }
            c->name[len++] = '0' + n % 10;
            c->name[len] = 0;
        }
    }
    c->size = entry->size;
    c->group = group;
    candidateCount++;
}

static void scanDirectory(const char * directory, int group) {
    UnitDirectoryEntry entry;
    for (uint32_t i = 0; api->readDirectory(directory, i, &entry) == 1; i++) {
        if (entry.name[0] == '.') {
            continue;                   // hidden, and macOS "._name" metadata files
        }
        if (entry.isDirectory) {
            if (group == 0 && groupCount < MAX_GROUPS) {
                copyString(groupNames[groupCount++], entry.name, sizeof(groupNames[0]));
            }
        } else if (isWav(entry.name)) {
            addCandidate(directory, &entry, group);
        }
    }
}

static void loadSampleFolder(void) {
    UnitDirectoryEntry probe;
    if (api->readDirectory(SAMPLE_FOLDER, 0, &probe) < 0) {
        api->log("Drums: no %s folder on the SD card", SAMPLE_FOLDER);
        return;
    }
    scanDirectory(SAMPLE_FOLDER, 0);
    for (int g = 0; g < groupCount; g++) {
        char path[160];
        char * p = path;
        for (const char * s = SAMPLE_FOLDER "/"; *s; s++) *p++ = *s;
        for (const char * s = groupNames[g]; *s; s++) *p++ = *s;
        *p = 0;
        scanDirectory(path, g + 1);
    }

    // Sort by parameter name so the parameter order doesn't depend on the order on disk.
    for (int i = 1; i < candidateCount; i++) {
        struct Candidate c = candidates[i];
        int j = i - 1;
        while (j >= 0) {
            const char * a = candidates[j].name;
            const char * b = c.name;
            while (*a && *a == *b) { a++; b++; }
            if ((uint8_t)*a <= (uint8_t)*b) {
                break;
            }
            candidates[j + 1] = candidates[j];
            j--;
        }
        candidates[j + 1] = c;
    }

    for (int i = 0; i < candidateCount; i++) {
        struct Sound * sound = &sounds[soundCount];
        *sound = (struct Sound) { .chokeGroup = candidates[i].group };
        copyString(sound->name, candidates[i].name, sizeof(sound->name));
        const char * error = loadWav(candidates[i].path, candidates[i].size, sound);
        if (error) {
            api->log("Drums: skipping %s: %s", candidates[i].path, error);
        } else {
            soundCount++;
        }
    }
}

// =============================================================================================
// Playback
// =============================================================================================

struct Voice {
    int active;
    int sound;
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

static void trigger(struct Instance * inst, int sound, uint8_t velocity) {
    if (velocity == 0) {
        return;             // note-on with velocity 0 is a note-off
    }
    int group = sounds[sound].chokeGroup;
    if (group) {
        for (int v = 0; v < MAX_VOICES; v++) {
            struct Voice * voice = &inst->voices[v];
            if (voice->active && !voice->fade && sounds[voice->sound].chokeGroup == group) {
                voice->fade = CHOKE_FADE_SAMPLES;
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
        .gain = v * (0.5F + 0.5F * v),
    };
}

// Parameter ids: the sounds are 1..soundCount, in name order
#define PARAM_VOLUME        1000

static void process(void * instance, UnitProcessHost * host,
                    size_t nrInChannels, float ** inSamples,
                    size_t nrOutChannels, float ** outSamples,
                    size_t frameCount,
                    const UnitParamEvent * paramEvents, size_t paramEventCount) {
    struct Instance * inst = instance;
    if (!inst || nrOutChannels < 2) {
        return;
    }
    for (size_t c = 0; c < paramEventCount; c++) {
        uint32_t id = paramEvents[c].id;
        if (id >= 1 && id <= (uint32_t)soundCount) {
            trigger(inst, id - 1, (uint8_t)(paramEvents[c].value * 127.0 + 0.5));   // value: velocity 0..1
        } else if (id == PARAM_VOLUME) {
            inst->volume = paramEvents[c].value;
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
        const struct Sound * sound = &sounds[voice->sound];
        const int16_t * data = sound->data;
        int stereo = sound->channels == 2;
        float gainLeft = voice->gain * sound->gainLeft * toFloat;
        float gainRight = voice->gain * sound->gainRight * toFloat;
        for (size_t i = 0; i < frameCount; i++) {
            if (voice->position >= sound->frames) {
                voice->active = 0;
                break;
            }
            uint32_t p = voice->position++;
            float l = stereo ? data[2 * p] : data[p];
            float r = stereo ? data[2 * p + 1] : l;
            if (voice->fade) {
                float f = (float)voice->fade / CHOKE_FADE_SAMPLES;
                l *= f;
                r *= f;
                if (--voice->fade == 0) {
                    voice->active = 0;
                    break;
                }
            }
            left[i] += l * gainLeft;
            right[i] += r * gainRight;
        }
    }
}

// =============================================================================================
// Plugin interface
// =============================================================================================

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

static void releaseInstance(void * instance) {
    ((struct Instance *)instance)->active = 0;
}

static uint32_t getNumInputs(void) { return 0; }
static uint32_t getNumOutputs(void) { return 2; }
static uint32_t getParamCount(void) { return soundCount + 1; }    // the sounds, then volume

static int getParamInfo(uint32_t index, UnitParamInfo * info) {
    if (index < (uint32_t)soundCount) {
        const struct Sound * sound = &sounds[index];
        *info = (UnitParamInfo) {
            .id = index + 1,
            .flags = UNIT_PARAM_IS_TRIGGER,     // value: velocity
            .minValue = 0,
            .maxValue = 1,
            .defaultValue = 0,
        };
        copyString(info->name, sound->name, sizeof(info->name));
        copyString(info->module, sound->chokeGroup ? groupNames[sound->chokeGroup - 1] : "", sizeof(info->module));
        return 1;
    }
    if (index == (uint32_t)soundCount) {
        *info = (UnitParamInfo) {
            .id = PARAM_VOLUME,
            .flags = UNIT_PARAM_IS_AUTOMATABLE,
            .name = "volume",
            .minValue = 0,
            .maxValue = 1,
            .defaultValue = 0.8,
        };
        return 1;
    }
    return 0;
}

static int getParamValue(void * instance, uint32_t id, double * value) {
    if (id != PARAM_VOLUME) {
        return 0;           // the sounds are triggers
    }
    *value = ((struct Instance *)instance)->volume;
    return 1;
}

static const UnitPlugin plugin = {
    .size = sizeof(UnitPlugin),
    .name = "Drums",
    .vendor = "metalhead",
    .version = "1.1",
    .description = "Drum sampler playing the WAV files in /samples/drums",
    .createInstance = createInstance,
    .releaseInstance = releaseInstance,
    .getNumInputs = getNumInputs,
    .getNumOutputs = getNumOutputs,
    .getParamCount = getParamCount,
    .getParamInfo = getParamInfo,
    .getParamValue = getParamValue,
    .process = process,
};

const UnitPlugin * drums_plugin(const UnitHostApi * hostApi) {
    api = hostApi;
    if (!UNIT_HOST_HAS(api, readDirectory)) {
        api->log("Drums: this firmware has no file access for units; update the firmware");
        return &plugin;
    }
    loadSampleFolder();
    if (soundCount == 0) {
        api->log("Drums: no usable WAV files in %s, using the built-in kit", SAMPLE_FOLDER);
        loadBuiltinKit();
    }
    api->log("Drums ready with %d sounds", soundCount);
    return &plugin;
}
