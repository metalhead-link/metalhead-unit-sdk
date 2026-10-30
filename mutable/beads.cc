// Beads: texture synthesizer, granular processor and delay (eurorack/beads, 4 kHz block rate:
// 12 frames at 48 kHz, 8 at 32 kHz, 6 at 24 kHz).
//
// Outputs: 1 left, 2 right. Inputs: 1 left, 2 right audio; CV (1.0 = 5 V, added to the knobs,
// 5 V is the full range): 3 time, 4 size, 5 shape, 6 1 V/oct pitch, 7 density; gates (high
// above 1 V): 8 seed, 9 freeze. The seed input counts as patched once it has seen a gate (the
// hardware detects the jack): with seed_lock on, its pulses are then divided or picked at
// random by density, as on the module.
//
// Notes on midiIn press the SEED button: held while the note is, a grain at the note on and
// more at the density's rate while it's held. Like the button, the first press unlocks the
// seed lock (seed_lock reads 0 from then on); set seed_lock to 1 again for the free-running
// grains. trigger is a short press. Notes don't change the pitch.
//
// The *_random parameters are the attenuverters as the hardware uses them with nothing patched
// into the CV input: right of centre a slow random walk, left of centre a random value per
// grain. The CV inputs are added to the knobs instead (not scaled by the attenuverters). size
// above 0.99 is the delay mode, as on the module; the size CV doesn't take it there.
//
// The quality (media) setting picks the rate the hardware's codec runs at: 48 kHz for Digital
// and Clean tape (which halves the rate itself), 32 kHz for Vintage digital, 24 kHz for Dirty
// tape (ENABLE_HARDWARE_24K in processor.h). The wrapper resamples 48 kHz to and from the
// lower rates (resampler.h). Latency: one block (0.25 ms) at 48 kHz; 2.4 ms at 32 kHz and
// 4.2 ms at 24 kHz with the filters' group delay. Changing the quality or mono clears the
// recording buffers (1.5 MB of memory writes, in process()). Not ported: the synth mode the
// module falls into with nothing patched, the input AGC, the gate output mode.
#include "mutable.h"
#include "resampler.h"

// The module's variable length arrays (a GNU extension; clang warns)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wvla-cxx-extension"
#endif
#include "beads/dsp/processor.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

using namespace mutable_units;

