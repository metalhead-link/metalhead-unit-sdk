// Marbles: random sampler, gates and voltages (eurorack/marbles, run at 48 kHz, any block size).
//
// Outputs: 1 t1, 2 t2, 3 t3 (gates, 0 or 1), 4 x1, 5 x2, 6 x3, 7 y (CV, 1.0 = 5 V). Inputs:
// 1 t clock, 2 x clock (gates, high above 1 V), then CV (1.0 = 5 V) 3 rate (12 semitones per
// volt), 4 t bias, 5 jitter, 6 deja vu, 7 x spread (also the voltage that x_external samples),
// 8 x bias, 9 steps.
//
// The T section's clock (t_clock): "Host" runs the module's external clock input from the host
// transport, one pulse per quarter note (free running at the host tempo while the transport is
// stopped), and the rate knob picks the ratio, 1/4..4 as on the module (t_range multiplies or
// divides it by 4 further): at the defaults T2 ticks on every beat. "Internal" is the module's
// free running clock, 2 Hz at rate 0, and "Input" clocks from input 1. The X section follows
// the T section (x_clock "T1 T2 T3": X1 follows T1, X2 follows T2, X3 follows T3, or all follow
// one), or input 2; the module detects that itself from what's patched, here it's a parameter.
// The transport starting resets both sections, so deja vu loops line up with the song; so does
// the reset trigger.
//
// Sample rate: the T and X generators take the sample rate in Init() and get 48 kHz, so rates,
// pulse widths and the lag/glide follow it. The ramp extractor (external clocks) keeps two
// constants written for 32 kHz: audio rate clocks are detected above 150 Hz instead of 100 Hz,
// and it gives up on a stopped clock after 2 s instead of 3. The module code processes any
// number of samples per call, so there is no block adapter and no latency.
//
// Not ported: the scale recorder, the explicit reset option (jitter and steps CV as resets),
// gate output delays (they align gates with the module's DACs). midi_out sends the quantised
// X1 as notes on MIDI channel 1 (X1 at 0 V = C4), struck by the clock of X1 and released when
// that gate falls.
#include "mutable.h"

#include "marbles/random/random_generator.h"
#include "marbles/random/random_stream.h"
#include "marbles/random/t_generator.h"
#include "marbles/random/x_y_generator.h"

using namespace mutable_units;

