/*
 *   Host test for src/trainer_math.h.
 *
 *   The rules that turn a distance into a depth, a depth into a level, a level
 *   into a completed hold and a run into session totals are the parts most
 *   likely to be wrong and the hardest to see on a 480x480 display. They carry
 *   no hardware dependency on purpose, so they run here in a second:
 *
 *       test/host/run.sh
 */

#include "../../src/trainer_math.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failures = 0;
static int checks = 0;

static void expectNear(const char *what, float got, float want, float tolerance = 0.001f)
{
    checks++;
    if (!(std::fabs(got - want) <= tolerance))
    {
        std::printf("FAIL %-46s got %.4f want %.4f\n", what, got, want);
        failures++;
    }
}

static void expectInt(const char *what, int got, int want)
{
    checks++;
    if (got != want)
    {
        std::printf("FAIL %-46s got %d want %d\n", what, got, want);
        failures++;
    }
}

static void expectStr(const char *what, const char *got, const char *want)
{
    checks++;
    if (std::strcmp(got, want) != 0)
    {
        std::printf("FAIL %-46s got \"%s\" want \"%s\"\n", what, got, want);
        failures++;
    }
}

static void expectTrue(const char *what, bool got)
{
    checks++;
    if (!got)
    {
        std::printf("FAIL %-46s expected true\n", what);
        failures++;
    }
}

