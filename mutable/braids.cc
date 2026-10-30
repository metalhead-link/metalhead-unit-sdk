// Braids: macro oscillator, 47 shapes (eurorack/braids). The module renders at 96 kHz (its
// pitch, filter and envelope tables assume it) in blocks of 24 samples, like the hardware; each
// block is decimated 2:1 with a 47-tap half-band FIR, so the unit plays 12-frame blocks at
// 48 kHz with correct pitch and envelope times. Latency: one 12-frame block plus 11.5 frames of
// filter delay.
//
// Outputs: 1 out. Inputs (CV, 1.0 = 5 V): 1 V/oct (quantized with `scale`), 2 timbre, 3 color,
// 4 FM (1 V/oct, not quantized), 5 trigger (rising above 1 V: strike + envelope, and hard sync
// for the analog shapes when no envelope setting is in use, as on the module). Notes on midiIn
// set the pitch and trigger the AD envelope; with `env_vca` on, the envelope is the VCA once a
// note or trigger came in (before that the oscillator drones). The hardware's settings menu
// becomes parameters (AD envelope and its destinations, quantizer, bit and rate reduction);
// left out: the signature waveshaper, VCO drift/flattening, trigger delay, auto trigger, META
// (shape from the FM input) and the easter egg shape.
#include "mutable.h"

#include "braids/envelope.h"
#include "braids/macro_oscillator.h"
#include "braids/quantizer.h"
#include "braids/quantizer_scales.h"

using namespace mutable_units;

