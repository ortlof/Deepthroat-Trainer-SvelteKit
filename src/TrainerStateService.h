#ifndef TrainerStateService_h
#define TrainerStateService_h

/*
 *   Live trainer state: one object for the browser, the display and the MQTT
 *   bridge.
 *
 *   Two kinds of data share the object, and they are kept apart on purpose:
 *     - commands: session_start, session_stop, zero, reset_peak, goal_level,
 *       display_on
 *     - telemetry: everything measured, derived or reported by the hardware
 *
 *   Telemetry is written by the trainer task with origin TRAINER_DEVICE_ORIGIN,
 *   and the command applier ignores updates that carry that origin. Without that
 *   rule one push would re-trigger the command it reports.
 *
 *   The object is served on /rest/trainerState and pushed on the WebSocket event
 *   "trainer". It is live data, so it is not written to the file system; the
 *   session summary of the last run is the only part that outlives a session,
 *   and it lives in RAM.
 */

#include <Arduino.h>

#include <ESP32SvelteKit.h>
#include <EventEndpoint.h>
#include <EventSocket.h>
#include <HttpEndpoint.h>
#include <StatefulService.h>

#define TRAINER_STATE_ENDPOINT_PATH "/rest/trainerState"
#define TRAINER_STATE_EVENT "trainer"
/* Origin used for every update the device itself pushes. The command applier
 * ignores it, so telemetry can never look like a command. */
#define TRAINER_DEVICE_ORIGIN "trainerDevice"

#define TRAINER_NOTICE_LENGTH 64

class TrainerState
{
public:
    /* --- commands from the client (all transient except goal_level) --- */
    bool sessionStart = false;
    bool sessionStop = false;
    bool zero = false;      /* set the zero reference at the current distance */
    bool resetPeak = false; /* forget the peak of this session */
    int goalLevel = 0;      /* 0 = free play, 1..3 = the goal to hold */
    bool displayOn = true;

    /* --- what the sensor sees --- */
    bool sensorPresent = false;
    int sensorModelId = 0;
    char sensorModel[24] = "unknown";
    char sensorMode[16] = "absent";
    int sensorStatus = -1;
    char sensorStatusText[48] = "no sample yet";
    float rangeMm = -1.0f; /* -1: no usable sample */
    uint32_t sensorGood = 0;
    uint32_t sensorBad = 0;

    /* --- what the trainer makes of it --- */
    float depthMm = 0.0f;
    float depthRawMm = 0.0f;
    float peakMm = 0.0f;
    float gauge = 0.0f;
    int level = 0;
    char levelName[24] = "Warm up";
    float goalMm = 0.0f;
    /* The top of the goal's band. A depth above this does not count for the goal, so the
     * goal is a target and not "as deep as you like". */
    float goalLimitMm = 0.0f;
    bool inZone = false;
    bool holdComplete = false;
    float holdProgress = 0.0f;
    float holdSecondsHeld = 0.0f;
    float holdSecondsNeeded = 3.0f;

    /* --- the session --- */
    bool sessionActive = false;
    float sessionSeconds = 0.0f;
    float sessionAvgMm = 0.0f;
    float sessionDeepSeconds = 0.0f;
    uint32_t sessionHolds = 0;

    /* Rep mode */
    uint8_t playMode = 0;   /* 0 = hold the goal, 1 = count reps */
    uint16_t repCount = 0;  /* reps completed in this session */

    /* A panel effect: 0 none, 1 a rep counted, 2 the set is finished. The trainer task
     * starts them; a client may start one too, which is how the animation is tested
     * without doing a rep. The start time is always set on the device. */
    uint8_t fx = 0;
    uint32_t fxStartMs = 0;
    uint16_t fxCount = 0;
    char fxText[24] = "";
    uint16_t repTarget = 0; /* 0 = count up, N = count down from N */
    bool repArmed = true;   /* the depth is back at the start: the next rep counts */

    /* --- the session before this one --- */
    float lastSessionPeakMm = 0.0f;
    float lastSessionAvgMm = 0.0f;
    float lastSessionSeconds = 0.0f;
    uint32_t lastSessionHolds = 0;
    uint32_t lastSessionAt = 0; /* epoch seconds, 0 when the clock is not set */

    /* --- the board --- */
    bool displayPresent = false;
    bool displayOnState = true;
    uint8_t displayBrightness = 70;
    uint32_t displayFrames = 0;
    bool touchPresent = false;
    uint32_t touchPresses = 0;

    /* What the touch controller really answers. A panel whose touch does nothing is
     * read over the API with these instead of over a serial capture. */
    uint8_t touchChipId = 0;
    uint8_t touchIrq = 0;
    uint8_t touchAutoSleep = 0;
    int16_t touchX = 0;
    int16_t touchY = 0;
    uint32_t touchReads = 0;
    uint32_t touchReadFailures = 0;
    uint32_t touchIrqPulses = 0;    /* falling edges seen on the interrupt line */
    uint32_t touchFrames = 0;       /* frames that carried a real point */
    float batteryV = 0.0f;
    bool batteryKnown = false;

    /* --- mirror of the settings the UI wants next to the live values --- */
    uint8_t units = 0;
    float referenceMm = 150.0f;
    bool autoAdvance = false;

    /* --- one line for the user: calibration done, sensor missing, hold
     * complete. Written by the device, read by the UI. --- */
    char notice[96] = "";

