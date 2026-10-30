/*
 * unitclap — a unit file's plugins as CLAP plugins, for a DAW on the Mac.
 *
 * The unit ABI is modelled on CLAP (see unit_abi.h), so this is mostly a translation: the
 * unit's factory becomes the CLAP plugin factory, UnitParamInfo becomes clap_param_info and
 * CLAP parameter events become UnitParamEvents. clap-wrapper turns the resulting CLAP into a
 * VST3 and an AUv2 (CMakeLists.txt). The unit's sources are compiled natively, as for
 * `make test`; nothing in them changes.
 *
 * What the firmware does and this does too:
 *   - process() gets blocks of one millisecond at most (48 frames at 48 kHz), like the USB
 *     frames on the device; parameter and MIDI events land at the start of the block they
 *     fall in.
 *   - outputs start zeroed, missing inputs are silence, file access fails inside process().
 *   - the SD card is a folder: $METALHEAD_SD_ROOT, else ~/Music/metalhead
 *     (/samples/drums is ~/Music/metalhead/samples/drums).
 *   - log() goes to the unified log (Console.app, subsystem "metalhead.unit") and stderr.
 *
 * What a DAW adds:
 *   - MIDI notes play trigger parameters (Drums): the n-th trigger is note 36 + n, the velocity
 *     its value. All MIDI also reaches the unit's midiIn, as on the device.
 *   - The plugin state is the parameter values, as text: "<id> <value> <name>" per line.
 *
 * What a DAW doesn't do: play/stop/requestTempo (the DAW owns the transport; they're ignored),
 * and VST3 and AU hosts don't pass raw MIDI or SysEx to or from a plugin, so the Launchpad units
 * (VUMeter, ClipGrid) only talk to a Launchpad in a CLAP host.
 */
#include <clap/clap.h>
#include <unit_abi.h>

#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "unitclap.h"

#define MAX_PLUGINS         32
#define MAX_CHANNELS        32
#define MAX_PORTS           (MAX_CHANNELS / 2)
#define MAX_PARAMS          256
#define MAX_TRIGGERS        64
#define MAX_BLOCK           256     // frames per process() call; one millisecond is less
#define MAX_MIDI_IN         256     // per block
#define SYSEX_ARENA         4096    // SysEx bytes the unit can send per host block
#define FIRST_TRIGGER_NOTE  36      // C1, the bass drum in General MIDI
#define ID_PREFIX           "metalhead.unit."
#define STATE_HEADER        "metalhead-unit-state 1"

// --- The host API: what the firmware offers units, on the Mac --------------------------------

static os_log_t logHandle;
static char sdRoot[1024];
static _Thread_local int inProcess;     // the firmware refuses file access while units process

static void hostLog(const char * format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    os_log(logHandle, "%{public}s", message);
    fprintf(stderr, "metalhead unit: %s\n", message);
}

static void * hostAllocate(size_t size) {
    void * p = 0;
    return posix_memalign(&p, 8, size ? size : 1) == 0 ? p : 0;
}

static int localPath(const char * path, char * local, size_t size) {
    if (inProcess || !path || path[0] != '/' || strstr(path, "/..")) {
        return 0;
    }
    return snprintf(local, size, "%s%s", sdRoot, path) < (int)size;
}

static int32_t hostFileSize(const char * path) {
    char local[2048];
    struct stat st;
    if (!localPath(path, local, sizeof(local)) || stat(local, &st) != 0 || S_ISDIR(st.st_mode)) {
        return -1;
    }
    return st.st_size > INT32_MAX ? -1 : (int32_t)st.st_size;
}

static int32_t hostReadFile(const char * path, uint32_t offset, void * buffer, uint32_t length) {
    char local[2048];
    FILE * f;
    if (!localPath(path, local, sizeof(local)) || !(f = fopen(local, "rb"))) {
        return -1;
    }
    size_t n = fseeko(f, offset, SEEK_SET) == 0 ? fread(buffer, 1, length, f) : 0;
    fclose(f);
    return (int32_t)n;
}

