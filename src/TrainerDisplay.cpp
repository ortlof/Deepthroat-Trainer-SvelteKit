#include "TrainerDisplay.h"

#include <math.h>

/* Vendored Adafruit GFX fonts (BSD, see src/fonts/FONT-LICENSE.txt). Arduino_GFX
 * has no fonts of its own except the classic 5x7 one, and a 5x7 readout is not
 * readable from where the trainer is used. */
#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSansBold12pt7b.h"
#include "fonts/FreeSansBold24pt7b.h"

/* ------------------------------------------------------------------ layout -- */

#define CX 240
#define CY 240
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
#define FOOTER_CY 392
#define HEADER_CY 100

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
    _drawnPeakGauge = -1.0f;
    _drawnInZone = false;
    _lastSessionActive = false;
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
        const float fromAngle = GAUGE_START + GAUGE_SWEEP * _drawnGauge;
        const float span = targetAngle - fromAngle;
        if (span > ANGLE_EPSILON)
        {
            _gfx->fillArc(CX, CY, R_OUT, R_IN, fromAngle, targetAngle, color);
        }
    }
    else if (target < _drawnGauge - 0.0005f)
    {
        const float fromAngle = GAUGE_START + GAUGE_SWEEP * target;
        const float span = (GAUGE_START + GAUGE_SWEEP * _drawnGauge) - fromAngle;
        if (span > ANGLE_EPSILON)
        {
            _gfx->fillArc(CX, CY, R_OUT, R_IN, fromAngle, fromAngle + span, COL_TRACK);
        }
    }
    _drawnGauge = target;
}

void TrainerDisplay::drawTicks(float goalGauge, float peakGauge)
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
    if (screen.goalLevel <= 0)
    {
        snprintf(buffer, sizeof(buffer), "free play - %s", trainer::levelName(screen.level));
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "goal %s (%d mm)", trainer::levelName(screen.goalLevel), (int)lroundf(screen.goalMm));
    }

    if (_lastLevelLine != buffer)
    {
        uint16_t color = COL_TEXT;
        if (screen.goalLevel > 0 && screen.level >= screen.goalLevel)
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

void TrainerDisplay::render(const TrainerScreen &screen)
{
    if (!_present)
    {
        return;
    }

    if (!_valid)
    {
        drawStatic();
        _valid = true;
    }

    drawGauge(screen.gauge, screen.inZone && screen.sessionActive);
    drawTicks(trainer::gaugeFraction(screen.goalMm, screen.maxDepthMm), screen.peakGauge);
    drawReadout(screen);
    drawUnit(screen);
    drawLevelLine(screen);
    drawSessionLine(screen);
    drawHoldBar(screen);
    drawFooter(screen);

    _gfx->flush();
    _frames++;
}
