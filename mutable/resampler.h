// Rate conversion between the host's 48 kHz and a module that runs at another rate (Clouds at
// 32 kHz, Beads at 32 or 24 kHz in its lo-fi modes), with the module's fixed block size.
//
// Resampler: a polyphase FIR for a rational ratio L/M (output rate = input rate * L / M),
// stereo, with a Kaiser-windowed sinc designed at plugin entry (DesignLowPass).
// RateBridge: host frames in, module blocks rendered at the module's rate, host frames out. It
// collects a block of the module's samples, renders it, and plays the previous block meanwhile
// (BlockAdapter in mutable.h, plus the two filters). Control inputs (CV, gates) aren't
// resampled: each module sample carries the values of the host frame it was made in.
//
// Used by clouds.cc and beads.cc; included by each, so everything here is internal to the file.
#ifndef MUTABLE_RESAMPLER_H_
#define MUTABLE_RESAMPLER_H_

#include <math.h>
#include <stddef.h>
#include <string.h>

namespace mutable_units {
namespace {

// Zeroth order modified Bessel function of the first kind (Kaiser window)
inline float BesselI0(float x) {
  float sum = 1.0f;
  float term = 1.0f;
  for (int k = 1; k < 32; k++) {
    float t = x / (2.0f * k);
    term *= t * t;
    sum += term;
    if (term < sum * 1e-9f) {
      break;
    }
  }
  return sum;
}

// A low pass FIR of l * taps coefficients for interpolation by l (gain l): cutoff in Hz at the
// intermediate rate (the input rate times l). Kaiser beta 7.5: about 75 dB of stop band.
inline void DesignLowPass(float * h, int l, int taps, float intermediateRate, float cutoff) {
  const int n = l * taps;
  const float fc = cutoff / intermediateRate;           // cycles per intermediate sample
  const float beta = 7.5f;
  const float i0Beta = BesselI0(beta);
  const float centre = 0.5f * (n - 1);
  for (int i = 0; i < n; i++) {
    float t = i - centre;
    float x = 2.0f * static_cast<float>(M_PI) * fc * t;
    float sinc = t == 0.0f ? 1.0f : sinf(x) / x;
    float r = t / (centre + 0.5f);
    float window = BesselI0(beta * sqrtf(fmaxf(0.0f, 1.0f - r * r))) / i0Beta;
    h[i] = static_cast<float>(l) * 2.0f * fc * sinc * window;
  }
}

class Resampler {
 public:
  static const int kMaxTaps = 96;        // per phase: the length of the input history

  // h: l * taps coefficients from DesignLowPass(h, l, taps, ...), kept by the caller
  void Init(int l, int m, const float * h, int taps) {
    l_ = l;
    m_ = m;
    h_ = h;
    taps_ = taps;
    memset(historyL_, 0, sizeof(historyL_));
    memset(historyR_, 0, sizeof(historyR_));
    position_ = 0;
    phase_ = 0;
  }

  // One input frame; emit(l, r) for each output frame it completes
  template <typename Emit>
  inline void Push(float inL, float inR, Emit emit) {
    // Newest sample first: x[i - k] is history[position_ + k], doubled so a read never wraps
    position_ = position_ == 0 ? taps_ - 1 : position_ - 1;
    historyL_[position_] = historyL_[position_ + taps_] = inL;
    historyR_[position_] = historyR_[position_ + taps_] = inR;
    const float * xl = &historyL_[position_];
    const float * xr = &historyR_[position_];
    while (phase_ < l_) {
      const float * h = h_ + phase_;
      float yl = 0.0f;
      float yr = 0.0f;
      for (int k = 0; k < taps_; k++) {
        float c = h[k * l_];
        yl += c * xl[k];
        yr += c * xr[k];
      }
      emit(yl, yr);
      phase_ += m_;
    }
    phase_ -= l_;
  }

 private:
  int l_;
  int m_;
  const float * h_;
  int taps_;
  float historyL_[2 * kMaxTaps];
  float historyR_[2 * kMaxTaps];
  int position_;
  int phase_;
};

// Coefficients for module rates of 48 kHz * l / m; DesignFilters() once at plugin entry
struct RateFilters {
  static const int kTaps32k = 64;       // 48 <-> 32 kHz, through 96 kHz
  static const int kTaps24k = 96;       // 48 <-> 24 kHz, at 48 kHz
  float down32k[2 * kTaps32k];          // l = 2, m = 3
  float up32k[3 * kTaps32k];            // l = 3, m = 2
  float down24k[1 * kTaps24k];          // l = 1, m = 2
  float up24k[2 * kTaps24k];            // l = 2, m = 1

