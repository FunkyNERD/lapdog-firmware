#pragma once
// Gate-pass detection from band power (dB), RotorHazard style: smooth with an
// EMA, then a peak is "entered" when the smoothed level rises enter_db above
// the baseline and "exited" when it falls back below exit_db. The pass time is
// the time of the highest smoothed sample in between. Pure logic, no IDF -
// unit-tested natively (test/test_detector).

#include <stdint.h>

struct DetectorParams
{
    float emaAlpha;
    float enterDb;    // above baseline
    float exitDb;     // above baseline; must be < enterDb (hysteresis)
    uint32_t minLapMs;
};

class Detector
{
public:
    explicit Detector(const DetectorParams &p) : p_(p) {}

    // Forget any pass in progress and the lap reference (e.g. on arm).
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

    // Feed one band-power sample. Returns true when a gate pass completes;
    // passMs is then the peak's timestamp and lapMs the time since the
    // previous pass (0 for the first pass after reset, which starts lap 1).
    bool feed(float db, uint32_t nowMs, uint32_t &passMs, uint32_t &lapMs)
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
            }
            return false;
        }

        if (ema_ > peakDb_)
        {
            peakDb_ = ema_;
            peakMs_ = nowMs;
        }
        if (above > p_.exitDb) return false;

        inPeak_ = false;
        if (haveLast_ && peakMs_ - lastPassMs_ < p_.minLapMs) return false; // bounce / too soon
        passMs = peakMs_;
        lapMs = haveLast_ ? peakMs_ - lastPassMs_ : 0;
        lastPassMs_ = peakMs_;
        haveLast_ = true;
        return true;
    }

private:
    DetectorParams p_;
    float baseline_ = 0;
    float ema_ = 0;
    bool haveEma_ = false;
    bool inPeak_ = false;
    float peakDb_ = 0;
    uint32_t peakMs_ = 0;
    bool haveLast_ = false;
    uint32_t lastPassMs_ = 0;
};