static int32_t hostReadDirectory(const char * path, uint32_t index, UnitDirectoryEntry * entry) {
    char local[2048];
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
            char file[4096];
            struct stat st = { 0 };
            snprintf(file, sizeof(file), "%s/%s", local, d->d_name);
            stat(file, &st);
            strcpy(entry->name, d->d_name);
            entry->size = st.st_size > UINT32_MAX ? UINT32_MAX : (uint32_t)st.st_size;
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

// --- Plugin types: one per plugin in the unit file -------------------------------------------

typedef struct PluginType {
    const UnitPlugin * unit;
    char id[sizeof(ID_PREFIX) + UNIT_NAME_LENGTH];
    const char * features[5];
    clap_plugin_descriptor_t descriptor;
    uint32_t inputs;
    uint32_t outputs;
    uint32_t paramCount;
    UnitParamInfo params[MAX_PARAMS];
    uint32_t triggerCount;
    uint32_t triggers[MAX_TRIGGERS];    // parameter indexes of the triggers, in order
} PluginType;

static PluginType types[MAX_PLUGINS];
static uint32_t typeCount;

static uint32_t paramIndex(const PluginType * type, clap_id id) {
    for (uint32_t i = 0; i < type->paramCount; i++) {
        if (type->params[i].id == id) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int isPersistent(const UnitParamInfo * info) {
    return !(info->flags & (UNIT_PARAM_IS_TRIGGER | UNIT_PARAM_IS_READONLY));
}

// Audio ports: stereo pairs of the unit's channels, and a mono port for an odd last channel.
static uint32_t portCount(uint32_t channels) {
    return (channels + 1) / 2;
}

static uint32_t portChannels(uint32_t channels, uint32_t port) {
    return channels - 2 * port >= 2 ? 2 : 1;
}

// --- An instance -------------------------------------------------------------------------------

typedef struct Wrapper {
    clap_plugin_t plugin;
    const clap_host_t * host;
    const clap_host_params_t * hostParams;
    const PluginType * type;
    void * instance;
    double sampleRate;
    int active;

    // Values set while process() isn't running (params.flush, state.load): the next process()
    // sends them. pending[i] is set after value[i] is written.
    double pendingValue[MAX_PARAMS];
    atomic_int pending[MAX_PARAMS];
    double lastValue[MAX_PARAMS];       // what the host was told last (read-only parameters, triggers)

    // During process()
    const clap_output_events_t * out;
    uint32_t blockOffset;
    uint8_t sysex[SYSEX_ARENA];
    uint32_t sysexUsed;
    uint32_t sysexStart;                // of the SysEx message being received from the unit
    int inSysex;
    uint64_t samples;                   // processed since activation, when the host has no steady time
    int warnedTransport;

    float inputBuffers[MAX_CHANNELS][MAX_BLOCK];
    float silence[MAX_BLOCK];
    UnitParamEvent params[MAX_PARAMS + MAX_MIDI_IN];
    UnitMidiPacket midiIn[MAX_MIDI_IN];
} Wrapper;

static Wrapper * wrapperOf(const clap_plugin_t * plugin) {
    return (Wrapper *)plugin->plugin_data;
}

// --- Transport and MIDI out (UnitProcessHost) ------------------------------------------------

static Wrapper * processing(struct UnitHostData * hostData) {
    return (Wrapper *)hostData;
}

static void play(void) {}
static void stop(void) {}
static void requestTempo(float tempoBpm) {}

static void sendMidiBytes(Wrapper * w, const uint8_t * bytes) {
    clap_event_midi_t event = {
        .header = {
            .size = sizeof(event), .time = w->blockOffset, .space_id = CLAP_CORE_EVENT_SPACE_ID,
            .type = CLAP_EVENT_MIDI, .flags = 0,
        },
        .port_index = 0,
        .data = { bytes[0], bytes[1], bytes[2] },
    };
    w->out->try_push(w->out, &event.header);
}

static void sysexBytes(Wrapper * w, const uint8_t * bytes, int count, int end) {
    if (!w->inSysex) {
        w->inSysex = 1;
        w->sysexStart = w->sysexUsed;
    }
    for (int i = 0; i < count; i++) {
        if (w->sysexUsed < SYSEX_ARENA) {
            w->sysex[w->sysexUsed++] = bytes[i];
        }
    }
    if (!end) {
        return;
    }
    w->inSysex = 0;
    if (w->sysexUsed == SYSEX_ARENA) {
        return;                 // didn't fit: dropped, as the device drops MIDI beyond its limit
    }
    clap_event_midi_sysex_t event = {
        .header = {
            .size = sizeof(event), .time = w->blockOffset, .space_id = CLAP_CORE_EVENT_SPACE_ID,
            .type = CLAP_EVENT_MIDI_SYSEX, .flags = 0,
        },
        .port_index = 0,
        .buffer = &w->sysex[w->sysexStart],
        .size = w->sysexUsed - w->sysexStart,
    };
    w->out->try_push(w->out, &event.header);
}

// A USB-MIDI event packet to MIDI bytes (USB MIDI 1.0, table 4-1). The cable number is ignored:
// the plugin has one MIDI output.
static void sendMIDIEvent(struct UnitHostData * hostData, UnitMidiPacket packet) {
    Wrapper * w = processing(hostData);
    const uint8_t bytes[3] = { packet.midi0, packet.midi1, packet.midi2 };
    switch (packet.header & 0x0F) {
    case UNIT_MIDI_CIN_SYSEX_START:
        sysexBytes(w, bytes, 3, 0);
        break;
    case UNIT_MIDI_CIN_SYSEX_END_1BYTE:
        if (w->inSysex || bytes[0] == 0xF0) {
            sysexBytes(w, bytes, 1, 1);
        } else {
            sendMidiBytes(w, (const uint8_t[3]) { bytes[0], 0, 0 });    // single-byte system common
        }
        break;
    case UNIT_MIDI_CIN_SYSEX_END_2BYTE:
        sysexBytes(w, bytes, 2, 1);
        break;
    case UNIT_MIDI_CIN_SYSEX_END_3BYTE:
        sysexBytes(w, bytes, 3, 1);
        break;
    case 0x2: case 0x3:                         // system common
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE:
        sendMidiBytes(w, bytes);
        break;
    case 0xF:                                   // single byte (real time)
        sendMidiBytes(w, (const uint8_t[3]) { bytes[0], 0, 0 });
        break;
    }
}

// MIDI bytes to a USB-MIDI packet on cable 0
static int midiToPacket(const uint8_t * data, UnitMidiPacket * packet) {
    uint8_t status = data[0];
    uint8_t cin;
    if (status >= 0x80 && status < 0xF0) {
        cin = status >> 4;
    } else if (status == 0xF2) {
        cin = 0x3;
    } else if (status == 0xF1 || status == 0xF3) {
        cin = 0x2;
    } else if (status >= 0xF4) {
        cin = 0xF;
    } else {
        return 0;
    }
    *packet = (UnitMidiPacket) { cin, data[0], data[1], data[2] };
    return 1;
}

// --- Parameters (clap_plugin_params) ---------------------------------------------------------

static uint32_t paramsCount(const clap_plugin_t * plugin) {
    return wrapperOf(plugin)->type->paramCount;
}

static bool paramsGetInfo(const clap_plugin_t * plugin, uint32_t index, clap_param_info_t * info) {
    const PluginType * type = wrapperOf(plugin)->type;
    if (index >= type->paramCount) {
        return false;
    }
    const UnitParamInfo * p = &type->params[index];
    memset(info, 0, sizeof(*info));
    info->id = p->id;
    // The UNIT_PARAM_IS_* flags CLAP has are CLAP's values
    info->flags = (p->flags & (UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_PERIODIC | UNIT_PARAM_IS_HIDDEN |
                               UNIT_PARAM_IS_READONLY | UNIT_PARAM_IS_BYPASS | UNIT_PARAM_IS_AUTOMATABLE |
                               UNIT_PARAM_IS_ENUM))
                  | CLAP_PARAM_REQUIRES_PROCESS;
    snprintf(info->name, sizeof(info->name), "%s", p->name);
    snprintf(info->module, sizeof(info->module), "%s", p->module);
    info->min_value = p->minValue;
    info->max_value = p->maxValue;
    info->default_value = (p->flags & UNIT_PARAM_IS_TRIGGER) ? p->minValue : p->defaultValue;
    return true;
}

static bool paramsGetValue(const clap_plugin_t * plugin, clap_id id, double * value) {
    Wrapper * w = wrapperOf(plugin);
    uint32_t i = paramIndex(w->type, id);
    if (i == UINT32_MAX) {
        return false;
    }
    if (atomic_load_explicit(&w->pending[i], memory_order_acquire)) {
        *value = w->pendingValue[i];
    } else if (!w->type->unit->getParamValue(w->instance, id, value)) {
        *value = w->lastValue[i];       // a trigger: its last value
    }
    return true;
}

static bool paramsValueToText(const clap_plugin_t * plugin, clap_id id, double value, char * text, uint32_t size) {
    const PluginType * type = wrapperOf(plugin)->type;
    uint32_t i = paramIndex(type, id);
    if (i == UINT32_MAX || size == 0) {
        return false;
    }
    if (type->unit->paramValueToText && type->unit->paramValueToText(id, value, text, size)) {
        text[size - 1] = 0;
        return true;
    }
    snprintf(text, size, (type->params[i].flags & UNIT_PARAM_IS_STEPPED) ? "%.0f" : "%.3g", value);
    return true;
}

static bool paramsTextToValue(const clap_plugin_t * plugin, clap_id id, const char * text, double * value) {
    const PluginType * type = wrapperOf(plugin)->type;
    uint32_t i = paramIndex(type, id);
    if (i == UINT32_MAX) {
        return false;
    }
    if (type->unit->paramTextToValue && type->unit->paramTextToValue(id, text, value)) {
        return true;
    }
    char * end;
    *value = strtod(text, &end);
    return end != text;
}

static void setPending(Wrapper * w, uint32_t i, double value) {
    const UnitParamInfo * info = &w->type->params[i];
    if (value < info->minValue) value = info->minValue;
    if (value > info->maxValue) value = info->maxValue;
    w->pendingValue[i] = value;
    atomic_store_explicit(&w->pending[i], 1, memory_order_release);
}

// Called on the main thread when the plugin isn't processing, else on the audio thread
// instead of process(). Units only take parameters in process(), so they're kept for the next
// block; getValue() reports them meanwhile.
static void paramsFlush(const clap_plugin_t * plugin, const clap_input_events_t * in, const clap_output_events_t * out) {
    Wrapper * w = wrapperOf(plugin);
    uint32_t n = in->size(in);
    for (uint32_t e = 0; e < n; e++) {
        const clap_event_header_t * header = in->get(in, e);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t * pv = (const clap_event_param_value_t *)header;
            uint32_t i = paramIndex(w->type, pv->param_id);
            if (i != UINT32_MAX && !(w->type->params[i].flags & UNIT_PARAM_IS_READONLY)) {
                setPending(w, i, pv->value);
            }
        }
    }
}

static const clap_plugin_params_t paramsExtension = {
    .count = paramsCount,
    .get_info = paramsGetInfo,
    .get_value = paramsGetValue,
    .value_to_text = paramsValueToText,
    .text_to_value = paramsTextToValue,
    .flush = paramsFlush,
};

// --- State (clap_plugin_state): the parameter values as text ---------------------------------

static bool writeAll(const clap_ostream_t * stream, const char * text, size_t length) {
    while (length > 0) {
        int64_t n = stream->write(stream, text, length);
        if (n <= 0) {
            return false;
        }
        text += n;
        length -= (size_t)n;
    }
    return true;
}

static bool stateSave(const clap_plugin_t * plugin, const clap_ostream_t * stream) {
    Wrapper * w = wrapperOf(plugin);
    char line[128];
    int len = snprintf(line, sizeof(line), STATE_HEADER " %s\n", w->type->unit->name);
    if (!writeAll(stream, line, len)) {
        return false;
    }
    for (uint32_t i = 0; i < w->type->paramCount; i++) {
        const UnitParamInfo * info = &w->type->params[i];
        double value;
        if (isPersistent(info) && paramsGetValue(plugin, info->id, &value)) {
            len = snprintf(line, sizeof(line), "%u %.17g %s\n", info->id, value, info->name);
            if (!writeAll(stream, line, len)) {
                return false;
            }
        }
    }
    return true;
}

static bool stateLoad(const clap_plugin_t * plugin, const clap_istream_t * stream) {
    Wrapper * w = wrapperOf(plugin);
    const size_t size = 65536;
    char * text = malloc(size);
    size_t length = 0;
    int64_t n;
    if (!text) {
        return false;
    }
    while (length < size - 1 && (n = stream->read(stream, text + length, size - 1 - length)) > 0) {
        length += (size_t)n;
    }
    text[length] = 0;
    if (strncmp(text, STATE_HEADER, strlen(STATE_HEADER)) != 0) {
        free(text);
        return false;
    }
    // Parameters the state doesn't name go back to their defaults
    double values[MAX_PARAMS];
    for (uint32_t i = 0; i < w->type->paramCount; i++) {
        values[i] = w->type->params[i].defaultValue;
    }
    for (char * line = strchr(text, '\n'); line; line = strchr(line, '\n')) {
        line++;
        unsigned id;
        double value;
        if (sscanf(line, "%u %lf", &id, &value) == 2) {
            uint32_t i = paramIndex(w->type, id);
            if (i != UINT32_MAX && isfinite(value)) {
                values[i] = value;
            }
        }
    }
    free(text);
    for (uint32_t i = 0; i < w->type->paramCount; i++) {
        if (isPersistent(&w->type->params[i])) {
            setPending(w, i, values[i]);
        }
    }
    if (w->hostParams) {
        w->hostParams->rescan(w->host, CLAP_PARAM_RESCAN_VALUES);
        w->hostParams->request_flush(w->host);
    }
    return true;
}

static const clap_plugin_state_t stateExtension = {
    .save = stateSave,
    .load = stateLoad,
};

// --- Ports ------------------------------------------------------------------------------------

static uint32_t audioPortsCount(const clap_plugin_t * plugin, bool isInput) {
    const PluginType * type = wrapperOf(plugin)->type;
    return portCount(isInput ? type->inputs : type->outputs);
}

static bool audioPortsGet(const clap_plugin_t * plugin, uint32_t index, bool isInput, clap_audio_port_info_t * info) {
    const PluginType * type = wrapperOf(plugin)->type;
    uint32_t channels = isInput ? type->inputs : type->outputs;
    if (index >= portCount(channels)) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->channel_count = portChannels(channels, index);
    info->port_type = info->channel_count == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->in_place_pair = CLAP_INVALID_ID;
    if (channels <= 2) {
        snprintf(info->name, sizeof(info->name), "%s", isInput ? "In" : "Out");
    } else if (info->channel_count == 2) {
        snprintf(info->name, sizeof(info->name), "%s %u-%u", isInput ? "In" : "Out", 2 * index + 1, 2 * index + 2);
    } else {
        snprintf(info->name, sizeof(info->name), "%s %u", isInput ? "In" : "Out", 2 * index + 1);
    }
    return true;
}

static const clap_plugin_audio_ports_t audioPortsExtension = {
    .count = audioPortsCount,
    .get = audioPortsGet,
};

static uint32_t notePortsCount(const clap_plugin_t * plugin, bool isInput) {
    return 1;
}

static bool notePortsGet(const clap_plugin_t * plugin, uint32_t index, bool isInput, clap_note_port_info_t * info) {
    if (index != 0) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    info->id = 0;
    info->supported_dialects = isInput ? (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP) : CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    snprintf(info->name, sizeof(info->name), "MIDI %s", isInput ? "In" : "Out");
    return true;
}

static const clap_plugin_note_ports_t notePortsExtension = {
    .count = notePortsCount,
    .get = notePortsGet,
};

// Units don't say how long they ring on; keep processing them.
static uint32_t tailGet(const clap_plugin_t * plugin) {
    return UINT32_MAX;
}

static const clap_plugin_tail_t tailExtension = {
    .get = tailGet,
};

// --- Processing ----------------------------------------------------------------------------------

static void addParam(Wrapper * w, size_t * count, uint32_t i, double value) {
    const UnitParamInfo * info = &w->type->params[i];
    if ((info->flags & UNIT_PARAM_IS_TRIGGER) && value <= info->minValue) {
        w->lastValue[i] = value;    // a trigger let go: nothing to play
        return;
    }
    if (*count < MAX_PARAMS + MAX_MIDI_IN) {
        w->params[(*count)++] = (UnitParamEvent) { .time = 0, .id = w->type->params[i].id, .value = value };
        w->lastValue[i] = value;
    }
}

static void addMidi(Wrapper * w, size_t * paramCount, size_t * midiCount, UnitMidiPacket packet) {
    if (*midiCount < MAX_MIDI_IN) {
        w->midiIn[(*midiCount)++] = packet;
    }
    // A note on plays the trigger for its key
    if ((packet.header & 0x0F) == UNIT_MIDI_CIN_NOTE_ON && packet.midi2 > 0 && packet.midi1 >= FIRST_TRIGGER_NOTE) {
        uint32_t t = packet.midi1 - FIRST_TRIGGER_NOTE;
        if (t < w->type->triggerCount) {
            uint32_t i = w->type->triggers[t];
            const UnitParamInfo * info = &w->type->params[i];
            addParam(w, paramCount, i, info->minValue + packet.midi2 / 127.0 * (info->maxValue - info->minValue));
        }
    }
}

static void inputEvent(Wrapper * w, const clap_event_header_t * header, size_t * paramCount, size_t * midiCount) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
        return;
    }
    switch (header->type) {
    case CLAP_EVENT_PARAM_VALUE: {
        const clap_event_param_value_t * pv = (const clap_event_param_value_t *)header;
        uint32_t i = paramIndex(w->type, pv->param_id);
        if (i != UINT32_MAX && !(w->type->params[i].flags & UNIT_PARAM_IS_READONLY)) {
            atomic_store_explicit(&w->pending[i], 0, memory_order_relaxed);    // superseded
            addParam(w, paramCount, i, pv->value);
        }
        break;
    }
    case CLAP_EVENT_MIDI: {
        UnitMidiPacket packet;
        if (midiToPacket(((const clap_event_midi_t *)header)->data, &packet)) {
            addMidi(w, paramCount, midiCount, packet);
        }
        break;
    }
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF: {
        const clap_event_note_t * note = (const clap_event_note_t *)header;
        if (note->key < 0 || note->key > 127) {
            break;
        }
        int channel = note->channel >= 0 && note->channel < 16 ? note->channel : 0;
        int on = header->type == CLAP_EVENT_NOTE_ON;
        int velocity = (int)lround(note->velocity * 127);
        velocity = velocity < 0 ? 0 : velocity > 127 ? 127 : velocity;
        if (on && velocity == 0) {
            velocity = 1;
        }
        uint8_t status = (on ? 0x90 : 0x80) | channel;
        addMidi(w, paramCount, midiCount, (UnitMidiPacket) { status >> 4, status, (uint8_t)note->key, (uint8_t)velocity });
        break;
    }
    case CLAP_EVENT_MIDI_SYSEX: {
        // To USB-MIDI SysEx packets: three bytes each, the last one says how many it holds
        const clap_event_midi_sysex_t * sx = (const clap_event_midi_sysex_t *)header;
        for (uint32_t pos = 0; pos < sx->size; pos += 3) {
            uint32_t left = sx->size - pos;
            uint8_t b[3] = { sx->buffer[pos], left > 1 ? sx->buffer[pos + 1] : 0, left > 2 ? sx->buffer[pos + 2] : 0 };
            uint8_t cin = left > 3 ? UNIT_MIDI_CIN_SYSEX_START : left == 3 ? UNIT_MIDI_CIN_SYSEX_END_3BYTE
                        : left == 2 ? UNIT_MIDI_CIN_SYSEX_END_2BYTE : UNIT_MIDI_CIN_SYSEX_END_1BYTE;
            addMidi(w, paramCount, midiCount, (UnitMidiPacket) { cin, b[0], b[1], b[2] });
        }
        break;
    }
    }
}

