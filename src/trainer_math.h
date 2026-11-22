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
#include <cstdio>
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

/* The goals are a list the user edits: 1 to 10 levels, and level 0 in the state means "no
 * goal, free play". */
constexpr int kMaxLevels = 10;

/* A level is a *band*, not a floor: it only counts while the depth is between the level's
 * depth and its upper limit. Going past the top of a level is then not a way to complete it,
 * which is the point of the upper limit (level 1 counts between 40 mm and 60 mm, not from
 * 40 mm on for ever). */
struct LevelBand
{
    float low = 0.0f;
    float high = 0.0f;
};

/* How many levels are actually in use. Anything outside 1..kMaxLevels is clamped, so a
 * client cannot ask for zero levels or for more than there is storage for. */
inline int levelCountInUse(int requested)
{
    return (requested < 1) ? 1 : ((requested > kMaxLevels) ? kMaxLevels : requested);
}

inline LevelBand bandForLevel(int level, const float lows[kMaxLevels], const float highs[kMaxLevels],
                              int count, float minWidthMm)
{
    LevelBand band;
    if (level < 1 || level > levelCountInUse(count))
    {
        return band;
    }
    band.low = lows[level - 1];
    band.high = (highs[level - 1] < band.low) ? band.low : highs[level - 1];
    /* A band narrower than minWidth is not a target anyone can hit. A level saved before the
     * upper limits existed arrives with its limit equal to its depth, which would make it
     * impossible, so the width is enforced here as well as when the settings are written. */
    if (band.high < band.low + minWidthMm)
    {
        band.high = band.low + minWidthMm;
    }
    return band;
}

/* Which level the depth is at now: the deepest level it has reached. 0 means shallower than
 * every level. The upper limits do not enter into it - the level is where the depth is, and
 * the band decides whether a hold counts. */
inline int levelForDepth(float depthMm, const float lows[kMaxLevels], int count)
{
    int level = 0;
    const int inUse = levelCountInUse(count);
    for (int i = 0; i < inUse; i++)
    {
        if (lows[i] > 0.0f && depthMm >= lows[i])
        {
            level = i + 1;
        }
    }
    return level;
}

/* Does the depth count for this level, right now? Both ends are inclusive, so the value in
 * the setting is reachable. */
inline bool inBand(float depthMm, const LevelBand &band)
{
    return band.low > 0.0f && depthMm >= band.low && depthMm <= band.high;
}

/* How far the depth has got across the band, 0..1. Used for the hold bar and for the web
 * interface, not for the decision itself. */
inline float bandProgress(float depthMm, const LevelBand &band)
{
    const float span = band.high - band.low;
    if (span <= 0.0f)
    {
        return inBand(depthMm, band) ? 1.0f : 0.0f;
    }
    return clampf((depthMm - band.low) / span, 0.0f, 1.0f);
}

/* The next level in the direction the user picked. Counting down starts at the last level
 * and walks back to the first, which is a different game from climbing. Returns 0 when the
 * walk is finished, and the caller ends the run. */
inline int nextLevel(int level, int count, bool countDown)
{
    const int inUse = levelCountInUse(count);
    if (countDown)
    {
        return (level > 1) ? (level - 1) : 0;
    }
    return (level < inUse) ? (level + 1) : 0;
}

/* Where a run starts: the first level, or the last one when counting down. */
inline int firstLevel(int count, bool countDown)
{
    return countDown ? levelCountInUse(count) : 1;
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
    case 4:
        return "Level 4";
    case 5:
        return "Level 5";
    case 6:
        return "Level 6";
    case 7:
        return "Level 7";
    case 8:
        return "Level 8";
    case 9:
        return "Level 9";
    case 10:
        return "Level 10";
    default:
        return "Warm up";
    }
}

