// Stages: segment generator, 6 segments (eurorack/stages). The module code assumes 31.25 kHz
// (kSampleRate in segment_generator.h, used at compile time, and the rate and portamento tables),
// so it runs at exactly that rate here: gates are taken from the host's 48 kHz frames (a gate
// that is high in any frame of a module sample's interval is high in that sample, so short
// triggers survive), and the outputs are linearly interpolated back to 48 kHz (these are
// envelopes and LFOs; the audio-rate oscillator of the ALT type images a little above 15 kHz).
// The module renders blocks of 8 samples; latency 9 module samples (0.29 ms, 14 frames).
// resampler.h's RateBridge (stereo, 2/3 and 1/2 ratios) doesn't fit 31.25 kHz and 6 outputs.
//
// Each channel k has the module's controls: type (ramp, step, hold, alt), loop, slider (the
// primary parameter: time or level), pot (secondary: shape, level, time or portamento) and, in
// place of patching a cable into its gate input, gate: none, input (audio input k, high above
// 1 V) or MIDI (held notes on midiIn, and the trigger parameter). As on the module, a channel
// with a gate starts a group that takes in the following channels without one; channels before
// the first gated one run on their own (an LFO, a slew limiter, ...). The default patch: 1 a
// free-running LFO, 2-5 an ADSR on MIDI notes (ramp, ramp, looped hold, ramp), 6 a sample and
// hold of its CV on gate input 6.
//
// Outputs: 1-6 (1.0 = 5 V; the module's 8 V envelope peak is 1.6). Inputs: 1-6 gates, 7-12 CV
// added to the sliders (1.0 = 5 V; the slider spans 8 V). Left out: chaining modules, the
// Ouroboros mode, and the 2 s hold of an "unpatch" (a gate source change applies at once).
#include "mutable.h"

#include "stages/segment_generator.h"

using namespace mutable_units;