static void transportAt(Wrapper * w, const clap_process_t * process, uint32_t offset, UnitTimeInfo * ti) {
    const clap_event_transport_t * t = process->transport;
    double sampleRate = w->sampleRate;
    int64_t steady = process->steady_time >= 0 ? process->steady_time : (int64_t)w->samples;
    *ti = (UnitTimeInfo) {
        .microSeconds = (uint64_t)((double)(steady + offset) * 1e6 / sampleRate),
        .sampleRate = (float)sampleRate,
        .isPlaying = 0,
        .barStartPos = 0,
        .ppqPos = 0,
        .tempo = 120,
        .timeSigNumerator = 4,
    };
    if (!t) {
        return;
    }
    if (t->flags & CLAP_TRANSPORT_HAS_TEMPO) {
        ti->tempo = (float)t->tempo;
    }
    if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) {
        ti->timeSigNumerator = t->tsig_num;
    }
    ti->isPlaying = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
    if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
        ti->ppqPos = (double)t->song_pos_beats / CLAP_BEATTIME_FACTOR;
        ti->barStartPos = (double)t->bar_start / CLAP_BEATTIME_FACTOR;
        if (ti->isPlaying && ti->tempo > 0) {
            ti->ppqPos += offset * ti->tempo / (60.0 * sampleRate);
        }
    }
}