namespace {

const UnitHostApi * api;

const size_t kModuleBlock = 24;                 // at 96 kHz, as braids.cc renders
const size_t kBlock = kModuleBlock / 2;         // at 48 kHz
const int kNumShapes = braids::MACRO_OSC_SHAPE_DIGITAL_MODULATION + 1;
const int kNumScales = sizeof(braids::scales) / sizeof(braids::scales[0]);

enum ParamId {
  P_SHAPE, P_NOTE, P_TIMBRE, P_COLOR, P_ATTACK, P_DECAY, P_ENV_TIMBRE, P_ENV_COLOR, P_ENV_FM,
  P_ENV_VCA, P_SCALE, P_ROOT, P_BITS, P_RATE, P_TRIGGER, P_COUNT
};

const uint32_t kEnum = UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE;

const UnitParamInfo params[P_COUNT] = {
  { P_SHAPE, kEnum, "shape", "", 0, kNumShapes - 1, 0 },
  { P_NOTE, UNIT_PARAM_IS_AUTOMATABLE, "note", "", 12, 108, 48 },
  { P_TIMBRE, UNIT_PARAM_IS_AUTOMATABLE, "timbre", "", 0, 1, 0.5 },
  { P_COLOR, UNIT_PARAM_IS_AUTOMATABLE, "color", "", 0, 1, 0.5 },
  { P_ATTACK, UNIT_PARAM_IS_AUTOMATABLE, "attack", "env", 0, 1, 0 },
  { P_DECAY, UNIT_PARAM_IS_AUTOMATABLE, "decay", "env", 0, 1, 0.5 },
  { P_ENV_TIMBRE, UNIT_PARAM_IS_AUTOMATABLE, "env_timbre", "env", 0, 1, 0 },
  { P_ENV_COLOR, UNIT_PARAM_IS_AUTOMATABLE, "env_color", "env", 0, 1, 0 },
  { P_ENV_FM, UNIT_PARAM_IS_AUTOMATABLE, "env_fm", "env", 0, 1, 0 },
  { P_ENV_VCA, kEnum, "env_vca", "env", 0, 1, 1 },
  { P_SCALE, kEnum, "scale", "quantizer", 0, kNumScales - 1, 0 },
  { P_ROOT, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_AUTOMATABLE, "root", "quantizer", 0, 11, 0 },
  { P_BITS, kEnum, "bits", "lofi", 0, braids::RESOLUTION_LAST - 1, braids::RESOLUTION_16_BIT },
  { P_RATE, kEnum, "rate", "lofi", 0, braids::SAMPLE_RATE_LAST - 1, braids::SAMPLE_RATE_96K },
  { P_TRIGGER, UNIT_PARAM_IS_TRIGGER, "trigger", "", 0, 1, 1 },
};

// In the order of MacroOscillatorShape
const char * const shapeNames[kNumShapes] = {
  "CSaw", "Morph", "Saw/square", "Sine/triangle fold", "Buzz", "Square sub", "Saw sub",
  "Square sync", "Saw sync", "Triple saw", "Triple square", "Triple triangle", "Triple sine",
  "Triple ring mod", "Saw swarm", "Saw comb", "Toy", "Filter LP", "Filter peak", "Filter BP",
  "Filter HP", "VOSIM", "Vowel", "Vowel FOF", "Harmonics", "FM", "Feedback FM",
  "Chaotic feedback FM", "Plucked", "Bowed", "Blown", "Fluted", "Struck bell", "Struck drum",
  "Kick", "Cymbal", "Snare", "Wavetables", "Wave map", "Wave line", "Paraphonic wavetables",
  "Filtered noise", "Twin peaks noise", "Clocked noise", "Granular cloud", "Particle noise",
  "Digital modulation",
};

// In the order of braids::scales
const char * const scaleNames[] = {
  "Off", "Semitones", "Ionian", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Aeolian",
  "Locrian", "Blues major", "Blues minor", "Pentatonic major", "Pentatonic minor", "Folk",
  "Japanese", "Gamelan", "Gypsy", "Arabian", "Flamenco", "Whole tone", "Pythagorean",
  "1/4 Eb", "1/4 E", "1/4 Ea", "Bhairav", "Gunakri", "Marwa", "Shree", "Purvi", "Bilawal",
  "Yaman", "Kafi", "Bhimpalasree", "Darbari", "Rageshree", "Khamaj", "Mimal", "Parameshwari",
  "Rangeshwari", "Gangeshwari", "Kameshwari", "Pa kafi", "Natbhairav", "M kauns", "Bairagi",
  "B todi", "Chandradeep", "Kaushik todi", "Jogeshwari",
};
static_assert(sizeof(scaleNames) / sizeof(scaleNames[0]) == kNumScales, "scale names");

const char * const bitsNames[braids::RESOLUTION_LAST] = {
  "2 bit", "3 bit", "4 bit", "6 bit", "8 bit", "12 bit", "16 bit",
};
const char * const rateNames[braids::SAMPLE_RATE_LAST] = {
  "4 kHz", "8 kHz", "16 kHz", "24 kHz", "32 kHz", "48 kHz", "96 kHz",
};
const char * const offOnNames[2] = { "off", "on" };

// braids.cc: sample rate and bit reduction
const uint16_t bitMasks[braids::RESOLUTION_LAST] = {
  0xc000, 0xe000, 0xf000, 0xf800, 0xff00, 0xfff0, 0xffff };
const size_t decimationFactors[braids::SAMPLE_RATE_LAST] = { 24, 12, 6, 4, 3, 2, 1 };

// 96 -> 48 kHz: the odd taps (1, 3, ... 23 from the centre) of a 47-tap half-band low pass
// (Kaiser window, beta 9): -0.15 dB at 20 kHz, -35 dB from 28 kHz. The centre tap is 0.5.
const int kHalfTaps = 12;
const int kFilterHistory = 4 * kHalfTaps - 2;      // 46 older samples
const float halfBand[kHalfTaps] = {
  0.315764388f, -0.098686166f, 0.051978624f, -0.030424480f, 0.018012619f, -0.010342151f,
  0.005596695f, -0.002776675f, 0.001218259f, -0.000445001f, 0.000118146f, -0.000012655f,
};

enum { IN_VOCT, IN_TIMBRE, IN_COLOR, IN_FM, IN_TRIGGER, IN_COUNT };

struct Instance {
  Instance * next;
  braids::MacroOscillator osc;
  braids::Envelope envelope;
  braids::Quantizer quantizer;
  int scale;
  float values[P_COUNT];
  MonoNotes notes;
  bool trigger;           // strike at the next module block
  bool played;            // a note or trigger came in: the envelope is the VCA from now on
  bool gateIn;            // the trigger input is above its threshold
  uint16_t gainLp;
  uint8_t sync[kModuleBlock];
  int16_t render[kModuleBlock];
  float history[kFilterHistory + kModuleBlock];
  BlockAdapter<kBlock, IN_COUNT, 1> blocks;
};

InstancePool<Instance> pool;

// Quantizer::Init() left-shifts negative numbers (undefined behaviour, the sanitizers stop the
// test), and Configure() replaces everything it sets but the hysteresis state: zero that
// instead (an empty cell around 0, which every scale contains), which also forgets the
// previous scale's note.
void configureQuantizer(Instance * s, int scale) {
  memset(static_cast<void *>(&s->quantizer), 0, sizeof(s->quantizer));
  s->quantizer.Configure(braids::scales[scale]);
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
  s->osc.Init();
  s->osc.set_shape(braids::MACRO_OSC_SHAPE_CSAW);
  s->envelope.Init();
  // Envelope::Init leaves the state alone: start it finished, at 0
  s->envelope.Update(0, 0);
  s->envelope.Trigger(braids::ENV_SEGMENT_DEAD);
  s->scale = 0;
  configureQuantizer(s, 0);
  paramDefaults(params, P_COUNT, s->values);
  s->notes.Init(params[P_NOTE].defaultValue);
  s->trigger = false;
  s->played = false;
  s->gateIn = false;
  s->gainLp = 0;
  memset(s->history, 0, sizeof(s->history));
  s->blocks.Init();
  return s;
}

void releaseInstance(void * instance) {
  pool.Give(static_cast<Instance *>(instance));
}

uint32_t getNumInputs() { return IN_COUNT; }
uint32_t getNumOutputs() { return 1; }
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
    case P_SHAPE: return copyText(shapeNames[i], text, size);
    case P_ENV_VCA: return copyText(offOnNames[i], text, size);
    case P_SCALE: return copyText(scaleNames[i], text, size);
    case P_BITS: return copyText(bitsNames[i], text, size);
    case P_RATE: return copyText(rateNames[i], text, size);
  }
  return 0;
}

