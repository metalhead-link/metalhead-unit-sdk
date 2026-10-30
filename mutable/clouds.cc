// Clouds: texture synthesizer, granular processor (eurorack/clouds, 32 kHz, 32-frame blocks).
//
// Outputs: 1 left, 2 right. Inputs: 1 left, 2 right audio; CV (1.0 = 5 V, added to the knobs,
// 5 V is the full range): 3 position, 4 size, 5 1 V/oct pitch, 6 density, 7 texture, 8 blend
// (dry/wet); gates (high above 1 V): 9 trigger, 10 freeze. Notes on midiIn trigger a grain
// (or resync the delay/stretch) and hold the gate input (the spectral mode's glitches) while
// held; they don't change the pitch.
//
// The module runs at 32 kHz: the wrapper resamples 48 -> 32 kHz in front of it and back after
// it (resampler.h), so pitch and time are those of the hardware. Latency about 2.7 ms: a
// 32-frame block at 32 kHz plus the two filters' group delay (0.66 + 1.0 ms).
// GranularProcessor::Prepare(), the hardware's main loop work (FFTs of the spectral mode, the
// stretch mode's correlation search), runs with each block, in process().
#include "mutable.h"
#include "resampler.h"

#include "clouds/dsp/granular_processor.h"

using namespace mutable_units;

