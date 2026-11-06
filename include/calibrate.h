#pragma once
// Receiver calibration (issue #12). Pure logic, no IDF - unit-tested natively
// (test/test_calibrate), including a replay of a real indoor session.
//
// What session 1 showed: a powered quad anywhere in a small room is 15-25 dB
// above the quad-off floor, and the gate pass is another ~25 dB above that.
// Thresholds measured from a tracked baseline (which settles on the floor)
// can't tell flying from passing. So calibration measures three levels and
// the detector runs with a fixed baseline at the floor:
//
//   1. floor     quad off (or VTX off): the receiver's own level, dBFS
//   2. flight    the pilot flies a few laps: the in-flight level (median of
//                the envelope) and the gate passes (the big local maxima)
//   3. advice    gain right? (no clipped passes, headroom left) - else fly
//                again at the suggested gain; then enter/exit, dB above the
//                floor, between the in-flight level and the pass level
//
// The envelope is the max level per BUCKET_MS, which keeps a minute of flying
// in a few KB and makes short gate passes stand out.

#include <math.h>
#include <stdint.h>

namespace calib
{

constexpr uint32_t BUCKET_MS = 100;
constexpr float PASS_RISE_DB = 12.0f;   // a pass stands this far above the in-flight level
constexpr uint32_t PASS_WINDOW_MS = 2000; // and is the highest within this either side
constexpr unsigned MIN_PASSES = 3;
constexpr float HEADROOM_MIN_DB = 6.0f;  // passes this far below full scale, at least
constexpr float HEADROOM_MAX_DB = 24.0f; // more than this and the gain can come up
constexpr int GAIN_STEP = 8;             // ~6 dB at this front end
constexpr float LOW_CONTRAST_DB = 12.0f; // pass vs. in-flight below this: unreliable
constexpr float ENTER_FRACTION = 0.6f;   // of the way from in-flight to pass level
constexpr float EXIT_FRACTION = 0.35f;
// Detector::mergeWithin() for a calibrated timer. Session 1: a pass split in two
// re-entered within 80-330 ms; separate events (gate hit and retry, a section near
// the gate) were 520 ms or more apart. One session's worth - check it again.
constexpr uint32_t MERGE_MS = 400;
// "cal auto": a round ends after AUTO_PASSES passes (counted live) or AUTO_ROUND_MS,
// and re-flies at the advised gain up to AUTO_ROUNDS times.
constexpr unsigned AUTO_PASSES = 6;
constexpr uint32_t AUTO_ROUND_MS = 90000;
constexpr unsigned AUTO_ROUNDS = 3;

// Histogram for medians without sorting: HIST_STEP_DB bins over HIST_MIN..MAX dBFS.
constexpr float HIST_MIN_DB = -100.0f;
constexpr float HIST_MAX_DB = 10.0f;
constexpr float HIST_STEP_DB = 0.25f;
constexpr unsigned HIST_BINS = (unsigned)((HIST_MAX_DB - HIST_MIN_DB) / HIST_STEP_DB);

struct Bucket
{
    float maxDb;
    bool clipped;
};

// Collects one calibration step. The caller owns the bucket storage (heap on
// the device: static data has to stay clear of the SDR's capture bank).
class Recorder
{
public:
    Recorder(Bucket *buf, unsigned capacity) : buf_(buf), cap_(capacity) { reset(); }

    void reset()
    {
        n_ = 0;
        samples_ = 0;
        sumDb_ = 0;
        open_ = false;
        for (unsigned i = 0; i < HIST_BINS; i++) hist_[i] = 0;
    }

    // One sample: band power in dBFS, and whether the snapshot clipped.
    void add(uint32_t ms, float db, bool clipped)
    {
        samples_++;
        sumDb_ += db;
        uint32_t b = ms / BUCKET_MS;
        if (open_ && b == bucketNo_)
        {
            Bucket &cur = buf_[n_ - 1];
            if (db > cur.maxDb) cur.maxDb = db;
            cur.clipped |= clipped;
            return;
        }
        if (open_) histAdd(buf_[n_ - 1].maxDb);
        if (n_ == cap_)
        {
            open_ = false; // full: keep what we have
            return;
        }
        buf_[n_++] = {db, clipped};
        bucketNo_ = b;
        open_ = true;
    }

