/*
 * unittest — runs a unit's plugins on the Mac, through the unit ABI, without a device.
 *
 * `make test` in a unit directory compiles the unit's sources natively (clang) together with
 * this file and runs it. It calls unit_entry() the way the firmware does, checks the factory,
 * each plugin and its parameters the way the loader and plugins.c do, then creates instances
 * and runs process() with parameter events: every settable parameter is set to its minimum,
 * maximum and default, and getParamValue() has to report each value back.
 *
 *   build/host/unittest [-r <sd card folder>] [-v] [-w <folder>]
 *
 * -r maps the unit's file access (/samples/drums/...) to a folder on the Mac; without it the
 * unit sees no files. -v prints every parameter. -w renders a few seconds of each plugin at its
 * defaults, playing notes into midiIn and a plucked sawtooth into its inputs, to
 * <folder>/<plugin>.wav (outputs 1 and 2). Exits with 1 if anything failed.
 *
 * The native build differs from the device: other compiler, other libc (memcpy and friends),
 * no relocations. It tests the plugin logic and the ABI usage, not the code generation.
 */
#include <unit_abi.h>
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define FRAMES          48      // one USB frame at 48 kHz
#define MAX_CHANNELS    32
#define MAX_EVENTS      16

static const char * root;
static const char * renderFolder;
static int verbose;
static int failures;
static int midiSent;

#define CHECK(condition, ...) do { \
        if (!(condition)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
    } while (0)

// --- The host API ----------------------------------------------------------------------------

static void hostLog(const char * format, ...) {
    va_list args;
    va_start(args, format);
    printf("  [log] ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

static void * hostAllocate(size_t size) {
    void * p = 0;
    return posix_memalign(&p, 8, size) == 0 ? p : 0;
}

static int localPath(const char * path, char * local, size_t size) {
    if (!root || !path || path[0] != '/') {
        return 0;
    }
    snprintf(local, size, "%s%s", root, path);
    return 1;
}

static int32_t hostFileSize(const char * path) {
    char local[1024];
    struct stat st;
    if (!localPath(path, local, sizeof(local)) || stat(local, &st) != 0 || S_ISDIR(st.st_mode)) {
        return -1;
    }
    return (int32_t)st.st_size;
}

static int32_t hostReadFile(const char * path, uint32_t offset, void * buffer, uint32_t length) {
    char local[1024];
    FILE * f;
    if (!localPath(path, local, sizeof(local)) || !(f = fopen(local, "rb"))) {
        return -1;
    }
    size_t n = fseek(f, offset, SEEK_SET) == 0 ? fread(buffer, 1, length, f) : 0;
    fclose(f);
    return (int32_t)n;
}

static int32_t hostReadDirectory(const char * path, uint32_t index, UnitDirectoryEntry * entry) {
    char local[1024];
    DIR * dir;
    if (!localPath(path, local, sizeof(local)) || !(dir = opendir(local))) {
        return -1;
    }
    struct dirent * d;
    uint32_t n = 0;
    int32_t result = 0;
    while ((d = readdir(dir))) {
        if (d->d_name[0] == '.' || strlen(d->d_name) >= sizeof(entry->name)) {
            continue;
        }
        if (n++ == index) {
            char file[2048];
            struct stat st;
            snprintf(file, sizeof(file), "%s/%s", local, d->d_name);
            stat(file, &st);
            strcpy(entry->name, d->d_name);
            entry->size = (uint32_t)st.st_size;
            entry->isDirectory = S_ISDIR(st.st_mode);
            result = 1;
            break;
        }
    }
    closedir(dir);
    return result;
}

static const UnitHostApi hostApi = {
    .abiVersion = UNIT_ABI_VERSION,
    .size = sizeof(UnitHostApi),
    .log = hostLog,
    .allocate = hostAllocate,
    .fileSize = hostFileSize,
    .readFile = hostReadFile,
    .readDirectory = hostReadDirectory,
    .processHostSize = sizeof(UnitProcessHost),
};

static void play(void) {}
static void stop(void) {}
static void requestTempo(float tempo) {}
static void sendMIDIEvent(struct UnitHostData * hostData, UnitMidiPacket packet) { midiSent++; }

// --- Checks ----------------------------------------------------------------------------------

static int validName(const char * name, size_t maxLength, const char * extra) {
    size_t len = strnlen(name, maxLength);
    if (len == 0 || len == maxLength) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || strchr(extra, c))) {
            return 0;
        }
    }
    return 1;
}

