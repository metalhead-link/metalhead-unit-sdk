// Rings: resonator, 6 models and the Disastrous Peace string synth (eurorack/rings, 48 kHz,
// 24-frame blocks).
//
// Outputs: 1 odd, 2 even. Inputs: 1 exciter (audio), then CV (1.0 = 5 V, added to the knob, so
// 5 V sweeps its whole range) 2 structure, 3 brightness, 4 damping, 5 position. Notes on midiIn
// strum, and transpose from the note param the way 1 V/oct CV transposes the frequency knob:
// MIDI note 48 plays at `note` (default 48, so notes play at their own pitch). Without notes,
// the exciter input's onsets strum (as with nothing in the module's strum jack).
//
// The `exciter` param stands in for the module's jack normalisation: "internal" excites the
// resonator with its own pulse/noise burst on every strum (nothing patched in IN), "external"
// uses the audio input, "auto" (default) is external while the input has carried signal in the
// last second. `easter_egg` switches to the string synth; `model` then picks its effect
// (formant, chorus, reverb, formant 2, ensemble, reverb 2) and `structure` its chord.
#include "mutable.h"

#include "rings/dsp/part.h"
#include "rings/dsp/strummer.h"
#include "rings/dsp/string_synth_part.h"

using namespace mutable_units;

