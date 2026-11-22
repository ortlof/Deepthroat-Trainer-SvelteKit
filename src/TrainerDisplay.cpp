#include "TrainerDisplay.h"

#include <math.h>

/* Vendored Adafruit GFX fonts (BSD, see src/fonts/FONT-LICENSE.txt). Arduino_GFX
 * has no fonts of its own except the classic 5x7 one, and a 5x7 readout is not
 * readable from where the trainer is used. */
#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSansBold12pt7b.h"
#include "fonts/FreeSansBold24pt7b.h"

/* ------------------------------------------------------------------ layout -- */

/* The centre of the visible round window inside the framebuffer. It is a variable and not a
 * constant, because the window is not always glued on straight: see setCentre(). Every
 * drawing call in this file goes through CX and CY, so one pair of values moves the whole
 * layout. */
static int16_t g_centreX = 240;
static int16_t g_centreY = 240;
static bool g_centreChanged = false;

#define CX g_centreX
#define CY g_centreY
#define R_OUT 212
#define R_IN 184

#define GAUGE_START 135.0f
#define GAUGE_SWEEP 270.0f
/* fillArc() treats an exact 0 degree span as a full circle and draws a half
 * ring instead, so every span keeps a hair of width. */
#define ANGLE_EPSILON 0.08f

#define TICK_GOAL_R_OUT 224
#define TICK_GOAL_R_IN 216
#define TICK_PEAK_R_OUT 234
#define TICK_PEAK_R_IN 227

#define READOUT_CY 200
#define READOUT_SCALE 2
#define UNIT_CY 262
#define LEVEL_CY 305
#define SESSION_CY 332
#define HOLD_BAR_X 165
#define HOLD_BAR_Y 349
#define HOLD_BAR_W 150
#define HOLD_BAR_H 15
/* Below the menu chip: the footer erases a box around itself before drawing, and a chip in
 * that band would be wiped off the panel every time the signal reading changes. */
#define FOOTER_CY trainer::kPanelFooterCy
#define HEADER_CY 100

/* The clear disc the animation's words live in, drawn instead of a rectangle. The rings and the
 * words share a centre, so a disc means every ring at or above this radius lies wholly outside the
 * cleared area and cannot be sliced - which is what a rectangular box did, once per frame: a
 * straight chord of black across each crossing ring, flickering as the rings grew. The radius is
 * the gauge's own inner radius, so the disc exactly fills the hole the gauge leaves and the words
 * still fit (their old 150x100 box reached only 180 at the corners). */
#define FX_CLEAR_RADIUS 182

/* Battery symbol: beside the menu button, where the controls are. It used to sit in the top
 * left, where it competed with the peak readout. */
#define BATTERY_X (CX + (int16_t)trainer::kPanelBatteryCentreDx)
#define BATTERY_CY (CY + (int16_t)trainer::kMenuButtonCentreY)
#define BATTERY_W 30

/* fillArc works in whole degrees, so the join between the ring's old segment and the new one
 * lands a pixel either side of where the value says - visible as a thin radial stripe at the
 * leading edge while the depth moves. Every segment starts this much early and every erase
 * runs this much past its end, which covers the join. */
#define GAUGE_SEAM 1.2f

/* ------------------------------------------------------------------ colours -- */

#define COL_BG 0x0000
#define COL_TRACK 0x2104
#define COL_ACCENT 0x07FF  /* cyan */
#define COL_ZONE 0x07E0    /* green: the goal is held */
#define COL_DONE 0xFFFF    /* white: hold complete */
#define COL_GOAL 0xFD20    /* orange tick: the goal of the selected level */
#define COL_PEAK 0x7BEF    /* light grey tick: the peak of the session */
#define COL_TEXT 0xFFFF
#define COL_DIM 0x9CF3
#define COL_WARN 0xF800

/* ------------------------------------------------------------------- panel -- */

/* The bounce buffer is given in pixels and must be a multiple of the panel width. */
#define TRAINER_BOUNCE_PX ((size_t)(TRAINER_TFT_WIDTH * TRAINER_TFT_BOUNCE_LINES))

static Arduino_ESP32RGBPanel *makePanel(size_t bouncePixels)
{
    return new Arduino_ESP32RGBPanel(
        TRAINER_TFT_DE, TRAINER_TFT_VSYNC, TRAINER_TFT_HSYNC, TRAINER_TFT_PCLK,
        TRAINER_TFT_R0, TRAINER_TFT_R1, TRAINER_TFT_R2, TRAINER_TFT_R3, TRAINER_TFT_R4,
        TRAINER_TFT_G0, TRAINER_TFT_G1, TRAINER_TFT_G2, TRAINER_TFT_G3, TRAINER_TFT_G4, TRAINER_TFT_G5,
        TRAINER_TFT_B0, TRAINER_TFT_B1, TRAINER_TFT_B2, TRAINER_TFT_B3, TRAINER_TFT_B4,
        TRAINER_TFT_HSYNC_POLARITY, TRAINER_TFT_HSYNC_FRONT_PORCH, TRAINER_TFT_HSYNC_PULSE_WIDTH, TRAINER_TFT_HSYNC_BACK_PORCH,
        TRAINER_TFT_VSYNC_POLARITY, TRAINER_TFT_VSYNC_FRONT_PORCH, TRAINER_TFT_VSYNC_PULSE_WIDTH, TRAINER_TFT_VSYNC_BACK_PORCH,
        TRAINER_TFT_PCLK_ACTIVE_NEG,
        TRAINER_TFT_PCLK_HZ /* prefer_speed: the vendor example value */,
        false /* useBigEndian */,
        0 /* de_idle_high */, 0 /* pclk_idle_high */,
        bouncePixels);
}