// Read-only parameters are the unit's output: tell the host when they change
static void reportReadOnly(Wrapper * w, uint32_t time) {
    for (uint32_t i = 0; i < w->type->paramCount; i++) {
        const UnitParamInfo * info = &w->type->params[i];
        double value;
        if ((info->flags & UNIT_PARAM_IS_READONLY) && w->type->unit->getParamValue(w->instance, info->id, &value) &&
                value != w->lastValue[i]) {
            w->lastValue[i] = value;
            clap_event_param_value_t event = {
                .header = {
                    .size = sizeof(event), .time = time, .space_id = CLAP_CORE_EVENT_SPACE_ID,
                    .type = CLAP_EVENT_PARAM_VALUE, .flags = 0,
                },
                .param_id = info->id, .cookie = 0, .note_id = -1, .port_index = -1, .channel = -1, .key = -1,
                .value = value,
            };
            w->out->try_push(w->out, &event.header);
        }
    }
}

static clap_process_status pluginProcess(const clap_plugin_t * plugin, const clap_process_t * process) {
    Wrapper * w = wrapperOf(plugin);
    const PluginType * type = w->type;
    uint32_t frames = process->frames_count;
    uint32_t eventCount = process->in_events->size(process->in_events);
    uint32_t nextEvent = 0;
    uint32_t block = (uint32_t)(w->sampleRate / 1000);     // one USB frame, as on the device
    block = block < 16 ? 16 : block > MAX_BLOCK ? MAX_BLOCK : block;

    w->out = process->out_events;
    w->sysexUsed = 0;
    w->inSysex = 0;

    float * in[MAX_CHANNELS];
    float * out[MAX_CHANNELS];
    for (uint32_t offset = 0; offset < frames; offset += block) {
        uint32_t n = frames - offset < block ? frames - offset : block;
        size_t paramCount = 0;
        size_t midiCount = 0;

        // Values set outside process() first, then the events of this block, in order
        for (uint32_t i = 0; i < type->paramCount; i++) {
            if (atomic_exchange_explicit(&w->pending[i], 0, memory_order_acquire)) {
                addParam(w, &paramCount, i, w->pendingValue[i]);
            }
        }
        while (nextEvent < eventCount) {
            const clap_event_header_t * header = process->in_events->get(process->in_events, nextEvent);
            if (header->time >= offset + n && offset + n < frames) {
                break;
            }
            inputEvent(w, header, &paramCount, &midiCount);
            nextEvent++;
        }

        // Inputs are copied: the host may process in place, and the outputs start zeroed
        for (uint32_t c = 0; c < type->inputs; c++) {
            uint32_t port = c / 2, ch = c % 2;
            const clap_audio_buffer_t * buffer = port < process->audio_inputs_count ? &process->audio_inputs[port] : 0;
            if (buffer && buffer->data32 && ch < buffer->channel_count && buffer->data32[ch]) {
                memcpy(w->inputBuffers[c], buffer->data32[ch] + offset, n * sizeof(float));
                in[c] = w->inputBuffers[c];
            } else {
                memset(w->inputBuffers[c], 0, n * sizeof(float));
                in[c] = w->inputBuffers[c];
            }
        }
        for (uint32_t c = 0; c < type->outputs; c++) {
            uint32_t port = c / 2, ch = c % 2;
            const clap_audio_buffer_t * buffer = port < process->audio_outputs_count ? &process->audio_outputs[port] : 0;
            if (buffer && buffer->data32 && ch < buffer->channel_count && buffer->data32[ch]) {
                out[c] = buffer->data32[ch] + offset;
            } else {
                out[c] = w->silence;        // nowhere to go; the unit still gets a buffer
            }
            memset(out[c], 0, n * sizeof(float));
        }

        UnitProcessHost host = {
            .play = play,
            .stop = stop,
            .requestTempo = requestTempo,
            .sendMIDIEvent = sendMIDIEvent,
            .hostData = (struct UnitHostData *)w,
            .midiIn = w->midiIn,
            .midiInCount = midiCount,
        };
        transportAt(w, process, offset, &host.timeInfo);
        w->blockOffset = offset;
        inProcess = 1;
        type->unit->process(w->instance, &host, type->inputs, in, type->outputs, out, n, w->params, paramCount);
        inProcess = 0;
        reportReadOnly(w, offset);
    }
    // Events after the last frame (a zero-frame block) still count
    while (nextEvent < eventCount) {
        size_t paramCount = 0, midiCount = 0;
        inputEvent(w, process->in_events->get(process->in_events, nextEvent++), &paramCount, &midiCount);
        for (size_t p = 0; p < paramCount; p++) {
            uint32_t i = paramIndex(type, w->params[p].id);
            setPending(w, i, w->params[p].value);
        }
    }
    w->samples += frames;
    w->out = 0;
    return CLAP_PROCESS_CONTINUE;
}