namespace {

const UnitHostApi * api;

enum ParamId {
  P_POLYPHONY, P_MODEL, P_NOTE, P_STRUCTURE, P_BRIGHTNESS, P_DAMPING, P_POSITION, P_EXCITER,
  P_EASTER_EGG, P_STRUM, P_COUNT
};

const UnitParamInfo params[P_COUNT] = {
  { P_POLYPHONY, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_AUTOMATABLE, "polyphony", "", 1, 4, 1 },
  { P_MODEL, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "model", "", 0, 5, 0 },
  { P_NOTE, UNIT_PARAM_IS_AUTOMATABLE, "note", "", 12, 96, 48 },
  { P_STRUCTURE, UNIT_PARAM_IS_AUTOMATABLE, "structure", "", 0, 1, 0.5 },
  { P_BRIGHTNESS, UNIT_PARAM_IS_AUTOMATABLE, "brightness", "", 0, 1, 0.5 },
  { P_DAMPING, UNIT_PARAM_IS_AUTOMATABLE, "damping", "", 0, 1, 0.5 },
  { P_POSITION, UNIT_PARAM_IS_AUTOMATABLE, "position", "", 0, 1, 0.5 },
  { P_EXCITER, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "exciter", "", 0, 2, 0 },
  { P_EASTER_EGG, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "easter_egg", "", 0, 1, 0 },
  { P_STRUM, UNIT_PARAM_IS_TRIGGER, "strum", "", 0, 1, 1 },
};

// rings::ResonatorModel order
const char * const modelNames[6] = {
  "Modal", "Sympathetic strings", "String", "FM voice", "Quantised sympathetic", "String and reverb",
};
const char * const exciterNames[3] = { "Auto", "Internal", "External" };
const char * const onOffNames[2] = { "Off", "On" };

enum { IN_AUDIO, IN_STRUCTURE, IN_BRIGHTNESS, IN_DAMPING, IN_POSITION, IN_COUNT };

const size_t kBlockSize = rings::kMaxBlockSize;
const size_t kReverbBufferSize = 32768;          // uint16_t words, as in rings.cc
const float kNoiseGateThreshold = 0.00003f;      // rings.cc
const int32_t kExternalHoldBlocks = 48000 / kBlockSize;   // auto exciter: 1 s

struct Instance {
  Instance * next;
  rings::Part part;
  rings::StringSynthPart stringSynth;
  rings::Strummer strummer;
  uint16_t * reverbBuffer;
  float values[P_COUNT];
  MonoNotes notes;
  bool strum;              // a note or the strum trigger came in, for the next module block
  bool played;             // notes strum from now on; before, the exciter input's onsets do
  float inLevel;           // noise gate of rings.cc
  int32_t silentBlocks;    // since the exciter input last carried signal
  int32_t polyphony;       // as last given to the module
  int32_t model;
  int32_t easterEgg;
  int32_t chord;           // with hysteresis, as cv_scaler.cc
  BlockAdapter<kBlockSize, IN_COUNT, 2> blocks;
};

InstancePool<Instance> pool;

void * createInstance() {
  Instance * s = pool.Take();
  if (!s) {
    void * memory = api->allocate(sizeof(Instance));
    void * reverb = api->allocate(kReverbBufferSize * sizeof(uint16_t));
    if (!memory || !reverb) {
      return NULL;
    }
    s = new (memory) Instance();
    s->reverbBuffer = static_cast<uint16_t *>(reverb);
  }
  memset(s->reverbBuffer, 0, kReverbBufferSize * sizeof(uint16_t));
  s->strummer.Init(0.01f, rings::kSampleRate / kBlockSize);
  s->part.Init(s->reverbBuffer);
  s->stringSynth.Init(s->reverbBuffer);
  paramDefaults(params, P_COUNT, s->values);
  s->polyphony = static_cast<int32_t>(params[P_POLYPHONY].defaultValue);
  s->model = static_cast<int32_t>(params[P_MODEL].defaultValue);
  s->easterEgg = 0;
  s->part.set_polyphony(s->polyphony);
  s->part.set_model(static_cast<rings::ResonatorModel>(s->model));
  s->stringSynth.set_polyphony(s->polyphony);
  s->stringSynth.set_fx(static_cast<rings::FxType>(s->model));
  s->notes.Init(48.0f);
  s->strum = false;
  s->played = false;
  s->inLevel = 0.0f;
  s->silentBlocks = kExternalHoldBlocks;
  s->chord = 0;
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
  int v = static_cast<int>(value + 0.5);
  if (id == P_MODEL && v >= 0 && v <= 5) {
    return copyText(modelNames[v], text, size);
  } else if (id == P_EXCITER && v >= 0 && v <= 2) {
    return copyText(exciterNames[v], text, size);
  } else if (id == P_EASTER_EGG && v >= 0 && v <= 1) {
    return copyText(onOffNames[v], text, size);
  }
  return 0;
}

// Block mean of a CV input, added to a knob and clamped like cv_scaler.cc's ATTENUVERT
float knob(const float * value, const float * cv, float max) {
  float sum = 0.0f;
  for (size_t i = 0; i < kBlockSize; i++) {
    sum += cv[i];
  }
  float x = *value + sum * (1.0f / kBlockSize);
  return x < 0.0f ? 0.0f : (x > max ? max : x);
}

void render(Instance * s, size_t nrIn, float (*in)[kBlockSize], float (*out)[kBlockSize]) {
  const float * v = s->values;

  // Settings the module's UI sets; the module reconfigures on change only
  int32_t polyphony = static_cast<int32_t>(v[P_POLYPHONY] + 0.5f);
  int32_t model = static_cast<int32_t>(v[P_MODEL] + 0.5f);
  int32_t easterEgg = v[P_EASTER_EGG] >= 0.5f;
  if (polyphony != s->polyphony) {
    s->polyphony = polyphony;
    s->part.set_polyphony(polyphony);
    s->stringSynth.set_polyphony(polyphony);
  }
  if (model != s->model) {
    s->model = model;
    s->part.set_model(static_cast<rings::ResonatorModel>(model));
    s->stringSynth.set_fx(static_cast<rings::FxType>(model));
  }
  if (easterEgg != s->easterEgg) {
    // Both share the reverb buffer: don't let one play the other's leftovers
    s->easterEgg = easterEgg;
    memset(s->reverbBuffer, 0, kReverbBufferSize * sizeof(uint16_t));
  }

  rings::Patch patch;
  // Below 1: the module's tables are interpolated at value * size (and its ADCs never read 1.0)
  patch.structure = knob(&v[P_STRUCTURE], in[IN_STRUCTURE], 0.9995f);
  patch.brightness = knob(&v[P_BRIGHTNESS], in[IN_BRIGHTNESS], 0.9995f);
  patch.damping = knob(&v[P_DAMPING], in[IN_DAMPING], 0.9995f);
  patch.position = knob(&v[P_POSITION], in[IN_POSITION], 0.9995f);

  // Noise gate on the exciter input (rings.cc), which also tells whether it carries signal
  float gated[kBlockSize];
  for (size_t i = 0; i < kBlockSize; i++) {
    float x = in[IN_AUDIO][i];
    float error = x * x - s->inLevel;
    s->inLevel += error * (error > 0.0f ? 0.1f : 0.0001f);
    float gain = s->inLevel <= kNoiseGateThreshold ? (1.0f / kNoiseGateThreshold) * s->inLevel : 1.0f;
    gated[i] = gain * x;
  }
  if (nrIn > IN_AUDIO && s->inLevel > kNoiseGateThreshold) {
    s->silentBlocks = 0;
  } else if (s->silentBlocks < kExternalHoldBlocks) {
    s->silentBlocks++;
  }

  rings::PerformanceState ps;
  int exciter = static_cast<int>(v[P_EXCITER] + 0.5f);
  ps.internal_exciter = exciter == 1 || (exciter == 0 && s->silentBlocks >= kExternalHoldBlocks);
  ps.internal_strum = !s->played;     // no strum jack: the strummer decides from the input
  ps.internal_note = true;            // keeps the strummer off note changes; notes strum below
  ps.tonic = v[P_NOTE];
  ps.note = s->notes.note() - 48.0f;
  ps.fm = 0.0f;
  ps.strum = s->strum;
  s->strum = false;

  // Chord (string synth, quantised sympathetic strings) from structure, cv_scaler.cc
  float chord = patch.structure * static_cast<float>(rings::kNumChords - 1);
  float hysteresis = chord - s->chord > 0.0f ? -0.1f : +0.1f;
  s->chord = static_cast<int32_t>(chord + hysteresis + 0.5f);
  s->chord = s->chord < 0 ? 0 : (s->chord > rings::kNumChords - 1 ? rings::kNumChords - 1 : s->chord);
  ps.chord = s->chord;

  if (easterEgg) {
    s->strummer.Process(NULL, kBlockSize, &ps);
    s->stringSynth.Process(ps, patch, in[IN_AUDIO], out[0], out[1], kBlockSize);
  } else {
    s->strummer.Process(gated, kBlockSize, &ps);
    s->part.Process(ps, patch, gated, out[0], out[1], kBlockSize);
  }
}

void process(void * instance, UnitProcessHost * host,
             size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount,
             const UnitParamEvent * events, size_t eventCount) {
  Instance * s = static_cast<Instance *>(instance);
  for (size_t i = 0; i < eventCount; i++) {
    if (events[i].id == P_STRUM) {
      s->strum = true;
      s->played = true;
    } else if (events[i].id < P_COUNT) {
      s->values[events[i].id] = static_cast<float>(events[i].value);
    }
  }
  if (s->notes.Process(api, host)) {
    s->strum = true;
    s->played = true;
  }
  s->blocks.Process(nrIn, in, nrOut, out, frameCount,
                    [s, nrIn](float (*i)[kBlockSize], float (*o)[kBlockSize]) {
    render(s, nrIn, i, o);
  });
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Rings", "Mutable Instruments (port)", "1.1", "Resonator: modal, sympathetic strings, string, FM",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * rings_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  return &plugin;
}