namespace {

const UnitHostApi * api;

enum ParamId {
  P_TIME, P_SIZE, P_SHAPE, P_PITCH, P_TIME_RANDOM, P_SIZE_RANDOM, P_SHAPE_RANDOM, P_PITCH_RANDOM,
  P_DENSITY, P_FEEDBACK, P_DRY_WET, P_REVERB, P_FREEZE, P_SEED_LOCK, P_QUALITY, P_MONO,
  P_TRIGGER, P_COUNT
};

const UnitParamInfo params[P_COUNT] = {
  { P_TIME, UNIT_PARAM_IS_AUTOMATABLE, "time", "", 0, 1, 0.5 },
  { P_SIZE, UNIT_PARAM_IS_AUTOMATABLE, "size", "", 0, 1, 0.5 },
  { P_SHAPE, UNIT_PARAM_IS_AUTOMATABLE, "shape", "", 0, 1, 0.5 },
  { P_PITCH, UNIT_PARAM_IS_AUTOMATABLE, "pitch", "", -24, 24, 0 },
  { P_TIME_RANDOM, UNIT_PARAM_IS_AUTOMATABLE, "time_random", "random", -1, 1, 0 },
  { P_SIZE_RANDOM, UNIT_PARAM_IS_AUTOMATABLE, "size_random", "random", -1, 1, 0 },
  { P_SHAPE_RANDOM, UNIT_PARAM_IS_AUTOMATABLE, "shape_random", "random", -1, 1, 0 },
  { P_PITCH_RANDOM, UNIT_PARAM_IS_AUTOMATABLE, "pitch_random", "random", -1, 1, 0 },
  { P_DENSITY, UNIT_PARAM_IS_AUTOMATABLE, "density", "", 0, 1, 0.25 },
  { P_FEEDBACK, UNIT_PARAM_IS_AUTOMATABLE, "feedback", "mix", 0, 1, 0 },
  { P_DRY_WET, UNIT_PARAM_IS_AUTOMATABLE, "dry_wet", "mix", 0, 1, 0.5 },
  { P_REVERB, UNIT_PARAM_IS_AUTOMATABLE, "reverb", "mix", 0, 1, 0 },
  { P_FREEZE, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_AUTOMATABLE, "freeze", "", 0, 1, 0 },
  { P_SEED_LOCK, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_AUTOMATABLE, "seed_lock", "", 0, 1, 1 },
  { P_QUALITY, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "quality", "", 0, 3, 3 },
  { P_MONO, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_AUTOMATABLE, "mono", "", 0, 1, 0 },
  { P_TRIGGER, UNIT_PARAM_IS_TRIGGER, "trigger", "", 0, 1, 1 },
};

// beads::Processor::Media
const char * const qualityNames[4] = {
  "Dirty tape", "Clean tape", "Vintage digital", "Digital",
};

enum {
  IN_LEFT, IN_RIGHT, IN_TIME, IN_SIZE, IN_SHAPE, IN_PITCH, IN_DENSITY, IN_SEED, IN_FREEZE,
  IN_COUNT
};

// The buffers beads.cc gives Processor::Init: 512 KB in two banks, 256 KB, and the reverb's
const size_t kBankSize = 256 * 1024;
const size_t kNumBanks = 3;
const size_t kReverbBufferSize = 32768;          // uint16_t

const int kBlockRate = 4000;
const size_t kMaxBlock = 48000 / kBlockRate;
const float kGateThreshold = 0.2f;               // 1 V

RateFilters filters;

struct Instance {
  Instance * next;
  beads::Processor processor;
  uint8_t * banks[kNumBanks];
  uint16_t * reverbBuffer;
  float values[P_COUNT];
  MonoNotes notes;
  int media;                    // what processor and bridge are configured for
  bool mono;
  int seedPress;                // blocks left of a trigger's SEED press
  bool seedPatched;             // the seed input has seen a gate
  stmlib::GateFlags seedFlags;  // previous seed input state
  RateBridge<kMaxBlock, IN_COUNT> bridge;
};

InstancePool<Instance> pool;

void Configure(Instance * s, int media, bool mono) {
  s->media = media;
  s->mono = mono;
  s->processor.Configure(static_cast<beads::Processor::Media>(media), mono);
  int sampleRate = static_cast<int>(s->processor.sample_rate());
  int l = sampleRate == 32000 ? 2 : 1;
  int m = sampleRate == 32000 ? 3 : (sampleRate == 24000 ? 2 : 1);
  s->bridge.Init(&filters, l, m, sampleRate / kBlockRate);
}

void * createInstance() {
  Instance * s = pool.Take();
  uint8_t * banks[kNumBanks];
  uint16_t * reverb;
  if (s) {
    memcpy(banks, s->banks, sizeof(banks));
    reverb = s->reverbBuffer;
  } else {
    s = static_cast<Instance *>(api->allocate(sizeof(Instance)));
    for (size_t i = 0; i < kNumBanks; i++) {
      banks[i] = static_cast<uint8_t *>(api->allocate(kBankSize));
    }
    reverb = static_cast<uint16_t *>(api->allocate(kReverbBufferSize * sizeof(uint16_t)));
    if (!s || !banks[0] || !banks[1] || !banks[2] || !reverb) {
      return NULL;
    }
  }
  // Value-initialised, as the hardware's globals are zeroed: Init leaves some state alone
  s = new (s) Instance();
  memcpy(s->banks, banks, sizeof(banks));
  s->reverbBuffer = reverb;
  s->processor.Init(s->banks, s->reverbBuffer);
  paramDefaults(params, P_COUNT, s->values);
  s->notes.Init(60);
  s->seedPress = 0;
  s->seedPatched = false;
  s->seedFlags = stmlib::GATE_FLAG_LOW;
  Configure(s, static_cast<int>(params[P_QUALITY].defaultValue), false);
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
  int choice = static_cast<int>(value + 0.5);
  if (id != P_QUALITY || choice < 0 || choice > 3) {
    return 0;
  }
  return copyText(qualityNames[choice], text, size);
}

inline float Clamp(float x, float min, float max) {
  return x < min ? min : (x > max ? max : x);
}

inline beads::RandomizedParameter Randomized(float base, float random) {
  beads::RandomizedParameter p;
  p.base = base;
  p.attenuverter = random;
  p.cv = 0.0f;
  p.has_cv = false;
  return p;
}

void process(void * instance, UnitProcessHost * host,
             size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount,
             const UnitParamEvent * events, size_t eventCount) {
  Instance * s = static_cast<Instance *>(instance);
  for (size_t i = 0; i < eventCount; i++) {
    if (events[i].id == P_TRIGGER) {
      s->seedPress = 1;
      s->values[P_SEED_LOCK] = 0.0f;
    } else if (events[i].id < P_COUNT) {
      s->values[events[i].id] = static_cast<float>(events[i].value);
    }
  }
  if (s->notes.Process(api, host)) {
    s->values[P_SEED_LOCK] = 0.0f;
  }
  int media = static_cast<int>(s->values[P_QUALITY] + 0.5f) & 3;
  bool mono = s->values[P_MONO] >= 0.5f;
  if (media != s->media || mono != s->mono) {
    Configure(s, media, mono);
  }

  s->bridge.Process(nrIn, in, nrOut, out, frameCount,
                    [s](float (*audio)[kMaxBlock], float (*o)[kMaxBlock], float (*cv)[kMaxBlock]) {
    const float * v = s->values;
    const size_t size = s->bridge.blockSize();
    const size_t last = size - 1;

    beads::FloatFrame input[kMaxBlock];
    beads::FloatFrame output[kMaxBlock];
    bool freeze[kMaxBlock];
    stmlib::GateFlags seed[kMaxBlock];
    bool freezeParam = v[P_FREEZE] >= 0.5f;
    for (size_t i = 0; i < size; i++) {
      input[i].l = audio[0][i];
      input[i].r = audio[1][i];
      freeze[i] = freezeParam || cv[IN_FREEZE - 2][i] > kGateThreshold;
      bool high = cv[IN_SEED - 2][i] > kGateThreshold;
      s->seedPatched = s->seedPatched || high;
      s->seedFlags = seed[i] = stmlib::ExtractGateFlags(s->seedFlags, high);
    }

    beads::SeedButtonState button = beads::SEED_BUTTON_STATE_OFF;
    if (s->notes.gate() || s->seedPress) {
      button = beads::SEED_BUTTON_STATE_ON;
    } else if (v[P_SEED_LOCK] >= 0.5f) {
      button = beads::SEED_BUTTON_STATE_LOCKED;
    }
    if (s->seedPress) {
      s->seedPress--;
    }

    // The CV is read once per block, as the hardware's ADC is
    beads::Parameters p;
    p.time = Randomized(Clamp(v[P_TIME] + cv[IN_TIME - 2][last], 0.0f, 1.0f), v[P_TIME_RANDOM]);
    float size_knob = Clamp(v[P_SIZE], 0.0f, 1.0f);
    float size_param = size_knob + cv[IN_SIZE - 2][last];
    p.size = Randomized(size_knob > 0.99f ? size_knob : Clamp(size_param, 0.0f, 0.98f), v[P_SIZE_RANDOM]);
    p.shape = Randomized(Clamp(v[P_SHAPE] + cv[IN_SHAPE - 2][last], 0.0f, 1.0f), v[P_SHAPE_RANDOM]);
    p.pitch = Randomized(v[P_PITCH] + cv[IN_PITCH - 2][last] * 60.0f, v[P_PITCH_RANDOM]);
    p.monophonic_mode = false;

    beads::ModulatedParameter density;
    density.base = Clamp(v[P_DENSITY], 0.0f, 1.0f);
    density.cv = cv[IN_DENSITY - 2][last];

    beads::Processor::MixParameters m;
    m.feedback = Clamp(v[P_FEEDBACK], 0.0f, 1.0f);
    m.dry_wet = Clamp(v[P_DRY_WET], 0.0f, 1.0f);
    m.reverb_amount = Clamp(v[P_REVERB], 0.0f, 1.0f);
    m.output_gate = false;

    s->processor.Process(button, s->seedPatched ? seed : NULL, freeze, density, p, m,
                         input, output, size);
    for (size_t i = 0; i < size; i++) {
      o[0][i] = output[i].l;
      o[1][i] = output[i].r;
    }
  });
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Beads", "Mutable Instruments (port)", "1.0", "Texture synthesizer, granular processor and delay",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * beads_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  filters.Design();
  return &plugin;
}
