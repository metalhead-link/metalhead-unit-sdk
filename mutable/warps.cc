// Warps: meta-modulator (eurorack/warps, run at 96 kHz behind a 2x resampler, 60-frame blocks).
//
// Inputs: 1 carrier, 2 modulator (audio), then CV (1.0 = 5 V): 3 algorithm, 4 timbre, 5 level 1,
// 6 level 2, 7 V/oct for the internal oscillator (0.2 per volt). Outputs: 1 out, 2 aux (the
// module's aux: the two inputs after their level stages, or the internal oscillator).
//
// The levels are the module's level knobs: a drive into each channel's saturating amplifier,
// level squared as on the module with nothing patched. Their CV inputs add to that drive (on the
// module a patched level CV is scaled by the knob instead, but a silent input here would then
// mute the channel). The algorithm knob keeps the module's response curve, with flat spots on
// each algorithm. oscillator replaces the carrier input with the internal oscillator (sine,
// triangle or saw for the cross-modulation algorithms; the vocoder gets saw, pulse or noise
// as on the module); input 1 then frequency modulates it. Its pitch is note, and notes on
// midiIn set it. mode "Frequency shifter" is the module's easter egg: algorithm sets the shift
// (or with oscillator off, the phase of the input as carrier), timbre the up/down mix, level 1
// the feedback, level 2 dry/wet.
//
// Sample rate: the module runs at 96 kHz, and some of it is written for that rate only (the
// vocoder's filter bank coefficients, the oscillator's pitch table), so this wrapper resamples
// 48 -> 96 -> 48 kHz with a 65-tap half-band FIR (Blackman window, flat to about 20 kHz)
// and runs the module exactly as on the hardware, 60 frames at a time. Latency: 30 frames of
// blocks plus 32 of the two resampling filters, about 1.3 ms. Raspberry Pi only (heavy).
#include "mutable.h"

#include "stmlib/dsp/dsp.h"
#include "warps/dsp/modulator.h"
#include "warps/resources.h"

using namespace mutable_units;

namespace {

const UnitHostApi * api;

enum ParamId {
  P_ALGORITHM, P_TIMBRE, P_LEVEL_1, P_LEVEL_2, P_OSCILLATOR, P_NOTE, P_MODE, P_COUNT
};

const UnitParamInfo params[P_COUNT] = {
  { P_ALGORITHM, UNIT_PARAM_IS_AUTOMATABLE, "algorithm", "", 0, 1, 0.25 },
  { P_TIMBRE, UNIT_PARAM_IS_AUTOMATABLE, "timbre", "", 0, 1, 0.5 },
  { P_LEVEL_1, UNIT_PARAM_IS_AUTOMATABLE, "level_1", "", 0, 1, 0.7 },
  { P_LEVEL_2, UNIT_PARAM_IS_AUTOMATABLE, "level_2", "", 0, 1, 0.7 },
  { P_OSCILLATOR, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "oscillator", "", 0, 3, 0 },
  { P_NOTE, UNIT_PARAM_IS_AUTOMATABLE, "note", "", 12, 108, 48 },
  { P_MODE, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "mode", "", 0, 1, 0 },
};

const char * const oscillatorNames[4] = { "Off", "Sine", "Triangle", "Saw" };
const char * const modeNames[2] = { "Meta-modulator", "Frequency shifter" };

enum { IN_CARRIER, IN_MODULATOR, IN_ALGORITHM, IN_TIMBRE, IN_LEVEL_1, IN_LEVEL_2, IN_VOCT, IN_COUNT };

// The module's codec block: 60 frames at 96 kHz
const size_t kModuleBlock = 60;
const size_t kBlock = kModuleBlock / 2;

// --- 2x resampling ---------------------------------------------------------------------------

// A half-band FIR: 0.5 in the middle, zero at the other even offsets, and kTaps coefficients on
// each side at the odd offsets 1, 3, 5, ...; halfband[j] is twice the tap at offset 2j + 1.
const int kTaps = 16;
float halfband[kTaps];

void initHalfband() {
  float sum = 0.0f;
  for (int j = 0; j < kTaps; j++) {
    float d = static_cast<float>(2 * j + 1);
    float x = 3.14159265f * d / (2.0f * kTaps);
    float window = 0.42f + 0.5f * cosf(x) + 0.08f * cosf(2.0f * x);
    halfband[j] = (j & 1 ? -2.0f : 2.0f) / (3.14159265f * d) * window;
    sum += halfband[j];
  }
  for (int j = 0; j < kTaps; j++) {
    halfband[j] *= 0.5f / sum;     // unity gain at DC
  }
}

// 48 -> 96 kHz, kTaps samples of latency (at 48 kHz)
struct Upsampler {
  float history[4 * kTaps];   // the last 2 * kTaps inputs, twice, so a window is contiguous
  int position;

