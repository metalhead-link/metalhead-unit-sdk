// Tides (2018): tidal modulator, 4 related slopes (eurorack/tides2). The generator takes
// frequencies relative to the sample rate (the module runs at 62.5 kHz, its tables don't
// depend on it), so it runs here at 48 kHz with the frequencies computed for 48 kHz: pitch and
// times are right. It renders the host's blocks directly, in pieces of at most 8 frames (the
// module's control rate block), so there is no added latency. The module computes the
// slope/phase and frequency modes at half its rate; here every mode runs at 48 kHz.
//
// Outputs: 1-4 (1.0 = 5 V; the module's 0..8 V envelopes reach 1.6, ±5 V cycles ±1). Inputs
// (CV, 1.0 = 5 V): 1 V/oct, 2 FM (up to ±96 semitones per 5 V, scaled by fm_amount), 3 shape,
// 4 slope, 5 smoothness, 6 shift (each scaled by its *_amount attenuverter; like the knobs the
// attenuverters have a cubic response), 7 trig/gate, 8 clock (both: high above 1 V). Notes on
// midiIn gate the trig input; in the audio range they also set the pitch (C3 = the frequency
// knob's centre, 130.81 Hz). The clock input counts as patched while it has had a rising edge in
// the last 4 s; the trig input (with notes and the trigger parameter) likewise: as on the
// module, with a clock and no trig the slopes follow the clock (frequency ratio from the
// frequency parameter), with both the clock sets the frequency and trig restarts the slopes.
// Not included: the hardware's switch for the hidden output-mode colours (the LEDs).
#include "mutable.h"

#include "stmlib/dsp/units.h"
#include "tides2/poly_slope_generator.h"
#include "tides2/ramp/ramp_extractor.h"

using namespace mutable_units;

