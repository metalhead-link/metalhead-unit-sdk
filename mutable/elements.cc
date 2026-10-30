// Elements: modal voice, bow/blow/strike exciters into a modal or string resonator, with
// reverb (eurorack/elements, 32 kHz, 16-sample blocks, resampled to and from 48 kHz).
//
// Outputs: 1 main, 2 aux. Inputs (audio): 1 blow, 2 strike, as the module's two external
// exciter inputs. Notes on midiIn hold the gate (each new note retriggers it), set the strength
// (the pressure CV) from their velocity, and transpose from the note param the way 1 V/oct CV
// transposes the coarse knob: MIDI note 48 plays at `note` (default 48, so notes play at their
// own pitch). The play trigger is the module's button: a 0.2 s gate. `space` above 1.75 freezes
// the reverb, as CV can push it there.
//
// Left out: the Ominous Voice easter egg. Its SineFm() converts negative floats to uint32_t,
// undefined behaviour that `make test`'s sanitizer stops at (ARM saturates them to 0, as the
// STM32F4 did, so it would play as on the module).
//
// Sample rate: everything in elements/dsp is computed for kSampleRate = 32 kHz: the pitch
// lookup tables (lut_midi_to_f_*), envelope and decay times, delay lines, reverb lengths. So the
// module runs at 32 kHz between two polyphase windowed-sinc resamplers (2:3 down for the inputs,
// 3:2 up for the outputs; Kaiser window, 15 kHz cutoff). A host block of 24 frames is exactly
// one module block of 16 samples, so BlockAdapter does the buffering. Latency: 24 frames of
// blocks plus the filters' group delay (47.5 + 71.5 samples at 96 kHz): about 1.7 ms.
#include "mutable.h"

#include <math.h>

#include "elements/dsp/part.h"

using namespace mutable_units;

namespace {

const UnitHostApi * api;

enum ParamId {
  P_MODEL, P_NOTE, P_FINE, P_CONTOUR, P_BOW_LEVEL, P_BOW_TIMBRE, P_BLOW_LEVEL, P_FLOW,
  P_BLOW_TIMBRE, P_STRIKE_LEVEL, P_MALLET, P_STRIKE_TIMBRE, P_GEOMETRY, P_BRIGHTNESS, P_DAMPING,
  P_POSITION, P_SPACE, P_PLAY, P_COUNT
};

// Defaults: Part::Init's patch
const UnitParamInfo params[P_COUNT] = {
  { P_MODEL, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "model", "", 0, 2, 0 },
  { P_NOTE, UNIT_PARAM_IS_AUTOMATABLE, "note", "", 12, 96, 48 },
  { P_FINE, UNIT_PARAM_IS_AUTOMATABLE, "fine", "", -2, 2, 0 },
  { P_CONTOUR, UNIT_PARAM_IS_AUTOMATABLE, "contour", "exciter", 0, 1, 1 },
  { P_BOW_LEVEL, UNIT_PARAM_IS_AUTOMATABLE, "bow_level", "exciter", 0, 1, 0 },
  { P_BOW_TIMBRE, UNIT_PARAM_IS_AUTOMATABLE, "bow_timbre", "exciter", 0, 1, 0.5 },
  { P_BLOW_LEVEL, UNIT_PARAM_IS_AUTOMATABLE, "blow_level", "exciter", 0, 1, 0 },
  { P_FLOW, UNIT_PARAM_IS_AUTOMATABLE, "flow", "exciter", 0, 1, 0.5 },
  { P_BLOW_TIMBRE, UNIT_PARAM_IS_AUTOMATABLE, "blow_timbre", "exciter", 0, 1, 0.5 },
  { P_STRIKE_LEVEL, UNIT_PARAM_IS_AUTOMATABLE, "strike_level", "exciter", 0, 1, 0.8 },
  { P_MALLET, UNIT_PARAM_IS_AUTOMATABLE, "mallet", "exciter", 0, 1, 0.5 },
  { P_STRIKE_TIMBRE, UNIT_PARAM_IS_AUTOMATABLE, "strike_timbre", "exciter", 0, 1, 0.5 },
  { P_GEOMETRY, UNIT_PARAM_IS_AUTOMATABLE, "geometry", "resonator", 0, 1, 0.2 },
  { P_BRIGHTNESS, UNIT_PARAM_IS_AUTOMATABLE, "brightness", "resonator", 0, 1, 0.5 },
  { P_DAMPING, UNIT_PARAM_IS_AUTOMATABLE, "damping", "resonator", 0, 1, 0.25 },
  { P_POSITION, UNIT_PARAM_IS_AUTOMATABLE, "position", "resonator", 0, 1, 0.3 },
  { P_SPACE, UNIT_PARAM_IS_AUTOMATABLE, "space", "", 0, 2, 0.5 },
  { P_PLAY, UNIT_PARAM_IS_TRIGGER, "play", "", 0, 1, 1 },
};

// elements::ResonatorModel order
const char * const modelNames[3] = { "Modal", "String", "Strings" };

enum { IN_BLOW, IN_STRIKE, IN_COUNT };

const size_t kModuleBlock = elements::kMaxBlockSize;     // 16 samples at 32 kHz
const size_t kHostBlock = kModuleBlock * 3 / 2;          // 24 frames at 48 kHz
const size_t kReverbBufferSize = 32768;                  // uint16_t words, as in elements.cc
const float kNoiseGateThreshold = 0.0001f;               // elements.cc
const int32_t kPlayBlocks = 32000 / kModuleBlock / 5;    // the play trigger's gate: 0.2 s

// --- Resampling 48 kHz <-> 32 kHz ----------------------------------------------------------
//
// Rational L/M polyphase resampler: conceptually zero-stuffs by L, low-pass filters at L times
// the input rate and keeps every Mth sample. The prototype filter has L * kTaps taps; phase p
// uses taps p, p + L, p + 2L, ...
const int kTaps = 48;
const float kPi = 3.14159265f;

template <int L, int M>
class Resampler {
 public:
  void Init(const float (*h)[kTaps]) {
    h_ = h;
    memset(x_, 0, sizeof(x_));
    position_ = 0;
    phase_ = 0;
  }