// --- The CLAP plugin --------------------------------------------------------------------------

static bool pluginInit(const clap_plugin_t * plugin) {
    Wrapper * w = wrapperOf(plugin);
    w->hostParams = (const clap_host_params_t *)w->host->get_extension(w->host, CLAP_EXT_PARAMS);
    return true;
}

static void pluginDestroy(const clap_plugin_t * plugin) {
    Wrapper * w = wrapperOf(plugin);
    if (w->type->unit->releaseInstance) {
        w->type->unit->releaseInstance(w->instance);
    } else {
        hostLog("%s has no releaseInstance: this instance stays in use until the DAW quits", w->type->unit->name);
    }
    free(w);
}

static bool pluginActivate(const clap_plugin_t * plugin, double sampleRate, uint32_t minFrames, uint32_t maxFrames) {
    Wrapper * w = wrapperOf(plugin);
    w->sampleRate = sampleRate > 0 ? sampleRate : 48000;
    w->samples = 0;
    w->active = 1;
    return true;
}

static void pluginDeactivate(const clap_plugin_t * plugin) {
    wrapperOf(plugin)->active = 0;
}

static bool pluginStartProcessing(const clap_plugin_t * plugin) { return true; }
static void pluginStopProcessing(const clap_plugin_t * plugin) {}
static void pluginReset(const clap_plugin_t * plugin) {}
static void pluginOnMainThread(const clap_plugin_t * plugin) {}

