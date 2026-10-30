// Plaits: macro oscillator, 24 synthesis models (eurorack/plaits, 48 kHz, 12-frame blocks).
//
// Outputs: 1 out, 2 aux. Inputs (CV, 1.0 = 5 V): 1 V/oct, 2 timbre, 3 morph, 4 harmonics,
// 5 FM, 6 level; the attenuverters (fm_amount, ...) scale 2, 3 and 5. Notes on midiIn set the
// pitch and strike the internal low pass gate; without notes the LPG stays open.
#include "mutable.h"

#include "plaits/dsp/voice.h"

using namespace mutable_units;

namespace {

const UnitHostApi * api;

enum ParamId {
  P_ENGINE, P_NOTE, P_HARMONICS, P_TIMBRE, P_MORPH, P_DECAY, P_LPG_COLOUR,
  P_FM_AMOUNT, P_TIMBRE_AMOUNT, P_MORPH_AMOUNT, P_TRIGGER, P_COUNT
};

const UnitParamInfo params[P_COUNT] = {
  { P_ENGINE, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "engine", "", 0, 23, 8 },
  { P_NOTE, UNIT_PARAM_IS_AUTOMATABLE, "note", "", 12, 108, 48 },
  { P_HARMONICS, UNIT_PARAM_IS_AUTOMATABLE, "harmonics", "", 0, 1, 0.5 },
  { P_TIMBRE, UNIT_PARAM_IS_AUTOMATABLE, "timbre", "", 0, 1, 0.5 },
  { P_MORPH, UNIT_PARAM_IS_AUTOMATABLE, "morph", "", 0, 1, 0.5 },
  { P_DECAY, UNIT_PARAM_IS_AUTOMATABLE, "decay", "lpg", 0, 1, 0.5 },
  { P_LPG_COLOUR, UNIT_PARAM_IS_AUTOMATABLE, "lpg_colour", "lpg", 0, 1, 0.5 },
  { P_FM_AMOUNT, UNIT_PARAM_IS_AUTOMATABLE, "fm_amount", "cv", -1, 1, 0 },
  { P_TIMBRE_AMOUNT, UNIT_PARAM_IS_AUTOMATABLE, "timbre_amount", "cv", -1, 1, 0 },
  { P_MORPH_AMOUNT, UNIT_PARAM_IS_AUTOMATABLE, "morph_amount", "cv", -1, 1, 0 },
  { P_TRIGGER, UNIT_PARAM_IS_TRIGGER, "trigger", "", 0, 1, 1 },
};

// In the order of Voice::Init's RegisterInstance calls: the green bank of Plaits 1.2 first
const char * const engineNames[24] = {
  "VA VCF", "Phase distortion", "6-op FM 1", "6-op FM 2", "6-op FM 3", "Wave terrain",
  "String machine", "Chiptune",
  "Virtual analog", "Waveshaping", "FM", "Grain", "Additive", "Wavetable", "Chords", "Speech",
  "Swarm", "Noise", "Particle", "String", "Modal", "Bass drum", "Snare drum", "Hi-hat",
};

enum { IN_VOCT, IN_TIMBRE, IN_MORPH, IN_HARMONICS, IN_FM, IN_LEVEL, IN_COUNT };

struct Instance {
  Instance * next;
  plaits::Voice voice;
  char * sharedBuffer;
  float values[P_COUNT];
  MonoNotes notes;
  bool trigger;
  bool played;            // a note or trigger came in: the LPG is struck from now on
  BlockAdapter<plaits::kBlockSize, IN_COUNT, 2> blocks;
};

const size_t kSharedBufferSize = 16384;
InstancePool<Instance> pool;

void * createInstance() {
  Instance * s = pool.Take();
  if (!s) {
    void * memory = api->allocate(sizeof(Instance));
    char * shared = static_cast<char *>(api->allocate(kSharedBufferSize));
    if (!memory || !shared) {
      return NULL;
    }
    s = new (memory) Instance();
    s->sharedBuffer = shared;
  }
  stmlib::BufferAllocator allocator(s->sharedBuffer, kSharedBufferSize);
  s->voice.Init(&allocator);
  paramDefaults(params, P_COUNT, s->values);
  s->notes.Init(params[P_NOTE].defaultValue);
  s->trigger = false;
  s->played = false;
  s->blocks.Init();
  return s;
}

void releaseInstance(void * instance) {
  pool.Give(static_cast<Instance *>(instance));
}

uint32_t getNumInputs() { return IN_COUNT; }
uint32_t getNumOutputs() { return 2; }
uint32_t getParamCount() { return P_COUNT; }

int getParamInfo(uint32_t index, UnitParamInfo * info) {
  return paramInfo(params, P_COUNT, index, info);
}

int getParamValue(void * instance, uint32_t id, double * value) {
  return paramValue(params, P_COUNT, static_cast<Instance *>(instance)->values, id, value);
}

int paramValueToText(uint32_t id, double value, char * text, uint32_t size) {
  int engine = static_cast<int>(value + 0.5);
  if (id != P_ENGINE || engine < 0 || engine > 23) {
    return 0;
  }
  return copyText(engineNames[engine], text, size);
}

void process(void * instance, UnitProcessHost * host,
             size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount,
             const UnitParamEvent * events, size_t eventCount) {
  Instance * s = static_cast<Instance *>(instance);
  for (size_t i = 0; i < eventCount; i++) {
    if (events[i].id == P_TRIGGER) {
      s->trigger = true;
    } else if (events[i].id < P_COUNT) {
      s->values[events[i].id] = static_cast<float>(events[i].value);
    }
  }
  if (s->notes.Process(api, host)) {
    s->values[P_NOTE] = s->notes.note();
    s->trigger = true;
  }
  const float * v = s->values;
  plaits::Patch patch;
  patch.engine = static_cast<int>(v[P_ENGINE] + 0.5f);
  patch.note = v[P_NOTE];
  patch.harmonics = v[P_HARMONICS];
  patch.timbre = v[P_TIMBRE];
  patch.morph = v[P_MORPH];
  patch.decay = v[P_DECAY];
  patch.lpg_colour = v[P_LPG_COLOUR];
  patch.frequency_modulation_amount = v[P_FM_AMOUNT];
  patch.timbre_modulation_amount = v[P_TIMBRE_AMOUNT];
  patch.morph_modulation_amount = v[P_MORPH_AMOUNT];

  s->blocks.Process(nrIn, in, nrOut, out, frameCount,
                    [s, &patch, nrIn](float (*cv)[plaits::kBlockSize], float (*o)[plaits::kBlockSize]) {
    plaits::Modulations m;
    // The module samples its CV once per block
    m.engine = 0.0f;
    m.note = cv[IN_VOCT][0] * 60.0f;        // 0.2 per volt, 12 semitones per volt
    m.frequency = cv[IN_FM][0] * 60.0f;
    m.harmonics = cv[IN_HARMONICS][0] * 2.0f;
    m.timbre = cv[IN_TIMBRE][0] * 2.0f;
    m.morph = cv[IN_MORPH][0] * 2.0f;
    m.level = cv[IN_LEVEL][0] * 2.0f;
    // A connected input that is silent reads as 0 V: treat inputs 2, 3 and 5 as patched
    // whenever they exist, so the attenuverters always apply (they default to 0)
    m.frequency_patched = nrIn > IN_FM;
    m.timbre_patched = nrIn > IN_TIMBRE;
    m.morph_patched = nrIn > IN_MORPH;
    m.level_patched = false;
    // Notes strike the LPG; until the first one it.s left open (trigger unpatched), a drone
    s->played = s->played || s->trigger;
    m.trigger_patched = s->played;
    m.trigger = s->trigger ? 1.0f : 0.0f;
    s->trigger = false;

    plaits::Voice::Frame frames[plaits::kBlockSize];
    s->voice.Render(patch, m, frames, plaits::kBlockSize);
    for (size_t i = 0; i < plaits::kBlockSize; i++) {
      o[0][i] = fromShort(frames[i].out);
      o[1][i] = fromShort(frames[i].aux);
    }
  });
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Plaits", "Mutable Instruments (port)", "1.2", "Macro oscillator, 24 models",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * plaits_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  return &plugin;
}
