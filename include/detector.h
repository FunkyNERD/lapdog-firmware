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
//
// The baseline (the in-room level) is either fixed with setBaseline() or, after
// trackBaseline(rise), tracked: it starts at the first sample, follows the
// smoothed level down quickly and up only slowly, and holds still during a
// peak - so a pass never lifts its own floor.
//
// Tracking settles on the quad-off floor, and a powered quad anywhere in a
// small room sits 15-25 dB above that (issue #12), so with tracking the
// thresholds can't be small. After calibration (calibrate.h) the baseline is
// the measured floor, fixed, and enter/exit come from the in-flight and gate
// levels; a fixed baseline is kept across reset().

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
        pending_ = false;
        haveLast_ = false;
        haveEma_ = false;
        if (rise_ > 0) haveBaseline_ = false; // a fixed (calibrated) floor survives a reset
    }

    void setBaseline(float db)
    {
        baseline_ = db;
        haveBaseline_ = true;
    }
    // rise: per-sample pull up towards the level outside peaks (0 = fixed baseline).
    void trackBaseline(float rise) { rise_ = rise; }
    // A gate pass can dip under exitDb for a moment and come back (session 1:
    // 0.3-0.9 s later, sometimes stronger). With a merge window, an exit waits
    // up to ms for a re-entry, and a re-entry continues the same peak. The pass
    // time is still the peak's; the result just comes out up to ms later. The
    // parked check then uses the longest stretch above exitDb. 0 = off.
    void mergeWithin(uint32_t ms) { mergeMs_ = ms; }
    float baseline() const { return baseline_; }
    float smoothed() const { return ema_; }
    bool inPeak() const { return inPeak_; }

    // Feed one band-power sample. On PASS the first pass after reset has
    // lapMs 0 (it starts lap 1).
    PassResult feed(float db, uint32_t nowMs, Pass &out)
    {
        ema_ = haveEma_ ? ema_ + p_.emaAlpha * (db - ema_) : db;
        haveEma_ = true;
        // Not during a peak, nor on the sample that starts one.
        if (rise_ > 0 && !inPeak_ && !pending_ && !(haveBaseline_ && ema_ - baseline_ >= p_.enterDb))
        {
            if (!haveBaseline_) baseline_ = ema_;
            else if (ema_ < baseline_) baseline_ += BASELINE_FALL * (ema_ - baseline_);
            else baseline_ += rise_ * (ema_ - baseline_);
            haveBaseline_ = true;
        }
        float above = ema_ - baseline_;

        if (!inPeak_)
        {
            if (pending_ && above >= p_.enterDb)
            {
                pending_ = false; // the same pass, back again
                inPeak_ = true;
                stretchMs_ = nowMs;
            }
            else if (pending_)
            {
                if (nowMs - exitMs_ < mergeMs_) return PassResult::NONE;
                pending_ = false;
                return finish(out);
            }
            else
            {
                if (above >= p_.enterDb)
                {
                    inPeak_ = true;
                    peakDb_ = ema_;
                    peakMs_ = nowMs;
                    enterMs_ = stretchMs_ = nowMs;
                    longestMs_ = 0;
                }
                return PassResult::NONE;
            }
        }

        if (ema_ > peakDb_)
        {
            peakDb_ = ema_;
            peakMs_ = nowMs;
        }
        if (above > p_.exitDb) return PassResult::NONE;

        inPeak_ = false;
        if (nowMs - stretchMs_ > longestMs_) longestMs_ = nowMs - stretchMs_;
        exitMs_ = nowMs;
        if (mergeMs_)
        {
            pending_ = true;
            return PassResult::NONE;
        }
        return finish(out);
    }

private:
    PassResult finish(Pass &out)
    {
        out.atMs = peakMs_;
        out.durationMs = mergeMs_ ? longestMs_ : exitMs_ - enterMs_;
        out.lapMs = haveLast_ ? peakMs_ - lastPassMs_ : 0;
        if (p_.maxPeakMs && out.durationMs > p_.maxPeakMs) return PassResult::TOO_LONG;
        if (haveLast_ && out.lapMs < p_.minLapMs) return PassResult::TOO_SOON;
        lastPassMs_ = peakMs_;
        haveLast_ = true;
        return PassResult::PASS;
    }

    static constexpr float BASELINE_FALL = 0.2f;
    DetectorParams p_;
    float rise_ = 0;
    float baseline_ = 0;
    bool haveBaseline_ = false;
    float ema_ = 0;
    bool haveEma_ = false;
    bool inPeak_ = false;
    float peakDb_ = 0;
    uint32_t peakMs_ = 0;
    uint32_t enterMs_ = 0;
    uint32_t stretchMs_ = 0; // start of the current stretch above exit (merging)
    uint32_t longestMs_ = 0;
    uint32_t exitMs_ = 0;
    uint32_t mergeMs_ = 0;
    bool pending_ = false; // exited, waiting mergeMs_ for a re-entry
    bool haveLast_ = false;
    uint32_t lastPassMs_ = 0;
};