  void Design() {
    // Cut off at 90% of the module's Nyquist frequency, the stop band starts at about its Nyquist
    DesignLowPass(down32k, 2, kTaps32k, 96000.0f, 14400.0f);
    DesignLowPass(up32k, 3, kTaps32k, 96000.0f, 14400.0f);
    DesignLowPass(down24k, 1, kTaps24k, 48000.0f, 10800.0f);
    DesignLowPass(up24k, 2, kTaps24k, 48000.0f, 10800.0f);
  }
};

// kInputs host channels: 2 audio (left, right), then kInputs - 2 control inputs. Two outputs.
template <size_t kMaxBlockSize, size_t kInputs>
class RateBridge {
 public:
  static const size_t kControls = kInputs > 2 ? kInputs - 2 : 1;

  // Module rate 48 kHz * l / m, with (l, m) one of (1, 1), (2, 3), (1, 2)
  void Init(const RateFilters * filters, int l, int m, size_t blockSize) {
    l_ = l;
    m_ = m;
    blockSize_ = blockSize;
    if (l == 2 && m == 3) {
      down_.Init(2, 3, filters->down32k, RateFilters::kTaps32k);
      up_.Init(3, 2, filters->up32k, RateFilters::kTaps32k);
    } else if (l == 1 && m == 2) {
      down_.Init(1, 2, filters->down24k, RateFilters::kTaps24k);
      up_.Init(2, 1, filters->up24k, RateFilters::kTaps24k);
    } else {
      l_ = m_ = 1;
    }
    memset(in_, 0, sizeof(in_));
    memset(out_, 0, sizeof(out_));
    memset(controls_, 0, sizeof(controls_));
    position_ = 0;
    fifoRead_ = fifoWrite_ = 0;
  }

  size_t blockSize() const { return blockSize_; }

  // render(FloatFrame-like in[2][kMaxBlockSize], out[2][kMaxBlockSize],
  //        controls[kControls][kMaxBlockSize]) renders blockSize() frames
  template <typename Render>
  void Process(size_t nrIn, float ** in, size_t nrOut, float ** out, size_t frameCount, Render render) {
    for (size_t i = 0; i < frameCount; i++) {
      float l = nrIn > 0 ? in[0][i] : 0.0f;
      float r = nrIn > 1 ? in[1][i] : 0.0f;
      for (size_t c = 0; c < kControls; c++) {
        frameControls_[c] = c + 2 < nrIn ? in[c + 2][i] : 0.0f;
      }
      if (l_ == 1 && m_ == 1) {
        ModuleSample(l, r, render, [this](float ol, float orr) { FifoPush(ol, orr); });
      } else {
        down_.Push(l, r, [this, &render](float dl, float dr) {
          ModuleSample(dl, dr, render, [this](float ol, float orr) {
            up_.Push(ol, orr, [this](float ul, float ur) { FifoPush(ul, ur); });
          });
        });
      }
      float ol = 0.0f;
      float orr = 0.0f;
      if (fifoRead_ != fifoWrite_) {
        ol = fifoL_[fifoRead_];
        orr = fifoR_[fifoRead_];
        fifoRead_ = (fifoRead_ + 1) & (kFifo - 1);
      }
      if (nrOut > 0) {
        out[0][i] = ol;
      }
      if (nrOut > 1) {
        out[1][i] = orr;
      }
    }
  }

 private:
  static const unsigned kFifo = 8;

  // One sample at the module's rate: into the block, and the previous block's sample out
  template <typename Render, typename Output>
  inline void ModuleSample(float l, float r, Render & render, Output output) {
    in_[0][position_] = l;
    in_[1][position_] = r;
    for (size_t c = 0; c < kControls; c++) {
      controls_[c][position_] = frameControls_[c];
    }
    output(out_[0][position_], out_[1][position_]);
    if (++position_ == blockSize_) {
      render(in_, out_, controls_);
      position_ = 0;
    }
  }

  inline void FifoPush(float l, float r) {
    unsigned next = (fifoWrite_ + 1) & (kFifo - 1);
    if (next != fifoRead_) {
      fifoL_[fifoWrite_] = l;
      fifoR_[fifoWrite_] = r;
      fifoWrite_ = next;
    }
  }

  int l_;
  int m_;
  size_t blockSize_;
  Resampler down_;
  Resampler up_;
  float in_[2][kMaxBlockSize];
  float out_[2][kMaxBlockSize];
  float controls_[kControls][kMaxBlockSize];
  float frameControls_[kControls];
  size_t position_;
  float fifoL_[kFifo];
  float fifoR_[kFifo];
  unsigned fifoRead_;
  unsigned fifoWrite_;
};

}  // namespace
}  // namespace mutable_units

#endif  // MUTABLE_RESAMPLER_H_