static const void * pluginGetExtension(const clap_plugin_t * plugin, const char * id) {
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &paramsExtension;
    if (!strcmp(id, CLAP_EXT_STATE)) return &stateExtension;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPortsExtension;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notePortsExtension;
    if (!strcmp(id, CLAP_EXT_TAIL)) return &tailExtension;
    return 0;
}

static const clap_plugin_t * createPlugin(const clap_plugin_factory_t * factory, const clap_host_t * host, const char * id) {
    if (!clap_version_is_compatible(host->clap_version)) {
        return 0;
    }
    for (uint32_t t = 0; t < typeCount; t++) {
        if (strcmp(types[t].id, id) != 0) {
            continue;
        }
        void * instance = types[t].unit->createInstance();
        if (!instance) {
            hostLog("%s: no instance available (the unit has a fixed number)", types[t].unit->name);
            return 0;
        }
        Wrapper * w = calloc(1, sizeof(Wrapper));
        if (!w) {
            if (types[t].unit->releaseInstance) {
                types[t].unit->releaseInstance(instance);
            }
            return 0;
        }
        w->host = host;
        w->type = &types[t];
        w->instance = instance;
        w->sampleRate = 48000;
        for (uint32_t i = 0; i < types[t].paramCount; i++) {
            w->lastValue[i] = types[t].params[i].flags & UNIT_PARAM_IS_TRIGGER
                ? types[t].params[i].minValue : types[t].params[i].defaultValue;
        }
        w->plugin = (clap_plugin_t) {
            .desc = &types[t].descriptor,
            .plugin_data = w,
            .init = pluginInit,
            .destroy = pluginDestroy,
            .activate = pluginActivate,
            .deactivate = pluginDeactivate,
            .start_processing = pluginStartProcessing,
            .stop_processing = pluginStopProcessing,
            .reset = pluginReset,
            .process = pluginProcess,
            .get_extension = pluginGetExtension,
            .on_main_thread = pluginOnMainThread,
        };
        return &w->plugin;
    }
    return 0;
}