  // One input sample in, the outputs it completes out; returns how many (0..ceil(L/M))
  size_t Push(float in, float * out) {
    position_ = (position_ == 0 ? kTaps : position_) - 1;
    x_[position_] = x_[position_ + kTaps] = in;   // twice: x_[position_..+kTaps) is newest first
    const float * x = &x_[position_];
    size_t n = 0;
    while (phase_ < L) {
      const float * h = h_[phase_];
      float sum = 0.0f;
      for (int k = 0; k < kTaps; k++) {
        sum += h[k] * x[k];
      }
      out[n++] = sum;
      phase_ += M;
    }
    phase_ -= L;
    return n;
  }

 private:
  const float (*h_)[kTaps];
  float x_[2 * kTaps];
  int position_;
  int phase_;
};

// The prototype: a Kaiser-windowed sinc (beta 7, about 70 dB) at the upsampled rate, 96 kHz both
// ways, cutoff 15 kHz (below 32 kHz's Nyquist). Computed once, at unit_entry().
float besselI0(float x) {
  float sum = 1.0f, term = 1.0f;
  for (int k = 1; k < 32; k++) {
    term *= (x / (2.0f * k)) * (x / (2.0f * k));
    sum += term;
  }
  return sum;
}

template <int L>
void designFilter(float (*h)[kTaps], float upsampledRate) {
  const int n = L * kTaps;
  const float cutoff = 15000.0f / upsampledRate;   // cycles per sample at the upsampled rate
  const float beta = 7.0f;
  float proto[n];
  float sum = 0.0f;
  for (int i = 0; i < n; i++) {
    float t = i - (n - 1) * 0.5f;
    float x = 2.0f * cutoff * t;
    float sinc = t == 0.0f ? 1.0f : sinf(kPi * x) / (kPi * x);
    float r = 2.0f * i / (n - 1) - 1.0f;
    float window = besselI0(beta * sqrtf(1.0f - r * r)) / besselI0(beta);
    proto[i] = sinc * window;
    sum += proto[i];
  }
  for (int p = 0; p < L; p++) {
    for (int k = 0; k < kTaps; k++) {
      h[p][k] = proto[p + k * L] * L / sum;    // unity gain at DC for every phase
    }
  }
}

float downFilter[2][kTaps];     // 48 -> 32 kHz: L = 2, M = 3, at 96 kHz
float upFilter[3][kTaps];       // 32 -> 48 kHz: L = 3, M = 2, at 96 kHz

// --- Instances --------------------------------------------------------------------------------

struct Instance {
  Instance * next;
  elements::Part part;
  uint16_t * reverbBuffer;
  float values[P_COUNT];
  MonoNotes notes;
  bool pendingTrigger;     // a new note or play: the gate must rise in the next module block
  bool lastGate;
  int32_t playBlocks;      // left of the play trigger's gate
  float strength;
  float inLevel[IN_COUNT];  // noise gates of elements.cc
  Resampler<2, 3> down[IN_COUNT];
  Resampler<3, 2> up[2];
  BlockAdapter<kHostBlock, IN_COUNT, 2> blocks;
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
  s->part.Init(s->reverbBuffer);
  s->part.set_easter_egg(false);   // Init leaves it alone
  paramDefaults(params, P_COUNT, s->values);
  s->notes.Init(48.0f);
  s->pendingTrigger = false;
  s->lastGate = false;
  s->playBlocks = 0;
  s->strength = 0.5f;        // unity accent gain, as the pressure input unpatched
  for (int c = 0; c < IN_COUNT; c++) {
    s->inLevel[c] = 0.0f;
    s->down[c].Init(downFilter);
  }
  s->up[0].Init(upFilter);
  s->up[1].Init(upFilter);
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
  if (id == P_MODEL && v >= 0 && v <= 2) {
    return copyText(modelNames[v], text, size);
  }
  return 0;
}

// The ranges of cv_scaler.cc (pots read below 1.0; the module's tables are interpolated at
// value * size)
float clamp(float x, float max) {
  return x < 0.0f ? 0.0f : (x > max ? max : x);
}

void render(Instance * s, float (*in)[kHostBlock], float (*out)[kHostBlock]) {
  const float * v = s->values;
  elements::Patch * patch = s->part.mutable_patch();
  patch->exciter_envelope_shape = clamp(v[P_CONTOUR], 0.9995f);
  patch->exciter_bow_level = clamp(v[P_BOW_LEVEL], 0.9995f);
  patch->exciter_bow_timbre = clamp(v[P_BOW_TIMBRE], 0.9995f);
  patch->exciter_blow_level = clamp(v[P_BLOW_LEVEL], 0.9995f);
  patch->exciter_blow_meta = clamp(v[P_FLOW], 0.9995f);
  patch->exciter_blow_timbre = clamp(v[P_BLOW_TIMBRE], 0.9995f);
  patch->exciter_strike_level = clamp(v[P_STRIKE_LEVEL], 0.9995f);
  patch->exciter_strike_meta = clamp(v[P_MALLET], 0.9995f);
  patch->exciter_strike_timbre = clamp(v[P_STRIKE_TIMBRE], 0.995f);
  patch->resonator_geometry = clamp(v[P_GEOMETRY], 0.9995f);
  patch->resonator_brightness = clamp(v[P_BRIGHTNESS], 0.9995f);
  patch->resonator_damping = clamp(v[P_DAMPING], 0.9995f);
  patch->resonator_position = clamp(v[P_POSITION], 0.9995f);
  patch->space = clamp(v[P_SPACE], 2.0f);
  s->part.set_resonator_model(static_cast<elements::ResonatorModel>(static_cast<int>(v[P_MODEL] + 0.5f)));

  // 24 frames at 48 kHz are 16 samples at 32 kHz exactly (the resamplers' phases repeat), then
  // the noise gates of elements.cc
  float exciter[IN_COUNT][kModuleBlock];
  for (int c = 0; c < IN_COUNT; c++) {
    size_t n = 0;
    for (size_t i = 0; i < kHostBlock; i++) {
      n += s->down[c].Push(in[c][i], &exciter[c][n]);
    }
    for (size_t i = 0; i < kModuleBlock; i++) {
      float x = exciter[c][i];
      float error = x * x - s->inLevel[c];
      s->inLevel[c] += error * (error > 0.0f ? 0.1f : 0.0001f);
      float gain = s->inLevel[c] <= kNoiseGateThreshold ? (1.0f / kNoiseGateThreshold) * s->inLevel[c] : 1.0f;
      exciter[c][i] = gain * x;
    }
  }

  // The gate: held by notes and the play button; a new note drops it for one block first
  bool gate = s->notes.gate() || s->playBlocks > 0;
  if (s->pendingTrigger) {
    if (s->lastGate) {
      gate = false;
    } else {
      gate = true;
      s->pendingTrigger = false;
    }
  }
  s->lastGate = gate;
  if (s->playBlocks > 0) {
    s->playBlocks--;
  }

  elements::PerformanceState ps;
  ps.gate = gate;
  ps.note = v[P_NOTE] + (s->notes.note() - 48.0f) + v[P_FINE];
  ps.modulation = 0.0f;
  ps.strength = s->strength;

  float main[kModuleBlock], aux[kModuleBlock];
  s->part.Process(ps, exciter[IN_BLOW], exciter[IN_STRIKE], main, aux, kModuleBlock);

  size_t n = 0, m = 0;
  for (size_t i = 0; i < kModuleBlock; i++) {
    n += s->up[0].Push(main[i], &out[0][n]);
    m += s->up[1].Push(aux[i], &out[1][m]);
  }
}

void process(void * instance, UnitProcessHost * host,
             size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount,
             const UnitParamEvent * events, size_t eventCount) {
  Instance * s = static_cast<Instance *>(instance);
  for (size_t i = 0; i < eventCount; i++) {
    if (events[i].id == P_PLAY) {
      s->pendingTrigger = true;
      s->playBlocks = kPlayBlocks;
      s->strength = 0.25f + 0.5f * clamp(static_cast<float>(events[i].value), 1.0f);
    } else if (events[i].id < P_COUNT) {
      s->values[events[i].id] = static_cast<float>(events[i].value);
    }
  }
  if (s->notes.Process(api, host)) {
    s->pendingTrigger = true;
    s->strength = 0.25f + 0.5f * s->notes.velocity();   // velocity 64: unity accent gain
  }
  s->blocks.Process(nrIn, in, nrOut, out, frameCount,
                    [s](float (*i)[kHostBlock], float (*o)[kHostBlock]) {
    render(s, i, o);
  });
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Elements", "Mutable Instruments (port)", "1.1", "Modal voice: bow, blow, strike into a resonator",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * elements_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  designFilter<2>(downFilter, 96000.0f);
  designFilter<3>(upFilter, 96000.0f);
  return &plugin;
}