namespace {

const UnitHostApi * api;

const float kSampleRate = 48000.0f;
const size_t kBlock = 8;                                // tides2/io_buffer.h kBlockSize
const uint32_t kPatchedTimeout = 4 * 48000;             // frames without an edge: unpatched
const uint32_t kTriggerFrames = 480;                    // the trigger parameter: a 10 ms gate

enum ParamId {
  P_MODE, P_OUTPUT_MODE, P_RANGE, P_FREQUENCY, P_SHAPE, P_SLOPE, P_SMOOTHNESS, P_SHIFT,
  P_FM_AMOUNT, P_SHAPE_AMOUNT, P_SLOPE_AMOUNT, P_SMOOTHNESS_AMOUNT, P_SHIFT_AMOUNT, P_TRIGGER,
  P_COUNT
};

const uint32_t kEnum = UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE;

const UnitParamInfo params[P_COUNT] = {
  { P_MODE, kEnum, "mode", "", 0, tides::RAMP_MODE_LAST - 1, tides::RAMP_MODE_AD },
  { P_OUTPUT_MODE, kEnum, "output_mode", "", 0, tides::OUTPUT_MODE_LAST - 1, tides::OUTPUT_MODE_GATES },
  { P_RANGE, kEnum, "range", "", 0, 2, 1 },
  { P_FREQUENCY, UNIT_PARAM_IS_AUTOMATABLE, "frequency", "", -48, 48, 0 },
  { P_SHAPE, UNIT_PARAM_IS_AUTOMATABLE, "shape", "", 0, 1, 0.5 },
  { P_SLOPE, UNIT_PARAM_IS_AUTOMATABLE, "slope", "", 0, 1, 0.5 },
  { P_SMOOTHNESS, UNIT_PARAM_IS_AUTOMATABLE, "smoothness", "", 0, 1, 0.5 },
  { P_SHIFT, UNIT_PARAM_IS_AUTOMATABLE, "shift", "", 0, 1, 1 },
  { P_FM_AMOUNT, UNIT_PARAM_IS_AUTOMATABLE, "fm_amount", "cv", -1, 1, 0 },
  { P_SHAPE_AMOUNT, UNIT_PARAM_IS_AUTOMATABLE, "shape_amount", "cv", -1, 1, 0 },
  { P_SLOPE_AMOUNT, UNIT_PARAM_IS_AUTOMATABLE, "slope_amount", "cv", -1, 1, 0 },
  { P_SMOOTHNESS_AMOUNT, UNIT_PARAM_IS_AUTOMATABLE, "smoothness_amount", "cv", -1, 1, 0 },
  { P_SHIFT_AMOUNT, UNIT_PARAM_IS_AUTOMATABLE, "shift_amount", "cv", -1, 1, 0 },
  { P_TRIGGER, UNIT_PARAM_IS_TRIGGER, "trigger", "", 0, 1, 1 },
};

const char * const modeNames[tides::RAMP_MODE_LAST] = { "AD", "Cycle", "AR" };
const char * const outputModeNames[tides::OUTPUT_MODE_LAST] = {
  "Gates", "Amplitude", "Slope/phase", "Frequency",
};
const char * const rangeNames[3] = { "Low", "Medium", "Audio" };

// tides.cc: the frequency knob's centre in each range, and the clock ratios
const float rootFrequency[3] = { 0.125f, 2.0f, 130.81f };

const tides::Ratio clockRatios[20] = {
  { 0.0625f, 16 }, { 0.125f, 8 }, { 0.1666666f, 6 }, { 0.25f, 4 }, { 0.3333333f, 3 },
  { 0.5f, 2 }, { 0.6666666f, 3 }, { 0.75f, 4 }, { 0.8f, 5 }, { 1.0f, 1 }, { 1.0f, 1 },
  { 1.25f, 4 }, { 1.3333333f, 3 }, { 1.5f, 2 }, { 2.0f, 1 }, { 3.0f, 1 }, { 4.0f, 1 },
  { 6.0f, 1 }, { 8.0f, 1 }, { 16.0f, 1 },
};

// cv_reader.cc: the response of the shape knob
const float shapeBreakpoints[10] = {
  0.0f, 0.26f, 0.34f, 0.42f, 0.5f, 0.58f, 0.66f, 0.74f, 1.0f, 1.0f
};

enum {
  IN_VOCT, IN_FM, IN_SHAPE, IN_SLOPE, IN_SMOOTHNESS, IN_SHIFT, IN_TRIG, IN_CLOCK, IN_COUNT
};

// A gate input: high above 1 V (0.2), low below 0.5 V
struct GateInput {
  bool high;
  stmlib::GateFlags flags;
  uint32_t idle;            // frames since the last rising edge

  void Init() {
    high = false;
    flags = stmlib::GATE_FLAG_LOW;
    idle = kPatchedTimeout;
  }

  stmlib::GateFlags Process(bool value) {
    flags = stmlib::ExtractGateFlags(flags, value);
    if (flags & stmlib::GATE_FLAG_RISING) {
      idle = 0;
    } else if (idle < kPatchedTimeout) {
      idle++;
    }
    return flags;
  }

  bool Threshold(float x) {
    high = high ? x > 0.1f : x > 0.2f;
    return high;
  }