namespace {

const UnitHostApi * api;

enum ParamId {
  P_T_CLOCK, P_RATE, P_T_BIAS, P_JITTER, P_T_MODEL, P_T_RANGE, P_T_DEJA_VU,
  P_PULSE_WIDTH, P_PULSE_WIDTH_STD,
  P_DEJA_VU, P_LENGTH,
  P_X_CLOCK, P_X_SPREAD, P_X_BIAS, P_X_STEPS, P_X_DEJA_VU, P_X_MODE, P_X_RANGE, P_X_SCALE,
  P_X_EXTERNAL,
  P_Y_SPREAD, P_Y_BIAS, P_Y_STEPS, P_Y_DIVIDER, P_Y_RANGE,
  P_MIDI_OUT, P_RESET, P_COUNT
};

const uint32_t kEnum = UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE;
const uint32_t kKnob = UNIT_PARAM_IS_AUTOMATABLE;

const UnitParamInfo params[P_COUNT] = {
  { P_T_CLOCK, kEnum, "t_clock", "t", 0, 2, 0 },
  { P_RATE, kKnob, "rate", "t", -60, 60, 0 },
  { P_T_BIAS, kKnob, "t_bias", "t", 0, 1, 0.5 },
  { P_JITTER, kKnob, "jitter", "t", 0, 1, 0 },
  { P_T_MODEL, kEnum, "t_model", "t", 0, 6, 0 },
  { P_T_RANGE, kEnum, "t_range", "t", 0, 2, 1 },
  { P_T_DEJA_VU, kEnum, "t_deja_vu", "t", 0, 2, 0 },
  { P_PULSE_WIDTH, kKnob, "pulse_width", "t", 0, 1, 0.5 },
  { P_PULSE_WIDTH_STD, kKnob, "pulse_width_std", "t", 0, 1, 0 },
  { P_DEJA_VU, kKnob, "deja_vu", "", 0, 1, 0.5 },
  { P_LENGTH, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_AUTOMATABLE, "length", "", 1, 16, 8 },
  { P_X_CLOCK, kEnum, "x_clock", "x", 0, 4, 0 },
  { P_X_SPREAD, kKnob, "x_spread", "x", 0, 1, 0.5 },
  { P_X_BIAS, kKnob, "x_bias", "x", 0, 1, 0.5 },
  { P_X_STEPS, kKnob, "x_steps", "x", 0, 1, 0.5 },
  { P_X_DEJA_VU, kEnum, "x_deja_vu", "x", 0, 2, 0 },
  { P_X_MODE, kEnum, "x_mode", "x", 0, 2, 0 },
  { P_X_RANGE, kEnum, "x_range", "x", 0, 2, 2 },
  { P_X_SCALE, kEnum, "x_scale", "x", 0, 5, 0 },
  { P_X_EXTERNAL, kEnum, "x_external", "x", 0, 1, 0 },
  { P_Y_SPREAD, kKnob, "y_spread", "y", 0, 1, 0.5 },
  { P_Y_BIAS, kKnob, "y_bias", "y", 0, 1, 0.5 },
  { P_Y_STEPS, kKnob, "y_steps", "y", 0, 1, 0 },
  { P_Y_DIVIDER, kEnum, "y_divider", "y", 0, 11, 6 },
  { P_Y_RANGE, kEnum, "y_range", "y", 0, 2, 2 },
  { P_MIDI_OUT, kEnum, "midi_out", "", 0, 1, 0 },
  { P_RESET, UNIT_PARAM_IS_TRIGGER, "reset", "", 0, 1, 1 },
};

const char * const tClockNames[] = { "Host", "Internal", "Input" };
const char * const tModelNames[] = {
  "Coin toss", "Clusters", "Drums", "Independent", "Divider", "Three states", "Markov",
};
const char * const rangeNames[] = { "0.25x", "1x", "4x" };
const char * const dejaVuNames[] = { "Off", "On", "Locked" };
const char * const xClockNames[] = { "T1 T2 T3", "T1", "T2", "T3", "Input" };
const char * const xModeNames[] = { "Identical", "Bump", "Tilt" };
const char * const voltageRangeNames[] = { "+2 V", "+5 V", "+-5 V" };
const char * const scaleNames[] = { "Major", "Minor", "Pentatonic", "Pelog", "Bhairav", "Shri" };
const char * const offOnNames[] = { "Off", "On" };
const char * const dividerNames[] = {
  "1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "1/4", "1/3", "1/2", "1/1",
};

const char * const * enumNames(uint32_t id) {
  switch (id) {
    case P_T_CLOCK: return tClockNames;
    case P_T_MODEL: return tModelNames;
    case P_T_RANGE: return rangeNames;
    case P_T_DEJA_VU: case P_X_DEJA_VU: return dejaVuNames;
    case P_X_CLOCK: return xClockNames;
    case P_X_MODE: return xModeNames;
    case P_X_RANGE: case P_Y_RANGE: return voltageRangeNames;
    case P_X_SCALE: return scaleNames;
    case P_X_EXTERNAL: case P_MIDI_OUT: return offOnNames;
    case P_Y_DIVIDER: return dividerNames;
    default: return NULL;
  }
}

enum { T_CLOCK_HOST, T_CLOCK_INTERNAL, T_CLOCK_INPUT };
enum { X_CLOCK_INPUT = 4 };

enum {
  IN_T_CLOCK, IN_X_CLOCK, IN_RATE, IN_T_BIAS, IN_JITTER, IN_DEJA_VU, IN_X_SPREAD, IN_X_BIAS,
  IN_X_STEPS, IN_COUNT
};
enum { OUT_T1, OUT_T2, OUT_T3, OUT_X1, OUT_X2, OUT_X3, OUT_Y, OUT_COUNT };

const int kCvCount = IN_COUNT - IN_RATE;

// The module filters each CV once per 5-sample block at 32 kHz (cv_reader.cc); per sample at
// 48 kHz the same filters are 1 - (1 - k) ^ (6400 / 48000)
const float cvFilter[kCvCount] = {
  0.0293f,                               // rate (0.2)
  0.00682f, 0.00682f, 0.00682f,          // t bias, jitter, deja vu (0.05)
  0.01395f, 0.01395f, 0.00682f,          // x spread, x bias (0.1), steps (0.05)
};

// marbles/settings.cc (preset_scales, which has internal linkage there)
const marbles::Scale presetScales[6] = {
  { 1.0f, 12, { { 0.0000f, 255 }, { 0.0833f, 16 }, { 0.1667f, 96 }, { 0.2500f, 24 },
                { 0.3333f, 128 }, { 0.4167f, 64 }, { 0.5000f, 8 }, { 0.5833f, 192 },
                { 0.6667f, 16 }, { 0.7500f, 96 }, { 0.8333f, 24 }, { 0.9167f, 128 } } },
  { 1.0f, 12, { { 0.0000f, 255 }, { 0.0833f, 16 }, { 0.1667f, 96 }, { 0.2500f, 128 },
                { 0.3333f, 8 }, { 0.4167f, 64 }, { 0.5000f, 4 }, { 0.5833f, 192 },
                { 0.6667f, 96 }, { 0.7500f, 16 }, { 0.8333f, 128 }, { 0.9167f, 16 } } },
  { 1.0f, 12, { { 0.0000f, 255 }, { 0.0833f, 4 }, { 0.1667f, 96 }, { 0.2500f, 4 },
                { 0.3333f, 4 }, { 0.4167f, 140 }, { 0.5000f, 4 }, { 0.5833f, 192 },
                { 0.6667f, 4 }, { 0.7500f, 96 }, { 0.8333f, 4 }, { 0.9167f, 4 } } },
  { 1.0f, 7, { { 0.0000f, 255 }, { 0.1275f, 128 }, { 0.2625f, 32 }, { 0.4600f, 8 },
               { 0.5883f, 192 }, { 0.7067f, 64 }, { 0.8817f, 16 } } },
  { 1.0f, 12, { { 0.0000f, 255 }, { 0.0752f, 128 }, { 0.1699f, 4 }, { 0.2630f, 4 },
                { 0.3219f, 128 }, { 0.4150f, 64 }, { 0.4918f, 4 }, { 0.5850f, 192 },
                { 0.6601f, 64 }, { 0.7549f, 4 }, { 0.8479f, 4 }, { 0.9069f, 64 } } },
  { 1.0f, 12, { { 0.0000f, 255 }, { 0.0752f, 4 }, { 0.1699f, 128 }, { 0.2630f, 64 },
                { 0.3219f, 4 }, { 0.4150f, 128 }, { 0.4918f, 4 }, { 0.5850f, 192 },
                { 0.6601f, 4 }, { 0.7549f, 64 }, { 0.8479f, 128 }, { 0.9069f, 4 } } },
};

// marbles/marbles.cc
const marbles::Ratio yDividerRatios[12] = {
  { 1, 64 }, { 1, 48 }, { 1, 32 }, { 1, 24 }, { 1, 16 }, { 1, 12 },
  { 1, 8 }, { 1, 6 }, { 1, 4 }, { 1, 3 }, { 1, 2 }, { 1, 1 },
};

// The module code runs on chunks of at most this many samples (its buffers are on the stack)
const size_t kChunk = 16;

// A gate input: high above 1 V, low again below 0.5 V
struct GateInput {
  bool high;