static Arduino_RGB_Display *makeDisplay(Arduino_ESP32RGBPanel *panel, Arduino_XL9535SWSPI *bus)
{
#ifdef TRAINER_PANEL_2_8_INCH
    return new Arduino_RGB_Display(TRAINER_TFT_WIDTH, TRAINER_TFT_HEIGHT, panel, 0 /* rotation */,
                                   true /* auto flush */, bus, GFX_NOT_DEFINED /* RST */,
                                   st7701_type20_init_operations, sizeof(st7701_type20_init_operations));
#else
    /* The 2.1 inch full circle board (st7701_type4). Half circle boards use
     * another sequence; see the LilyGo repository for the pin table. */
    return new Arduino_RGB_Display(TRAINER_TFT_WIDTH, TRAINER_TFT_HEIGHT, panel, 0 /* rotation */,
                                   true /* auto flush */, bus, GFX_NOT_DEFINED /* RST */,
                                   st7701_type4_init_operations, sizeof(st7701_type4_init_operations));
#endif
}

/* The panel's own scanout buffer, as the display object already holds it. Asking the panel for it
 * again would build a second panel configuration and a second framebuffer, which is why this goes
 * through the accessor the display class already exposes. */
uint16_t *TrainerDisplay::frameBuffer()
{
    if (_gfx == nullptr)
    {
        return nullptr;
    }
    return static_cast<Arduino_RGB_Display *>(_gfx)->getFramebuffer();
}

bool TrainerDisplay::begin()
{
    /* The XL9535 data bus talks over I2C, and it does not start Wire itself:
     * the bus must be up before the panel is built. */
    Wire.begin(TRAINER_I2C_SDA, TRAINER_I2C_SCL, TRAINER_I2C_SPEED);

    _bus = new Arduino_XL9535SWSPI(TRAINER_I2C_SDA, TRAINER_I2C_SCL,
                                   TRAINER_XL9535_PIN_POWER_EN, TRAINER_XL9535_PIN_CS,
                                   TRAINER_XL9535_PIN_SCLK, TRAINER_XL9535_PIN_MOSI);

    _panel = makePanel(TRAINER_BOUNCE_PX);
    _gfx = makeDisplay(_panel, _bus);
    _present = _gfx->begin();

    if (!_present && TRAINER_TFT_BOUNCE_LINES > 0)
    {
        /* A driver that refuses the bounce buffer setting would leave the panel
         * dark, and a dark panel is worse than a torn picture. Build the panel
         * again without it. Both failed objects stay allocated: freeing a half
         * built display is a bigger risk than losing a few hundred bytes. */
        Serial.println("[display] panel refused the bounce buffer, retrying without it");
        _panel = makePanel(0);
        _gfx = makeDisplay(_panel, _bus);
        _present = _gfx->begin();
    }
    if (!_present)
    {
        Serial.println("[display] panel did not start");
        return false;
    }

    _gfx->fillScreen(COL_BG);
    _gfx->setTextWrap(false);

    /* Backlight: PWM so the brightness can be set, and the panel is dark until
     * the first frame is drawn. */
    ledcAttach(TRAINER_TFT_BL, TRAINER_TFT_BL_PWM_HZ, TRAINER_TFT_BL_PWM_BITS);
    setBrightness(_brightness);

    Serial.printf("[display] %dx%d ready, bounce buffer %u lines\n", (int)_gfx->width(), (int)_gfx->height(),
                  (unsigned)TRAINER_TFT_BOUNCE_LINES);
    _valid = false;
    return true;
}

void TrainerDisplay::setBrightness(uint8_t percent)
{
    _brightness = (percent > 100) ? 100 : percent;
    if (!_present)
    {
        return;
    }
    const uint32_t steps = (1u << TRAINER_TFT_BL_PWM_BITS) - 1u;
    const uint32_t duty = (uint32_t)_brightness * steps / 100u;
    ledcWrite(TRAINER_TFT_BL, duty);
}

/* ------------------------------------------------------------------ drawing -- */

int16_t TrainerDisplay::textWidth(const GFXfont *font, const char *text, uint8_t scale)
{
    int16_t width = 0;
    const uint16_t first = pgm_read_word(&font->first);
    const uint16_t last = pgm_read_word(&font->last);
    for (const char *p = text; *p; p++)
    {
        const uint8_t c = (uint8_t)*p;
        if (c < first || c > last)
        {
            width += (int16_t)(6 * scale);
            continue;
        }
        GFXglyph *glyph = pgm_read_glyph_ptr(font, c - first);
        width += (int16_t)pgm_read_byte(&glyph->xAdvance) * scale;
    }
    return width;
}

void TrainerDisplay::eraseBox(int16_t cx, int16_t cy, int16_t halfWidth, int16_t halfHeight)
{
    _gfx->fillRect(cx - halfWidth, cy - halfHeight, halfWidth * 2, halfHeight * 2, COL_BG);
}

/* Arduino_GFX places a custom-font string with y as the BASELINE, and the
 * baseline sits 2/3 of the line height below the top of the glyph box. For a box
 * of height yAdvance * scale centred on cy the baseline is therefore
 * cy + (yAdvance * scale) / 6. */
void TrainerDisplay::drawCentered(const GFXfont *font, uint8_t scale, int16_t cx, int16_t cy, const char *text, uint16_t color)
{
    const int16_t width = textWidth(font, text, scale);
    const int16_t yAdvance = (int16_t)pgm_read_byte(&font->yAdvance) * scale;
    _gfx->setFont(font);
    _gfx->setTextSize(scale);
    _gfx->setTextColor(color, color);
    _gfx->setCursor(cx - width / 2, cy + yAdvance / 6);
    _gfx->print(text);
}

