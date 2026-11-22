#pragma once

/*
 *   Round trainer display on the LilyGo T-RGB 2.1 inch (ST7701S over the
 *   ESP32-S3 RGB bus, 480x480).
 *
 *   The whole panel init runs as 3-wire SPI through the XL9535 I2C expander,
 *   which is what Arduino_GFX's Arduino_XL9535SWSPI data bus does. Wire must be
 *   started before the panel, because that data bus speaks over I2C.
 *
 *   Drawing is incremental on purpose: the panel has one framebuffer in PSRAM
 *   with a bounce buffer, and a full screen repaint at 10 Hz is visible as
 *   flicker. render() only touches what changed since the previous frame, and a
 *   value that is unchanged costs nothing.
 *
 *   A view of what to draw is passed in, so the display never reads the state
 *   service itself and never blocks on the network.
 */

#include <Arduino.h>

#include <Arduino_GFX_Library.h>

#include "TrainerConfig.h"
#include "trainer_math.h"

struct TrainerScreen
{
    float depthMm = 0.0f;
    float peakMm = 0.0f;
    float maxDepthMm = 120.0f;
    float gauge = 0.0f;     /* depth / maxDepth, 0..1 */
    float peakGauge = 0.0f; /* peak / maxDepth, 0..1 */

    int level = 0;      /* level the depth is at now, 0..5 */
    int goalLevel = 0;  /* goal the user picked, 0 = free play */
    float goalMm = 0.0f;
    float goalLimitMm = 0.0f; /* the top of the goal's band: above this it is not the goal */

    float holdProgress = 0.0f; /* 0..1 of the hold at the goal */
    float holdSeconds = 0.0f;
    float holdNeeded = 0.0f;
    bool inZone = false;
    bool holdComplete = false;

    bool sessionActive = false;
    float sessionSeconds = 0.0f;
    float sessionAvgMm = 0.0f;
    uint32_t sessionHolds = 0;

    /* Rep mode: the count takes the place of the level name on the panel. */
    bool repMode = false;
    uint16_t repCount = 0;
    uint16_t repTarget = 0; /* 0 = count up */

    /* A panel animation in flight: 0 none, 1 a rep counted, 2 the set is finished. The
     * start time is the device clock, so the display ends it by itself and the normal
     * layout returns - even if the client that asked for it is long gone. */
    uint8_t fx = 0;
    uint32_t fxStartMs = 0;
    uint16_t fxCount = 0;
    char fxText[24] = "";

    bool sensorOk = false;
    bool batteryKnown = false;
    float batteryV = 0.0f;

    /* The panel menu: play mode and level, picked with a finger. The menu is only open
     * when no session runs, so it never competes with a running set for the glass. */
    bool menuOpen = false;
    bool menuLocked = false; /* a session runs: the button refuses and says so */
    uint8_t menuMode = 0;
    int menuLevel = 0;    /* 0 = free play */
    uint8_t levelCount = 5;
    char menuNotice[28] = "";  /* shown for a couple of seconds after a refused tap */
    uint32_t menuNoticeMs = 0;

    /* The calibration guide, when it is running. It takes the panel from the menu, and it needs
     * the sensor awake: the point of it is to record what the sensor reads at each end. */
    uint8_t calStep = 0;      /* 0 off, else trainer::kCalStepZero/Full/Done */
    float calRangeMm = -1.0f; /* the live reading, so the operator can see it settle */
    float calZeroMm = -1.0f;  /* captured at the 0 position */
    float calFullMm = -1.0f;  /* captured at the full position */
    bool calStable = false;   /* true when the reading has stopped moving */

    /* One line at the bottom: network and sensor. A plain char array, because
     * this struct is copied between the trainer task and the display task and
     * must stay a POD (see the snapshot lock in main.cpp). */
    char footer[40] = "booting";
    /* Seconds of the rest between reps, 0 when there is none. Shown on the unit line, which is
     * the one piece of the panel a break has room for. */
    uint16_t repBreakS = 0;
    uint8_t unit = 0; /* 0 mm, 1 cm, 2 in */
};

class TrainerDisplay
{
public:
    /* Builds the panel and the backlight. Returns false when the expander or
     * the panel did not answer; the firmware then runs headless. */
    bool begin();

    /* Read-only access to the panel's own framebuffer, for the /rest/frame crop dump. It exists
     * because a seam in the ring's drawing cannot be settled by reading the library's scanline
     * fill - it has to be looked at. Nothing may write through this pointer: the panel scans it
     * out continuously. */
    uint16_t *frameBuffer();

    bool present() const { return _present; }

    void setBrightness(uint8_t percent);
    uint8_t brightness() const { return _brightness; }

    /* The palette, from the settings. A change wipes the panel and repaints it in the new
     * colours on the next render(). */
    void setColours(const TrainerColours &colours);

    /* Draws the frame. Only the parts that changed since the previous call are
     * painted. */
    void render(const TrainerScreen &screen);

    /* Repaints everything on the next render() call. Needed after a unit change
     * or a session reset, when the whole layout shifts. */
    void invalidate() { _valid = false; }