  void Init() {
    memset(history, 0, sizeof(history));
    position = 0;
  }

  void Process(const float * in, float * out, size_t size) {
    for (size_t i = 0; i < size; i++) {
      history[position] = history[position + 2 * kTaps] = in[i];
      position = position + 1 == 2 * kTaps ? 0 : position + 1;
      const float * x = &history[position];      // x[2 * kTaps - 1] is the newest
      float odd = 0.0f;
      for (int j = 0; j < kTaps; j++) {
        odd += halfband[j] * (x[kTaps - 1 - j] + x[kTaps + j]);
      }
      *out++ = x[kTaps - 1];
      *out++ = odd;
    }
  }
};

// 96 -> 48 kHz, 2 * kTaps - 1 samples of latency (at 96 kHz)
struct Downsampler {
  float history[8 * kTaps];   // the last 4 * kTaps inputs, twice
  int position;

  void Init() {
    memset(history, 0, sizeof(history));
    position = 0;
  }

  void Process(const float * in, float * out, size_t size) {
    for (size_t i = 0; i < size; i += 2) {
      for (int k = 0; k < 2; k++) {
        history[position] = history[position + 4 * kTaps] = in[i + k];
        position = position + 1 == 4 * kTaps ? 0 : position + 1;
      }
      const float * x = &history[position + 1];  // x[4 * kTaps - 2] is the newest
      float y = 0.5f * x[2 * kTaps - 1];
      for (int j = 0; j < kTaps; j++) {
        y += 0.5f * halfband[j] * (x[2 * kTaps - 2 - 2 * j] + x[2 * kTaps + 2 * j]);
      }
      *out++ = y;
    }
  }
};

// --- The plugin ------------------------------------------------------------------------------

struct Instance {
  Instance * next;
  warps::Modulator modulator;
  float values[P_COUNT];
  float pot[4];           // algorithm, timbre, level 1, level 2, filtered
  float cv[4];            // their CV inputs, filtered
  float note;
  MonoNotes notes;
  Upsampler up[2];
  Downsampler down[2];
  BlockAdapter<kBlock, IN_COUNT, 2> blocks;
};

InstancePool<Instance> pool;

void * createInstance() {
  Instance * s = pool.Take();
  if (!s) {
    void * memory = api->allocate(sizeof(Instance));
    if (!memory) {
      return NULL;
    }
    s = new (memory) Instance();
  }
  s->modulator.Init(96000.0f);
  paramDefaults(params, P_COUNT, s->values);
  for (int i = 0; i < 4; i++) {
    s->pot[i] = s->values[P_ALGORITHM + i];
    s->cv[i] = 0.0f;
  }
  s->note = s->values[P_NOTE];
  s->notes.Init(s->values[P_NOTE]);
  for (int i = 0; i < 2; i++) {
    s->up[i].Init();
    s->down[i].Init();
  }
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
  int index = static_cast<int>(value + 0.5);
  if (id == P_OSCILLATOR && index >= 0 && index < 4) {
    return copyText(oscillatorNames[index], text, size);
  } else if (id == P_MODE && index >= 0 && index < 2) {
    return copyText(modeNames[index], text, size);
  }
  return 0;
}

inline float clamp(float x, float low, float high) {
  return x < low ? low : (x > high ? high : x);
}

void render(Instance * s, float (*in)[kBlock], float (*out)[kBlock]) {
  const float * v = s->values;

  // Knobs and CV, filtered once per block as cv_scaler.cc does
  for (int i = 0; i < 4; i++) {
    s->pot[i] += 0.33f * 0.08f * (v[P_ALGORITHM + i] - s->pot[i]);
    s->cv[i] += 0.08f * (in[IN_ALGORITHM + i][0] - s->cv[i]);
  }
  warps::Parameters * p = s->modulator.mutable_parameters();
  // lut_pot_curve has 513 entries: stay below 1 so Interpolate doesn't read past it
  float algorithmPot = clamp(s->pot[0], 0.0f, 0.99999f);
  float algorithm = clamp(stmlib::Interpolate(warps::lut_pot_curve, algorithmPot, 512.0f) + s->cv[0],
                          0.0f, 1.0f);
  if (algorithm <= 0.125f) {
    algorithm = clamp(algorithm * 1.08f - 0.01f, 0.0f, 1.0f);   // as the module: no fold bleed
  }
  p->modulation_algorithm = algorithm;
  p->modulation_parameter = clamp(s->pot[1] + s->cv[1], 0.0f, 1.0f);
  for (int i = 0; i < 2; i++) {
    p->channel_drive[i] = clamp(s->pot[2 + i] * s->pot[2 + i] + s->cv[2 + i], 0.0f, 1.0f);
  }
  p->frequency_shift_pot = s->pot[0];
  p->frequency_shift_cv = clamp(s->cv[0], -1.0f, 1.0f);
  p->phase_shift = clamp(s->pot[0] + s->cv[0], 0.0f, 1.0f);

  // Pitch: jumps (notes) right away, small changes smoothly
  float note = v[P_NOTE] + in[IN_VOCT][0] * 60.0f;
  float interval = note - s->note;
  s->note = interval < -0.4f || interval > 0.4f ? note : s->note + 0.1f * interval;
  p->note = clamp(s->note, 0.0f, 127.0f);
  int oscillator = static_cast<int>(v[P_OSCILLATOR] + 0.5f);
  p->carrier_shape = oscillator < 0 ? 0 : (oscillator > 3 ? 3 : oscillator);
  s->modulator.set_easter_egg(v[P_MODE] >= 0.5f);

  float up[2][kModuleBlock];
  for (int c = 0; c < 2; c++) {
    s->up[c].Process(in[IN_CARRIER + c], up[c], kBlock);
  }
  warps::ShortFrame frames[kModuleBlock];
  warps::ShortFrame output[kModuleBlock];
  for (size_t i = 0; i < kModuleBlock; i++) {
    frames[i].l = toShort(up[0][i]);
    frames[i].r = toShort(up[1][i]);
  }
  s->modulator.Process(frames, output, kModuleBlock);
  for (size_t i = 0; i < kModuleBlock; i++) {
    up[0][i] = fromShort(output[i].l);
    up[1][i] = fromShort(output[i].r);
  }
  for (int c = 0; c < 2; c++) {
    s->down[c].Process(up[c], out[c], kModuleBlock);
  }
}

void process(void * instance, UnitProcessHost * host,
             size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount,
             const UnitParamEvent * events, size_t eventCount) {
  Instance * s = static_cast<Instance *>(instance);
  for (size_t i = 0; i < eventCount; i++) {
    if (events[i].id < P_COUNT) {
      s->values[events[i].id] = static_cast<float>(events[i].value);
    }
  }
  if (s->notes.Process(api, host)) {
    s->values[P_NOTE] = s->notes.note();
  }
  s->blocks.Process(nrIn, in, nrOut, out, frameCount,
                    [s](float (*i)[kBlock], float (*o)[kBlock]) { render(s, i, o); });
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Warps", "Mutable Instruments (port)", NULL, "Meta-modulator",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * warps_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  initHalfband();
  return &plugin;
}
