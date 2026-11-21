#pragma once

/*
 *   Pure trainer maths: no Arduino headers, no hardware, no state that a device
 *   owns. Every rule that turns a distance into a depth, a depth into a level, a
 *   level into a hold, and a run into a session total lives here, so it can be
 *   compiled and tested on a PC (test/host/test_trainer_math.cpp) instead of
 *   only on the board.
 *
 *   Conventions:
 *     range    distance the sensor reports, millimetres
 *     depth    how far the trainer was taken, millimetres, 0 at the reference
 *     reference distance from the sensor that counts as depth 0
 *     calibration  a constant added to every range sample (mounting tolerance)
 *     depth = clamp(reference - (range + calibration), 0, maxDepth)
 */

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace trainer
{

inline float clampf(float value, float low, float high)
{
    return (value < low) ? low : ((value > high) ? high : value);
}

/* --------------------------------------------------------------- rep mode ---- */

/* A rep is the goal held for the hold time, and the next rep needs a full return to
 * the start. That is what makes the count a workout: holding the goal without coming
 * back up counts once, and the countdown cannot be gamed by staying deep. */
struct RepCount
{
    uint16_t count = 0;
    bool armed = true;         /* ready to accept a completed hold */
    bool targetReached = false;

    void reset(uint16_t target)
    {
        count = 0;
        armed = true;
        targetReached = (target == 0) ? false : (count >= target);
    }

    /* Returns true when the hold counts as a rep. A rep that arrives while the
     * previous one has not been released is ignored. */
    bool countRep(uint16_t target)
    {
        if (!armed)
        {
            return false;
        }
        count++;
        armed = false;
        if (target > 0 && count >= target)
        {
            targetReached = true;
        }
        return true;
    }

    /* The return to the start: depth at or below releaseMm arms the next rep. */
    bool release(float depthMm, float releaseMm)
    {
        if (armed || !(depthMm <= releaseMm))
        {
            return false;
        }
        armed = true;
        return true;
    }

    uint16_t remaining(uint16_t target) const
    {
        return (target > count) ? (uint16_t)(target - count) : 0;
    }
};

/* ---------------------------------------------------------------- depth ---- */

inline float depthFromRange(float rangeMm, float calibrationMm, float referenceMm, float maxDepthMm)
{
    const float corrected = rangeMm + calibrationMm;
    return clampf(referenceMm - corrected, 0.0f, maxDepthMm);
}

/* The raw distance the sensor reports is never negative in practice; a huge
 * reading is how the sensor says "nothing in front of me", so the applier keeps
 * whatever is assumed beyond the range as depth 0. */
inline bool rangeIsUsable(float rangeMm)
{
    return std::isfinite(rangeMm) && rangeMm > 0.0f && rangeMm < 250.0f;
}

inline float gaugeFraction(float depthMm, float maxDepthMm)
{
    if (!(maxDepthMm > 0.0f))
    {
        return 0.0f;
    }
    return clampf(depthMm / maxDepthMm, 0.0f, 1.0f);
}

/* ---------------------------------------------------------------- level ---- */

/* 0 = below the first goal, 1..3 = that goal or deeper */
inline int levelForDepth(float depthMm, float level1Mm, float level2Mm, float level3Mm)
{
    if (depthMm >= level3Mm && level3Mm > 0.0f)
    {
        return 3;
    }
    if (depthMm >= level2Mm && level2Mm > 0.0f)
    {
        return 2;
    }
    if (depthMm >= level1Mm && level1Mm > 0.0f)
    {
        return 1;
    }
    return 0;
}

inline const char *levelName(int level)
{
    switch (level)
    {
    case 1:
        return "Level 1";
    case 2:
        return "Level 2";
    case 3:
        return "Level 3";
    default:
        return "Warm up";
    }
}

/* The user picks the goal as a level number; 0 means "no goal, free play". */
inline float goalForLevel(int level, float level1Mm, float level2Mm, float level3Mm)
{
    switch (level)
    {
    case 1:
        return level1Mm;
    case 2:
        return level2Mm;
    case 3:
        return level3Mm;
    default:
        return 0.0f;
    }
}

/* --------------------------------------------------------------- filter ---- */

/* Exponential moving average whose behaviour does not depend on the sample
 * interval. tau is the time constant in seconds; tau = 0 passes the sample
 * through unchanged. */
class EmaFilter
{
public:
    void reset()
    {
        _value = 0.0f;
        _valid = false;
    }

    void seed(float value)
    {
        _value = value;
        _valid = true;
    }

    float push(float sample, float dtSeconds, float tauSeconds)
    {
        if (!_valid || !(tauSeconds > 0.0f) || !(dtSeconds > 0.0f))
        {
            seed(sample);
            return _value;
        }
        float alpha = 1.0f - std::exp(-dtSeconds / tauSeconds);
        alpha = clampf(alpha, 0.0f, 1.0f);
        _value += alpha * (sample - _value);
        return _value;
    }

    float value() const { return _value; }
    bool valid() const { return _valid; }

private:
    float _value = 0.0f;
    bool _valid = false;
};

/* ---------------------------------------------------------------- holds ---- */

/* Counts how long the depth stays at or above a goal. A hold is complete once
 * the goal was held for holdSeconds without dropping out of the zone. */
class HoldTracker
{
public:
    void reset(float holdSeconds)
    {
        _need = (holdSeconds > 0.0f) ? holdSeconds : 0.0f;
        _held = 0.0f;
        _inZone = false;
        _complete = false;
    }

    /* dtSeconds is capped by the caller, so a pause in the sample stream cannot
     * jump the hold to complete in one step. */
    void update(bool atGoal, float dtSeconds)
    {
        _inZone = atGoal;
        if (!atGoal)
        {
            _complete = false;
            _held = 0.0f;
            return;
        }
        if (dtSeconds > 0.0f)
        {
            _held += dtSeconds;
        }
        if (_need <= 0.0f || _held >= _need)
        {
            _complete = true;
        }
    }

    /* Called after a completed hold was counted, so the same hold is not
     * counted again on the next sample. */
    void acknowledge()
    {
        _complete = false;
        _held = 0.0f;
    }

    void retarget(float holdSeconds)
    {
        _need = (holdSeconds > 0.0f) ? holdSeconds : 0.0f;
    }

    bool inZone() const { return _inZone; }
    bool complete() const { return _complete; }
    float heldSeconds() const { return _held; }
    float neededSeconds() const { return _need; }

    float progress() const
    {
        if (_need <= 0.0f)
        {
            return _inZone ? 1.0f : 0.0f;
        }
        return clampf(_held / _need, 0.0f, 1.0f);
    }

private:
    float _need = 3.0f;
    float _held = 0.0f;
    bool _inZone = false;
    bool _complete = false;
};

/* -------------------------------------------------------------- session ---- */

struct SessionSummary
{
    uint32_t samples = 0;
    float elapsedSeconds = 0.0f;
    float peakMm = 0.0f;
    float timeWeightedDepth = 0.0f; /* integral of depth over time, mm*s */
    float deepSeconds = 0.0f;       /* time at or beyond the deep threshold */
    uint32_t holds = 0;
    float longestHoldSeconds = 0.0f;

    float averageMm() const
    {
        return (elapsedSeconds > 0.0f) ? (timeWeightedDepth / elapsedSeconds) : 0.0f;
    }
};

class Session
{
public:
    void begin()
    {
        _summary = SessionSummary();
        _active = true;
    }

    void end()
    {
        _active = false;
    }

    void clear()
    {
        _summary = SessionSummary();
        _active = false;
    }

    /* The caller integrates, this only accumulates, so one place decides how the
     * sample interval is handled. dtSeconds is clamped in main, not here. */
    void addSample(float depthMm, float dtSeconds, float deepThresholdMm)
    {
        if (dtSeconds <= 0.0f)
        {
            return;
        }
        _summary.samples++;
        _summary.elapsedSeconds += dtSeconds;
        _summary.timeWeightedDepth += depthMm * dtSeconds;
        if (depthMm > _summary.peakMm)
        {
            _summary.peakMm = depthMm;
        }
        if (deepThresholdMm > 0.0f && depthMm >= deepThresholdMm)
        {
            _summary.deepSeconds += dtSeconds;
        }
    }

    void countHold(float heldSeconds)
    {
        _summary.holds++;
        if (heldSeconds > _summary.longestHoldSeconds)
        {
            _summary.longestHoldSeconds = heldSeconds;
        }
    }

    bool active() const { return _active; }
    const SessionSummary &summary() const { return _summary; }

private:
    SessionSummary _summary;
    bool _active = false;
};

/* ------------------------------------------------------------- panel effects ---- */

/* The big animations on the panel run from a start time to a duration, on the device.
 * That way they cannot get stuck when the page that asked for one is closed, and the
 * timing can be tested on a PC. A start time of 0 means "nothing running". */

inline bool fxActive(uint32_t nowMs, uint32_t startMs, uint32_t durationMs)
{
    if (startMs == 0 || durationMs == 0)
    {
        return false;
    }
    return (nowMs - startMs) < durationMs;
}

inline float fxProgress(uint32_t nowMs, uint32_t startMs, uint32_t durationMs)
{
    if (durationMs == 0)
    {
        return 1.0f;
    }
    const uint32_t elapsed = (nowMs >= startMs) ? (nowMs - startMs) : 0u;
    if (elapsed >= durationMs)
    {
        return 1.0f;
    }
    return (float)elapsed / (float)durationMs;
}

/* One ring of the burst: it leaves the middle at its own moment and reaches the rim
 * at the end of its own span, then stays there. The stagger is what makes the burst
 * read as movement rather than as one growing circle. */
inline float fxRingGauge(uint8_t index, float progress)
{
    /* Three rings, each a little later than the one before, and the last one still
     * reaches the rim inside the animation: 0.15 + 3 * 0.15 + 0.55 = 1.00. */
    const float ringOffset = 0.15f;
    const float ringSpan = 0.55f;
    float t = (progress - ringOffset * (float)index) / ringSpan;
    return (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
}

inline int16_t fxRingRadius(uint8_t index, float progress, int16_t outRadius)
{
    return (int16_t)lroundf(fxRingGauge(index, progress) * (float)outRadius);
}

/* A pulse, so an animation that has settled still looks alive. */
inline bool fxPulseOn(uint32_t nowMs, uint32_t periodMs)
{
    if (periodMs == 0)
    {
        return true;
    }
    return ((nowMs / periodMs) & 1u) != 0u;
}

} /* namespace trainer */