static void runBlocks(const UnitPlugin * plugin, void * instance, const UnitParamEvent * events, size_t eventCount, int blocks) {
    static float inBuffers[MAX_CHANNELS][FRAMES];
    static float outBuffers[MAX_CHANNELS][FRAMES];
    float * in[MAX_CHANNELS];
    float * out[MAX_CHANNELS];
    uint32_t inputs = plugin->getNumInputs();
    uint32_t outputs = plugin->getNumOutputs();
    static double ppq;
    for (int b = 0; b < blocks; b++) {
        for (uint32_t c = 0; c < inputs && c < MAX_CHANNELS; c++) {
            for (int i = 0; i < FRAMES; i++) {
                inBuffers[c][i] = 0.25F * sinf((float)(b * FRAMES + i) * 0.05F * (c + 1));
            }
            in[c] = inBuffers[c];
        }
        for (uint32_t c = 0; c < outputs && c < MAX_CHANNELS; c++) {
            memset(outBuffers[c], 0, sizeof(outBuffers[c]));
            out[c] = outBuffers[c];
        }
        UnitProcessHost host = {
            .play = play, .stop = stop, .requestTempo = requestTempo, .sendMIDIEvent = sendMIDIEvent,
            .timeInfo = {
                .microSeconds = (uint64_t)(ppq * 500000), .sampleRate = 48000, .isPlaying = 1,
                .barStartPos = floor(ppq / 4) * 4, .ppqPos = ppq, .tempo = 120, .timeSigNumerator = 4,
            },
        };
        plugin->process(instance, &host, inputs, in, outputs, out, FRAMES, b == 0 ? events : 0, b == 0 ? eventCount : 0);
        ppq += FRAMES / 24000.0;    // 120 BPM
        for (uint32_t c = 0; c < outputs && c < MAX_CHANNELS; c++) {
            for (int i = 0; i < FRAMES; i++) {
                if (!isfinite(outBuffers[c][i])) {
                    CHECK(0, "%s: output %u is not finite", plugin->name, c + 1);
                    return;
                }
            }
        }
    }
}

static void setAndCheck(const UnitPlugin * plugin, void * instance, const UnitParamInfo * info, double value) {
    UnitParamEvent event = { .time = 0, .id = info->id, .value = value };
    runBlocks(plugin, instance, &event, 1, 2);
    double reported = NAN;
    int has = plugin->getParamValue(instance, info->id, &reported);
    CHECK(has, "%s:%s has no value after setting it", plugin->name, info->name);
    // Units may keep values as float
    CHECK(!has || fabs(reported - value) <= 1e-6 * fmax(1, fabs(value)),
          "%s:%s set to %g, reports %g", plugin->name, info->name, value, reported);
}