void TrainerDisplay::drawStatic()
{
    _gfx->fillScreen(COL_BG);

    /* The ring track: 270 degrees from the lower left, clockwise over the top. */
    _gfx->fillArc(CX, CY, R_OUT, R_IN, GAUGE_START, GAUGE_START + GAUGE_SWEEP - ANGLE_EPSILON, COL_TRACK);

    _gfx->fillRoundRect(HOLD_BAR_X, HOLD_BAR_Y, HOLD_BAR_W, HOLD_BAR_H, HOLD_BAR_H / 2, COL_TRACK);

    _lastReadout = "";
    _lastUnit = "";
    _lastLevelLine = "";
    _lastSessionLine = "";
    _lastFooter = "";
    _lastReadoutWidth = 0;
    _lastUnitWidth = 0;
    _lastLevelWidth = 0;
    _lastSessionWidth = 0;
    _lastFooterWidth = 0;
    _lastHoldProgress = -1.0f;
    _drawnGauge = 0.0f;
    _drawnGoalGauge = -1.0f;
    _drawnLimitGauge = -1.0f;
    _drawnPeakGauge = -1.0f;
    _drawnInZone = false;
    _lastSessionActive = false;
    _batteryBucket = -2; /* the screen is black again: the symbol has to be redrawn */
    _menuButtonLocked = -1; /* so is the menu chip, in whichever state it is now */
    _calDrawn = false;      /* and the calibration page, when it next runs */
    _calDrawnStep = 0xFF;
}

void TrainerDisplay::drawGauge(float gauge, bool inZone)
{
    const uint16_t color = inZone ? COL_ZONE : COL_ACCENT;

    if (inZone != _drawnInZone)
    {
        /* The colour changed, so the whole arc has to be repainted. */
        _gfx->fillArc(CX, CY, R_OUT, R_IN, GAUGE_START, GAUGE_START + GAUGE_SWEEP - ANGLE_EPSILON, COL_TRACK);
        _drawnGauge = 0.0f;
        _drawnInZone = inZone;
    }

    const float target = (gauge < 0.0f) ? 0.0f : ((gauge > 1.0f) ? 1.0f : gauge);
    const float targetAngle = GAUGE_START + GAUGE_SWEEP * target;

    if (target > _drawnGauge + 0.0005f)
    {
        const float fromAngle = GAUGE_START + GAUGE_SWEEP * _drawnGauge - GAUGE_SEAM;
        const float span = targetAngle - fromAngle;
        if (span > ANGLE_EPSILON)
        {
            _gfx->fillArc(CX, CY, R_OUT, R_IN, fromAngle, targetAngle, color);
        }
    }
    else if (target < _drawnGauge - 0.0005f)
    {
        /* The erase is a whole degree wide at each end, so the arc's own leading edge is
         * painted back afterwards: without that, shrinking would nibble a stripe out of the
         * value it is supposed to keep. */
        const float newAngle = GAUGE_START + GAUGE_SWEEP * target;
        const float oldAngle = GAUGE_START + GAUGE_SWEEP * _drawnGauge;
        if ((oldAngle - newAngle) > ANGLE_EPSILON)
        {
            _gfx->fillArc(CX, CY, R_OUT, R_IN, newAngle - GAUGE_SEAM, oldAngle + GAUGE_SEAM, COL_TRACK);
            if ((newAngle - GAUGE_START) > ANGLE_EPSILON)
            {
                _gfx->fillArc(CX, CY, R_OUT, R_IN, newAngle - GAUGE_SEAM, newAngle, color);
            }
        }
    }
    _drawnGauge = target;
}

void TrainerDisplay::drawTicks(float goalGauge, float limitGauge, float peakGauge)
{
    const bool goalValid = goalGauge > 0.0f && goalGauge <= 1.0f;
    if (goalValid != (_drawnGoalGauge > 0.0f) || (goalValid && fabsf(goalGauge - _drawnGoalGauge) > 0.001f))
    {
        if (_drawnGoalGauge > 0.0f)
        {
            const float oldAngle = GAUGE_START + GAUGE_SWEEP * _drawnGoalGauge;
            _gfx->fillArc(CX, CY, TICK_GOAL_R_OUT, TICK_GOAL_R_IN, oldAngle - 1.4f, oldAngle + 1.4f, COL_BG);
        }
        if (goalValid)
        {
            const float angle = GAUGE_START + GAUGE_SWEEP * goalGauge;
            _gfx->fillArc(CX, CY, TICK_GOAL_R_OUT, TICK_GOAL_R_IN, angle - 1.4f, angle + 1.4f, COL_GOAL);
        }
        _drawnGoalGauge = goalValid ? goalGauge : -1.0f;
    }

    /* The top of the goal's band. It is dimmer and thinner than the goal mark: it says where
     * the target stops, so a depth past it is visibly past the target and not a better hold. */
    const bool limitValid = limitGauge > 0.0f && limitGauge <= 1.0f;
    if (limitValid != (_drawnLimitGauge > 0.0f) || (limitValid && fabsf(limitGauge - _drawnLimitGauge) > 0.001f))
    {
        if (_drawnLimitGauge > 0.0f)
        {
            const float oldAngle = GAUGE_START + GAUGE_SWEEP * _drawnLimitGauge;
            _gfx->fillArc(CX, CY, TICK_GOAL_R_OUT, TICK_GOAL_R_IN, oldAngle - 0.8f, oldAngle + 0.8f, COL_BG);
        }
        if (limitValid)
        {
            const float angle = GAUGE_START + GAUGE_SWEEP * limitGauge;
            _gfx->fillArc(CX, CY, TICK_GOAL_R_OUT, TICK_GOAL_R_IN, angle - 0.8f, angle + 0.8f, COL_DIM);
        }
        _drawnLimitGauge = limitValid ? limitGauge : -1.0f;
    }

    const bool peakValid = peakGauge > 0.0f && peakGauge <= 1.0f;
    if (peakValid != (_drawnPeakGauge > 0.0f) || (peakValid && fabsf(peakGauge - _drawnPeakGauge) > 0.001f))
    {
        if (_drawnPeakGauge > 0.0f)
        {
            const float oldAngle = GAUGE_START + GAUGE_SWEEP * _drawnPeakGauge;
            _gfx->fillArc(CX, CY, TICK_PEAK_R_OUT, TICK_PEAK_R_IN, oldAngle - 0.9f, oldAngle + 0.9f, COL_BG);
        }
        if (peakValid)
        {
            const float angle = GAUGE_START + GAUGE_SWEEP * peakGauge;
            _gfx->fillArc(CX, CY, TICK_PEAK_R_OUT, TICK_PEAK_R_IN, angle - 0.9f, angle + 0.9f, COL_PEAK);
        }
        _drawnPeakGauge = peakValid ? peakGauge : -1.0f;
    }
}

