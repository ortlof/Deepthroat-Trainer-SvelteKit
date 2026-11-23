/*
 *   A very small console on the USB serial port.
 *
 *   It exists because this board is often on no network at all: its radio cannot hold a join at the
 *   distances it sometimes sits at, and without an address nothing answers over HTTP. The serial
 *   port still works, so it carries two things a network would otherwise carry:
 *
 *     s            one line of live state, in a shape a script can read
 *     f x y w h    a crop of the panel as base64 RGB565, so the glass can be looked at from here
 *
 *   The crop is the point of the whole thing. A 60x60 crop is 7 kB, which is about a second of text
 *   at 115200 - cheap enough to ask for whenever a drawing question comes up.
 */

#ifndef TrainerConsole_h
#define TrainerConsole_h

#include <Arduino.h>

#include "TrainerConfig.h"
#include "TrainerDisplay.h"
#include "trainer_math.h"

/* Set by the serial console when it is asked to press the menu chip. The trainer task owns the
 * state, so the console only raises this and the task presses it - the same path a finger takes. */
static volatile bool g_consoleMenuPress = false;

struct TrainerConsoleState
{
    const char *levelName = "";
    float depthMm = 0.0f;
    float peakMm = 0.0f;
    bool sessionActive = false;
    uint16_t reps = 0;
    uint16_t repTarget = 0;
    bool repArmed = true;
    uint16_t breakS = 0;
};

inline void printStateLine(TrainerDisplay &display, const TrainerConsoleState &state)
{
    /* One line, keys and values, so the host can grep it without a parser. */
    Serial.printf("state level=%s depth=%.1f peak=%.1f session=%u reps=%u/%u armed=%u break_s=%u"
                  " frames=%lu heap=%lu psram=%lu\n",
                  state.levelName, (double)state.depthMm, (double)state.peakMm,
                  state.sessionActive ? 1u : 0u, (unsigned)state.reps, (unsigned)state.repTarget,
                  state.repArmed ? 1u : 0u, (unsigned)state.breakS,
                  (unsigned long)display.frames(), (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getFreePsram());
}

inline void printFrameCrop(TrainerDisplay &display, int x, int y, int w, int h)
{
    uint16_t *frame = display.frameBuffer();
    if (frame == nullptr)
    {
        Serial.println("ERR no panel");
        return;
    }

    /* Clamp to the panel and to a size that fits in a reply people will wait for. */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 120) w = 120;
    if (h > 120) h = 120;
    const int panelW = TRAINER_TFT_WIDTH;
    const int panelH = TRAINER_TFT_HEIGHT;
    if (x + w > panelW) w = panelW - x;
    if (y + h > panelH) h = panelH - y;
    if (w < 1 || h < 1)
    {
        Serial.println("ERR crop outside the panel");
        return;
    }

    const size_t bytes = (size_t)w * (size_t)h * 2u;
    const size_t textSize = ((bytes + 2u) / 3u) * 4u + 1u;
    uint8_t *raw = (uint8_t *)malloc(bytes);
    char *text = (char *)malloc(textSize);
    if (raw == nullptr || text == nullptr)
    {
        free(raw);
        free(text);
        Serial.println("ERR no memory for the crop");
        return;
    }

    /* Copied, not borrowed: the panel is scanning this buffer out while the copy runs. */
    for (int row = 0; row < h; ++row)
    {
        memcpy(raw + (size_t)row * (size_t)w * 2u,
               frame + (size_t)(y + row) * (size_t)panelW + (size_t)x, (size_t)w * 2u);
    }
    trainer::base64Encode(raw, bytes, text, textSize);

    Serial.printf("frame x=%d y=%d w=%d h=%d b64=%u\n", x, y, w, h, (unsigned)strlen(text));
    Serial.println(text);
    Serial.println("end");
    free(raw);
    free(text);
}

/* Call this from the task that already reads the sensor, so the values printed are the live ones. */
inline void trainerConsole(TrainerDisplay &display, const TrainerConsoleState &state)
{
    static char line[24];
    static size_t used = 0;

    while (Serial.available() > 0)
    {
        const char c = (char)Serial.read();
        if (c == '\r')
        {
            continue;
        }
        if (c != '\n')
        {
            if (used < sizeof(line) - 1)
            {
                line[used++] = c;
            }
            continue;
        }

        line[used] = '\0';
        used = 0;
        if (line[0] == 's')
        {
            printStateLine(display, state);
        }
        else if (line[0] == 'm')
        {
            /* The same press a finger makes on the menu chip, so the panel can be driven from
             * here when the board has no network and nobody is standing in front of it. */
            g_consoleMenuPress = true;
            Serial.println("menu press requested");
        }
        else if (line[0] == 'f')
        {
            int cx = 0;
            int cy = 0;
            int cw = 0;
            int ch = 0;
            if (sscanf(line + 1, "%d %d %d %d", &cx, &cy, &cw, &ch) == 4)
            {
                printFrameCrop(display, cx, cy, cw, ch);
            }
            else
            {
                Serial.println("ERR usage: f x y w h");
            }
        }
        else if (line[0] == 'h')
        {
            Serial.println("s = state, m = press the menu chip, f x y w h = panel crop as base64");
        }
        else if (line[0] != '\0')
        {
            Serial.println("ERR unknown command");
        }
    }
}

#endif