  stmlib::GateFlags Process(float x) {
    bool was = high;
    high = x > (high ? 0.1f : 0.2f);
    return stmlib::ExtractGateFlags(was ? stmlib::GATE_FLAG_HIGH : stmlib::GATE_FLAG_LOW, high);
  }
};

struct Instance {
  Instance * next;
  marbles::RandomGenerator generator;
  marbles::RandomStream stream;
  marbles::TGenerator t;
  marbles::XYGenerator xy;
  float values[P_COUNT];
  float cv[kCvCount];
  GateInput tClock;
  GateInput xClock;
  double beat;            // the host clock, in quarter notes
  bool beatHigh;
  bool wasPlaying;
  bool started;
  bool reset;
  // midi_out
  bool noteGate;
  int noteDelay;          // samples until the note is read from X1 (it changes just after)
  int playingNote;        // -1 if none
};

InstancePool<Instance> pool;
uint32_t instanceCount;

void * createInstance() {
  Instance * s = pool.Take();
  if (!s) {
    void * memory = api->allocate(sizeof(Instance));
    if (!memory) {
      return NULL;
    }
    s = new (memory) Instance();
  }
  // Without the module's hardware RNG the stream reads the generator only: seed each instance
  // differently (and again from the clock in the first process())
  s->generator.Init(0x9E3779B9u * ++instanceCount);
  s->stream.Init(&s->generator);
  s->t.Init(&s->stream, 48000.0f);
  s->xy.Init(&s->stream, 48000.0f);
  for (int i = 0; i < 6; i++) {
    s->xy.LoadScale(i, presetScales[i]);
  }
  paramDefaults(params, P_COUNT, s->values);
  memset(s->cv, 0, sizeof(s->cv));
  s->tClock.high = false;
  s->xClock.high = false;
  s->beat = 0.0;
  s->beatHigh = false;
  s->wasPlaying = false;
  s->started = false;
  s->reset = false;
  s->noteGate = false;
  s->noteDelay = 0;
  s->playingNote = -1;
  return s;
}

void releaseInstance(void * instance) {
  pool.Give(static_cast<Instance *>(instance));
}

uint32_t getNumInputs() { return IN_COUNT; }
uint32_t getNumOutputs() { return OUT_COUNT; }
uint32_t getParamCount() { return P_COUNT; }

int getParamInfo(uint32_t index, UnitParamInfo * info) {
  return paramInfo(params, P_COUNT, index, info);
}

int getParamValue(void * instance, uint32_t id, double * value) {
  return paramValue(params, P_COUNT, static_cast<Instance *>(instance)->values, id, value);
}

int paramValueToText(uint32_t id, double value, char * text, uint32_t size) {
  const char * const * names = enumNames(id);
  int index = static_cast<int>(value + 0.5);
  if (!names || index < params[id].minValue || index > params[id].maxValue) {
    return 0;
  }
  return copyText(names[index], text, size);
}

inline int toInt(float x) {
  return static_cast<int>(x + 0.5f);
}

inline float clamp01(float x) {
  return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

void sendNote(UnitProcessHost * host, uint8_t cin, int note, uint8_t velocity) {
  if (host->sendMIDIEvent) {
    UnitMidiPacket p = { cin, static_cast<uint8_t>(cin << 4), static_cast<uint8_t>(note), velocity };
    host->sendMIDIEvent(host->hostData, p);
  }
}

// One pulse per quarter note from the host transport, high for the first half of the beat
void hostClock(Instance * s, const UnitTimeInfo & time, size_t offset, size_t n,
               stmlib::GateFlags * flags) {
  float tempo = time.tempo > 1.0f && time.tempo < 1000.0f ? time.tempo : 120.0f;
  double perSample = tempo / (60.0 * 48000.0);
  for (size_t i = 0; i < n; i++) {
    double beat = time.isPlaying ? time.ppqPos + (offset + i) * perSample : s->beat + perSample;
    double whole = __builtin_floor(beat);
    bool newBeat = whole != __builtin_floor(s->beat);
    bool high = beat - whole < 0.5;
    if (newBeat || (high && !s->beatHigh)) {
      flags[i] = static_cast<stmlib::GateFlags>(stmlib::GATE_FLAG_RISING | stmlib::GATE_FLAG_HIGH);
      high = true;
    } else if (high) {
      flags[i] = stmlib::GATE_FLAG_HIGH;
    } else {
      flags[i] = s->beatHigh ? stmlib::GATE_FLAG_FALLING : stmlib::GATE_FLAG_LOW;
    }
    s->beatHigh = high;
    s->beat = beat;
  }
}

void processChunk(Instance * s, UnitProcessHost * host, size_t nrIn, float ** in, size_t nrOut,
                  float ** out, size_t offset, size_t n) {
  const float * v = s->values;

  // CV inputs, filtered as the module does
  for (int c = 0; c < kCvCount; c++) {
    size_t channel = IN_RATE + c;
    if (channel < nrIn) {
      float k = cvFilter[c];
      float lp = s->cv[c];
      const float * x = in[channel] + offset;
      for (size_t i = 0; i < n; i++) {
        lp += k * (x[i] - lp);
      }
      s->cv[c] = lp;
    }
  }
  const float * cv = s->cv;
  bool xExternal = toInt(v[P_X_EXTERNAL]) != 0;

  float rate = v[P_RATE] + cv[IN_RATE - IN_RATE] * 60.0f;
  rate = rate < -120.0f ? -120.0f : (rate > 120.0f ? 120.0f : rate);

  // Deja vu: the module's dead band around 12 o'clock
  float dejaVu = clamp01(v[P_DEJA_VU] + cv[IN_DEJA_VU - IN_RATE]);
  if (dejaVu < 0.47f) {
    dejaVu *= 1.06382978723f;
  } else if (dejaVu > 0.53f) {
    dejaVu = 0.5f + (dejaVu - 0.53f) * 1.06382978723f;
  } else {
    dejaVu = 0.5f;
  }
  int length = toInt(v[P_LENGTH]);
  length = length < 1 ? 1 : (length > 16 ? 16 : length);

  // Clocks
  stmlib::GateFlags tFlags[kChunk];
  stmlib::GateFlags xFlags[kChunk];
  int tClock = toInt(v[P_T_CLOCK]);
  int xClock = toInt(v[P_X_CLOCK]);
  if (tClock == T_CLOCK_HOST) {
    hostClock(s, host->timeInfo, offset, n, tFlags);
  } else {
    for (size_t i = 0; i < n; i++) {
      tFlags[i] = s->tClock.Process(IN_T_CLOCK < nrIn ? in[IN_T_CLOCK][offset + i] : 0.0f);
    }
  }
  for (size_t i = 0; i < n; i++) {
    xFlags[i] = s->xClock.Process(IN_X_CLOCK < nrIn ? in[IN_X_CLOCK][offset + i] : 0.0f);
  }

  // T section
  float rampBuffer[kChunk * 4];
  bool gates[kChunk * 2];
  float voltages[kChunk * 4];
  marbles::Ramps ramps;
  ramps.master = &rampBuffer[0];
  ramps.external = &rampBuffer[kChunk];
  ramps.slave[0] = &rampBuffer[kChunk * 2];
  ramps.slave[1] = &rampBuffer[kChunk * 3];

  int tDejaVu = toInt(v[P_T_DEJA_VU]);
  bool tReset = s->reset;
  s->t.set_model(static_cast<marbles::TGeneratorModel>(toInt(v[P_T_MODEL])));
  s->t.set_range(static_cast<marbles::TGeneratorRange>(toInt(v[P_T_RANGE])));
  s->t.set_rate(rate);
  s->t.set_bias(clamp01(v[P_T_BIAS] + cv[IN_T_BIAS - IN_RATE]));
  s->t.set_jitter(clamp01(v[P_JITTER] + cv[IN_JITTER - IN_RATE]));
  s->t.set_deja_vu(tDejaVu == 2 ? 0.5f : (tDejaVu == 1 ? dejaVu : 0.0f));
  s->t.set_length(length);
  // The module's settings are bytes: 0.5 is 128 / 256
  s->t.set_pulse_width_mean(v[P_PULSE_WIDTH] * (255.0f / 256.0f));
  s->t.set_pulse_width_std(v[P_PULSE_WIDTH_STD] * (255.0f / 256.0f));
  s->t.Process(tClock != T_CLOCK_INTERNAL, &tReset, tFlags, ramps, gates, n);
  // A slave ramp holds at exactly 1.0 when its pulse is over, and the X section's lag processor
  // then reads one entry past lut_raised_cosine (with weight 0; harmless on the module, but
  // an out of bounds read all the same)
  for (size_t i = 0; i < n * 3; i++) {
    float & r = i < n ? ramps.master[i] : ramps.slave[i / n - 1][i % n];
    r = r < 0.99999994f ? r : 0.99999994f;
  }

  // X and Y sections
  marbles::GroupSettings x, y;
  int xDejaVu = toInt(v[P_X_DEJA_VU]);
  x.control_mode = static_cast<marbles::ControlMode>(toInt(v[P_X_MODE]));
  x.voltage_range = static_cast<marbles::VoltageRange>(toInt(v[P_X_RANGE]));
  x.register_mode = xExternal;
  // External processing samples the spread CV, which then doesn't move the spread knob
  float spreadCv = IN_X_SPREAD < nrIn ? in[IN_X_SPREAD][offset] : 0.0f;
  x.register_value = clamp01(0.5f * (spreadCv + 1.0f));
  x.spread = clamp01(v[P_X_SPREAD] + (xExternal ? 0.0f : cv[IN_X_SPREAD - IN_RATE]));
  x.bias = clamp01(v[P_X_BIAS] + cv[IN_X_BIAS - IN_RATE]);
  x.steps = clamp01(v[P_X_STEPS] + cv[IN_X_STEPS - IN_RATE]);
  x.deja_vu = xDejaVu == 2 ? 0.5f : (xDejaVu == 1 ? dejaVu : 0.0f);
  x.length = length;
  x.ratio.p = 1;
  x.ratio.q = 1;
  x.scale_index = toInt(v[P_X_SCALE]);

  y.control_mode = marbles::CONTROL_MODE_IDENTICAL;
  y.voltage_range = static_cast<marbles::VoltageRange>(toInt(v[P_Y_RANGE]));
  y.register_mode = false;
  y.register_value = 0.0f;
  y.spread = v[P_Y_SPREAD];
  y.bias = v[P_Y_BIAS];
  y.steps = v[P_Y_STEPS];
  y.deja_vu = 0.0f;
  y.length = 1;
  int divider = toInt(v[P_Y_DIVIDER]);
  y.ratio = yDividerRatios[divider < 0 ? 0 : (divider > 11 ? 11 : divider)];
  y.scale_index = x.scale_index;

  marbles::ClockSource xSource = xClock == X_CLOCK_INPUT
      ? marbles::CLOCK_SOURCE_EXTERNAL
      : static_cast<marbles::ClockSource>(marbles::CLOCK_SOURCE_INTERNAL_T1_T2_T3 + xClock);
  bool xReset = s->reset;
  s->xy.Process(xSource, x, y, &xReset, xFlags, ramps, voltages, n);
  s->reset = false;

  // Outputs
  bool midiOut = toInt(v[P_MIDI_OUT]) != 0;
  for (size_t i = 0; i < n; i++) {
    float o[OUT_COUNT];
    o[OUT_T1] = gates[2 * i] ? 1.0f : 0.0f;
    o[OUT_T2] = ramps.master[i] < 0.5f ? 1.0f : 0.0f;
    o[OUT_T3] = gates[2 * i + 1] ? 1.0f : 0.0f;
    for (int c = 0; c < 4; c++) {
      float volts = voltages[4 * i + c];
      volts = volts < -5.0f ? -5.0f : (volts > 5.0f ? 5.0f : volts);
      o[OUT_X1 + c] = volts * 0.2f;
    }
    for (size_t c = 0; c < OUT_COUNT && c < nrOut; c++) {
      out[c][offset + i] = o[c];
    }

    // midi_out: X1 as notes, on the gate that clocks X1
    bool gate;
    switch (xClock) {
      case 2: gate = o[OUT_T2] != 0.0f; break;
      case 3: gate = o[OUT_T3] != 0.0f; break;
      case X_CLOCK_INPUT: gate = (xFlags[i] & stmlib::GATE_FLAG_HIGH) != 0; break;
      default: gate = o[OUT_T1] != 0.0f; break;
    }
    if (!midiOut) {
      gate = false;
    }
    if (gate && !s->noteGate) {
      s->noteDelay = 2;
    } else if (!gate && s->noteGate) {
      s->noteDelay = 0;
      if (s->playingNote >= 0) {
        sendNote(host, UNIT_MIDI_CIN_NOTE_OFF, s->playingNote, 0);
        s->playingNote = -1;
      }
    }
    s->noteGate = gate;
    if (s->noteDelay && --s->noteDelay == 0) {
      float note = 60.0f + o[OUT_X1] * 60.0f;     // 12 semitones per volt
      int number = static_cast<int>(note + (note < 0.0f ? -0.5f : 0.5f));
      number = number < 0 ? 0 : (number > 127 ? 127 : number);
      if (s->playingNote >= 0) {
        sendNote(host, UNIT_MIDI_CIN_NOTE_OFF, s->playingNote, 0);
      }
      sendNote(host, UNIT_MIDI_CIN_NOTE_ON, number, 100);
      s->playingNote = number;
    }
  }
}

void process(void * instance, UnitProcessHost * host,
             size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount,
             const UnitParamEvent * events, size_t eventCount) {
  Instance * s = static_cast<Instance *>(instance);
  for (size_t i = 0; i < eventCount; i++) {
    if (events[i].id == P_RESET) {
      s->reset = true;
    } else if (events[i].id < P_COUNT) {
      s->values[events[i].id] = static_cast<float>(events[i].value);
    }
  }
  const UnitTimeInfo & time = host->timeInfo;
  if (!s->started) {
    s->started = true;
    uint32_t t = static_cast<uint32_t>(time.microSeconds);
    s->generator.Init(0x9E3779B9u * instanceCount ^ t * 2654435761u);
    s->beat = time.isPlaying ? time.ppqPos - 1e-9 : 0.0;
  }
  if (time.isPlaying && !s->wasPlaying) {
    s->reset = true;
  }
  s->wasPlaying = time.isPlaying != 0;

  for (size_t offset = 0; offset < frameCount; offset += kChunk) {
    size_t n = frameCount - offset < kChunk ? frameCount - offset : kChunk;
    processChunk(s, host, nrIn, in, nrOut, out, offset, n);
  }
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Marbles", "Mutable Instruments (port)", NULL, "Random sampler: gates and voltages",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * marbles_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  return &plugin;
}
