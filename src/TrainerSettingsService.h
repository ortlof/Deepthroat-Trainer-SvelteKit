#ifndef TrainerSettingsService_h
#define TrainerSettingsService_h

/*
 *   Persisted trainer configuration.
 *
 *   These values survive a firmware update and a power cycle because they live
 *   as JSON on the LittleFS partition (/config/trainerSettings.json), and they
 *   are edited from the browser. Everything the sensor needs to turn a distance
 *   into a depth is here, so a new mount is a matter of calibration, not of a
 *   new build.
 *
 *   All limits are applied in the JSON updater, which runs under the state
 *   mutex. Validation in an update handler would run after the transaction
 *   ended, where two clients can interleave.
 */

#include <Arduino.h>

#include <ESP32SvelteKit.h>
#include <FSPersistence.h>
#include <HttpEndpoint.h>
#include <StatefulService.h>

#include "trainer_math.h"

#define TRAINER_SETTINGS_ENDPOINT_PATH "/rest/trainerSettings"
#define TRAINER_SETTINGS_FILE_PATH "/config/trainerSettings.json"

/* A band narrower than this is not a target anyone can hit: a level and its upper limit are
 * pushed apart to at least this much. It also makes an old configuration safe: a level that
 * predates the upper limits arrives with its limit equal to its depth. */
#define TRAINER_MIN_BAND_MM 10.0f

/* Widest calibration the applier accepts, in millimetres. A mounting error is
 * centimetres, not metres; a bigger value hides a wrong reference distance. */
#define TRAINER_CALIBRATION_LIMIT_MM 60.0f

class TrainerSettings
{
public:
    /* --- geometry and sensor --- */
    float reference_mm = 150.0f;   /* distance that counts as depth 0 */
    float max_depth_mm = 145.0f;   /* full scale of the gauge and the clamp: with the
                                    * reference at 150 mm this reaches 100 % at 5 mm,
                                    * which is as close as the sensor measures well */
    float calibration_mm = 0.0f;   /* added to every range sample */

    /* --- goals ---
     * Every level is a band: it counts from its depth up to its own upper limit. Going past
     * the top of a level does not count as that level, so the upper limit is what makes the
     * goal a target rather than "deeper is better". The list is edited in the web interface
     * and may be 1 to 10 levels long, which is why it is an array and not named fields. */
    uint8_t level_count = 5;
    float level_mm[trainer::kMaxLevels] = {40.0f, 60.0f, 80.0f, 100.0f, 120.0f,
                                           140.0f, 160.0f, 180.0f, 200.0f, 200.0f};
    float level_max_mm[trainer::kMaxLevels] = {60.0f, 80.0f, 100.0f, 120.0f, 140.0f,
                                               160.0f, 180.0f, 200.0f, 220.0f, 220.0f};
    float hold_seconds = 3.0f;      /* how long a goal must be held */
    bool auto_advance = false;      /* move to the next level after a set or a hold */
    bool level_count_down = false;  /* walk the levels from the last one down to the first */

    /* --- play --- */
    uint8_t play_mode = 0;         /* 0 = hold the goal, 1 = count reps */
    uint16_t reps_target = 0;      /* reps: 0 = count up, N = count down from N */
    char praise[24] = "GOOD THING"; /* shown on the panel when a set is finished */

    /* --- behaviour --- */
    float filter_tau_s = 0.20f;    /* sensor smoothing; 0 = raw */
    float session_stop_s = 8.0f;   /* no reading for this long ends a session */
    uint8_t units = 0;             /* 0 mm, 1 cm, 2 in */

    /* --- display --- */
    uint8_t display_brightness = 70; /* percent */
    uint8_t dim_brightness = 10;     /* percent while idle */
    uint16_t display_timeout_s = 0;  /* 0 = never dim */

    /* Where the visible round window sits inside the 480x480 framebuffer. Measured from a
     * video of this board: the window is about 15 px left and 9 px above the middle of the
     * framebuffer, which is easy to see on a ring this close to the edge. Nudge it until
     * the ring looks centred. */
    int16_t panel_centre_x = 225;
    int16_t panel_centre_y = 231;