void TrainerDisplay::drawReadout(const TrainerScreen &screen)
{
    char buffer[24];
    const float value = (screen.unit == 1) ? (screen.depthMm / 10.0f)
                                           : ((screen.unit == 2) ? (screen.depthMm / 25.4f) : screen.depthMm);
    if (screen.unit == 0)
    {
        snprintf(buffer, sizeof(buffer), "%d", (int)lroundf(value));
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "%.1f", value);
    }

    if (_lastReadout != buffer)
    {
        const int16_t halfHeight = (int16_t)pgm_read_byte(&FreeSansBold24pt7b.yAdvance) * READOUT_SCALE / 2 + 4;
        eraseBox(CX, READOUT_CY, _lastReadoutWidth / 2 + 6, halfHeight);
        _lastReadout = buffer;
        _lastReadoutWidth = textWidth(&FreeSansBold24pt7b, buffer, READOUT_SCALE);
        drawCentered(&FreeSansBold24pt7b, READOUT_SCALE, CX, READOUT_CY, buffer, COL_TEXT);
    }

    char peak[24];
    if (screen.unit == 0)
    {
        snprintf(peak, sizeof(peak), "PEAK %d", (int)lroundf(screen.peakMm));
    }
    else
    {
        snprintf(peak, sizeof(peak), "PEAK %.1f", (screen.unit == 1) ? screen.peakMm / 10.0f : screen.peakMm / 25.4f);
    }
    if (_lastPeakLine != peak)
    {
        const int16_t halfHeight = (int16_t)pgm_read_byte(&FreeSans9pt7b.yAdvance) / 2 + 3;
        eraseBox(CX, HEADER_CY, _lastPeakWidth / 2 + 6, halfHeight);
        _lastPeakLine = peak;
        _lastPeakWidth = textWidth(&FreeSans9pt7b, peak, 1);
        drawCentered(&FreeSans9pt7b, 1, CX, HEADER_CY, peak, COL_DIM);
    }
}

void TrainerDisplay::drawUnit(const TrainerScreen &screen)
{
    const char *unit = (screen.unit == 1) ? "cm" : ((screen.unit == 2) ? "in" : "mm");
    if (_lastUnit != unit)
    {
        const int16_t halfHeight = (int16_t)pgm_read_byte(&FreeSansBold12pt7b.yAdvance) / 2 + 3;
        eraseBox(CX, UNIT_CY, _lastUnitWidth / 2 + 6, halfHeight);
        _lastUnit = unit;
        _lastUnitWidth = textWidth(&FreeSansBold12pt7b, unit, 1);
        drawCentered(&FreeSansBold12pt7b, 1, CX, UNIT_CY, unit, COL_DIM);
    }
}

void TrainerDisplay::drawLevelLine(const TrainerScreen &screen)
{
    char buffer[40];
    if (screen.repMode)
    {
        /* Rep mode: the count takes the line, because the count is the game. The level goes
         * on the line as well, or a climb through the levels after each set would be
         * invisible on the panel. */
        if (screen.repTarget > 0)
        {
            snprintf(buffer, sizeof(buffer), "REP %u/%u  L%d", (unsigned)screen.repCount,
                     (unsigned)screen.repTarget, screen.goalLevel);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "REP %u  L%d", (unsigned)screen.repCount, screen.goalLevel);
        }
    }
    else if (screen.goalLevel <= 0)
    {
        snprintf(buffer, sizeof(buffer), "free play - %s", trainer::levelName(screen.level));
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "%s: %d-%d mm", trainer::levelName(screen.goalLevel),
                 (int)lroundf(screen.goalMm), (int)lroundf(screen.goalLimitMm));
    }

    if (_lastLevelLine != buffer)
    {
        uint16_t color = COL_TEXT;
        if (screen.repMode && screen.repTarget > 0 && screen.repCount >= screen.repTarget)
        {
            color = COL_DONE; /* the set is done */
        }
        else if (screen.goalLevel > 0 && screen.level >= screen.goalLevel)
        {
            color = COL_DONE;
        }
        const int16_t halfHeight = (int16_t)pgm_read_byte(&FreeSansBold12pt7b.yAdvance) / 2 + 3;
        eraseBox(CX, LEVEL_CY, _lastLevelWidth / 2 + 6, halfHeight);
        _lastLevelLine = buffer;
        _lastLevelWidth = textWidth(&FreeSansBold12pt7b, buffer, 1);
        drawCentered(&FreeSansBold12pt7b, 1, CX, LEVEL_CY, buffer, color);
    }
}

/* A battery symbol at the top left, inside the ring: a body with a nub, filled by the
 * cell voltage. 3.30 V is the empty mark and 4.20 V the full one, the usual range of
 * the single cell this board carries. The fill only repaints when it crosses a 5 %
 * step, so an idle panel costs one comparison per frame. */