static void testPlugin(const UnitPlugin * plugin, uint32_t index) {
    if (!plugin) {
        CHECK(0, "getPlugin(%u) returned NULL", index);
        return;
    }
    CHECK(plugin->size >= sizeof(UnitPlugin), "plugin %u: size %u, ABI needs %zu", index, plugin->size, sizeof(UnitPlugin));
    CHECK(plugin->name && validName(plugin->name, UNIT_NAME_LENGTH, ""), "plugin %u: invalid name", index);
    if (!plugin->createInstance || !plugin->getNumInputs || !plugin->getNumOutputs || !plugin->getParamCount ||
            !plugin->getParamInfo || !plugin->getParamValue || !plugin->process) {
        CHECK(0, "%s: a required function is NULL", plugin->name);
        return;
    }
    uint32_t count = plugin->getParamCount();
    printf("plugin %s: %u in, %u out, %u parameters  (%s %s: %s)\n", plugin->name,
           plugin->getNumInputs(), plugin->getNumOutputs(), count,
           plugin->vendor ? plugin->vendor : "-", plugin->version ? plugin->version : "-",
           plugin->description ? plugin->description : "");

    UnitParamInfo infos[256];
    CHECK(count <= 256, "%s: %u parameters (test limit 256)", plugin->name, count);
    count = count > 256 ? 256 : count;
    for (uint32_t i = 0; i < count; i++) {
        UnitParamInfo * info = &infos[i];
        memset(info, 0x55, sizeof(*info));
        CHECK(plugin->getParamInfo(i, info), "%s: getParamInfo(%u) failed", plugin->name, i);
        CHECK(validName(info->name, sizeof(info->name), ".-"), "%s: parameter %u has an invalid name", plugin->name, i);
        CHECK(memchr(info->module, 0, sizeof(info->module)) != 0, "%s:%s: module not terminated", plugin->name, info->name);
        CHECK(info->id != UNIT_INVALID_ID, "%s:%s: invalid id", plugin->name, info->name);
        CHECK(isfinite(info->minValue) && isfinite(info->maxValue) &&
              info->minValue <= info->defaultValue && info->defaultValue <= info->maxValue,
              "%s:%s: bad range %g..%g default %g", plugin->name, info->name, info->minValue, info->maxValue, info->defaultValue);
        for (uint32_t j = 0; j < i; j++) {
            CHECK(infos[j].id != info->id && strcmp(infos[j].name, info->name) != 0,
                  "%s: parameters %s and %s share a name or id", plugin->name, infos[j].name, info->name);
        }
        if (verbose) {
            char text[64] = "";
            if (!plugin->paramValueToText || !plugin->paramValueToText(info->id, info->defaultValue, text, sizeof(text))) {
                snprintf(text, sizeof(text), "%g", info->defaultValue);
            }
            printf("  %-14s id %-5u flags %-7x %g..%g default %s%s%s\n", info->name, info->id, info->flags,
                   info->minValue, info->maxValue, text, info->module[0] ? "  module " : "", info->module);
        }
    }
    CHECK(!plugin->getParamInfo(count, &infos[0]) || count == 256, "%s: getParamInfo(%u) beyond the count succeeded", plugin->name, count);

    void * instance = plugin->createInstance();
    CHECK(instance, "%s: createInstance() returned NULL", plugin->name);
    if (!instance) {
        return;
    }
    // A new instance starts at the defaults
    for (uint32_t i = 0; i < count; i++) {
        double value;
        if (!(infos[i].flags & UNIT_PARAM_IS_TRIGGER) && plugin->getParamValue(instance, infos[i].id, &value)) {
            CHECK(fabs(value - infos[i].defaultValue) <= 1e-6 * fmax(1, fabs(value)),
                  "%s:%s starts at %g, default is %g", plugin->name, infos[i].name, value, infos[i].defaultValue);
        }
    }
    runBlocks(plugin, instance, 0, 0, 20);
    for (uint32_t i = 0; i < count; i++) {
        const UnitParamInfo * info = &infos[i];
        if (info->flags & UNIT_PARAM_IS_READONLY) {
            continue;
        }
        if (info->flags & UNIT_PARAM_IS_TRIGGER) {
            UnitParamEvent press = { .id = info->id, .value = info->maxValue };
            runBlocks(plugin, instance, &press, 1, 10);
            double value;
            CHECK(!plugin->getParamValue(instance, info->id, &value), "%s:%s is a trigger but has a value", plugin->name, info->name);
            continue;
        }
        setAndCheck(plugin, instance, info, info->minValue);
        setAndCheck(plugin, instance, info, info->maxValue);
        setAndCheck(plugin, instance, info, info->defaultValue);
        char text[64];
        if (plugin->paramValueToText && plugin->paramValueToText(info->id, info->maxValue, text, sizeof(text))) {
            CHECK(memchr(text, 0, sizeof(text)) != 0, "%s:%s: valueToText not terminated", plugin->name, info->name);
            double back;
            if (plugin->paramTextToValue && plugin->paramTextToValue(info->id, text, &back)) {
                CHECK(fabs(back - info->maxValue) <= 1e-3 * fmax(1, fabs(back)), "%s:%s: \"%s\" reads back as %g",
                      plugin->name, info->name, text, back);
            }
        }
    }
    // Every event in one block, as a preset recall sends them
    UnitParamEvent events[MAX_EVENTS];
    size_t eventCount = 0;
    for (uint32_t i = 0; i < count && eventCount < MAX_EVENTS; i++) {
        events[eventCount++] = (UnitParamEvent) { .id = infos[i].id, .value = infos[i].defaultValue };
    }
    runBlocks(plugin, instance, events, eventCount, 10);
    // An unknown id is ignored
    UnitParamEvent unknown = { .id = 0xFFFFFFF0u, .value = 1 };
    runBlocks(plugin, instance, &unknown, 1, 1);

    if (plugin->releaseInstance) {
        plugin->releaseInstance(instance);
        void * again = plugin->createInstance();
        CHECK(again, "%s: no instance after releasing one", plugin->name);
        if (again) {
            plugin->releaseInstance(again);
        }
    }
}