static uint32_t factoryCount(const clap_plugin_factory_t * factory) {
    return typeCount;
}

static const clap_plugin_descriptor_t * factoryDescriptor(const clap_plugin_factory_t * factory, uint32_t index) {
    return index < typeCount ? &types[index].descriptor : 0;
}

static const clap_plugin_factory_t pluginFactory = {
    .get_plugin_count = factoryCount,
    .get_plugin_descriptor = factoryDescriptor,
    .create_plugin = createPlugin,
};

// --- Entry ----------------------------------------------------------------------------------

static void describe(PluginType * type, const UnitPlugin * unit) {
    type->unit = unit;
    snprintf(type->id, sizeof(type->id), ID_PREFIX "%s", unit->name);
    type->inputs = unit->getNumInputs();
    type->outputs = unit->getNumOutputs();
    if (type->inputs > MAX_CHANNELS) type->inputs = MAX_CHANNELS;
    if (type->outputs > MAX_CHANNELS) type->outputs = MAX_CHANNELS;
    type->paramCount = unit->getParamCount();
    if (type->paramCount > MAX_PARAMS) {
        hostLog("%s: %u parameters, only the first %d are shown", unit->name, type->paramCount, MAX_PARAMS);
        type->paramCount = MAX_PARAMS;
    }
    for (uint32_t i = 0; i < type->paramCount; i++) {
        UnitParamInfo * info = &type->params[i];
        if (!unit->getParamInfo(i, info)) {
            memset(info, 0, sizeof(*info));
            info->id = UNIT_INVALID_ID - 1 - i;
            info->flags = UNIT_PARAM_IS_HIDDEN | UNIT_PARAM_IS_READONLY;
        }
        info->name[sizeof(info->name) - 1] = 0;
        info->module[sizeof(info->module) - 1] = 0;
        if ((info->flags & UNIT_PARAM_IS_TRIGGER) && type->triggerCount < MAX_TRIGGERS) {
            type->triggers[type->triggerCount++] = i;
        }
    }

    int f = 0;
    if (type->inputs == 0 && type->outputs > 0) {
        type->features[f++] = CLAP_PLUGIN_FEATURE_INSTRUMENT;
        if (type->triggerCount > 0) {
            type->features[f++] = CLAP_PLUGIN_FEATURE_DRUM_MACHINE;
        }
    } else if (type->outputs > 0) {
        type->features[f++] = CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
    } else {
        type->features[f++] = CLAP_PLUGIN_FEATURE_ANALYZER;
    }
    if (type->outputs == 2) {
        type->features[f++] = CLAP_PLUGIN_FEATURE_STEREO;
    } else if (type->outputs == 1) {
        type->features[f++] = CLAP_PLUGIN_FEATURE_MONO;
    }
    type->features[f] = 0;

    type->descriptor = (clap_plugin_descriptor_t) {
        .clap_version = CLAP_VERSION_INIT,
        .id = type->id,
        .name = unit->name,
        .vendor = unit->vendor ? unit->vendor : "metalhead",
        .url = "",
        .manual_url = "",
        .support_url = "",
        .version = unit->version ? unit->version : "0",
        .description = unit->description ? unit->description : "",
        .features = type->features,
    };
}

