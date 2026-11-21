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
    expectInt("below level 1", levelForDepth(10.0f, 40.0f, 70.0f, 100.0f), 0);
    expectInt("exactly level 1", levelForDepth(40.0f, 40.0f, 70.0f, 100.0f), 1);
    expectInt("level 2", levelForDepth(85.0f, 40.0f, 70.0f, 100.0f), 2);
    expectInt("level 3", levelForDepth(120.0f, 40.0f, 70.0f, 100.0f), 3);
    expectInt("no goals configured stays level 0", levelForDepth(120.0f, 0.0f, 0.0f, 0.0f), 0);
    expectStr("level 0 name", levelName(0), "Warm up");
    expectStr("level 2 name", levelName(2), "Level 2");
    expectNear("goal of level 3", goalForLevel(3, 40.0f, 70.0f, 100.0f), 100.0f);
    expectNear("goal of free play is 0", goalForLevel(0, 40.0f, 70.0f, 100.0f), 0.0f);

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

    /* ---- sensor sanity --------------------------------------------------- */
    expectTrue("150 mm is usable", rangeIsUsable(150.0f));
    expectTrue("0 mm is not usable", !rangeIsUsable(0.0f));
    expectTrue("nan is not usable", !rangeIsUsable(std::nanf("")));
    expectTrue("255 mm is out of range", !rangeIsUsable(255.0f));

    std::printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