// --- Rendering (-w) ---------------------------------------------------------------------------

#define RENDER_SECONDS  4
#define RENDER_FRAMES   (RENDER_SECONDS * 48000)

static void writeWav(const char * path, const float * left, const float * right, int frames) {
    FILE * f = fopen(path, "wb");
    if (!f) {
        CHECK(0, "can't write %s", path);
        return;
    }
    uint32_t dataSize = (uint32_t)frames * 4;
    uint32_t riffSize = 36 + dataSize;
    uint32_t fmtSize = 16, rate = 48000, byteRate = 48000 * 4;
    uint16_t format = 1, channels = 2, blockAlign = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtSize, 4, 1, f); fwrite(&format, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataSize, 4, 1, f);
    for (int i = 0; i < frames; i++) {
        int16_t s[2] = {
            (int16_t)(fmaxf(-1, fminf(1, left[i])) * 32767),
            (int16_t)(fmaxf(-1, fminf(1, right[i])) * 32767),
        };
        fwrite(s, 2, 2, f);
    }
    fclose(f);
}

// A fresh instance at its defaults; a note every half second into midiIn (C3 E3 G3 C4, note off
// after 0.3 s), a plucked sawtooth at the same pitch into every input. Blocks of varying size,
// from 1 to 64 frames, as the Pi (6 per microframe) and the BeagleBone (about 48) both send.
static void renderPlugin(const UnitPlugin * plugin) {
    static float left[RENDER_FRAMES], right[RENDER_FRAMES];
    static float inBuffers[MAX_CHANNELS][64], outBuffers[MAX_CHANNELS][64];
    static const uint8_t notes[] = { 48, 52, 55, 60 };
    float * in[MAX_CHANNELS];
    float * out[MAX_CHANNELS];
    uint32_t inputs = plugin->getNumInputs();
    uint32_t outputs = plugin->getNumOutputs();
    void * instance = plugin->createInstance();
    if (!instance) {
        return;
    }
    memset(left, 0, sizeof(left));
    memset(right, 0, sizeof(right));
    double phase = 0;
    uint32_t random = 1;
    clock_t start = clock();
    for (int pos = 0; pos < RENDER_FRAMES;) {
        random = random * 1664525 + 1013904223;
        int frames = 1 + (int)((random >> 16) % 64);
        if (frames > RENDER_FRAMES - pos) {
            frames = RENDER_FRAMES - pos;
        }
        // MIDI for the events inside this block
        UnitMidiPacket midi[4];
        size_t midiCount = 0;
        for (int i = 0; i < frames; i++) {
            int t = pos + i;
            int note = notes[(t / 24000) % 4];
            if (t % 24000 == 0) {
                midi[midiCount++] = (UnitMidiPacket) { UNIT_MIDI_CIN_NOTE_ON, 0x90, (uint8_t)note, 100 };
            } else if (t % 24000 == 14400) {
                midi[midiCount++] = (UnitMidiPacket) { UNIT_MIDI_CIN_NOTE_OFF, 0x80, (uint8_t)note, 0 };
            }
        }
        for (int i = 0; i < frames; i++) {
            int t = pos + i;
            float frequency = 440.0F * powf(2.0F, (notes[(t / 24000) % 4] - 69) / 12.0F);
            phase += frequency / 48000.0;
            phase -= floor(phase);
            float envelope = expf(-(float)(t % 24000) / 4800.0F);
            for (uint32_t c = 0; c < inputs && c < MAX_CHANNELS; c++) {
                inBuffers[c][i] = 0.5F * envelope * (float)(2 * phase - 1);
            }
        }
        for (uint32_t c = 0; c < inputs && c < MAX_CHANNELS; c++) {
            in[c] = inBuffers[c];
        }
        for (uint32_t c = 0; c < outputs && c < MAX_CHANNELS; c++) {
            memset(outBuffers[c], 0, sizeof(outBuffers[c]));
            out[c] = outBuffers[c];
        }
        UnitProcessHost host = {
            .play = play, .stop = stop, .requestTempo = requestTempo, .sendMIDIEvent = sendMIDIEvent,
            .timeInfo = {
                .microSeconds = (uint64_t)pos * 1000000 / 48000, .sampleRate = 48000, .isPlaying = 1,
                .ppqPos = pos / 24000.0, .barStartPos = floor(pos / 96000.0) * 4, .tempo = 120,
                .timeSigNumerator = 4,
            },
            .midiIn = midi, .midiInCount = midiCount,
        };
        plugin->process(instance, &host, inputs, in, outputs, out, frames, 0, 0);
        for (int i = 0; i < frames; i++) {
            left[pos + i] = outputs > 0 ? outBuffers[0][i] : 0;
            right[pos + i] = outputs > 1 ? outBuffers[1][i] : left[pos + i];
        }
        pos += frames;
    }
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    float peak = 0;
    double sum = 0;
    int finite = 1;
    for (int i = 0; i < RENDER_FRAMES; i++) {
        finite &= isfinite(left[i]) && isfinite(right[i]);
        peak = fmaxf(peak, fmaxf(fabsf(left[i]), fabsf(right[i])));
        sum += left[i] * left[i] + right[i] * right[i];
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.wav", renderFolder, plugin->name);
    writeWav(path, left, right, RENDER_FRAMES);
    printf("  rendered %s: peak %.3f, rms %.4f, %.1f%% of real time on this Mac (sanitized build)\n",
           path, peak, sqrt(sum / (2.0 * RENDER_FRAMES)), 100 * seconds / RENDER_SECONDS);
    CHECK(finite, "%s: rendered output is not finite", plugin->name);
    CHECK(outputs == 0 || peak > 1e-4F, "%s: rendered silence", plugin->name);
    if (plugin->releaseInstance) {
        plugin->releaseInstance(instance);
    }
}

int main(int argc, char ** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            renderFolder = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [-r <sd card folder>] [-v] [-w <folder>]\n", argv[0]);
            return 2;
        }
    }
    extern const UnitImageHeader __unit_header;
    printf("unit file %s, ABI %u\n", __unit_header.name, __unit_header.abiVersion);
    CHECK(__unit_header.magic == UNIT_IMAGE_MAGIC && __unit_header.abiVersion == UNIT_ABI_VERSION, "bad image header");
    CHECK(validName(__unit_header.name, UNIT_NAME_LENGTH, ""), "invalid unit file name");

    const UnitFactory * factory = unit_entry(&hostApi);
    CHECK(factory && factory->size >= sizeof(UnitFactory) && factory->getPluginCount && factory->getPlugin, "bad factory");
    if (!factory) {
        return 1;
    }
    uint32_t count = factory->getPluginCount();
    CHECK(count > 0, "no plugins");
    for (uint32_t i = 0; i < count; i++) {
        const UnitPlugin * plugin = factory->getPlugin(i);
        for (uint32_t j = 0; j < i; j++) {
            CHECK(!plugin || strcmp(factory->getPlugin(j)->name, plugin->name) != 0, "two plugins called %s", plugin->name);
        }
        testPlugin(plugin, i);
        if (renderFolder && plugin && plugin->createInstance) {
            renderPlugin(plugin);
        }
    }
    CHECK(!factory->getPlugin(count), "getPlugin(%u) beyond the count returned a plugin", count);
    printf("%s: %u plugins, %d MIDI events sent, %d failures\n", failures ? "FAILED" : "ok", count, midiSent, failures);
    return failures ? 1 : 0;
}