void TrainerDisplay::drawBattery(bool known, float volts)
{
    int percent = -1;
    if (known)
    {
        percent = (int)lroundf((volts - 3.30f) / (4.20f - 3.30f) * 100.0f);
        percent = (percent < 0) ? 0 : ((percent > 100) ? 100 : percent);
    }

    const int8_t bucket = known ? (int8_t)(percent / 5) : -1;
    if (bucket == _batteryBucket)
    {
        return;
    }
    _batteryBucket = bucket;

    const uint16_t color = !known ? COL_DIM
                                  : ((percent <= 15) ? COL_WARN : ((percent <= 40) ? COL_GOAL : COL_ZONE));
    /* Clear the body's area plus the nub, then draw the outline and the fill. */
    _gfx->fillRect(BATTERY_X - 3, BATTERY_CY - 11, BATTERY_W + 11, 22, COL_BG);
    _gfx->drawRoundRect(BATTERY_X, BATTERY_CY - 8, BATTERY_W, 16, 3, color);
    _gfx->fillRect(BATTERY_X + BATTERY_W, BATTERY_CY - 3, 4, 6, color);
    if (known && percent > 0)
    {
        const int16_t inner = (int16_t)((BATTERY_W - 6) * percent / 100);
        if (inner > 0)
        {
            _gfx->fillRect(BATTERY_X + 3, BATTERY_CY - 5, inner, 10, color);
        }
    }
}

void TrainerDisplay::drawSessionLine(const TrainerScreen &screen)
{
    char buffer[56];
    if (!screen.sessionActive)
    {
        snprintf(buffer, sizeof(buffer), "tap to start a session");
    }
    else if (screen.holdComplete)
    {
        snprintf(buffer, sizeof(buffer), "hold complete - %u in this session", (unsigned)screen.sessionHolds);
    }
    else if (screen.goalLevel > 0)
    {
        snprintf(buffer, sizeof(buffer), "hold %d mm for %.1f s: %.0f%%", (int)lroundf(screen.goalMm),
                 (double)screen.holdNeeded, (double)(screen.holdProgress * 100.0f));
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "%u:%02u  avg %d mm  peak %d mm",
                 (unsigned)(screen.sessionSeconds / 60.0f), (unsigned)fmodf(screen.sessionSeconds, 60.0f),
                 (int)lroundf(screen.sessionAvgMm), (int)lroundf(screen.peakMm));
    }

    if (_lastSessionLine != buffer)
    {
        uint16_t color = screen.holdComplete ? COL_ZONE : COL_DIM;
        const int16_t halfHeight = (int16_t)pgm_read_byte(&FreeSans9pt7b.yAdvance) / 2 + 3;
        eraseBox(CX, SESSION_CY, _lastSessionWidth / 2 + 6, halfHeight);
        _lastSessionLine = buffer;
        _lastSessionWidth = textWidth(&FreeSans9pt7b, buffer, 1);
        drawCentered(&FreeSans9pt7b, 1, CX, SESSION_CY, buffer, color);
    }
}

void TrainerDisplay::drawHoldBar(const TrainerScreen &screen)
{
    if (fabsf(screen.holdProgress - _lastHoldProgress) < 0.005f && screen.sessionActive == _lastSessionActive)
    {
        return;
    }
    _lastHoldProgress = screen.holdProgress;
    _lastSessionActive = screen.sessionActive;

    _gfx->fillRoundRect(HOLD_BAR_X, HOLD_BAR_Y, HOLD_BAR_W, HOLD_BAR_H, HOLD_BAR_H / 2, COL_TRACK);
    const float fraction = screen.sessionActive ? screen.holdProgress : 0.0f;
    const int16_t width = (int16_t)(HOLD_BAR_W * fraction);
    if (width > 3)
    {
        _gfx->fillRoundRect(HOLD_BAR_X, HOLD_BAR_Y, width, HOLD_BAR_H, HOLD_BAR_H / 2,
                           screen.holdComplete ? COL_DONE : COL_ACCENT);
    }
}

void TrainerDisplay::drawFooter(const TrainerScreen &screen)
{
    if (_lastFooter == screen.footer)
    {
        return;
    }
    const int16_t halfHeight = (int16_t)pgm_read_byte(&FreeSans9pt7b.yAdvance) / 2 + 3;
    eraseBox(CX, FOOTER_CY, _lastFooterWidth / 2 + 6, halfHeight);
    _lastFooter = screen.footer;
    _lastFooterWidth = textWidth(&FreeSans9pt7b, screen.footer, 1);
    drawCentered(&FreeSans9pt7b, 1, CX, FOOTER_CY, screen.footer,
                 screen.sensorOk ? COL_DIM : COL_WARN);
}

/* ------------------------------------------------------------ panel centre ---- */

/* Move the whole layout, because the round window is not always centred on the panel. The
 * next frame repaints everything, so a nudge in the web interface is visible at once. */
void TrainerDisplay::setCentre(int16_t x, int16_t y)
{
    if (x == g_centreX && y == g_centreY)
    {
        return;
    }
    g_centreX = x;
    g_centreY = y;
    g_centreChanged = true;
}