    static void read(TrainerState &state, JsonObject &root)
    {
        /* commands */
        root["session_start"] = state.sessionStart;
        root["session_stop"] = state.sessionStop;
        root["zero"] = state.zero;
        root["reset_peak"] = state.resetPeak;
        root["goal_level"] = state.goalLevel;
        root["display_on"] = state.displayOn;

        /* sensor */
        root["sensor_present"] = state.sensorPresent;
        root["sensor_model_id"] = state.sensorModelId;
        root["sensor_model"] = state.sensorModel;
        root["sensor_mode"] = state.sensorMode;
        root["sensor_status"] = state.sensorStatus;
        root["sensor_status_text"] = state.sensorStatusText;
        root["range_mm"] = state.rangeMm;
        root["sensor_good"] = state.sensorGood;
        root["sensor_bad"] = state.sensorBad;

        /* trainer */
        root["depth_mm"] = state.depthMm;
        root["depth_raw_mm"] = state.depthRawMm;
        root["peak_mm"] = state.peakMm;
        root["gauge"] = state.gauge;
        root["level"] = state.level;
        root["level_name"] = state.levelName;
        root["goal_mm"] = state.goalMm;
        root["goal_limit_mm"] = state.goalLimitMm;
        root["in_zone"] = state.inZone;
        root["hold_complete"] = state.holdComplete;
        root["hold_progress"] = state.holdProgress;
        root["hold_seconds_held"] = state.holdSecondsHeld;
        root["hold_seconds_needed"] = state.holdSecondsNeeded;

        /* session */
        root["session_active"] = state.sessionActive;
        root["session_seconds"] = state.sessionSeconds;
        root["session_avg_mm"] = state.sessionAvgMm;
        root["session_deep_seconds"] = state.sessionDeepSeconds;
        root["session_holds"] = state.sessionHolds;
        root["play_mode"] = state.playMode;
        root["rep_count"] = state.repCount;
        root["rep_target"] = state.repTarget;
        root["rep_armed"] = state.repArmed;
        root["reps_left"] = (state.repTarget > state.repCount) ? (state.repTarget - state.repCount) : 0;
        root["last_session_peak_mm"] = state.lastSessionPeakMm;
        root["last_session_avg_mm"] = state.lastSessionAvgMm;
        root["last_session_seconds"] = state.lastSessionSeconds;
        root["last_session_holds"] = state.lastSessionHolds;
        root["last_session_at"] = state.lastSessionAt;

        /* board */
        root["display_present"] = state.displayPresent;
        root["display_on_state"] = state.displayOnState;
        root["display_brightness"] = state.displayBrightness;
        root["display_frames"] = state.displayFrames;
        root["touch_present"] = state.touchPresent;
        root["touch_presses"] = state.touchPresses;
        root["touch_chip_id"] = state.touchChipId;
        root["touch_irq"] = state.touchIrq;
        root["touch_auto_sleep"] = state.touchAutoSleep;
        root["touch_x"] = state.touchX;
        root["touch_y"] = state.touchY;
        root["touch_reads"] = state.touchReads;
        root["touch_read_failures"] = state.touchReadFailures;
        root["touch_irq_pulses"] = state.touchIrqPulses;
        root["touch_frames"] = state.touchFrames;
        root["battery_v"] = state.batteryV;
        root["battery_known"] = state.batteryKnown;

        /* settings mirror */
        root["units"] = state.units;
        root["reference_mm"] = state.referenceMm;
        root["auto_advance"] = state.autoAdvance;
        root["notice"] = state.notice;
    }

    static StateUpdateResult update(JsonObject &root, TrainerState &state, const String &originId)
    {
        (void)originId;

        const bool sessionStart = root["session_start"] | state.sessionStart;
        const bool sessionStop = root["session_stop"] | state.sessionStop;
        const bool zero = root["zero"] | state.zero;
        const bool resetPeak = root["reset_peak"] | state.resetPeak;
        int goalLevel = root["goal_level"] | state.goalLevel;
        goalLevel = constrain(goalLevel, 0, 5);
        const bool displayOn = root["display_on"] | state.displayOn;

        /* A panel effect is always taken as a change, so the same animation can be
         * started twice in a row. The client only names the effect and its number; the
         * device stamps the start time, so a client cannot leave the panel stuck. */
        const bool effectRequested = root.containsKey("fx");

        if (!effectRequested &&
            sessionStart == state.sessionStart && sessionStop == state.sessionStop &&
            zero == state.zero && resetPeak == state.resetPeak &&
            goalLevel == state.goalLevel && displayOn == state.displayOn)
        {
            return StateUpdateResult::UNCHANGED;
        }

        state.sessionStart = sessionStart;
        state.sessionStop = sessionStop;
        state.zero = zero;
        state.resetPeak = resetPeak;
        state.goalLevel = goalLevel;
        state.displayOn = displayOn;

        if (effectRequested)
        {
            state.fx = (uint8_t)constrain((int)(root["fx"] | 0), 0, 2);
            state.fxStartMs = millis();
            state.fxCount = (uint16_t)constrain((int)(root["fx_count"] | (int)state.fxCount), 0, 9999);
            const char *text = root["fx_text"];
            snprintf(state.fxText, sizeof(state.fxText), "%s", (text != nullptr) ? text : "");
        }

        return StateUpdateResult::CHANGED;
    }
};

class TrainerStateService : public StatefulService<TrainerState>
{
public:
    TrainerStateService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit);

    void begin();

private:
    HttpEndpoint<TrainerState> _httpEndpoint;
    EventEndpoint<TrainerState> _eventEndpoint;
};

#endif /* TrainerStateService_h */