static pthread_once_t initOnce = PTHREAD_ONCE_INIT;

static void initialize(void) {
    logHandle = os_log_create("metalhead.unit", "units");
    const char * root = getenv("METALHEAD_SD_ROOT");
    const char * home = getenv("HOME");
    if (root && root[0]) {
        snprintf(sdRoot, sizeof(sdRoot), "%s", root);
    } else {
        snprintf(sdRoot, sizeof(sdRoot), "%s/Music/metalhead", home ? home : "");
    }
    size_t len = strlen(sdRoot);
    if (len > 1 && sdRoot[len - 1] == '/') {
        sdRoot[len - 1] = 0;
    }

    // Units are loaded once and never unloaded, as on the device
    const UnitFactory * factory = unit_entry(&hostApi);
    if (!factory || factory->size < sizeof(UnitFactory) || !factory->getPluginCount || !factory->getPlugin) {
        hostLog("the unit file has no factory");
        return;
    }
    uint32_t count = factory->getPluginCount();
    for (uint32_t i = 0; i < count && typeCount < MAX_PLUGINS; i++) {
        const UnitPlugin * unit = factory->getPlugin(i);
        if (!unit || unit->size < sizeof(UnitPlugin) || !unit->name || !unit->createInstance || !unit->getNumInputs ||
                !unit->getNumOutputs || !unit->getParamCount || !unit->getParamInfo || !unit->getParamValue || !unit->process) {
            hostLog("plugin %u of the unit file is incomplete, left out", i);
            continue;
        }
        describe(&types[typeCount++], unit);
    }
    hostLog("%u plugins, files from %s", typeCount, sdRoot);
}

bool unitclap_init(const char * pluginPath) {
    pthread_once(&initOnce, initialize);
    return typeCount > 0;
}

void unitclap_deinit(void) {}

const void * unitclap_get_factory(const char * factoryId) {
    if (!strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID)) {
        return &pluginFactory;
    }
    return 0;
}

// --- For the AUv2 factory (entry.cpp) ---------------------------------------------------------

// AudioUnits are named by four characters: the plugin name's first four letters and digits,
// made unique in the file with a digit at the end.
static int subtypeTaken(char codes[][5], uint32_t count, const char * code) {
    for (uint32_t u = 0; u < count; u++) {
        if (!memcmp(codes[u], code, 4)) {
            return 1;
        }
    }
    return 0;
}

bool unitclap_au_info(uint32_t index, char type[5], char subtype[5]) {
    static char codes[MAX_PLUGINS][5];
    pthread_once(&initOnce, initialize);
    if (index >= typeCount || types[index].outputs == 0) {
        return false;           // an AudioUnit has to have an output
    }
    for (uint32_t t = 0; t <= index; t++) {
        char code[5] = "____";
        const char * name = types[t].unit->name;
        for (int i = 0, j = 0; name[i] && j < 4; i++) {
            if ((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9')) {
                code[j++] = name[i];
            }
        }
        for (char digit = '0'; subtypeTaken(codes, t, code) && digit <= '9'; digit++) {
            code[3] = digit;
        }
        memcpy(codes[t], code, 5);
    }
    memcpy(subtype, codes[index], 5);
    // Instruments are 'aumu'; effects that take MIDI 'aumf', so Logic sends them notes
    memcpy(type, types[index].inputs == 0 ? "aumu" : "aumf", 5);
    return true;
}