/* ---------------------------------------------------------------- effects ---- */
void TrainerDisplay::drawEffect(const TrainerScreen &screen, uint32_t nowMs, float progress)
{
    const bool praise = (screen.fx == 2);
    const uint16_t accent = praise ? COL_DONE : COL_ACCENT;

    /* Once, when the animation starts: clear the panel. Nothing clears the whole screen
     * again after this, because this panel scans its framebuffer out while it is written
     * and a full clear every frame is exactly what made the panel flash. */
    if (!_fxCleared)
    {
        _gfx->fillScreen(COL_BG);
        for (uint8_t i = 0; i < TRAINER_FX_RINGS; i++)
        {
            _fxRingDrawn[i] = -1;
        }
        _fxCleared = true;
    }

    /* The rings, growing out of the middle, drawn the way the rest of this file draws:
     * only what moved is written, so a frame costs a few thousand pixels and not a screen.
     * Everything old is rubbed out before anything new is drawn, or two rings that meet
     * would erase each other. */
    int16_t radius[TRAINER_FX_RINGS];
    for (uint8_t i = 0; i < TRAINER_FX_RINGS; i++)
    {
        radius[i] = trainer::fxRingStarted(i, progress)
                        ? trainer::fxRingRadius(i, progress, TRAINER_FX_BURST_RADIUS)
                        : -1;
    }
    for (uint8_t i = 0; i < TRAINER_FX_RINGS; i++)
    {
        const int16_t old = _fxRingDrawn[i];
        if (old >= 2 && old != radius[i])
        {
            _gfx->drawCircle(CX, CY, old, COL_BG);
            _gfx->drawCircle(CX, CY, old - 1, COL_BG);
        }
    }
    for (uint8_t i = 0; i < TRAINER_FX_RINGS; i++)
    {
        if (radius[i] >= 2)
        {
            /* A ring inside the clear disc would be drawn and then cleared again on the same
             * frame: a flicker, and the thing that used to look like a stripe. Skip it. */
            if (radius[i] <= FX_CLEAR_RADIUS)
            {
                _fxRingDrawn[i] = radius[i];
                continue;
            }
            const uint16_t color = ((i & 1u) == 0u) ? accent : COL_TRACK;
            _gfx->drawCircle(CX, CY, radius[i], color);
            _gfx->drawCircle(CX, CY, radius[i] - 1, color);
        }
        _fxRingDrawn[i] = radius[i];
    }

    /* The words sit on top of the rings. Their box is cleared again on every frame, which
     * costs nothing to look at: the background is this same black, so the only thing the
     * clear changes is the ring line that ran through the words. The rings pass behind the
     * words, which is how the first version looked. */
    char big[24];
    char line[32];
    char tail[32];
    if (praise)
    {
        snprintf(big, sizeof(big), "%s", (screen.fxText[0] != '\0') ? screen.fxText : "GOOD");
        snprintf(line, sizeof(line), "%u reps done", (unsigned)screen.fxCount);
        snprintf(tail, sizeof(tail), "tap to start again");
    }
    else
    {
        snprintf(big, sizeof(big), "REP %u", (unsigned)screen.fxCount);
        snprintf(line, sizeof(line), "back out fully");
        snprintf(tail, sizeof(tail), "then go again");
    }

    /* The words sit on top of the rings, inside a disc that no ring enters. Clearing a disc
     * instead of the old 300x200 rectangle is the whole fix: a rectangle crossed rings that ran
     * outside it and cut a straight black chord out of each one, every frame. */
    _gfx->fillCircle(CX, CY, FX_CLEAR_RADIUS, COL_BG);

    /* At twice the size a long line would run over the words' box, so the size steps down
     * for anything wide. */
    const uint8_t scale = (textWidth(&FreeSansBold24pt7b, big, 2) <= TRAINER_FX_TEXT_WIDTH) ? 2 : 1;
    const uint16_t bigColor = trainer::fxPulseOn(nowMs, 480) ? accent : COL_TEXT;
    drawCentered(&FreeSansBold24pt7b, scale, CX, CY - 60, big, bigColor);
    drawCentered(&FreeSansBold12pt7b, 1, CX, CY + 30, line, COL_TEXT);
    drawCentered(&FreeSans9pt7b, 1, CX, CY + 70, tail, COL_DIM);
}

/* ------------------------------------------------------------------- menu ---- */

void TrainerDisplay::drawLeftAligned(const GFXfont *font, uint8_t scale, int16_t left, int16_t cy, const char *text, uint16_t color)
{
    /* drawCentered already places the baseline correctly for these fonts. */
    drawCentered(font, scale, left + textWidth(font, text, scale) / 2, cy, text, color);
}

/* The chip that opens the menu, under the hold bar. It is part of the static frame: it
 * is drawn with the panel and only repainted when a session starts or ends, because that
 * is the one thing that changes about it. */
void TrainerDisplay::drawMenuButton(const TrainerScreen &screen)
{
    const int locked = screen.menuLocked ? 1 : 0;
    if (_menuButtonLocked == locked)
    {
        return;
    }
    _menuButtonLocked = locked;

    const int16_t halfW = (int16_t)trainer::kMenuButtonHalfWidth;
    const int16_t halfH = (int16_t)trainer::kMenuButtonHalfHeight;
    const int16_t cy = CY + (int16_t)trainer::kMenuButtonCentreY;
    /* Locked is dim, not invisible: a chip drawn in the track colour on a black panel is a
     * control nobody can find, and the answer it gives (a notice that a play is running) is
     * worth being able to ask for. */
    const uint16_t edge = screen.menuLocked ? COL_DIM : COL_ACCENT;
    const uint16_t ink = screen.menuLocked ? COL_DIM : COL_TEXT;

    _gfx->fillRoundRect(CX - halfW, cy - halfH, halfW * 2, halfH * 2, halfH, COL_BG);
    _gfx->drawRoundRect(CX - halfW, cy - halfH, halfW * 2, halfH * 2, halfH, edge);
    drawCentered(&FreeSansBold12pt7b, 1, CX, cy, "MENU", ink);
}

/* One row: a name on the left, its value on the right. */
void TrainerDisplay::drawMenuRow(int row, const char *label, const char *value, uint16_t valueColor)
{
    const int16_t halfW = (int16_t)trainer::kMenuRowHalfWidth;
    const int16_t height = (int16_t)trainer::kMenuRowHeight;
    const int16_t cy = CY + (int16_t)(trainer::menuRowTop(row) + trainer::kMenuRowHeight / 2.0f);

    _gfx->fillRoundRect(CX - halfW, cy - height / 2, halfW * 2, height, 12, COL_TRACK);
    drawLeftAligned(&FreeSansBold12pt7b, 1, CX - halfW + 16, cy, label, COL_DIM);
    drawLeftAligned(&FreeSansBold12pt7b, 1, CX + halfW - 16 - textWidth(&FreeSansBold12pt7b, value, 1),
                    cy, value, valueColor);
}

/* The overlay. It clears the panel once, when it opens, and after that paints a row only
 * when that row's value changed: a per-frame clear is what makes this panel flash, and
 * the menu is otherwise a still picture. */