    unsigned size() const { return n_; }
    unsigned samples() const { return samples_; }
    const Bucket &operator[](unsigned i) const { return buf_[i]; }
    // Mean of every sample (the floor is flat to a fraction of a dB).
    float meanDb() const { return samples_ ? (float)(sumDb_ / samples_) : NAN; }

    // Median of the bucket maxima, to HIST_STEP_DB.
    float medianDb() const
    {
        // The open bucket isn't in the histogram yet: count it on the side.
        unsigned extra = open_ ? bin(buf_[n_ - 1].maxDb) : HIST_BINS;
        unsigned total = open_ ? 1 : 0;
        for (unsigned i = 0; i < HIST_BINS; i++) total += hist_[i];
        if (!total) return NAN;
        unsigned seen = 0;
        for (unsigned i = 0; i < HIST_BINS; i++)
        {
            seen += hist_[i] + (i == extra ? 1 : 0);
            if (seen * 2 >= total) return HIST_MIN_DB + (i + 0.5f) * HIST_STEP_DB;
        }
        return HIST_MAX_DB;
    }

private:
    static unsigned bin(float db)
    {
        if (!(db > HIST_MIN_DB)) return 0;
        unsigned i = (unsigned)((db - HIST_MIN_DB) / HIST_STEP_DB);
        return i < HIST_BINS ? i : HIST_BINS - 1;
    }
    void histAdd(float db) { hist_[bin(db)]++; }

