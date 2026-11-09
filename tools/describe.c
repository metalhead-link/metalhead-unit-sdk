/*
 * describe — a unit file's plugins and parameters as JSON, for stores and faceplate previews.
 *
 * `make describe` in a unit directory compiles the unit's sources natively with this file, calls
 * unit_entry() and prints, for every plugin, its descriptor, audio channels and parameters. A
 * parameter the plugin formats itself also gets `texts`: values across its range with its paramValueToText,
 * so a page without a device can show "250 ms" rather than 250 (every step of a stepped
 * parameter up to 128 of them, otherwise 65 points, evenly spaced). No instance is created and
 * no file is read: the unit sees no SD card.
 *
 *   build/host/describe > build/describe.json
 */
#include <unit_abi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define TEXT_POINTS     65
#define MAX_STEPS       128

static void hostLog(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static void * hostAllocate(size_t size) {
    void * p = 0;
    return posix_memalign(&p, 8, size) == 0 ? p : 0;
}

static int32_t noFileSize(const char * path) { return -1; }
static int32_t noReadFile(const char * path, uint32_t offset, void * buffer, uint32_t length) { return -1; }
static int32_t noReadDirectory(const char * path, uint32_t index, UnitDirectoryEntry * entry) { return -1; }

static const UnitHostApi hostApi = {
    .abiVersion = UNIT_ABI_VERSION,
    .size = sizeof(UnitHostApi),
    .log = hostLog,
    .allocate = hostAllocate,
    .fileSize = noFileSize,
    .readFile = noReadFile,
    .readDirectory = noReadDirectory,
    .processHostSize = sizeof(UnitProcessHost),
};

// A JSON string, or null
static void string(const char * s) {
    if (!s) {
        fputs("null", stdout);
        return;
    }
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (c < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

// %.17g gives back the same double; JSON has no NaN or infinity, and the loader refuses them.
static void number(double v) {
    printf("%.17g", isfinite(v) ? v : 0.0);
}

// 1 if the plugin formats this parameter's values itself
static int hasTexts(const UnitPlugin * plugin, const UnitParamInfo * info) {
    char text[64];
    return plugin->paramValueToText &&
        (plugin->paramValueToText(info->id, info->minValue, text, sizeof(text)) ||
         plugin->paramValueToText(info->id, info->maxValue, text, sizeof(text)) ||
         plugin->paramValueToText(info->id, info->defaultValue, text, sizeof(text)));
}

static void texts(const UnitPlugin * plugin, const UnitParamInfo * info) {
    double span = info->maxValue - info->minValue;
    int stepped = (info->flags & (UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM)) != 0;
    int points = stepped && span < MAX_STEPS ? (int)span + 1 : TEXT_POINTS;
    if (span <= 0) {
        points = 1;
    }
    fputs("[", stdout);
    for (int i = 0; i < points; i++) {
        double v = points == 1 ? info->minValue : info->minValue + span * i / (points - 1);
        if (stepped) {
            v = round(v);
        }
        char text[64] = "";
        if (!plugin->paramValueToText || !plugin->paramValueToText(info->id, v, text, sizeof(text))) {
            snprintf(text, sizeof(text), "%g", v);
        }
        text[sizeof(text) - 1] = 0;
        printf("%s[", i ? "," : "");
        number(v);
        fputs(",", stdout);
        string(text);
        fputs("]", stdout);
    }
    fputs("]", stdout);
}

int main(void) {
    const UnitFactory * factory = unit_entry(&hostApi);
    if (!factory || !factory->getPluginCount || !factory->getPlugin) {
        fprintf(stderr, "describe: unit_entry() returned no factory\n");
        return 1;
    }
    extern const UnitImageHeader __unit_header;
    printf("{\"abi\":%d,\"bundle\":", UNIT_ABI_VERSION);
    string(__unit_header.name);
    fputs(",\"plugins\":[", stdout);
    uint32_t count = factory->getPluginCount();
    for (uint32_t p = 0; p < count; p++) {
        const UnitPlugin * plugin = factory->getPlugin(p);
        if (!plugin) {
            continue;
        }
        printf("%s\n{\"name\":", p ? "," : "");
        string(plugin->name);
        fputs(",\"vendor\":", stdout);
        string(plugin->vendor);
        fputs(",\"version\":", stdout);
        string(plugin->version);
        fputs(",\"description\":", stdout);
        string(plugin->description);
        printf(",\"inputs\":%u,\"outputs\":%u,\"params\":[",
               plugin->getNumInputs ? plugin->getNumInputs() : 0, plugin->getNumOutputs ? plugin->getNumOutputs() : 0);
        uint32_t paramCount = plugin->getParamCount ? plugin->getParamCount() : 0;
        for (uint32_t i = 0; i < paramCount; i++) {
            UnitParamInfo info;
            if (!plugin->getParamInfo(i, &info)) {
                continue;
            }
            info.name[sizeof(info.name) - 1] = 0;
            info.module[sizeof(info.module) - 1] = 0;
            printf("%s\n  {\"id\":%u,\"flags\":%u,\"name\":", i ? "," : "", info.id, info.flags);
            string(info.name);
            fputs(",\"module\":", stdout);
            string(info.module);
            fputs(",\"min\":", stdout);
            number(info.minValue);
            fputs(",\"max\":", stdout);
            number(info.maxValue);
            fputs(",\"default\":", stdout);
            number(info.defaultValue);
            if (hasTexts(plugin, &info)) {
                fputs(",\"texts\":", stdout);
                texts(plugin, &info);
            }
            fputs("}", stdout);
        }
        fputs("]}", stdout);
    }
    fputs("]}\n", stdout);
    return 0;
}
