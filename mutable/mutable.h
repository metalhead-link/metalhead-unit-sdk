// Shared pieces of the Mutable Instruments plugin wrappers: the module code (eurorack/, MIT,
// Emilie Gillet) runs unmodified; each wrapper maps it onto the unit ABI.
//
// Conventions of the wrappers:
//   - Knobs and switches are parameters, with the module's own ranges (0..1 for most knobs).
//   - CV inputs that matter are extra audio inputs, so another unit's output can modulate them
//     (1.0 = 5 V, so 0.2 per volt for pitch CV); the module's own audio inputs come first.
//   - Notes arriving on midiIn set the pitch and gate/trigger the module (last note priority).
//   - The module renders blocks of its own size; BlockAdapter turns the host's variable block
//     size into that, with one module block of latency.
#ifndef MUTABLE_H_
#define MUTABLE_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>

extern "C" {
#include <unit_abi.h>
}

namespace mutable_units {

// --- Parameters --------------------------------------------------------------------------------

// A parameter table where each entry's id is its index
inline int paramInfo(const UnitParamInfo * table, uint32_t count, uint32_t index, UnitParamInfo * info) {
  if (index >= count) {
    return 0;
  }
  *info = table[index];
  return 1;
}

// Instance values live in a float array indexed by parameter id (a float can't be torn)
inline int paramValue(const UnitParamInfo * table, uint32_t count, const float * values, uint32_t id,
                      double * value) {
  if (id >= count || (table[id].flags & UNIT_PARAM_IS_TRIGGER)) {
    return 0;
  }
  *value = values[id];
  return 1;
}

// paramValueToText: copies a name (units have no strncpy)
inline int copyText(const char * name, char * text, uint32_t size) {
  if (size == 0) {
    return 0;
  }
  uint32_t i = 0;
  for (; name[i] && i < size - 1; i++) {
    text[i] = name[i];
  }
  text[i] = '\0';
  return 1;
}

inline void paramDefaults(const UnitParamInfo * table, uint32_t count, float * values) {
  for (uint32_t i = 0; i < count; i++) {
    values[i] = static_cast<float>(table[i].defaultValue);
  }
}

// --- MIDI notes ------------------------------------------------------------------------------

// Monophonic, last note priority, as a MIDI-to-CV interface would play the module
class MonoNotes {
 public:
  void Init(float note) {
    count_ = 0;
    note_ = note;
    triggered_ = false;
    velocity_ = 1.0f;
  }

  // Reads this block's midiIn; returns true if a new note started (a trigger)
  bool Process(const UnitHostApi * api, const UnitProcessHost * host) {
    triggered_ = false;
    if (!UNIT_PROCESS_HOST_HAS(api, midiIn)) {
      return false;
    }
    for (size_t i = 0; i < host->midiInCount; i++) {
      const UnitMidiPacket & p = host->midiIn[i];
      uint8_t cin = p.header & 0xF;
      if (cin == UNIT_MIDI_CIN_NOTE_ON && p.midi2) {
        Remove(p.midi1);
        if (count_ == kStack) {
          memmove(stack_, stack_ + 1, kStack - 1);
          count_--;
        }
        stack_[count_++] = p.midi1;
        note_ = p.midi1;
        velocity_ = p.midi2 / 127.0f;
        triggered_ = true;
      } else if (cin == UNIT_MIDI_CIN_NOTE_OFF || (cin == UNIT_MIDI_CIN_NOTE_ON && !p.midi2)) {
        Remove(p.midi1);
        if (count_) {
          note_ = stack_[count_ - 1];
        }
      }
    }
    return triggered_;
  }

  bool gate() const { return count_ > 0; }
  bool triggered() const { return triggered_; }
  float note() const { return note_; }          // MIDI note number
  float velocity() const { return velocity_; }  // 0..1 of the last note on

 private:
  static const int kStack = 16;

  void Remove(uint8_t note) {
    for (int i = 0; i < count_; i++) {
      if (stack_[i] == note) {
        memmove(stack_ + i, stack_ + i + 1, count_ - i - 1);
        count_--;
        return;
      }
    }
  }

  uint8_t stack_[kStack];
  int count_;
  float note_;
  float velocity_;
  bool triggered_;
};

// --- Blocks --------------------------------------------------------------------------------

// Collects kBlockSize input frames, calls render(in, out) once they're all there, and plays
// the previous block's output meanwhile: kBlockSize frames of latency, whatever the host's block
// size. Render is a callable taking (float in[kInputs][kBlockSize], float out[kOutputs][kBlockSize]).
template <size_t kBlockSize, size_t kInputs, size_t kOutputs>
class BlockAdapter {
 public:
  void Init() {
    memset(in_, 0, sizeof(in_));
    memset(out_, 0, sizeof(out_));
    position_ = 0;
  }

  template <typename Render>
  void Process(size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount, Render render) {
    for (size_t i = 0; i < frameCount; i++) {
      for (size_t c = 0; c < kInputs; c++) {
        in_[c][position_] = c < nrIn ? in[c][i] : 0.0f;
      }
      for (size_t c = 0; c < kOutputs && c < nrOut; c++) {
        out[c][i] = out_[c][position_];
      }
      if (++position_ == kBlockSize) {
        render(in_, out_);
        position_ = 0;
      }
    }
  }

 private:
  float in_[kInputs ? kInputs : 1][kBlockSize];
  float out_[kOutputs][kBlockSize];
  size_t position_;
};

// --- Instances -----------------------------------------------------------------------------

// Instances handed back by releaseInstance() wait here for the next createInstance(): memory
// from api->allocate() is never freed, so a config reload would leak them otherwise. T needs a
// `T * next` member.
template <typename T>
class InstancePool {
 public:
  T * Take() {
    T * t = free_;
    if (t) {
      free_ = t->next;
    }
    return t;
  }

  void Give(T * t) {
    t->next = free_;
    free_ = t;
  }

 private:
  T * free_;
};

// 0..1 audio in the unit graph to the module's 16-bit samples, and back
inline short toShort(float x) {
  x *= 32767.0f;
  return static_cast<short>(x > 32767.0f ? 32767 : (x < -32768.0f ? -32768 : x));
}

inline float fromShort(short x) {
  return x * (1.0f / 32768.0f);
}

}  // namespace mutable_units

#endif  // MUTABLE_H_