void TrainerDisplay::drawMenu(const TrainerScreen &screen, uint32_t nowMs)
{
    (void)nowMs;

    if (!_menuDrawn)
    {
        _gfx->fillScreen(COL_BG);
        drawCentered(&FreeSansBold12pt7b, 1, CX, CY - 148, "MENU", COL_TEXT);

        const int level = screen.menuLevel;
        const int count = (int)screen.levelCount;
        char text[24];
        char label[16];
        trainer::menuLevelLabel(level, count, label, sizeof(label));
        if ((level >= 1) && (level <= count))
        {
            snprintf(text, sizeof(text), "%s OF %d", label, count);
        }
        else
        {
            snprintf(text, sizeof(text), "FREE PLAY");
        }

        drawMenuRow(trainer::kMenuRowMode, "PLAY", trainer::playModeName(screen.menuMode),
                    screen.menuLocked ? COL_DIM : COL_ACCENT);
        drawMenuRow(trainer::kMenuRowLevel, "LEVEL", text, screen.menuLocked ? COL_DIM : COL_ACCENT);
        drawMenuRow(trainer::kMenuRowCalibrate, "CALIBRATE", "TWO POINTS",
                    screen.menuLocked ? COL_DIM : COL_ACCENT);
        drawMenuRow(trainer::kMenuRowClose, "", "CLOSE", COL_DIM);
        drawCentered(&FreeSans9pt7b, 1, CX, CY + (int16_t)trainer::menuRowTop(trainer::kMenuRowCount) + 30,
                     "TAP A ROW   LONG PRESS CLOSES", COL_DIM);

        _menuDrawnMode = (int)screen.menuMode;
        _menuDrawnLevel = level;
        _menuDrawnCount = count;
        _menuDrawn = true;
        /* _valid is NOT cleared here. Clearing it makes the next render run drawStatic(),
         * which fills the panel black - and because the rows below are drawn only when their
         * value changed, the menu then vanished one frame after it appeared, leaving the bare
         * static frame on the glass. The full repaint belongs on the way OUT, and the
         * _menuDrawn block below render() does it. */
        return;
    }

    /* A value that changed since the menu opened. */
    if ((_menuDrawnMode != (int)screen.menuMode) || (_menuDrawnLevel != screen.menuLevel) ||
        (_menuDrawnCount != (int)screen.levelCount))
    {
        const int level = screen.menuLevel;
        const int count = (int)screen.levelCount;
        char text[24];
        char label[16];
        trainer::menuLevelLabel(level, count, label, sizeof(label));
        if ((level >= 1) && (level <= count))
        {
            snprintf(text, sizeof(text), "%s OF %d", label, count);
        }
        else
        {
            snprintf(text, sizeof(text), "FREE PLAY");
        }
        drawMenuRow(trainer::kMenuRowMode, "PLAY", trainer::playModeName(screen.menuMode), COL_ACCENT);
        drawMenuRow(trainer::kMenuRowLevel, "LEVEL", text, COL_ACCENT);
        _menuDrawnMode = (int)screen.menuMode;
        _menuDrawnLevel = level;
        _menuDrawnCount = count;
    }
}

/* "the menu is closed while a play runs" - the answer a tap on the chip gets during a
 * session. It shows for a few seconds and then removes itself. It lives above the header,
 * which is the one band on this panel that no other element uses: at this y the visible
 * window is 161 px either side of the centre, and the words need about 95. */
void TrainerDisplay::drawMenuNotice(const TrainerScreen &screen, uint32_t nowMs)
{
    const int16_t cy = CY - 161;
    const bool fresh = (screen.menuNoticeMs != 0) &&
                       ((nowMs - screen.menuNoticeMs) < TRAINER_MENU_NOTICE_MS);

    if (fresh && (strcmp(_menuDrawnNotice, screen.menuNotice) != 0))
    {
        const int16_t halfWidth = _lastMenuNoticeWidth / 2 + 6;
        eraseBox(CX, cy, halfWidth, 12);
        _menuDrawnNotice[0] = '\0';
        if (screen.menuNotice[0] != '\0')
        {
            drawCentered(&FreeSans9pt7b, 1, CX, cy, screen.menuNotice, COL_WARN);
            snprintf(_menuDrawnNotice, sizeof(_menuDrawnNotice), "%s", screen.menuNotice);
        }
        _lastMenuNoticeWidth = textWidth(&FreeSans9pt7b, screen.menuNotice, 1);
        return;
    }

    if (!fresh && (_menuDrawnNotice[0] != '\0'))
    {
        eraseBox(CX, cy, _lastMenuNoticeWidth / 2 + 6, 12);
        _menuDrawnNotice[0] = '\0';
        _lastMenuNoticeWidth = 0;
    }
}

/* The calibration guide. One instruction at a time, the live reading under it so the operator
 * can see it settle before capturing, and a note that says whether the reading is still. Like
 * the menu it holds its own frame: it clears when the step changes, and after that only the
 * lines whose text changed are repainted. */