    /* --- the ladder builder ---
     * The auto builder lays bands of one width end to end from a starting depth, up to the
     * travel the calibration measured. These four presets are the shapes worth keeping, stored
     * on the device so the choice survives a browser, a phone or a reflash. A preset holds what
     * the builder needs to reproduce the ladder: where the first band starts, how wide the
     * bands are, and how many. */
    static constexpr int kLadderPresets = 4;
    uint8_t ladder_preset_count = 0; /* how many are saved, 0..kLadderPresets */
    char ladder_preset_name[kLadderPresets][16] = {"", "", "", ""};
    float ladder_preset_base[kLadderPresets] = {0.0f, 0.0f, 0.0f, 0.0f};
    float ladder_preset_width[kLadderPresets] = {20.0f, 30.0f, 40.0f, 50.0f};
    uint8_t ladder_preset_levels[kLadderPresets] = {6, 4, 3, 2};
    /* The builder's last values, so the web interface opens where it was left. */
    float ladder_base = 0.0f;
    float ladder_width = 20.0f;

    /* The goals as the pure maths wants them: one array of depths and one of upper limits. */
    void levels(float lows[trainer::kMaxLevels], float highs[trainer::kMaxLevels]) const
    {
        for (int i = 0; i < trainer::kMaxLevels; i++)
        {
            lows[i] = level_mm[i];
            highs[i] = level_max_mm[i];
        }
    }

    static void read(TrainerSettings &settings, JsonObject &root)
    {
        root["reference_mm"] = settings.reference_mm;
        root["max_depth_mm"] = settings.max_depth_mm;
        root["calibration_mm"] = settings.calibration_mm;
        root["level_count"] = settings.level_count;
        char levelKey[24];
        for (int i = 0; i < trainer::kMaxLevels; i++)
        {
            snprintf(levelKey, sizeof(levelKey), "level%d_mm", i + 1);
            root[levelKey] = settings.level_mm[i];
            snprintf(levelKey, sizeof(levelKey), "level%d_max_mm", i + 1);
            root[levelKey] = settings.level_max_mm[i];
        }
        root["hold_seconds"] = settings.hold_seconds;
        root["auto_advance"] = settings.auto_advance;
        root["level_count_down"] = settings.level_count_down;
        root["play_mode"] = settings.play_mode;
        root["reps_target"] = settings.reps_target;
        root["praise"] = settings.praise;
        root["filter_tau_s"] = settings.filter_tau_s;
        root["session_stop_s"] = settings.session_stop_s;
        root["units"] = settings.units;
        root["display_brightness"] = settings.display_brightness;
        root["dim_brightness"] = settings.dim_brightness;
        root["display_timeout_s"] = settings.display_timeout_s;
        root["panel_centre_x"] = settings.panel_centre_x;
        root["panel_centre_y"] = settings.panel_centre_y;

        /* the ladder builder and its saved shapes */
        root["ladder_base"] = settings.ladder_base;
        root["ladder_width"] = settings.ladder_width;
        root["ladder_preset_count"] = settings.ladder_preset_count;
        char ladderKey[32];
        for (int i = 0; i < kLadderPresets; ++i)
        {
            snprintf(ladderKey, sizeof(ladderKey), "ladder_preset%d_name", i + 1);
            root[ladderKey] = settings.ladder_preset_name[i];
            snprintf(ladderKey, sizeof(ladderKey), "ladder_preset%d_base", i + 1);
            root[ladderKey] = settings.ladder_preset_base[i];
            snprintf(ladderKey, sizeof(ladderKey), "ladder_preset%d_width", i + 1);
            root[ladderKey] = settings.ladder_preset_width[i];
            snprintf(ladderKey, sizeof(ladderKey), "ladder_preset%d_levels", i + 1);
            root[ladderKey] = settings.ladder_preset_levels[i];
        }
    }