namespace {

const UnitHostApi * api;

enum ParamId {
  P_POSITION, P_SIZE, P_PITCH, P_DENSITY, P_TEXTURE, P_BLEND, P_SPREAD, P_FEEDBACK, P_REVERB,
  P_FREEZE, P_MODE, P_QUALITY, P_TRIGGER, P_COUNT
};

const UnitParamInfo params[P_COUNT] = {
  { P_POSITION, UNIT_PARAM_IS_AUTOMATABLE, "position", "", 0, 1, 0 },
  { P_SIZE, UNIT_PARAM_IS_AUTOMATABLE, "size", "", 0, 1, 0.5 },
  { P_PITCH, UNIT_PARAM_IS_AUTOMATABLE, "pitch", "", -48, 48, 0 },
  { P_DENSITY, UNIT_PARAM_IS_AUTOMATABLE, "density", "", 0, 1, 0.7 },
  { P_TEXTURE, UNIT_PARAM_IS_AUTOMATABLE, "texture", "", 0, 1, 0.5 },
  { P_BLEND, UNIT_PARAM_IS_AUTOMATABLE, "blend", "blend", 0, 1, 0.5 },
  { P_SPREAD, UNIT_PARAM_IS_AUTOMATABLE, "spread", "blend", 0, 1, 0 },
  { P_FEEDBACK, UNIT_PARAM_IS_AUTOMATABLE, "feedback", "blend", 0, 1, 0 },
  { P_REVERB, UNIT_PARAM_IS_AUTOMATABLE, "reverb", "blend", 0, 1, 0 },
  { P_FREEZE, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_AUTOMATABLE, "freeze", "", 0, 1, 0 },
  { P_MODE, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "mode", "", 0, 3, 0 },
  { P_QUALITY, UNIT_PARAM_IS_STEPPED | UNIT_PARAM_IS_ENUM | UNIT_PARAM_IS_AUTOMATABLE, "quality", "", 0, 3, 0 },
  { P_TRIGGER, UNIT_PARAM_IS_TRIGGER, "trigger", "", 0, 1, 1 },
};

// clouds::PlaybackMode
const char * const modeNames[4] = {
  "Granular", "Pitch shifter/time stretcher", "Looping delay", "Spectral",
};

// GranularProcessor::set_quality: bit 0 mono, bit 1 lo-fi (8-bit mu-law at 16 kHz)
const char * const qualityNames[4] = {
  "16-bit stereo", "16-bit mono", "8-bit lo-fi stereo", "8-bit lo-fi mono",
};

enum {
  IN_LEFT, IN_RIGHT, IN_POSITION, IN_SIZE, IN_PITCH, IN_DENSITY, IN_TEXTURE, IN_BLEND,
  IN_TRIGGER, IN_FREEZE, IN_COUNT
};

// The buffers clouds.cc gives GranularProcessor::Init: main memory and the STM32F4's CCM
const size_t kLargeBufferSize = 118784;
const size_t kSmallBufferSize = 65536 - 128;

const size_t kBlock = clouds::kMaxBlockSize;
const float kGateThreshold = 0.2f;      // 1 V

RateFilters filters;

// --- Pitch shifter/time stretcher fix ---------------------------------------------------------
//
// In the pinned eurorack tree, clouds::Window::Start() no longer sets done_ = false: commits
// fbb53ba and 0e3756f each removed one of its two (duplicate) assignments, and the merge
// d1d8839 kept both removals. A window that has finished can then never play again, and the
// stretch mode is silent (the module's released firmware predates this). The code can't be
// changed here, so before each block the wrapper revives windows that Start() has set up and
// that haven't played yet (phase 0, not regenerated, an envelope set). A window started in the
// middle of a block begins at the next one instead: at most 32 samples late, in a crossfade of
// hundreds of samples.
//
// The fields are private: they're reached with pointers to members, made by explicit template
// instantiation (which is exempt from access checks).
template <typename Tag, typename Tag::type kMember>
struct PrivateMember {
  friend typename Tag::type get(Tag) { return kMember; }
};

#define CLOUDS_PRIVATE(tag, memberType, cls, member) \
  struct tag { typedef memberType cls::*type; friend type get(tag); }; \
  template struct PrivateMember<tag, &cls::member>

CLOUDS_PRIVATE(WsPlayerTag, clouds::WSOLASamplePlayer, clouds::GranularProcessor, ws_player_);
typedef clouds::Window WindowPair[2];
CLOUDS_PRIVATE(WindowsTag, WindowPair, clouds::WSOLASamplePlayer, windows_);
CLOUDS_PRIVATE(DoneTag, bool, clouds::Window, done_);
CLOUDS_PRIVATE(RegeneratedTag, bool, clouds::Window, regenerated_);
CLOUDS_PRIVATE(PhaseTag, int32_t, clouds::Window, phase_);
CLOUDS_PRIVATE(EnvelopeIncrementTag, float, clouds::Window, envelope_phase_increment_);

void ReviveStartedWindows(clouds::GranularProcessor * processor) {
  clouds::Window * windows = (processor->*get(WsPlayerTag())).*get(WindowsTag());
  for (int i = 0; i < 2; i++) {
    clouds::Window & w = windows[i];
    if (w.*get(DoneTag()) && w.*get(PhaseTag()) == 0 && !(w.*get(RegeneratedTag())) &&
        w.*get(EnvelopeIncrementTag()) > 0.0f) {
      w.*get(DoneTag()) = false;
    }
  }
}

// ----------------------------------------------------------------------------------------------

struct Instance {
  Instance * next;
  clouds::GranularProcessor processor;
  void * largeBuffer;
  void * smallBuffer;
  float values[P_COUNT];
  MonoNotes notes;
  bool trigger;
  bool triggerGate;       // the trigger input's previous state
  RateBridge<kBlock, IN_COUNT> bridge;
};

InstancePool<Instance> pool;

void * createInstance() {
  Instance * s = pool.Take();
  void * large;
  void * small;
  if (s) {
    large = s->largeBuffer;
    small = s->smallBuffer;
  } else {
    s = static_cast<Instance *>(api->allocate(sizeof(Instance)));
    large = api->allocate(kLargeBufferSize);
    small = api->allocate(kSmallBufferSize);
    if (!s || !large || !small) {
      return NULL;
    }
  }
  // Value-initialised: the processor relies on members its Init leaves alone (silence_,
  // freeze_lp_, the feedback buffer) being zero, as globals on the hardware are
  s = new (s) Instance();
  s->largeBuffer = large;
  s->smallBuffer = small;
  s->processor.Init(large, kLargeBufferSize, small, kSmallBufferSize);
  s->processor.set_silence(false);
  paramDefaults(params, P_COUNT, s->values);
  s->processor.set_playback_mode(clouds::PLAYBACK_MODE_GRANULAR);
  s->processor.set_quality(0);
  s->processor.Prepare();
  s->notes.Init(60);
  s->trigger = false;
  s->triggerGate = false;
  s->bridge.Init(&filters, 2, 3, kBlock);
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
  if (choice < 0 || choice > 3) {
    return 0;
  }
  if (id == P_MODE) {
    return copyText(modeNames[choice], text, size);
  } else if (id == P_QUALITY) {
    return copyText(qualityNames[choice], text, size);
  }
  return 0;
}

// The module's lookup tables read one entry past x * size: at exactly 1.0 that's past their end
// (the hardware's knobs never quite get there)
const float kAlmostOne = 0.99999f;

inline float Clamp01(float x) {
  return x < 0.0f ? 0.0f : (x > kAlmostOne ? kAlmostOne : x);
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
    s->trigger = true;
  }