namespace {

const UnitHostApi * api;

const int kChannels = 6;
const size_t kBlock = 8;                        // stages/io_buffer.h kBlockSize
const uint32_t kTriggerFrames = 480;            // the trigger parameter: a 10 ms gate
// 48 kHz frames to 31.25 kHz samples: 125 module samples per 192 frames
const uint32_t kRateUp = 125;
const uint32_t kRateDown = 192;
const uint32_t kLatency = kBlock + 1;           // module samples, see Instance::Output
const uint32_t kRing = 32;

enum Field { F_TYPE, F_LOOP, F_SLIDER, F_POT, F_GATE, F_COUNT };
const int P_TRIGGER = kChannels * F_COUNT;
const int P_COUNT = P_TRIGGER + 1;

enum GateSource { GATE_NONE, GATE_INPUT, GATE_MIDI, GATE_LAST };

const uint32_t kEnum = UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE;

#define STAGES_CHANNEL(k, type, loop, slider, pot, gate) \
  { (k - 1) * F_COUNT + F_TYPE, kEnum, "type" #k, "segment" #k, 0, 3, type }, \
  { (k - 1) * F_COUNT + F_LOOP, kEnum, "loop" #k, "segment" #k, 0, 1, loop }, \
  { (k - 1) * F_COUNT + F_SLIDER, UNIT_PARAM_IS_AUTOMATABLE, "slider" #k, "segment" #k, 0, 1, slider }, \
  { (k - 1) * F_COUNT + F_POT, UNIT_PARAM_IS_AUTOMATABLE, "pot" #k, "segment" #k, 0, 1, pot }, \
  { (k - 1) * F_COUNT + F_GATE, kEnum, "gate" #k, "segment" #k, 0, GATE_LAST - 1, gate }

const UnitParamInfo params[P_COUNT] = {
  STAGES_CHANNEL(1, stages::segment::TYPE_RAMP, 1, 0.5, 0.5, GATE_NONE),    // LFO
  STAGES_CHANNEL(2, stages::segment::TYPE_RAMP, 0, 0.15, 1.0, GATE_MIDI),   // attack, to 1
  STAGES_CHANNEL(3, stages::segment::TYPE_RAMP, 0, 0.35, 0.5, GATE_NONE),   // decay
  STAGES_CHANNEL(4, stages::segment::TYPE_HOLD, 1, 0.6, 0.5, GATE_NONE),    // sustain level
  STAGES_CHANNEL(5, stages::segment::TYPE_RAMP, 0, 0.4, 0.5, GATE_NONE),    // release
  STAGES_CHANNEL(6, stages::segment::TYPE_STEP, 0, 0.5, 0.0, GATE_INPUT),   // sample and hold
  { static_cast<uint32_t>(P_TRIGGER), UNIT_PARAM_IS_TRIGGER, "trigger", "", 0, 1, 1 },
};

const char * const typeNames[4] = { "Ramp", "Step", "Hold", "Alt" };
const char * const offOnNames[2] = { "off", "on" };
const char * const gateNames[GATE_LAST] = { "none", "input", "MIDI" };

enum { IN_GATE = 0, IN_CV = kChannels, IN_COUNT = 2 * kChannels };

struct Instance {
  Instance * next;
  stages::SegmentGenerator generators[kChannels];
  // As stages.cc: generator i quantizes its steps with quantizers[i + segment]
  stmlib::HysteresisQuantizer2 quantizers[kChannels + stages::kMaxNumSegments];
  float values[P_COUNT];
  MonoNotes notes;
  uint32_t triggerFrames;

  // ChainState: the configuration the generators have, -1 for none yet
  int configured[kChannels];
  int numSegments[kChannels];

  // Rate conversion
  uint32_t phase;                       // 0..191: where the module is between two samples
  uint32_t count;                       // module samples so far
  bool gateHigh[kChannels];             // input gate state, with hysteresis
  bool gateSeen[kChannels];             // high in some frame since the last module sample
  stmlib::GateFlags gateFlags[kChannels];
  stmlib::GateFlags gates[kChannels][kBlock];
  float ring[kChannels][kRing];         // module output samples, by count % kRing
};

InstancePool<Instance> pool;

// SegmentGenerator::Init resets its RampExtractor (tides2) to a pulse of 0 samples: if the first
// sample the extractor ever sees (in the tap LFO or PLL modes) is a rising edge, the period is
// 0, the tap LFO's frequency infinite and the PLL oscillator loops forever (`while
// (expected_phase >= 1.0f)` on infinity). On the module the gate is patched before its first
// edge. One low sample through the tap LFO (ramp, loop, gated) leaves a 1-sample pulse.
void primeRampExtractor(stages::SegmentGenerator * g) {
  stages::segment::Configuration c = { stages::segment::TYPE_RAMP, true };
  g->ConfigureSingleSegment(true, c);
  stmlib::GateFlags low = stmlib::GATE_FLAG_LOW;
  stages::SegmentGenerator::Output out = { 0.0f, 0.0f, 0 };
  g->Process(&low, &out, 1);
}

void * createInstance() {
  Instance * s = pool.Take();
  if (!s) {
    void * memory = api->allocate(sizeof(Instance));
    if (!memory) {
      return NULL;
    }
    s = new (memory) Instance();
  }
  for (size_t i = 0; i < kChannels + stages::kMaxNumSegments; i++) {
    s->quantizers[i].Init(13, 0.03f, false);
  }
  for (int i = 0; i < kChannels; i++) {
    s->generators[i].Init(&s->quantizers[i]);
    primeRampExtractor(&s->generators[i]);
    s->configured[i] = -1;
    s->numSegments[i] = 0;
    s->gateHigh[i] = false;
    s->gateSeen[i] = false;
    s->gateFlags[i] = stmlib::GATE_FLAG_LOW;
  }
  paramDefaults(params, P_COUNT, s->values);
  s->notes.Init(60);
  s->triggerFrames = 0;
  s->phase = 0;
  s->count = 0;
  memset(s->ring, 0, sizeof(s->ring));
  return s;
}

void releaseInstance(void * instance) {
  pool.Give(static_cast<Instance *>(instance));
}

uint32_t getNumInputs() { return IN_COUNT; }
uint32_t getNumOutputs() { return kChannels; }
uint32_t getParamCount() { return P_COUNT; }

int getParamInfo(uint32_t index, UnitParamInfo * info) {
  return paramInfo(params, P_COUNT, index, info);
}

int getParamValue(void * instance, uint32_t id, double * value) {
  return paramValue(params, P_COUNT, static_cast<Instance *>(instance)->values, id, value);
}

int paramValueToText(uint32_t id, double value, char * text, uint32_t size) {
  int i = static_cast<int>(value + 0.5);
  if (i < 0 || id >= static_cast<uint32_t>(P_COUNT) || !(params[id].flags & UNIT_PARAM_IS_ENUM) ||
      i > params[id].maxValue) {
    return 0;
  }
  switch (id % F_COUNT) {
    case F_TYPE: return copyText(typeNames[i], text, size);
    case F_LOOP: return copyText(offOnNames[i], text, size);
    case F_GATE: return copyText(gateNames[i], text, size);
  }
  return 0;
}

inline int stepped(float v, int max) {
  int i = static_cast<int>(v + 0.5f);
  return i < 0 ? 0 : (i > max ? max : i);
}

inline float field(const Instance * s, int channel, Field f) {
  return s->values[channel * F_COUNT + f];
}

inline stages::segment::Configuration configuration(const Instance * s, int channel) {
  stages::segment::Configuration c;
  c.type = stages::segment::Type(stepped(field(s, channel, F_TYPE), 3));
  c.loop = stepped(field(s, channel, F_LOOP), 1) != 0;
  return c;
}

inline bool patched(const Instance * s, int channel) {
  return stepped(field(s, channel, F_GATE), GATE_LAST - 1) != GATE_NONE;
}

// ChainState::Configure and BindLocalParameters for one module without neighbours
void configure(Instance * s, const float * cv) {
  int lastPatched = -1;
  for (int i = 0; i < kChannels; i++) {
    stages::SegmentGenerator & g = s->generators[i];
    int segments = 1;
    if (!patched(s, i)) {
      if (lastPatched >= 0) {
        g.ConfigureSlave(i - lastPatched);
        s->configured[i] = -1;
        continue;
      }
      g.ConfigureSingleSegment(false, configuration(s, i));
      s->configured[i] = -1;
    } else {
      lastPatched = i;
      stages::segment::Configuration c[kChannels];
      int signature = 0;
      for (segments = 0; i + segments < kChannels && (segments == 0 || !patched(s, i + segments)); segments++) {
        c[segments] = configuration(s, i + segments);
        signature = signature * 8 + c[segments].type * 2 + (c[segments].loop ? 1 : 0);
      }
      signature = signature * 8 + segments;
      if (signature != s->configured[i]) {
        // The alt mode is only available for single segments
        for (int j = 0; segments != 1 && j < segments; j++) {
          if (c[j].type == stages::segment::TYPE_ALT) {
            c[j].type = stages::segment::TYPE_RAMP;
          }
        }
        g.Configure(true, c, segments);
        s->configured[i] = signature;
      }
    }
    for (int j = 0; j < segments; j++) {
      // cv_reader.cc: slider + CV (8 V full scale), limited to -1..2
      float slider = field(s, i + j, F_SLIDER) + cv[i + j] * 0.625f;
      slider = slider < -1.0f ? -1.0f : (slider > 1.999995f ? 1.999995f : slider);
      // The pot's ADC reads at most 65535 / 65536: the portamento table has no entry for 1.0
      float pot = field(s, i + j, F_POT);
      pot = pot < 0.0f ? 0.0f : (pot > 65535.0f / 65536.0f ? 65535.0f / 65536.0f : pot);
      g.set_segment_parameters(j, slider, pot);
    }
  }
}

// One block at 31.25 kHz: stages.cc's Process
void renderBlock(Instance * s, const float * cv) {
  configure(s, cv);
  static const stmlib::GateFlags noGate[kBlock] = { 0 };
  stages::SegmentGenerator::Output out[kBlock];
  memset(out, 0, sizeof(out));
  uint32_t first = s->count - kBlock;
  for (int c = 0; c < kChannels; c++) {
    // Slaves read the previous generator's phase and segment from out
    s->generators[c].Process(patched(s, c) ? s->gates[c] : noGate, out, kBlock);
    for (size_t i = 0; i < kBlock; i++) {
      s->ring[c][(first + i) % kRing] = out[i].value * 1.6f;     // 8 V to 1.0 = 5 V
    }
  }
}

void process(void * instance, UnitProcessHost * host,
             size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount,
             const UnitParamEvent * events, size_t eventCount) {
  Instance * s = static_cast<Instance *>(instance);
  for (size_t i = 0; i < eventCount; i++) {
    if (events[i].id == static_cast<uint32_t>(P_TRIGGER)) {
      s->triggerFrames = kTriggerFrames;
    } else if (events[i].id < static_cast<uint32_t>(P_COUNT)) {
      s->values[events[i].id] = static_cast<float>(events[i].value);
    }
  }
  bool retrigger = s->notes.Process(api, host);

  for (size_t f = 0; f < frameCount; f++) {
    bool midiGate = s->notes.gate() || s->triggerFrames > 0;
    if (s->triggerFrames) {
      s->triggerFrames--;
    }
    for (int c = 0; c < kChannels; c++) {
      int source = stepped(field(s, c, F_GATE), GATE_LAST - 1);
      float x = IN_GATE + c < static_cast<int>(nrIn) ? in[IN_GATE + c][f] : 0.0f;
      s->gateHigh[c] = s->gateHigh[c] ? x > 0.1f : x > 0.2f;   // 1 V, with hysteresis
      bool high = source == GATE_INPUT ? s->gateHigh[c] : (source == GATE_MIDI && midiGate);
      s->gateSeen[c] = s->gateSeen[c] || high;
    }

    s->phase += kRateUp;
    if (s->phase >= kRateDown) {
      // A module sample
      s->phase -= kRateDown;
      size_t i = s->count % kBlock;
      for (int c = 0; c < kChannels; c++) {
        bool high = s->gateSeen[c];
        // A new note while one is held (legato) restarts the MIDI gated groups: one low sample
        if (retrigger && stepped(field(s, c, F_GATE), GATE_LAST - 1) == GATE_MIDI) {
          high = false;
        }
        s->gateFlags[c] = stmlib::ExtractGateFlags(s->gateFlags[c], high);
        s->gates[c][i] = s->gateFlags[c];
        s->gateSeen[c] = false;
      }
      retrigger = false;
      s->count++;
      if (s->count % kBlock == 0) {
        float cv[kChannels];
        for (int c = 0; c < kChannels; c++) {
          cv[c] = IN_CV + c < static_cast<int>(nrIn) ? in[IN_CV + c][f] : 0.0f;
        }
        renderBlock(s, cv);
      }
    }

    // Module time count - kLatency + phase / 192: sample count - kLatency + 1 is rendered at
    // the latest kBlock samples earlier (count rounded down to a block), so kLatency = kBlock + 1
    uint32_t a = s->count - kLatency;
    float t = s->phase * (1.0f / kRateDown);
    for (size_t c = 0; c < nrOut && c < static_cast<size_t>(kChannels); c++) {
      float y0 = s->ring[c][a % kRing];
      float y1 = s->ring[c][(a + 1) % kRing];
      out[c][f] = y0 + (y1 - y0) * t;
    }
  }
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Stages", "Mutable Instruments (port)", "1.1", "Segment generator, 6 segments",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * stages_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  return &plugin;
}