void TrainerDisplay::drawCalibration(const TrainerScreen &screen)
{
    if (!_calDrawn || (_calDrawnStep != screen.calStep))
    {
        _gfx->fillScreen(COL_BG);
        _calDrawn = true;
        _calDrawnStep = screen.calStep;
        _calDrawnLive[0] = '\0';
        _calDrawnNote[0] = '\0';

        drawCentered(&FreeSansBold12pt7b, 1, CX, CY - 168, "CALIBRATION", COL_DIM);

        if (screen.calStep == trainer::kCalStepZero)
        {
            drawCentered(&FreeSansBold12pt7b, 1, CX, CY - 96, "PUT THE HEAD AT 0", COL_TEXT);
            drawCentered(&FreeSans9pt7b, 1, CX, CY - 58, "then hold still and tap", COL_DIM);
        }
        else if (screen.calStep == trainer::kCalStepFull)
        {
            drawCentered(&FreeSansBold12pt7b, 1, CX, CY - 96, "PUT THE HEAD AT FULL", COL_TEXT);
            drawCentered(&FreeSans9pt7b, 1, CX, CY - 58, "then hold still and tap", COL_DIM);
            char captured[32];
            snprintf(captured, sizeof(captured), "0 at %d mm", (int)lroundf(screen.calZeroMm));
            drawCentered(&FreeSans9pt7b, 1, CX, CY - 24, captured, COL_DIM);
        }
        else
        {
            float travel = 0.0f;
            trainer::calibrationTravel(screen.calZeroMm, screen.calFullMm, &travel);
            char line[40];
            drawCentered(&FreeSansBold12pt7b, 1, CX, CY - 100, "CALIBRATED", COL_ZONE);
            snprintf(line, sizeof(line), "0 at %d mm", (int)lroundf(screen.calZeroMm));
            drawCentered(&FreeSans9pt7b, 1, CX, CY - 54, line, COL_TEXT);
            snprintf(line, sizeof(line), "full at %d mm", (int)lroundf(screen.calFullMm));
            drawCentered(&FreeSans9pt7b, 1, CX, CY - 24, line, COL_TEXT);
            snprintf(line, sizeof(line), "travel %d mm", (int)lroundf(travel));
            drawCentered(&FreeSans9pt7b, 1, CX, CY + 6, line, COL_ZONE);
            drawCentered(&FreeSans9pt7b, 1, CX, CY + 50, "tap to finish", COL_DIM);
        }
    }

    if (screen.calStep == trainer::kCalStepDone)
    {
        return; /* the finished page is a still picture */
    }

    /* The live reading, and whether it has stopped moving. */
    char live[24];
    if (screen.calRangeMm > 0.0f)
    {
        snprintf(live, sizeof(live), "%d mm", (int)lroundf(screen.calRangeMm));
    }
    else
    {
        snprintf(live, sizeof(live), "--");
    }
    if (strcmp(live, _calDrawnLive) != 0)
    {
        eraseBox(CX, CY + 40, 110, 26);
        drawCentered(&FreeSansBold24pt7b, 1, CX, CY + 40, live, screen.calStable ? COL_ZONE : COL_TEXT);
        snprintf(_calDrawnLive, sizeof(_calDrawnLive), "%s", live);
    }

    const char *note = screen.calStable ? "READY - TAP" : "MOVING...";
    if (strcmp(note, _calDrawnNote) != 0)
    {
        eraseBox(CX, CY + 118, 110, 14);
        drawCentered(&FreeSans9pt7b, 1, CX, CY + 118, note, screen.calStable ? COL_ZONE : COL_WARN);
        snprintf(_calDrawnNote, sizeof(_calDrawnNote), "%s", note);
    }
}

void TrainerDisplay::render(const TrainerScreen &screen)
{
    if (!_present)
    {
        return;
    }

    /* A new panel centre moves everything, so the whole frame is painted again. */
    if (g_centreChanged)
    {
        g_centreChanged = false;
        _valid = false;
    }

    if (!_valid)
    {
        drawStatic();
        _valid = true;
    }

    /* An animation owns the whole panel while it runs. It ends on its own clock, and
     * when it does, the static frame and every text box are drawn again from scratch:
     * the panel is full of the animation, so nothing may be left cached. */
    const uint32_t now = millis();
    const uint32_t duration = (screen.fx == 2) ? TRAINER_PRAISE_FX_MS : TRAINER_REP_FX_MS;
    if ((screen.fx == 1 || screen.fx == 2) && trainer::fxActive(now, screen.fxStartMs, duration))
    {
        drawEffect(screen, now, trainer::fxProgress(now, screen.fxStartMs, duration));
        _fxDrawn = true;
        _valid = false;
        _gfx->flush();
        _frames++;
        return;
    }

    if (_fxDrawn)
    {
        _fxDrawn = false;
        _fxCleared = false; /* the next animation clears the panel again by itself */
        _valid = false;
        drawStatic();
        _valid = true;
    }

    /* The calibration guide owns the panel while it runs, exactly like the menu, and it is
     * opened from the menu (which closes first), so the two never overlap. It must NOT clear
     * the full-repaint flag while it is up: that flag would make drawStatic() fill the panel
     * black on the next frame and the guide would vanish, the way the menu did. */
    if (screen.calStep != trainer::kCalStepOff)
    {
        drawCalibration(screen);
        _gfx->flush();
        _frames++;
        return;
    }

    if (_calDrawn)
    {
        _calDrawn = false;
        _valid = false; /* the guide covered the panel: everything is painted again */
        drawStatic();
        _valid = true;
    }

    /* The menu owns the panel while it is open. It is only ever open with no session
     * running, so the two never fight over the glass. */
    if (screen.menuOpen)
    {
        drawMenu(screen, now);
        _gfx->flush();
        _frames++;
        return;
    }

    if (_menuDrawn)
    {
        _menuDrawn = false;
        _valid = false; /* the menu covered the panel: everything is painted again */
        drawStatic();
        _valid = true;
    }

    drawGauge(screen.gauge, screen.inZone && screen.sessionActive);
    drawTicks(trainer::gaugeFraction(screen.goalMm, screen.maxDepthMm),
              trainer::gaugeFraction(screen.goalLimitMm, screen.maxDepthMm),
              screen.peakGauge);
    drawReadout(screen);
    drawUnit(screen);
    drawLevelLine(screen);
    drawSessionLine(screen);
    drawHoldBar(screen);
    drawBattery(screen.batteryKnown, screen.batteryV);
    /* The chip is drawn after the footer on purpose. Both erase their own box, and if a
     * later layout edit brings them into the same band, the failure should be a chip that
     * covers the footer rather than a chip that disappears. */
    drawFooter(screen);
    drawMenuButton(screen);
    drawMenuNotice(screen, now);

    _gfx->flush();
    _frames++;
}