  bool patched() const { return idle < kPatchedTimeout; }
};

struct Instance {
  Instance * next;
  tides::PolySlopeGenerator generator;
  tides::RampExtractor rampExtractor;
  stmlib::HysteresisQuantizer2 ratioQuantizer;
  float values[P_COUNT];
  MonoNotes notes;
  GateInput trig;
  GateInput clock;
  uint32_t triggerFrames;   // left of the trigger parameter's gate
  bool resetRampExtractor;
  int outputMode;
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
  s->generator.Init();
  s->rampExtractor.Init(kSampleRate, 40.0f / kSampleRate);
  s->ratioQuantizer.Init(20, 0.05f, false);
  paramDefaults(params, P_COUNT, s->values);
  s->notes.Init(48);
  s->trig.Init();
  s->clock.Init();
  s->triggerFrames = 0;
  s->resetRampExtractor = true;
  s->outputMode = static_cast<int>(params[P_OUTPUT_MODE].defaultValue);
  return s;
}

void releaseInstance(void * instance) {
  pool.Give(static_cast<Instance *>(instance));
}

uint32_t getNumInputs() { return IN_COUNT; }
uint32_t getNumOutputs() { return tides::PolySlopeGenerator::num_channels; }
uint32_t getParamCount() { return P_COUNT; }

int getParamInfo(uint32_t index, UnitParamInfo * info) {
  return paramInfo(params, P_COUNT, index, info);
}

int getParamValue(void * instance, uint32_t id, double * value) {
  return paramValue(params, P_COUNT, static_cast<Instance *>(instance)->values, id, value);
}

int paramValueToText(uint32_t id, double value, char * text, uint32_t size) {
  int i = static_cast<int>(value + 0.5);
  if (i < 0 || id >= P_COUNT || !(params[id].flags & UNIT_PARAM_IS_ENUM) || i > params[id].maxValue) {
    return 0;
  }
  switch (id) {
    case P_MODE: return copyText(modeNames[i], text, size);
    case P_OUTPUT_MODE: return copyText(outputModeNames[i], text, size);
    case P_RANGE: return copyText(rangeNames[i], text, size);
  }
  return 0;
}

inline int stepped(float v, int max) {
  int i = static_cast<int>(v + 0.5f);
  return i < 0 ? 0 : (i > max ? max : i);
}

inline float clamp(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// The module's attenuverters: cubic, ±1 at the ends
inline float attenuverter(float amount) {
  return amount * amount * amount;
}

// RampExtractor::Reset() leaves the pulse being measured at 0 samples: a rising edge in the
// next sample makes a period of 0, and ProcessInternal<true> then loops forever on
// `while (expected_phase >= 1.0f)` (infinity). On the module a clock is patched before its
// first edge; here the first edge is what patches it. One low sample after the reset avoids it.
void resetRampExtractor(Instance * s) {
  s->rampExtractor.Reset();
  stmlib::GateFlags low = stmlib::GATE_FLAG_LOW;
  float ramp;
  tides::Ratio r = { 1.0f, 1 };
  s->rampExtractor.Process(false, false, r, &low, &ramp, 1);
}

// One piece of up to kBlock frames, starting at frame `offset` of the host's block
void render(Instance * s, size_t nrIn, float ** in, size_t nrOut, float ** out, size_t offset, size_t size) {
  const float * v = s->values;
  float cv[IN_COUNT];
  for (size_t c = 0; c < IN_COUNT; c++) {
    cv[c] = c < nrIn ? in[c][offset] : 0.0f;
  }

  stmlib::GateFlags trigFlags[kBlock];
  stmlib::GateFlags clockFlags[kBlock];
  for (size_t i = 0; i < size; i++) {
    bool trigInput = IN_TRIG < nrIn && s->trig.Threshold(in[IN_TRIG][offset + i]);
    bool gate = trigInput || s->notes.gate() || s->triggerFrames > 0;
    if (s->triggerFrames) {
      s->triggerFrames--;
    }
    trigFlags[i] = s->trig.Process(gate);
    clockFlags[i] = s->clock.Process(IN_CLOCK < nrIn && s->clock.Threshold(in[IN_CLOCK][offset + i]));
  }

  const tides::RampMode rampMode = tides::RampMode(stepped(v[P_MODE], tides::RAMP_MODE_LAST - 1));
  const tides::OutputMode outputMode = tides::OutputMode(stepped(v[P_OUTPUT_MODE], tides::OUTPUT_MODE_LAST - 1));
  const int rangeIndex = stepped(v[P_RANGE], 2);
  const tides::Range range = rangeIndex < 2 ? tides::RANGE_CONTROL : tides::RANGE_AUDIO;

  float transposition = v[P_FREQUENCY] + cv[IN_VOCT] * 60.0f +
      cv[IN_FM] * 96.0f * attenuverter(v[P_FM_AMOUNT]);
  if (range == tides::RANGE_AUDIO) {
    transposition += s->notes.note() - 48.0f;
  }
  transposition = clamp(transposition, -128.0f, 127.0f);

  float frequency;
  float ramp[kBlock];
  bool followClock = s->clock.patched();
  if (followClock) {
    if (s->resetRampExtractor) {
      resetRampExtractor(s);
    }
    tides::Ratio r = s->ratioQuantizer.Lookup(clockRatios, 0.5f + transposition * 0.0105f);
    frequency = s->rampExtractor.Process(
        range == tides::RANGE_AUDIO,
        range == tides::RANGE_AUDIO && rampMode == tides::RAMP_MODE_AR,
        r, clockFlags, ramp, size);
    s->resetRampExtractor = false;
  } else {
    frequency = rootFrequency[rangeIndex] / kSampleRate * stmlib::SemitonesToRatio(transposition);
    s->resetRampExtractor = true;
  }

  if (outputMode != s->outputMode) {
    s->generator.Reset();
    s->outputMode = outputMode;
  }

  float shape = stmlib::Interpolate(shapeBreakpoints, clamp(v[P_SHAPE], 0.0f, 1.0f), 8.0f);
  shape = clamp(shape + cv[IN_SHAPE] * attenuverter(v[P_SHAPE_AMOUNT]), 0.0f, 1.0f);
  float slope = clamp(v[P_SLOPE] + cv[IN_SLOPE] * attenuverter(v[P_SLOPE_AMOUNT]), 0.0f, 1.0f);
  float smoothness = clamp(
      v[P_SMOOTHNESS] + cv[IN_SMOOTHNESS] * attenuverter(v[P_SMOOTHNESS_AMOUNT]), 0.0f, 1.0f);
  float shift = clamp(v[P_SHIFT] + cv[IN_SHIFT] * attenuverter(v[P_SHIFT_AMOUNT]), 0.0f, 1.0f);

  static const stmlib::GateFlags noGate[kBlock] = { 0 };
  bool trigPatched = s->trig.patched() || s->notes.gate();
  tides::PolySlopeGenerator::OutputSample samples[kBlock];
  s->generator.Render(
      rampMode, outputMode, range, frequency, slope, shape, smoothness, shift,
      trigPatched ? trigFlags : noGate,
      !trigPatched && followClock ? ramp : NULL,
      samples, size);

  for (size_t c = 0; c < nrOut && c < tides::PolySlopeGenerator::num_channels; c++) {
    for (size_t i = 0; i < size; i++) {
      out[c][offset + i] = samples[i].channel[c] * 0.2f;      // volts to 1.0 = 5 V
    }
  }
}

void process(void * instance, UnitProcessHost * host,
             size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount,
             const UnitParamEvent * events, size_t eventCount) {
  Instance * s = static_cast<Instance *>(instance);
  for (size_t i = 0; i < eventCount; i++) {
    if (events[i].id == P_TRIGGER) {
      s->triggerFrames = kTriggerFrames;
    } else if (events[i].id < P_COUNT) {
      s->values[events[i].id] = static_cast<float>(events[i].value);
    }
  }
  if (s->notes.Process(api, host)) {
    // A new note while one is held (legato) restarts the slopes too: a 1-frame low gate
    if (s->trig.flags & stmlib::GATE_FLAG_HIGH) {
      s->trig.flags = stmlib::GATE_FLAG_LOW;
    }
  }
  for (size_t offset = 0; offset < frameCount; offset += kBlock) {
    size_t size = frameCount - offset < kBlock ? frameCount - offset : kBlock;
    render(s, nrIn, in, nrOut, out, offset, size);
  }
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Tides", "Mutable Instruments (port)", "2.0", "Tidal modulator, 4 slopes",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * tides_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  return &plugin;
}