  s->bridge.Process(nrIn, in, nrOut, out, frameCount,
                    [s](float (*audio)[kBlock], float (*o)[kBlock], float (*cv)[kBlock]) {
    const float * v = s->values;
    const size_t last = kBlock - 1;
    clouds::GranularProcessor & p = s->processor;
    p.set_playback_mode(static_cast<clouds::PlaybackMode>(static_cast<int>(v[P_MODE] + 0.5f) & 3));
    p.set_quality(static_cast<int>(v[P_QUALITY] + 0.5f) & 3);

    // Gates: a rising edge anywhere in the block triggers; freeze follows its input's level
    bool trigger = s->trigger;
    s->trigger = false;
    for (size_t i = 0; i < kBlock; i++) {
      bool high = cv[IN_TRIGGER - 2][i] > kGateThreshold;
      trigger = trigger || (high && !s->triggerGate);
      s->triggerGate = high;
    }

    // The CV is sampled once per block, as the hardware's ADC is
    clouds::Parameters * m = p.mutable_parameters();
    m->position = Clamp01(v[P_POSITION] + cv[IN_POSITION - 2][last]);
    m->size = Clamp01(v[P_SIZE] + cv[IN_SIZE - 2][last]);
    float pitch = v[P_PITCH] + cv[IN_PITCH - 2][last] * 60.0f;     // 12 semitones per volt
    m->pitch = pitch < -48.0f ? -48.0f : (pitch > 48.0f ? 48.0f : pitch);
    m->density = Clamp01(v[P_DENSITY] + cv[IN_DENSITY - 2][last]);
    m->texture = Clamp01(v[P_TEXTURE] + cv[IN_TEXTURE - 2][last]);
    m->dry_wet = Clamp01(v[P_BLEND] + cv[IN_BLEND - 2][last]);
    m->stereo_spread = Clamp01(v[P_SPREAD]);
    m->feedback = Clamp01(v[P_FEEDBACK]);
    m->reverb = Clamp01(v[P_REVERB]);
    m->freeze = v[P_FREEZE] >= 0.5f || cv[IN_FREEZE - 2][last] > kGateThreshold;
    m->trigger = trigger;
    m->gate = s->notes.gate() || s->triggerGate;

    clouds::ShortFrame input[kBlock];
    clouds::ShortFrame output[kBlock];
    for (size_t i = 0; i < kBlock; i++) {
      input[i].l = toShort(audio[0][i]);
      input[i].r = toShort(audio[1][i]);
    }
    // Mode and quality changes take effect in Prepare(), which Process() waits for
    p.Prepare();
    if (p.playback_mode() == clouds::PLAYBACK_MODE_STRETCH) {
      ReviveStartedWindows(&p);
    }
    p.Process(input, output, kBlock);
    p.Prepare();
    for (size_t i = 0; i < kBlock; i++) {
      o[0][i] = fromShort(output[i].l);
      o[1][i] = fromShort(output[i].r);
    }
  });
}

const UnitPlugin plugin = {
  sizeof(UnitPlugin), "Clouds", "Mutable Instruments (port)", "1.0", "Texture synthesizer, granular processor",
  createInstance, releaseInstance, getNumInputs, getNumOutputs,
  getParamCount, getParamInfo, getParamValue, paramValueToText, NULL, process,
};

}  // namespace

extern "C" const UnitPlugin * clouds_plugin(const UnitHostApi * hostApi) {
  api = hostApi;
  filters.Design();
  return &plugin;
}