    static StateUpdateResult update(JsonObject &root, TrainerSettings &settings, const String &originId)
    {
        (void)originId;

        float reference = root["reference_mm"] | settings.reference_mm;
        reference = constrain(reference, 20.0f, 240.0f);

        float maxDepth = root["max_depth_mm"] | settings.max_depth_mm;
        maxDepth = constrain(maxDepth, 10.0f, 240.0f);

        float calibration = root["calibration_mm"] | settings.calibration_mm;
        calibration = constrain(calibration, -TRAINER_CALIBRATION_LIMIT_MM, TRAINER_CALIBRATION_LIMIT_MM);

        /* Five goals, each with its own upper limit. The levels must stay ordered, or
         * "level 2" could be shallower than "level 1" and the level display would jump
         * backwards. An upper limit is never below its own level, so a band can never be
         * inside out: the limit below the level would otherwise make the goal impossible. */
        float currentLows[trainer::kMaxLevels];
        float currentHighs[trainer::kMaxLevels];
        settings.levels(currentLows, currentHighs);

        const int count = trainer::levelCountInUse((int)(root["level_count"] | (int)settings.level_count));

        float lows[trainer::kMaxLevels];
        float highs[trainer::kMaxLevels];
        for (int i = 0; i < trainer::kMaxLevels; i++)
        {
            char key[24];
            snprintf(key, sizeof(key), "level%d_mm", i + 1);
            /* The top level is 200 mm: deeper than that is past the full scale of the ring
             * on any sane mounting, and it leaves room for the band above it. */
            const float floor = (i == 0) ? 5.0f : lows[i - 1];
            lows[i] = constrain((float)(root[key] | currentLows[i]), floor, 200.0f);

            snprintf(key, sizeof(key), "level%d_max_mm", i + 1);
            highs[i] = constrain((float)(root[key] | currentHighs[i]),
                                 lows[i] + TRAINER_MIN_BAND_MM, 240.0f);
        }

        bool goalsSame = true;
        for (int i = 0; i < trainer::kMaxLevels; i++)
        {
            if (lows[i] != currentLows[i] || highs[i] != currentHighs[i])
            {
                goalsSame = false;
            }
        }

        float hold = root["hold_seconds"] | settings.hold_seconds;
        hold = constrain(hold, 0.5f, 60.0f);

        bool autoAdvance = root["auto_advance"] | settings.auto_advance;
        bool countDown = root["level_count_down"] | settings.level_count_down;

        int playMode = root["play_mode"] | (int)settings.play_mode;
        playMode = constrain(playMode, 0, 1);

        int repsTarget = root["reps_target"] | (int)settings.reps_target;
        repsTarget = constrain(repsTarget, 0, 500);

        /* The words the panel shows when a set is finished. Twelve characters is what
         * the panel holds at that size, so a longer value is cut rather than refused. */
        const char *praiseSource = root["praise"];
        char praise[sizeof(settings.praise)];
        snprintf(praise, sizeof(praise), "%s", (praiseSource != nullptr) ? praiseSource : settings.praise);
        /* Upper case on the device, so the panel reads as written by the panel and not
         * as whatever the keyboard produced. */
        for (char *p = praise; *p != '\0'; p++)
        {
            *p = (char)toupper((unsigned char)*p);
        }

        float tau = root["filter_tau_s"] | settings.filter_tau_s;
        tau = constrain(tau, 0.0f, 2.0f);

        float stop = root["session_stop_s"] | settings.session_stop_s;
        stop = constrain(stop, 2.0f, 300.0f);

        int units = root["units"] | (int)settings.units;
        units = constrain(units, 0, 2);

        int brightness = root["display_brightness"] | (int)settings.display_brightness;
        brightness = constrain(brightness, 1, 100);

        int dim = root["dim_brightness"] | (int)settings.dim_brightness;
        dim = constrain(dim, 0, 100);

        int timeout = root["display_timeout_s"] | (int)settings.display_timeout_s;
        timeout = constrain(timeout, 0, 3600);

        /* The panel centre is a nudge, not a coordinate: the whole panel is 480 wide, so an
         * offset of more than a few tens of pixels means something else is wrong. */
        int centreX = root["panel_centre_x"] | (int)settings.panel_centre_x;
        centreX = constrain(centreX, 200, 280);
        int centreY = root["panel_centre_y"] | (int)settings.panel_centre_y;
        centreY = constrain(centreY, 200, 280);

        /* The ladder builder and its saved shapes. These need their own change flag: a
         * preset-only edit compares equal to every other value and would never be written. */
        const float ladderBase = root["ladder_base"] | settings.ladder_base;
        const float ladderWidth = root["ladder_width"] | settings.ladder_width;
        int presetCount = root["ladder_preset_count"] | (int)settings.ladder_preset_count;
        presetCount = constrain(presetCount, 0, (int)kLadderPresets);
        char presetName[kLadderPresets][16];
        float presetBase[kLadderPresets];
        float presetWidth[kLadderPresets];
        int presetLevels[kLadderPresets];
        bool ladderChanged = (fabsf(ladderBase - settings.ladder_base) > 0.01f) ||
                             (fabsf(ladderWidth - settings.ladder_width) > 0.01f) ||
                             (presetCount != (int)settings.ladder_preset_count);
        char ladderKey[32];
        for (int i = 0; i < kLadderPresets; ++i)
        {
            snprintf(ladderKey, sizeof(ladderKey), "ladder_preset%d_name", i + 1);
            const char *nameSource = root[ladderKey];
            snprintf(presetName[i], sizeof(presetName[i]), "%s",
                     (nameSource != nullptr) ? nameSource : settings.ladder_preset_name[i]);
            snprintf(ladderKey, sizeof(ladderKey), "ladder_preset%d_base", i + 1);
            presetBase[i] = root[ladderKey] | settings.ladder_preset_base[i];
            snprintf(ladderKey, sizeof(ladderKey), "ladder_preset%d_width", i + 1);
            presetWidth[i] = root[ladderKey] | settings.ladder_preset_width[i];
            snprintf(ladderKey, sizeof(ladderKey), "ladder_preset%d_levels", i + 1);
            presetLevels[i] = constrain((int)(root[ladderKey] | (int)settings.ladder_preset_levels[i]),
                                        1, trainer::kMaxLevels);

            ladderChanged = ladderChanged ||
                            (strcmp(presetName[i], settings.ladder_preset_name[i]) != 0) ||
                            (fabsf(presetBase[i] - settings.ladder_preset_base[i]) > 0.01f) ||
                            (fabsf(presetWidth[i] - settings.ladder_preset_width[i]) > 0.01f) ||
                            (presetLevels[i] != (int)settings.ladder_preset_levels[i]);
        }

        if (reference == settings.reference_mm && maxDepth == settings.max_depth_mm &&
            calibration == settings.calibration_mm && goalsSame &&
            count == (int)settings.level_count && countDown == settings.level_count_down &&
            hold == settings.hold_seconds && autoAdvance == settings.auto_advance &&
            playMode == (int)settings.play_mode && repsTarget == (int)settings.reps_target &&
            tau == settings.filter_tau_s && stop == settings.session_stop_s &&
            units == (int)settings.units && brightness == (int)settings.display_brightness &&
            dim == (int)settings.dim_brightness && timeout == (int)settings.display_timeout_s &&
            centreX == (int)settings.panel_centre_x && centreY == (int)settings.panel_centre_y &&
            !ladderChanged &&
            strcmp(praise, settings.praise) == 0)
        {
            return StateUpdateResult::UNCHANGED;
        }

        settings.reference_mm = reference;
        settings.max_depth_mm = maxDepth;
        settings.calibration_mm = calibration;
        settings.level_count = (uint8_t)count;
        for (int i = 0; i < trainer::kMaxLevels; i++)
        {
            settings.level_mm[i] = lows[i];
            settings.level_max_mm[i] = highs[i];
        }
        settings.level_count_down = countDown;
        settings.hold_seconds = hold;
        settings.auto_advance = autoAdvance;
        settings.play_mode = (uint8_t)playMode;
        settings.reps_target = (uint16_t)repsTarget;
        settings.filter_tau_s = tau;
        settings.session_stop_s = stop;
        settings.units = (uint8_t)units;
        settings.display_brightness = (uint8_t)brightness;
        settings.dim_brightness = (uint8_t)dim;
        settings.display_timeout_s = (uint16_t)timeout;
        settings.panel_centre_x = (int16_t)centreX;
        settings.panel_centre_y = (int16_t)centreY;
        settings.ladder_base = ladderBase;
        settings.ladder_width = ladderWidth;
        settings.ladder_preset_count = (uint8_t)presetCount;
        for (int i = 0; i < kLadderPresets; ++i)
        {
            snprintf(settings.ladder_preset_name[i], sizeof(settings.ladder_preset_name[i]), "%s", presetName[i]);
            settings.ladder_preset_base[i] = presetBase[i];
            settings.ladder_preset_width[i] = presetWidth[i];
            settings.ladder_preset_levels[i] = (uint8_t)presetLevels[i];
        }
        snprintf(settings.praise, sizeof(settings.praise), "%s", praise);
        return StateUpdateResult::CHANGED;
    }
};

class TrainerSettingsService : public StatefulService<TrainerSettings>
{
public:
    TrainerSettingsService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit);

    void begin();

private:
    HttpEndpoint<TrainerSettings> _httpEndpoint;
    FSPersistence<TrainerSettings> _fsPersistence;
};

#endif /* TrainerSettingsService_h */