    /* Where the centre of the visible round window sits inside the 480x480 framebuffer.
     * It is not always (240, 240): the window is glued on by hand, and a window 15 px off
     * is obvious on a ring this close to the edge. A change repaints the whole panel. */
    static void setCentre(int16_t x, int16_t y);

    /* The XL9535 expander bus, so the touch driver can reset its controller
     * through the same object instead of building a second one. */
    Arduino_XL9535SWSPI *bus() const { return _bus; }

    uint32_t frames() const { return _frames; }

private:
    void drawStatic();
    void drawGauge(float gauge, bool inZone);
    void drawTicks(float goalGauge, float limitGauge, float peakGauge);
    void drawReadout(const TrainerScreen &screen);
    void drawUnit(const TrainerScreen &screen);
    void drawLevelLine(const TrainerScreen &screen);
    void drawSessionLine(const TrainerScreen &screen);
    void drawHoldBar(const TrainerScreen &screen);
    void drawBattery(bool known, float volts);
    void drawFooter(const TrainerScreen &screen);

    /* A full-panel animation: a rep counted, or a finished set. The trainer task starts
     * it and it ends by itself after its duration, so the normal layout always returns. */
    void drawEffect(const TrainerScreen &screen, uint32_t nowMs, float progress);

    void eraseBox(int16_t cx, int16_t cy, int16_t halfWidth, int16_t halfHeight);
    void drawCentered(const GFXfont *font, uint8_t scale, int16_t cx, int16_t cy, const char *text, uint16_t color);
    /* The same call, but placed by its left edge: the menu's labels sit on one side and
     * its values on the other. */
    void drawLeftAligned(const GFXfont *font, uint8_t scale, int16_t left, int16_t cy, const char *text, uint16_t color);
    static int16_t textWidth(const GFXfont *font, const char *text, uint8_t scale);

    /* The menu: a chip under the hold bar opens it, the rows pick a mode and a level, and
     * a long press closes it. The chip is part of the static frame, so it is drawn with
     * the panel and only repainted when its locked state changes. */
    void drawMenuButton(const TrainerScreen &screen);
    void drawMenu(const TrainerScreen &screen, uint32_t nowMs);
    void drawMenuRow(int row, const char *label, const char *value, uint16_t valueColor);
    void drawMenuNotice(const TrainerScreen &screen, uint32_t nowMs);

    /* The calibration guide: a page of its own with one instruction at a time and the live
     * sensor reading, so the operator can see it stop moving before capturing it. */
    void drawCalibration(const TrainerScreen &screen);
    bool _calDrawn = false;
    uint8_t _calDrawnStep = 0xFF;
    char _calDrawnLive[24] = "";
    char _calDrawnNote[32] = "";

    Arduino_XL9535SWSPI *_bus = nullptr;
    Arduino_ESP32RGBPanel *_panel = nullptr;
    Arduino_RGB_Display *_gfx = nullptr;

    bool _present = false;
    bool _valid = false;
    uint8_t _brightness = 70;

    float _drawnGauge = 0.0f;
    float _drawnGoalGauge = -1.0f;
    float _drawnLimitGauge = -1.0f;
    float _drawnPeakGauge = -1.0f;
    bool _drawnInZone = false;
    uint32_t _frames = 0;

    /* Text caches: only a real change repaints a text box. */
    String _lastReadout;
    String _lastUnit;
    String _lastLevelLine;
    String _lastSessionLine;
    String _lastFooter;
    String _lastPeakLine;
    int16_t _lastReadoutWidth = 0;
    int16_t _lastUnitWidth = 0;
    int16_t _lastLevelWidth = 0;
    int16_t _lastSessionWidth = 0;
    int16_t _lastFooterWidth = 0;
    int16_t _lastPeakWidth = 0;
    float _lastHoldProgress = -1.0f;
    bool _lastSessionActive = false;
    int8_t _batteryBucket = -2; /* -2 = nothing drawn, -1 = battery unknown */
    bool _fxDrawn = false;      /* an animation took the panel: it has to be repainted */

    /* The animation is drawn incrementally, like everything else on this panel: it clears
     * once at its start, and afterwards only the arc band and the words are written. A
     * full clear per frame makes the panel flash, because the framebuffer is scanned out
     * while it is written. */
    bool _fxCleared = false;
    int16_t _fxRingDrawn[TRAINER_FX_RINGS] = {-1, -1, -1};

    /* The menu's own caches: the panel is scanned out while it is written, so a menu that
     * repainted itself every frame would flash. It clears once when it opens and after
     * that only a row whose value changed is painted again. */
    bool _menuDrawn = false;
    int _menuDrawnMode = -1;
    int _menuDrawnLevel = -9999;
    int _menuDrawnCount = -1;
    int _menuButtonLocked = -1; /* -1 = nothing drawn yet */
    char _menuDrawnNotice[sizeof(((TrainerScreen *)nullptr)->menuNotice)] = "";
    int16_t _lastMenuNoticeWidth = 0;
};