inline int32_t clamp(int32_t x, int32_t lo, int32_t hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

inline int stepped(float v) {
  return static_cast<int>(v + 0.5f);
}

// One module block: braids.cc's RenderBlock, at 96 kHz, then decimated to 48 kHz
void renderBlock(Instance * s, float (*cv)[kBlock], float * out) {
  const float * v = s->values;

  // The trigger input: rising edges strike, and hard sync the analog shapes
  bool edge = false;
  for (size_t i = 0; i < kBlock; i++) {
    float x = cv[IN_TRIGGER][i];
    bool high = s->gateIn ? x > 0.1f : x > 0.2f;     // 1 V, with hysteresis
    bool rising = high && !s->gateIn;
    s->gateIn = high;
    s->sync[2 * i] = rising;
    s->sync[2 * i + 1] = 0;
    edge = edge || rising;
  }
  s->trigger = s->trigger || edge;

  int scale = stepped(v[P_SCALE]);
  if (scale != s->scale && scale >= 0 && scale < kNumScales) {
    s->scale = scale;
    configureQuantizer(s, scale);
  }

  // Settings 0..15 times 8 index the 128 envelope rates
  s->envelope.Update(stepped(v[P_ATTACK] * 127.0f), stepped(v[P_DECAY] * 127.0f));
  int32_t ad = s->envelope.Render();

  int shape = clamp(stepped(v[P_SHAPE]), 0, kNumShapes - 1);
  s->osc.set_shape(static_cast<braids::MacroOscillatorShape>(shape));

  // Timbre and color: knob + CV (1.0 = full range) + envelope (up to 15/32 of the range)
  const float knobs[2] = { v[P_TIMBRE] + cv[IN_TIMBRE][0], v[P_COLOR] + cv[IN_COLOR][0] };
  const float envAmounts[2] = { v[P_ENV_TIMBRE], v[P_ENV_COLOR] };
  int16_t parameters[2];
  for (int i = 0; i < 2; i++) {
    int32_t value = static_cast<int32_t>(knobs[i] * 32767.0f) +
        static_cast<int32_t>(ad * envAmounts[i] * (15.0f / 32.0f));
    parameters[i] = clamp(value, 0, 32767);
  }
  // RenderWaveLine reads wave_line[(2 * timbre >> 10) + 1]: past the 64 entries at full timbre
  if (shape == braids::MACRO_OSC_SHAPE_WAVE_LINE && parameters[0] > 32255) {
    parameters[0] = 32255;
  }
  s->osc.set_parameters(parameters[0], parameters[1]);

  // Pitch in 1/128 semitones, MIDI note numbers: the quantizer works on note + V/oct only
  float note = v[P_NOTE] + cv[IN_VOCT][0] * 60.0f;
  int32_t pitch = static_cast<int32_t>(clamp(static_cast<int32_t>(note * 128.0f), 0, 16383));
  pitch = s->quantizer.Process(pitch, (60 + stepped(v[P_ROOT])) << 7);
  pitch += static_cast<int32_t>(cv[IN_FM][0] * 60.0f * 128.0f);
  pitch += static_cast<int32_t>(ad * v[P_ENV_FM] * (15.0f / 128.0f));   // up to 60 semitones
  s->osc.set_pitch(clamp(pitch, 0, 16383));

  if (s->trigger) {
    s->osc.Strike();
    s->envelope.Trigger(braids::ENV_SEGMENT_ATTACK);
    s->played = true;
    s->trigger = false;
  }

  bool vca = stepped(v[P_ENV_VCA]) != 0;
  if (vca || v[P_ENV_TIMBRE] != 0.0f || v[P_ENV_COLOR] != 0.0f || v[P_ENV_FM] != 0.0f) {
    memset(s->sync, 0, sizeof(s->sync));
  }
  s->osc.Render(s->sync, s->render, kModuleBlock);

  int16_t held = 0;
  size_t decimation = decimationFactors[clamp(stepped(v[P_RATE]), 0, braids::SAMPLE_RATE_LAST - 1)];
  uint16_t mask = bitMasks[clamp(stepped(v[P_BITS]), 0, braids::RESOLUTION_LAST - 1)];
  int32_t gain = vca && s->played ? ad : 65535;
  float * x = s->history + kFilterHistory;
  for (size_t i = 0; i < kModuleBlock; i++) {
    if (i % decimation == 0) {
      held = s->render[i] & mask;
    }
    int16_t sample = held * s->gainLp >> 16;
    s->gainLp += (gain - s->gainLp) >> 4;
    x[i] = fromShort(sample);
  }

  // Decimate: output i is centred on 96 kHz sample 2i + 1 - 23 of this block
  for (size_t i = 0; i < kBlock; i++) {
    const float * c = x + 2 * i + 1 - (2 * kHalfTaps - 1);
    float y = 0.5f * c[0];
    for (int k = 0; k < kHalfTaps; k++) {
      y += halfBand[k] * (c[-(2 * k + 1)] + c[2 * k + 1]);
    }
    out[i] = y;
  }
  memmove(s->history, s->history + kModuleBlock, kFilterHistory * sizeof(float));
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
  s->blocks.Process(nrIn, in, nrOut, out, frameCount,
                    [s](float (*cv)[kBlock], float (*o)[kBlock]) { renderBlock(s, cv, o[0]); });
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Braids", "Mutable Instruments (port)", "1.9", "Macro oscillator, 47 shapes",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * braids_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  return &plugin;
}