int main()
{
    using namespace trainer;

    /* ---- range -> depth -------------------------------------------------- */
    expectNear("rest position is depth 0", depthFromRange(150.0f, 0.0f, 150.0f, 120.0f), 0.0f);
    expectNear("100 mm closer is 50 mm deep", depthFromRange(100.0f, 0.0f, 150.0f, 120.0f), 50.0f);
    expectNear("past the reference stays at 0", depthFromRange(180.0f, 0.0f, 150.0f, 120.0f), 0.0f);
    expectNear("beyond full scale is clamped", depthFromRange(10.0f, 0.0f, 150.0f, 120.0f), 120.0f);
    expectNear("calibration shifts the reading", depthFromRange(100.0f, 5.0f, 150.0f, 120.0f), 45.0f);
    expectNear("negative calibration", depthFromRange(100.0f, -5.0f, 150.0f, 120.0f), 55.0f);

    /* ---- gauge ----------------------------------------------------------- */
    expectNear("gauge at zero", gaugeFraction(0.0f, 120.0f), 0.0f);
    expectNear("gauge at half", gaugeFraction(60.0f, 120.0f), 0.5f);
    expectNear("gauge clamps above full scale", gaugeFraction(200.0f, 120.0f), 1.0f);
    expectNear("gauge with no scale is 0", gaugeFraction(50.0f, 0.0f), 0.0f);

    /* ---- levels ---------------------------------------------------------- */
    {
        const float lows[kMaxLevels] = {40.0f, 60.0f, 80.0f, 100.0f, 120.0f, 140.0f, 160.0f, 180.0f, 200.0f, 200.0f};
        const float highs[kMaxLevels] = {60.0f, 80.0f, 100.0f, 120.0f, 140.0f, 160.0f, 180.0f, 200.0f, 220.0f, 220.0f};

        /* Five levels in use: everything past the fifth must be invisible. */
        expectInt("below level 1", levelForDepth(10.0f, lows, 5), 0);
        expectInt("exactly level 1", levelForDepth(40.0f, lows, 5), 1);
        expectInt("level 2", levelForDepth(65.0f, lows, 5), 2);
        expectInt("level 3", levelForDepth(85.0f, lows, 5), 3);
        expectInt("level 4", levelForDepth(105.0f, lows, 5), 4);
        expectInt("level 5", levelForDepth(195.0f, lows, 5), 5);
        expectInt("a sixth level is out of use, so the depth stays at 5", levelForDepth(150.0f, lows, 5), 5);
        expectInt("with ten levels the same depth is level 6", levelForDepth(150.0f, lows, 10), 6);
        expectInt("with ten levels it goes to 10", levelForDepth(250.0f, lows, 10), 10);

        /* The count is clamped, not trusted. */
        expectInt("zero levels becomes one", levelCountInUse(0), 1);
        expectInt("eleven levels becomes ten", levelCountInUse(11), kMaxLevels);
        expectInt("a sane count is left alone", levelCountInUse(7), 7);
        expectInt("one level is honoured", levelForDepth(200.0f, lows, levelCountInUse(-4)), 1);

        const float none[kMaxLevels] = {0.0f};
        expectInt("no goals configured stays level 0", levelForDepth(120.0f, none, 10), 0);

        expectStr("level 0 name", levelName(0), "Warm up");
        expectStr("level 2 name", levelName(2), "Level 2");
        expectStr("level 9 name", levelName(9), "Level 9");
        expectStr("level 10 name", levelName(10), "Level 10");
        expectNear("goal of level 3", goalForLevel(3, lows, 5), 80.0f);
        expectNear("goal of level 5", goalForLevel(5, lows, 5), 120.0f);
        expectNear("goal of free play is 0", goalForLevel(0, lows, 5), 0.0f);
        expectNear("a level past the list has no goal", goalForLevel(6, lows, 5), 0.0f);

        /* The band is the point of this: level 1 counts from 40 to 60 and no further. */
        const LevelBand band = bandForLevel(1, lows, highs, 5, 10.0f);
        expectNear("the band starts at the level", band.low, 40.0f);
        expectNear("the band ends at the upper limit", band.high, 60.0f);
        expectTrue("just below the level is out", !inBand(39.9f, band));
        expectTrue("the level itself is in", inBand(40.0f, band));
        expectTrue("the middle of the band is in", inBand(50.0f, band));
        expectTrue("the upper limit itself is in", inBand(60.0f, band));
        expectTrue("past the upper limit is OUT, which is the whole point", !inBand(60.1f, band));
        expectTrue("much deeper is out too", !inBand(120.0f, band));

        expectNear("progress at the bottom of the band", bandProgress(40.0f, band), 0.0f);
        expectNear("progress in the middle", bandProgress(50.0f, band), 0.5f);
        expectNear("progress at the top", bandProgress(60.0f, band), 1.0f);
        expectNear("progress past the top is clamped", bandProgress(90.0f, band), 1.0f);

        /* A band whose upper limit is below its level would be impossible to complete, so
         * the band builder refuses it and the free-play level has no band at all. */
        const float sillyHigh[kMaxLevels] = {10.0f, 80.0f, 100.0f, 120.0f, 140.0f};
        expectNear("an upper limit under the level is lifted to the level",
                   bandForLevel(1, lows, sillyHigh, 5, 0.0f).high, 40.0f);
        expectTrue("free play has no band, so nothing counts",
                   !inBand(50.0f, bandForLevel(0, lows, highs, 5, 10.0f)));
        expectTrue("a level past the list has no band either",
                   !inBand(50.0f, bandForLevel(6, lows, highs, 5, 10.0f)));

        /* A level saved before the upper limits existed has its limit equal to its depth.
         * With a minimum width that becomes a target that can actually be held. */
        const float flatHigh[kMaxLevels] = {40.0f, 60.0f, 80.0f, 100.0f, 120.0f};
        const LevelBand widened = bandForLevel(1, lows, flatHigh, 5, 10.0f);
        expectNear("a flat band is widened to the minimum", widened.high, 50.0f);
        expectTrue("and a depth in the widened part counts", inBand(45.0f, widened));
        expectTrue("while a depth past it still does not", !inBand(55.0f, widened));

        /* The walk through the levels, both ways. */
        expectInt("climbing from 1 is 2", nextLevel(1, 5, false), 2);
        expectInt("climbing from the last level ends the run", nextLevel(5, 5, false), 0);
        expectInt("with ten levels climbing goes to 10", nextLevel(9, 10, false), 10);
        expectInt("counting down from 3 is 2", nextLevel(3, 5, true), 2);
        expectInt("counting down from 1 ends the run", nextLevel(1, 5, true), 0);
        expectInt("a run starts at the first level", firstLevel(5, false), 1);
        expectInt("or at the last one when counting down", firstLevel(5, true), 5);
        expectInt("with seven levels, counting down starts at 7", firstLevel(7, true), 7);

        /* A whole run, climbed: 1,2,3 then done. */
        int walked = firstLevel(3, false), steps = 0;
        while (walked > 0 && steps < 20) { walked = nextLevel(walked, 3, false); steps++; }
        expectInt("climbing three levels takes three steps", steps, 3);
        int down = firstLevel(4, true), downSteps = 0;
        while (down > 0 && downSteps < 20) { down = nextLevel(down, 4, true); downSteps++; }
        expectInt("counting down four levels takes four steps", downSteps, 4);
    }

    /* ---- filter ---------------------------------------------------------- */
    {
        EmaFilter filter;
        filter.reset();
        expectNear("first sample is passed through", filter.push(100.0f, 0.1f, 0.2f), 100.0f);
        // One time constant later the value is 1 - 1/e = 63.2 % of the way.
        filter.seed(0.0f);
        expectNear("EMA after one tau", filter.push(100.0f, 0.2f, 0.2f), 63.21f, 0.05f);
        filter.seed(0.0f);
        expectNear("tau 0 passes through", filter.push(100.0f, 0.2f, 0.0f), 100.0f);
        filter.seed(0.0f);
        expectNear("a long dt snaps to the sample", filter.push(100.0f, 5.0f, 0.2f), 100.0f, 0.01f);
        filter.reset();
        expectTrue("reset clears validity", !filter.valid());
    }

    /* ---- holds ----------------------------------------------------------- */
    {
        HoldTracker hold;
        hold.reset(3.0f);
        hold.update(true, 1.0f);
        expectNear("hold progress after 1 s of 3", hold.progress(), 1.0f / 3.0f);
        expectTrue("hold not complete at 1 s", !hold.complete());
        hold.update(true, 1.0f);
        hold.update(true, 1.0f);
        expectTrue("hold complete at 3 s", hold.complete());
        expectNear("progress clamps at 1", hold.progress(), 1.0f);
        hold.acknowledge();
        expectTrue("acknowledged hold is not complete again", !hold.complete());
        expectNear("acknowledge restarts the hold", hold.progress(), 0.0f);
        hold.update(true, 2.0f);
        hold.update(false, 0.5f);
        expectNear("a drop out of the zone resets the hold", hold.progress(), 0.0f);
        expectTrue("outside the zone is not in zone", !hold.inZone());
        hold.reset(0.0f);
        hold.update(true, 0.0f);
        expectTrue("hold of zero seconds completes at once", hold.complete());
    }

    /* ---- session --------------------------------------------------------- */
    {
        Session session;
        expectTrue("a new session is not active", !session.active());
        session.begin();
        expectTrue("session is active after begin", session.active());
        session.addSample(50.0f, 1.0f, 70.0f);
        session.addSample(80.0f, 1.0f, 70.0f);
        session.addSample(20.0f, 2.0f, 70.0f);
        const SessionSummary &summary = session.summary();
        expectNear("elapsed time", summary.elapsedSeconds, 4.0f);
        expectNear("peak depth is kept", summary.peakMm, 80.0f);
        expectNear("time weighted average", summary.averageMm(), 42.5f);
        expectNear("deep time counts only the deep samples", summary.deepSeconds, 1.0f);
        expectInt("sample count", (int)summary.samples, 3);
        session.countHold(3.2f);
        session.countHold(2.1f);
        expectInt("holds are counted", (int)session.summary().holds, 2);
        expectNear("longest hold is kept", session.summary().longestHoldSeconds, 3.2f);
        session.addSample(90.0f, 0.0f, 70.0f);
        expectInt("a zero interval is ignored", (int)session.summary().samples, 3);
        session.end();
        expectTrue("session is not active after end", !session.active());
        session.clear();
        expectInt("clear empties the summary", (int)session.summary().samples, 0);
    }

    /* ---- rep mode --------------------------------------------------------- */
    {
        RepCount reps;
        reps.reset(0);
        expectTrue("a fresh set is armed", reps.armed);
        expectTrue("a completed hold counts as a rep", reps.countRep(0));
        expectInt("one rep after one hold", (int)reps.count, 1);
        expectTrue("the next rep waits for a release", !reps.armed);
        expectTrue("holding on does not count again", !reps.countRep(0));
        expectInt("still one rep", (int)reps.count, 1);
        expectTrue("a return to the start arms it", reps.release(3.0f, 8.0f));
        expectTrue("a release only happens once", !reps.release(2.0f, 8.0f));
        expectTrue("a deep depth does not arm it", !reps.release(60.0f, 8.0f) && reps.armed);
        expectTrue("the next hold counts", reps.countRep(0));
        expectInt("two reps", (int)reps.count, 2);

        RepCount set;
        set.reset(3);
        expectInt("three left at the start", (int)set.remaining(3), 3);
        set.countRep(3);
        set.release(1.0f, 8.0f);
        set.countRep(3);
        set.release(1.0f, 8.0f);
        expectInt("one left after two reps", (int)set.remaining(3), 1);
        expectTrue("the target is not reached yet", !set.targetReached);
        set.countRep(3);
        expectTrue("the third rep reaches the target", set.targetReached);
        expectInt("nothing left", (int)set.remaining(3), 0);
        expectTrue("a rep while unarmed is ignored", !set.countRep(3));
        expectInt("the count stays at the target", (int)set.count, 3);

        RepCount edge;
        edge.reset(0);
        edge.countRep(0);
        expectTrue("exactly at the threshold counts as released", edge.release(8.0f, 8.0f));
        edge.reset(2);
        expectTrue("a fresh set is never already done", !edge.targetReached);
    }

    /* ---- sensor sanity --------------------------------------------------- */
    expectTrue("150 mm is usable", rangeIsUsable(150.0f));
    expectTrue("0 mm is not usable", !rangeIsUsable(0.0f));
    expectTrue("nan is not usable", !rangeIsUsable(std::nanf("")));
    expectTrue("255 mm is out of range", !rangeIsUsable(255.0f));

    /* ---- panel effects --------------------------------------------------- */
    {
        expectTrue("no effect before one is started", !fxActive(1000, 0, 2600));
        expectTrue("an effect runs at its start", fxActive(1000, 1000, 2600));
        expectTrue("an effect runs in its middle", fxActive(2000, 1000, 2600));
        expectTrue("an effect stops at its end", !fxActive(3600, 1000, 2600));
        expectTrue("an effect stops after its end", !fxActive(9000, 1000, 2600));
        expectTrue("a zero duration never runs", !fxActive(1000, 1000, 0));

        expectNear("progress starts at zero", fxProgress(1000, 1000, 2000), 0.0f, 0.001f);
        expectNear("progress is half way at half time", fxProgress(2000, 1000, 2000), 0.5f, 0.001f);
        expectNear("progress is full at the end", fxProgress(3000, 1000, 2000), 1.0f, 0.001f);
        expectNear("progress is clamped past the end", fxProgress(90000, 1000, 2000), 1.0f, 0.001f);

        /* The burst: the first ring leaves the middle at once, the later ones wait their
         * turn, and no ring ever shrinks - that is what makes it read as a burst. */
        expectNear("ring 0 starts at the middle", (float)fxRingRadius(0, 0.0f, 200), 0.0f, 0.5f);
        expectTrue("ring 0 has not started at exactly the start", !fxRingStarted(0, 0.0f));
        expectTrue("ring 0 is under way almost at once", fxRingStarted(0, 0.01f));
        expectNear("ring 0 reaches the rim", (float)fxRingRadius(0, 0.6f, 200), 200.0f, 0.5f);
        expectNear("ring 1 waits its turn", (float)fxRingRadius(1, 0.0f, 200), 0.0f, 0.5f);
        expectTrue("ring 1 has not started yet", !fxRingStarted(1, 0.0f));
        expectTrue("ring 2 is still inside when ring 0 is out",
                   fxRingRadius(2, 0.6f, 200) < fxRingRadius(0, 0.6f, 200));
        expectNear("ring 2 reaches the rim by the end", (float)fxRingRadius(2, 1.0f, 200), 200.0f, 0.5f);
        expectNear("ring 3 is the rim at the end too", (float)fxRingRadius(3, 1.0f, 200), 200.0f, 0.5f);

        bool monotonic = true, allStarted = true;
        for (float p = 0.0f; p <= 1.0f; p += 0.05f)
        {
            for (uint8_t ring = 0; ring < 4; ring++)
            {
                if (fxRingRadius(ring, p + 0.05f, 200) < fxRingRadius(ring, p, 200))
                {
                    monotonic = false;
                }
                if (p > 0.5f && !fxRingStarted(ring, p))
                {
                    allStarted = false;
                }
            }
        }
        expectTrue("a ring never shrinks", monotonic);
        expectTrue("every ring is under way by half time", allStarted);

        expectTrue("the pulse is off in the first period", !fxPulseOn(100, 480));
        expectTrue("the pulse is on in the second one", fxPulseOn(600, 480));
        expectTrue("a zero period stays on", fxPulseOn(1234, 0));
    }

    /* ---- the panel menu: play mode, level, and where a finger lands ------- */
    expectInt("the mode steps from hold to reps", nextPlayMode(kPlayModeHold), kPlayModeReps);
    expectInt("and wraps back to hold", nextPlayMode(kPlayModeReps), kPlayModeHold);
    expectInt("a mode below the range clamps to hold", clampPlayMode(-3), kPlayModeHold);
    expectInt("a mode above the range clamps", clampPlayMode(9), kPlayModeCount - 1);
    expectStr("the hold mode is named", playModeName(kPlayModeHold), "HOLD GOAL");
    expectStr("the rep mode is named", playModeName(kPlayModeReps), "COUNT REPS");

    /* Every row is reachable, and the gaps between them are misses rather than a
     * neighbouring row's business. */
    expectInt("the first row is hit at its middle", menuRowAt(menuRowTop(kMenuRowMode) + kMenuRowHeight / 2.0f), kMenuRowMode);
    expectInt("the level row is its own row", menuRowAt(menuRowTop(kMenuRowLevel) + 1.0f), kMenuRowLevel);
    expectInt("the close row is the last one", menuRowAt(menuRowTop(kMenuRowClose) + kMenuRowHeight - 1.0f), kMenuRowClose);
    expectInt("above every row is a miss", menuRowAt(menuRowTop(kMenuRowMode) - 1.0f), -1);
    expectInt("the gap between two rows is a miss", menuRowAt(menuRowTop(kMenuRowMode) + kMenuRowHeight + 1.0f), -1);
    expectInt("below every row is a miss", menuRowAt(menuRowTop(kMenuRowClose) + kMenuRowHeight + 1.0f), -1);

    /* Geometry guards: everything the menu draws has to land inside the visible round
     * window, and the chip has to sit clear of the hold bar it is drawn under. */
    const float visibleRadius = 228.0f;
    expectTrue("the title is inside the round window", (kMenuRowTopY + 20.0f) > -visibleRadius);
    expectTrue("the last row is inside the round window", (menuRowTop(kMenuRowCount - 1) + kMenuRowHeight) < visibleRadius);
    expectTrue("the hint line is inside the round window", (menuRowTop(kMenuRowCount) + 30.0f) < visibleRadius);
    expectTrue("the chip is inside the round window", (kMenuButtonCentreY + kMenuButtonHalfHeight) < visibleRadius);

    /* The chip and the footer must stay in separate bands, at the panel centre this project
     * ships (231, from TrainerSettingsService::panel_centre_y). The footer erases a box
     * around itself before drawing, and a chip inside that band is wiped off the panel. */
    {
        const float centreY = 231.0f;
        const float chipBottom = centreY + kMenuButtonCentreY + kMenuButtonHalfHeight;
        const float footerTop = (float)kPanelFooterCy - (float)kPanelFooterBandHalf;
        expectTrue("the footer band is below the menu chip", chipBottom < footerTop);
        expectTrue("the chip is above the footer it would otherwise erase",
                   (centreY + kMenuButtonCentreY + kMenuButtonHalfHeight + 4.0f) <= (float)kPanelFooterCy);
        expectTrue("the footer is still inside the round window",
                   ((float)kPanelFooterCy + 10.0f - centreY) < visibleRadius);
    }

    /* The chip: a tap on it opens the menu, a tap beside it starts a session instead. The
     * accepted area is deliberately larger than the drawing. */
    expectTrue("the middle of the chip is a hit", menuButtonHit(0.0f, kMenuButtonCentreY));
    expectTrue("the outer edge of the accepted area is a hit",
               menuButtonHit(kMenuButtonTouchHalfWidth - 0.5f, kMenuButtonCentreY));
    expectTrue("beside the accepted area is a miss",
               !menuButtonHit(kMenuButtonTouchHalfWidth + 2.0f, kMenuButtonCentreY));
    expectTrue("above the accepted area is a miss",
               !menuButtonHit(0.0f, kMenuButtonCentreY - kMenuButtonTouchHalfHeight - 2.0f));
    expectTrue("the accepted area is larger than the drawing",
               kMenuButtonTouchHalfWidth > kMenuButtonHalfWidth &&
               kMenuButtonTouchHalfHeight > kMenuButtonHalfHeight);
    expectTrue("the drawn chip still fits inside the accepted area",
               kMenuButtonHalfWidth < kMenuButtonTouchHalfWidth &&
               kMenuButtonHalfHeight < kMenuButtonTouchHalfHeight);
    /* The battery sits beside the chip and outside its touch target, so tapping the battery
     * is not tapping the button. */
    expectTrue("the battery is left of the chip",
               kPanelBatteryCentreDx < -kMenuButtonHalfWidth);
    expectTrue("the battery is outside the chip's touch target",
               (kPanelBatteryCentreDx + 15.0f) < -kMenuButtonTouchHalfWidth);

    /* The level dial: free play, then 1..count, and it turns both ways. */
    expectInt("from free play, forward is level 1", menuLevelStep(0, 5, 1), 1);
    expectInt("forward walks up the levels", menuLevelStep(3, 5, 1), 4);
    expectInt("the last level wraps round to free play", menuLevelStep(5, 5, 1), 0);
    expectInt("from free play, back is the last level", menuLevelStep(0, 5, -1), 5);
    expectInt("back from level 1 is free play", menuLevelStep(1, 5, -1), 0);
    expectInt("a level the game no longer has reads as free play", menuLevelStep(9, 5, 1), 1);
    expectInt("with no levels at all the dial stays at free play", menuLevelStep(0, 0, 1), 0);
    expectInt("with ten levels it reaches level 10", menuLevelStep(9, 10, 1), 10);
    expectInt("one level climbs and then wraps", menuLevelStep(1, 1, 1), 0);

    /* A row's left third steps back, the rest steps forward. */
    expectInt("the left edge of a row goes back", menuStepDirection(-kMenuRowHalfWidth + 1.0f), -1);
    expectInt("the middle of a row goes forward", menuStepDirection(0.0f), 1);
    expectInt("the right edge of a row goes forward", menuStepDirection(kMenuRowHalfWidth - 1.0f), 1);

    /* The lock, which is the whole point of the feature. */
    expectTrue("the menu may be used while nothing plays", menuUsable(false));
    expectTrue("the menu refuses while a play runs", !menuUsable(true));

    {
        char label[16];
        menuLevelLabel(0, 5, label, sizeof(label));
        expectStr("free play is labelled", label, "FREE");
        menuLevelLabel(3, 5, label, sizeof(label));
        expectStr("a level is numbered", label, "3");
        menuLevelLabel(9, 5, label, sizeof(label));
        expectStr("a level out of range reads as free play", label, "FREE");
    }

    /* ---- the calibration guide: two points, and the scale they make ---------- */
    {
        float travel = 0.0f;
        expectTrue("a real travel is accepted", calibrationTravel(150.0f, 12.0f, &travel));
        expectNear("the travel is the difference between the two points", travel, 138.0f);
        expectTrue("no movement at all is refused", !calibrationTravel(150.0f, 150.0f, &travel));
        expectTrue("a head that moved the wrong way is refused", !calibrationTravel(150.0f, 190.0f, &travel));
        expectNear("the refused travel is still reported", travel, -40.0f);
        expectTrue("a travel below the minimum is refused", !calibrationTravel(150.0f, 145.0f, &travel));
        expectTrue("a travel beyond the panel's scale is refused", !calibrationTravel(300.0f, 10.0f, &travel));
        expectTrue("a missing reading is refused", !calibrationTravel(-1.0f, 20.0f, &travel));
        expectTrue("exactly the minimum travel is accepted", calibrationTravel(100.0f, 90.0f, &travel));

        float samples[kCalibrationSamples] = {150.0f, 151.0f, 149.5f, 150.5f, 150.0f, 149.0f, 151.0f, 150.0f};
        expectTrue("a still reading is stable", readingIsStable(samples, kCalibrationSamples));
        expectNear("the average of the window", readingAverage(samples, kCalibrationSamples), 150.125f);
        samples[3] = 170.0f;
        expectTrue("a moving reading is not stable", !readingIsStable(samples, kCalibrationSamples));
        expectTrue("a short window is not stable", !readingIsStable(samples, 3));

        /* Four rows now, and the last one still has to be reachable and inside the window. */
        expectInt("the guide is the third row", kMenuRowCalibrate, 2);
        expectInt("CLOSE moved to the fourth row", kMenuRowClose, 3);
        expectInt("there are four rows to draw", kMenuRowCount, 4);
        expectInt("the last row is reachable", menuRowAt(menuRowTop(kMenuRowCount - 1) + 1.0f), kMenuRowCount - 1);
        expectInt("just below the last row is still a miss",
                  menuRowAt(menuRowTop(kMenuRowCount - 1) + kMenuRowHeight + 2.0f), -1);
    }

    /* ---- the ladder builder: bands of one width, laid end to end ------------ */
    {
        float lows[kMaxLevels];
        float highs[kMaxLevels];
        int count = ladderBuild(0.0f, 20.0f, 137.0f, lows, highs);
        expectInt("a 137 mm travel takes six bands of 20 mm", count, 6);
        expectNear("the first band starts at the base", lows[0], 0.0f);
        expectNear("the first band is one width wide", highs[0], 20.0f);
        expectNear("bands are adjacent, not spaced", lows[1], 20.0f);
        expectNear("level 6 ends where six widths reach", highs[5], 120.0f);
        expectNear("level 7 is not used", highs[6], 0.0f);

        expectInt("a width that divides the travel exactly", ladderBuild(0.0f, 20.0f, 120.0f, lows, highs), 6);
        expectInt("a tenth band is allowed", ladderBuild(0.0f, 12.0f, 120.0f, lows, highs), 10);
        expectInt("no more than ten, however narrow the bands", ladderBuild(0.0f, 5.0f, 500.0f, lows, highs), kMaxLevels);
        expectInt("a base offsets the whole ladder", ladderBuild(30.0f, 20.0f, 137.0f, lows, highs), 5);
        expectNear("and the offset ladder starts at the base", lows[0], 30.0f);
        expectInt("a width wider than the travel still yields one band", ladderBuild(0.0f, 200.0f, 137.0f, lows, highs), 1);
        expectNear("that one band is clipped to the travel", highs[0], 137.0f);
        expectInt("a base beyond the travel yields one clipped band", ladderBuild(200.0f, 20.0f, 137.0f, lows, highs), 1);
        expectInt("a zero width does not divide by zero", ladderBuild(0.0f, 0.0f, 137.0f, lows, highs), 10);
        expectInt("exactly the minimum travel", ladderBuild(0.0f, 10.0f, 10.0f, lows, highs), 1);
        expectNear("the coverage of six bands of 20 mm", ladderCoverage(6, 0.0f, 20.0f, 137.0f), 120.0f);
        expectNear("coverage never exceeds the travel", ladderCoverage(10, 0.0f, 50.0f, 137.0f), 137.0f);

        /* The Bands slider: the operator asks for a number, the travel has the last word. */
        expectInt("asking for three bands gets three", ladderBuild(0.0f, 20.0f, 137.0f, lows, highs, 3), 3);
        expectNear("and the third is where three widths end", highs[2], 60.0f);
        expectNear("while the fourth stays unused", highs[3], 0.0f);
        expectInt("asking for one band gets one", ladderBuild(0.0f, 20.0f, 137.0f, lows, highs, 1), 1);
        expectInt("asking for nine when eight fit gets eight", ladderBuild(0.0f, 20.0f, 137.0f, lows, highs, 9), 6);
        expectInt("asking for more than the panel holds is capped", ladderBuild(0.0f, 5.0f, 500.0f, lows, highs, 40), kMaxLevels);
        expectInt("zero means as many as fit", ladderBuild(0.0f, 20.0f, 137.0f, lows, highs, 0), 6);
        expectInt("and so does a negative request", ladderBuild(0.0f, 20.0f, 137.0f, lows, highs, -3), 6);
    }

    /* ---- colours: the web sends "#rrggbb", the panel wants RGB565 ---------------- */
    {
        uint16_t out = 0;
        char text[16];
        expectInt("green parses", parseHexColour("#00ff00", &out, 0) ? 1 : 0, 1);
        expectInt("and lands in the middle field", (int)out, 0x07E0);
        expectInt("white parses", parseHexColour("#ffffff", &out, 0) ? 1 : 0, 1);
        expectInt("and is full scale everywhere", (int)out, 0xFFFF);
        expectInt("black parses", parseHexColour("#000000", &out, 0x1234) ? 1 : 0, 1);
        expectInt("and is zero", (int)out, 0);
        expectInt("upper case parses too", parseHexColour("#0A0B0C", &out, 0) ? 1 : 0, 1);
        expectInt("a short string is refused", parseHexColour("#abc", &out, 0x1234) ? 1 : 0, 0);
        expectInt("and leaves the fallback", (int)out, 0x1234);
        expectInt("a missing hash is refused", parseHexColour("00ff00", &out, 0) ? 1 : 0, 0);
        expectInt("a null is refused", parseHexColour(nullptr, &out, 0) ? 1 : 0, 0);
        expectInt("a non hex digit is refused", parseHexColour("#00ff0g", &out, 0) ? 1 : 0, 0);
        expectInt("a trailing character is refused", parseHexColour("#00ff00 ", &out, 0) ? 1 : 0, 0);

        formatHexColour(0x07E0, text, sizeof(text));
        expectInt("green formats back", strcmp(text, "#00ff00") == 0 ? 1 : 0, 1);
        formatHexColour(0xFFFF, text, sizeof(text));
        expectInt("white formats back", strcmp(text, "#ffffff") == 0 ? 1 : 0, 1);
        formatHexColour(0x0000, text, sizeof(text));
        expectInt("black formats back", strcmp(text, "#000000") == 0 ? 1 : 0, 1);
        formatHexColour(0xFD20, text, sizeof(text));
        parseHexColour(text, &out, 0);
        expectInt("orange survives a round trip", (int)out, 0xFD20);
    }

    /* ---- the rest between reps -------------------------------------------------- */
    {
        RepBreak rest;
        expectInt("nothing runs before a rep", rest.seconds(1000), 0);
        expectInt("and no break starts with no reps done", rest.update(true, true, 0, 1000) ? 1 : 0, 0);
        expectInt("standing at the start is not a break", rest.running ? 1 : 0, 0);

        expectInt("a break starts once a rep is done and we are back out", rest.update(true, true, 1, 5000) ? 1 : 0, 1);
        expectInt("and it is running", rest.running ? 1 : 0, 1);
        expectInt("immediately it reads zero", rest.seconds(5000), 0);
        expectInt("after one second it reads one", rest.seconds(6000), 1);
        expectInt("and it keeps counting", rest.seconds(12500), 7);
        expectInt("no second start while it runs", rest.update(true, true, 1, 7000) ? 1 : 0, 0);
        expectInt("and the clock is not restarted", rest.seconds(9000), 4);

        expectInt("the next rep ends it", rest.update(false, true, 2, 10000) ? 1 : 0, 0);
        expectInt("so it reads zero again", rest.seconds(10000), 0);
        expectInt("and stays stopped", rest.seconds(30000), 0);

        expectInt("a later arming starts a fresh break", rest.update(true, true, 2, 40000) ? 1 : 0, 1);
        expectInt("which counts from there", rest.seconds(43000), 3);

        expectInt("ending the session stops it", rest.update(true, false, 2, 44000) ? 1 : 0, 0);
        expectInt("and reads zero", rest.seconds(45000), 0);

        expectInt("a long absence is capped", rest.update(true, true, 3, 100000) ? 1 : 0, 1);
        expectInt("at ten minutes", rest.seconds(100000 + 900000), 600);
    }

    std::printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
