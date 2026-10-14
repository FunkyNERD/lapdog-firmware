#pragma once
// Gate-pass detection from band power (dB), RotorHazard style: smooth with an
// EMA, then a peak is "entered" when the smoothed level rises enter_db above
// the baseline and "exited" when it falls back below exit_db. The pass time is
// the time of the highest smoothed sample in between. Pure logic, no IDF -
// unit-tested natively (test/test_detector).
//
// False-read protection - a peak can be rejected instead of counted, and
// feed() says why so the timer can report it:
//   TOO_SOON  peak less than minLapMs after the last counted pass (a crash into
//             the gate, or a bounce back through it)
//   TOO_LONG  peak lasted longer than maxPeakMs (a quad parked at the gate; when
//             it's picked up the exit would otherwise count as a lap)
// Rejected peaks don't move the lap reference, so the next real pass still
// times from the last counted one.

#include <stdint.h>

struct DetectorParams
{
    float emaAlpha;
    float enterDb;      // above baseline
    float exitDb;       // above baseline; must be < enterDb (hysteresis)
    uint32_t minLapMs;  // passes sooner than this after the last one are ignored
    uint32_t maxPeakMs; // peaks longer than this are ignored; 0 = no limit
};

enum class PassResult : uint8_t
{
    NONE,     // no pass completed on this sample
    PASS,     // a counted gate pass
    TOO_SOON, // rejected: within minLapMs of the last pass
    TOO_LONG, // rejected: peak longer than maxPeakMs (parked quad)
};

struct Pass
{
    uint32_t atMs;       // peak timestamp
    uint32_t lapMs;      // PASS / TOO_SOON: time since the last counted pass (0 for the first)
    uint32_t durationMs; // enter -> exit
};

class Detector
{
public:
    explicit Detector(const DetectorParams &p) : p_(p) {}

    // Forget any pass in progress and the lap reference (e.g. on arm, at GO).
    void reset()
    {
        inPeak_ = false;
        haveLast_ = false;
        haveEma_ = false;
    }

    void setBaseline(float db) { baseline_ = db; }
    float baseline() const { return baseline_; }
    float smoothed() const { return ema_; }
    bool inPeak() const { return inPeak_; }

    // Feed one band-power sample. On PASS the first pass after reset has
    // lapMs 0 (it starts lap 1).
    PassResult feed(float db, uint32_t nowMs, Pass &out)
    {
        ema_ = haveEma_ ? ema_ + p_.emaAlpha * (db - ema_) : db;
        haveEma_ = true;
        float above = ema_ - baseline_;

        if (!inPeak_)
        {
            if (above >= p_.enterDb)
            {
                inPeak_ = true;
                peakDb_ = ema_;
                peakMs_ = nowMs;
                enterMs_ = nowMs;
            }
            return PassResult::NONE;
        }

        if (ema_ > peakDb_)
        {
            peakDb_ = ema_;
            peakMs_ = nowMs;
        }
        if (above > p_.exitDb) return PassResult::NONE;

        inPeak_ = false;
        out.atMs = peakMs_;
        out.durationMs = nowMs - enterMs_;
        out.lapMs = haveLast_ ? peakMs_ - lastPassMs_ : 0;
        if (p_.maxPeakMs && out.durationMs > p_.maxPeakMs) return PassResult::TOO_LONG;
        if (haveLast_ && out.lapMs < p_.minLapMs) return PassResult::TOO_SOON;
        lastPassMs_ = peakMs_;
        haveLast_ = true;
        return PassResult::PASS;
    }

private:
    DetectorParams p_;
    float baseline_ = 0;
    float ema_ = 0;
    bool haveEma_ = false;
    bool inPeak_ = false;
    float peakDb_ = 0;
    uint32_t peakMs_ = 0;
    uint32_t enterMs_ = 0;
    bool haveLast_ = false;
    uint32_t lastPassMs_ = 0;
};