    Bucket *buf_;
    unsigned cap_;
    unsigned n_;
    unsigned samples_;
    double sumDb_;
    bool open_;
    uint32_t bucketNo_ = 0;
    uint16_t hist_[HIST_BINS];
};

struct Flight
{
    unsigned passes;        // gate passes found
    unsigned clippedPasses; // of those, with a clipped snapshot within 200 ms
    float inFlightDb;       // median envelope level, dBFS
    float passDb;           // median pass peak, dBFS
    float weakestPassDb;
    float strongestPassDb;
};

// Is bucket i a gate pass: PASS_RISE_DB above the in-flight level and the highest
// within PASS_WINDOW_MS either side (ties: the first)? Buckets past the end count
// as lower, so only ask about i once i + window has closed (see PassCounter).
inline bool isPassAt(const Recorder &r, int i, float inFlightDb)
{
    const int w = (int)(PASS_WINDOW_MS / BUCKET_MS);
    const int n = (int)r.size();
    float v = r[i].maxDb;
    if (v < inFlightDb + PASS_RISE_DB) return false;
    for (int j = i - w; j <= i + w; j++)
    {
        if (j < 0 || j >= n || j == i) continue;
        if (j < i ? r[j].maxDb >= v : r[j].maxDb > v) return false;
    }
    return true;
}

// Counts passes while a flight is recorded, for progress ("one... two..."): each
// bucket is judged once its window has closed, against the in-flight level so far.
// It runs PASS_WINDOW_MS behind, and the in-flight level settles as the flight goes
// on, so analyse() at the end has the final word.
class PassCounter
{
public:
    void reset() { next_ = 0; }
    // After Recorder::add(); returns how many passes were found just now.
    unsigned update(const Recorder &r)
    {
        const int w = (int)(PASS_WINDOW_MS / BUCKET_MS);
        int closed = (int)r.size() - 1; // the last bucket is still filling
        unsigned found = 0;
        if (next_ + w >= closed) return 0;
        float flight = r.medianDb();
        for (; next_ + w < closed; next_++)
            if (isPassAt(r, next_, flight)) found++;
        return found;
    }

private:
    int next_ = 0;
};

// The gate passes in a flight: the buckets isPassAt() picks.
inline Flight analyse(const Recorder &r)
{
    Flight f = {};
    f.inFlightDb = r.medianDb();
    f.passDb = f.weakestPassDb = f.strongestPassDb = NAN;
    if (!r.size()) return f;
    const int n = (int)r.size();
    // Pass levels for the median: at most one per window, so few; keep a small sorted list.
    constexpr unsigned MAX_PASSES = 64;
    float levels[MAX_PASSES];
    for (int i = 0; i < n; i++)
    {
        if (!isPassAt(r, i, f.inFlightDb)) continue;
        float v = r[i].maxDb;
        bool clipped = false;
        for (int j = i - 2; j <= i + 2; j++)
            if (j >= 0 && j < n && r[j].clipped) clipped = true;
        if (clipped) f.clippedPasses++;
        if (f.passes < MAX_PASSES)
        {
            unsigned k = f.passes;
            while (k > 0 && levels[k - 1] > v)
            {
                levels[k] = levels[k - 1];
                k--;
            }
            levels[k] = v;
        }
        f.passes++;
    }
    unsigned kept = f.passes < MAX_PASSES ? f.passes : MAX_PASSES;
    if (kept)
    {
        f.weakestPassDb = levels[0];
        f.strongestPassDb = levels[kept - 1];
        f.passDb = kept % 2 ? levels[kept / 2] : 0.5f * (levels[kept / 2 - 1] + levels[kept / 2]);
    }
    return f;
}

enum class Verdict : uint8_t
{
    OK,             // use this gain and these thresholds
    LOWER_GAIN,     // passes clip or nearly: fly again at Advice::gain
    RAISE_GAIN,     // passes far below full scale: fly again at Advice::gain
    TOO_FEW_PASSES, // fly more laps through the gate
};

struct Advice
{
    Verdict verdict;
    int gain;          // the gain to use (OK) or to fly again at
    float headroomDb;  // strongest pass below full scale (0 dBFS)
    float contrastDb;  // median pass above the in-flight level
    bool lowContrast;  // contrast < LOW_CONTRAST_DB: passes may be missed or false
    float enterDb;     // above the floor; valid when OK (and set for the others, as a guide)
    float exitDb;
};

// floorDb: quad-off level at this gain. gainMax: the receiver's top index.
// clippedAt: the lowest gain already flown that clipped (-1 = none), so a
// raise never goes back into clipping.
inline Advice advise(const Flight &f, float floorDb, int gain, int gainMax, int clippedAt = -1)
{
    Advice a = {};
    a.gain = gain;
    a.headroomDb = isnan(f.strongestPassDb) ? NAN : -f.strongestPassDb;
    a.contrastDb = f.passDb - f.inFlightDb;
    a.lowContrast = !(a.contrastDb >= LOW_CONTRAST_DB);
    float flight = f.inFlightDb - floorDb, pass = f.passDb - floorDb;
    a.enterDb = flight + ENTER_FRACTION * (pass - flight);
    a.exitDb = flight + EXIT_FRACTION * (pass - flight);

    if (f.passes < MIN_PASSES)
    {
        a.verdict = Verdict::TOO_FEW_PASSES;
        return a;
    }
    if (f.clippedPasses || a.headroomDb < HEADROOM_MIN_DB)
    {
        a.verdict = Verdict::LOWER_GAIN;
        a.gain = gain - GAIN_STEP < 0 ? 0 : gain - GAIN_STEP;
        if (a.gain == gain) a.verdict = Verdict::OK; // nothing lower: live with it
        return a;
    }
    if (a.headroomDb > HEADROOM_MAX_DB)
    {
        int up = gain + GAIN_STEP;
        if (up > gainMax) up = gainMax;
        if (clippedAt >= 0 && up >= clippedAt) up = clippedAt - 1;
        if (up > gain)
        {
            a.verdict = Verdict::RAISE_GAIN;
            a.gain = up;
            return a;
        }
    }
    a.verdict = Verdict::OK;
    return a;
}

} // namespace calib