/* The user picks the goal as a level number; 0 means "no goal, free play". */
inline float goalForLevel(int level, const float lows[kMaxLevels], int count)
{
    if (level < 1 || level > levelCountInUse(count))
    {
        return 0.0f;
    }
    return lows[level - 1];
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

/* One ring of the burst: it leaves the middle at its own moment and reaches the rim at the
 * end of its own span, then stays there. The stagger is what makes the burst read as
 * movement rather than as one growing circle. */
inline float fxRingGauge(uint8_t index, float progress)
{
    /* Three rings, each a little later than the one before, and the last one still reaches
     * the rim inside the animation: 0.15 + 3 * 0.15 + 0.55 = 1.00. */
    const float ringOffset = 0.15f;
    const float ringSpan = 0.55f;
    float t = (progress - ringOffset * (float)index) / ringSpan;
    return (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
}

inline int16_t fxRingRadius(uint8_t index, float progress, int16_t outRadius)
{
    return (int16_t)lroundf(fxRingGauge(index, progress) * (float)outRadius);
}

/* A ring is only drawn once it has started growing: before that it would sit on the ring in
 * front of it and the burst would look like one thick line. */
inline bool fxRingStarted(uint8_t index, float progress)
{
    return fxRingGauge(index, progress) > 0.0f;
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

/* ------------------------------------------------------------------- menu ---- */

/* The panel menu: play mode and level, picked with a finger on the glass.

 * One source of truth for the geometry. The display draws the rows with these numbers and
 * the touch handler hit-tests with the same ones, so a row cannot be painted in one place
 * and picked in another. The y values are offsets from the panel centre, positive down. */

constexpr int kMenuRowMode = 0;
constexpr int kMenuRowLevel = 1;
constexpr int kMenuRowCalibrate = 2;
constexpr int kMenuRowClose = 3;
constexpr int kMenuRowCount = 4;

/* Four rows instead of three: tighter, so the last row and the hint line still land inside the
 * round window (checked by the host test). */
constexpr float kMenuRowTopY = -110.0f;
constexpr float kMenuRowHeight = 50.0f;
constexpr float kMenuRowGap = 6.0f;
constexpr float kMenuRowHalfWidth = 150.0f;

/* The button that opens the menu: a chip below the hold bar, inside the ring. The drawn size
 * and the touch target are separate: the chip is 132x40 px, and a finger gets 160x56. */
constexpr float kMenuButtonHalfWidth = 66.0f;      /* drawn */
constexpr float kMenuButtonHalfHeight = 20.0f;     /* drawn */
constexpr float kMenuButtonTouchHalfWidth = 80.0f; /* accepted */
constexpr float kMenuButtonTouchHalfHeight = 28.0f;
constexpr float kMenuButtonCentreY = 158.0f;

/* The battery symbol moves out of the top corner and sits beside the menu button, where the
 * eye already goes for the controls. Its centre is this far left of the panel centre, clear of
 * the chip's touch target so a tap there is not a tap on the button. */
constexpr float kPanelBatteryCentreDx = -100.0f;

/* The panel's absolute bottom band, kept here so it has one home. The footer sits below the
 * menu chip, and the two must not overlap: the footer erases a box around itself before it
 * draws, so a chip in that band is wiped off the panel every time the footer text changes.
 * kPanelFooterBandHalf is that erase box's half height. */
constexpr int kPanelFooterCy = 434;
constexpr int kPanelFooterBandHalf = 16;

inline float menuRowTop(int row)
{
    return kMenuRowTopY + (float)row * (kMenuRowHeight + kMenuRowGap);
}

/* Which row a finger at dy (relative to the centre) is on, or -1 for none. The gap
 * between two rows is a miss on purpose. */
inline int menuRowAt(float dy)
{
    for (int row = 0; row < kMenuRowCount; ++row)
    {
        const float top = menuRowTop(row);
        if (dy >= top && dy <= top + kMenuRowHeight)
        {
            return row;
        }
    }
    return -1;
}

/* The menu button: a rounded rectangle centred on the panel's vertical axis. The accepted
 * area is larger than the drawing, because the touch controller reports a point with a few
 * pixels of noise and a control that needs a precise hit reads as broken. */
inline bool menuButtonHit(float dx, float dy)
{
    return (dx >= -kMenuButtonTouchHalfWidth && dx <= kMenuButtonTouchHalfWidth &&
            dy >= kMenuButtonCentreY - kMenuButtonTouchHalfHeight &&
            dy <= kMenuButtonCentreY + kMenuButtonTouchHalfHeight);
}

/* A row has two halves where that makes sense: the left third steps back, the rest
 * steps forward. Returns -1 for back, +1 for forward. */
inline int menuStepDirection(float dx)
{
    return (dx < -(kMenuRowHalfWidth / 3.0f)) ? -1 : 1;
}

/* --------------------------------------------------------------- play mode ---- */

constexpr int kPlayModeHold = 0; /* hold the goal */
constexpr int kPlayModeReps = 1; /* count reps */
constexpr int kPlayModeCount = 2;

inline int clampPlayMode(int mode)
{
    return (mode < 0) ? 0 : ((mode >= kPlayModeCount) ? (kPlayModeCount - 1) : mode);
}

/* The menu shows one mode per tap, so the panel can always reach both. */
inline int nextPlayMode(int mode)
{
    return (clampPlayMode(mode) + 1) % kPlayModeCount;
}

inline const char *playModeName(int mode)
{
    return (clampPlayMode(mode) == kPlayModeReps) ? "COUNT REPS" : "HOLD GOAL";
}

/* ---------------------------------------------------------------- the level ---- */

/* Level stepping from the panel: free play (0) and then 1..count, wrapping in both
 * directions. Anything out of range is treated as free play, so a level list that shrank
 * cannot leave the menu pointing at a level the game no longer has. */
inline int menuLevelStep(int level, int count, int direction)
{
    if (count < 1)
    {
        return 0;
    }
    const int stops = count + 1; /* free play is one more stop on the dial */
    int index = ((level < 1) || (level > count)) ? 0 : level;
    index = (index + ((direction < 0) ? (stops - 1) : 1)) % stops;
    return index;
}

/* The menu changes the game, so it is closed to the finger while a session runs. One
 * place decides this, and both the button and the level rows ask it. */
inline bool menuUsable(bool sessionActive)
{
    return !sessionActive;
}

/* The level as the menu writes it: free play is 0, which is exactly what the goal level
 * field means everywhere else. */
inline int menuLevelLabel(int level, int count, char *buffer, size_t size)
{
    if ((level < 1) || (level > count))
    {
        snprintf(buffer, size, "FREE");
    }
    else
    {
        snprintf(buffer, size, "%d", level);
    }
    return 0;
}

/* A row press means nothing unless the menu is on the screen, exactly as on the glass: the
 * two inputs run the same path. This index is the calibration wizard's capture, which a finger
 * reaches by tapping while the guide is up. */
constexpr int kMenuPressCapture = 9;
/* The menu chip itself: open it, close it, or leave the calibration guide, exactly as a finger
 * on the chip does. */
constexpr int kMenuPressChip = 10;

/* ------------------------------------------------------------- calibration ---- */

/* Two points, and the two numbers the depth scale is built from: reference_mm is the distance
 * that reads 0 and max_depth_mm is the travel that reaches full scale. Point the head at the 0
 * position and capture its distance; point it at the full position and capture again. The
 * reference is then the first distance and the travel is the difference between them. */

constexpr float kCalibrationMinTravelMm = 10.0f;
constexpr float kCalibrationMaxTravelMm = 240.0f;

constexpr int kCalStepOff = 0;
constexpr int kCalStepZero = 1; /* waiting for the head at the 0 position */
constexpr int kCalStepFull = 2; /* waiting for the head at the full position */
constexpr int kCalStepDone = 3; /* showing what was measured */

/* The capture is only taken from a reading that is still. A hand moving the head past the
 * sensor gives a number the head was passing through, and a scale built on that is wrong
 * everywhere. */
constexpr int kCalibrationSamples = 8;
constexpr float kCalibrationToleranceMm = 6.0f;

inline bool readingIsStable(const float *samples, int count, float toleranceMm = kCalibrationToleranceMm)
{
    if (count < kCalibrationSamples)
    {
        return false;
    }
    float low = samples[0];
    float high = samples[0];
    for (int i = 1; i < count; ++i)
    {
        if (samples[i] < low)
        {
            low = samples[i];
        }
        if (samples[i] > high)
        {
            high = samples[i];
        }
    }
    return (high - low) <= toleranceMm;
}

inline float readingAverage(const float *samples, int count)
{
    if (count < 1)
    {
        return NAN;
    }
    float sum = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        sum += samples[i];
    }
    return sum / (float)count;
}

/* The travel between the two capture points, and whether it is a scale worth writing. Both
 * distances have to be real readings, and the head has to have moved by a sane amount towards
 * the sensor: a travel of zero means the head never moved, and one beyond the panel's own
 * range means a capture landed on the wrong end. */
inline bool calibrationTravel(float zeroRangeMm, float fullRangeMm, float *travelMm)
{
    const float travel = zeroRangeMm - fullRangeMm;
    if (travelMm != nullptr)
    {
        *travelMm = travel;
    }
    return (zeroRangeMm > 1.0f) && (fullRangeMm > 1.0f) &&
           (travel >= kCalibrationMinTravelMm) && (travel <= kCalibrationMaxTravelMm);
}

} /* namespace trainer */
