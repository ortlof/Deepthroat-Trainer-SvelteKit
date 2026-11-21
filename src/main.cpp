/*
 *   DTT-Trainer -- deepthroat trainer on the LilyGo T-RGB round display.
 *
 *   The board measures how close the trainer is with a VL6180X time-of-flight
 *   sensor, turns that distance into a depth in millimetres, and shows the depth
 *   on the round panel as a ring gauge. A session counts time, peak, average
 *   depth and completed holds; the browser UI shows the same data and holds the
 *   configuration.
 *
 *   Division of work:
 *     core 0: ESP32-SvelteKit (WiFi, HTTP, WebSocket, the web server)
 *     core 1: trainerTask (sensor, session, touch) and displayTask (panel)
 *
 *   Only ONE task touches I2C: the sensor, the touch controller and the panel's
 *   XL9535 expander all share one bus, and the Arduino Wire library has no
 *   locking. That task is trainerTask.
 *
 *   Every rule that turns a distance into a depth, a level and a hold lives in
 *   src/trainer_math.h and is tested on the host (test/host/run.sh).
 */

#include <Arduino.h>
#include <WiFi.h>

#include <ESP32SvelteKit.h>
#include <NotificationService.h>
#include <PsychicHttpServer.h>

#include "TrainerConfig.h"
#include "TrainerDisplay.h"
#include "TrainerSettingsService.h"
#include "TrainerStateService.h"
#include "TrainerTouch.h"
#include "VL6180Sensor.h"
#include "trainer_math.h"

#define SERIAL_BAUD_RATE 115200

/* The framework allocates the HTTP endpoints statically, so this count must
 * cover every server.on() in the framework, in the front end data routes and in
 * this project, plus spare. */
#define NUMBER_OF_ENDPOINTS 60

/* How often the browser is pushed a new state. The panel runs faster, at
 * TRAINER_DISPLAY_TASK_HZ, and reads the shared snapshot directly. */
#define STATE_PUSH_INTERVAL_MS 200

/* A depth change smaller than this does not count as activity and is not worth
 * an event; without the threshold every sample would be a push. */
#define DEPTH_PUSH_THRESHOLD_MM 0.4f

/* How long a notice stays on the state object. */
#define NOTICE_TIMEOUT_MS 12000

PsychicHttpServer server;
ESP32SvelteKit esp32sveltekit(&server, NUMBER_OF_ENDPOINTS);

TrainerSettingsService trainerSettingsService(&server, &esp32sveltekit);
TrainerStateService trainerStateService(&server, &esp32sveltekit);

TrainerDisplay display;
TrainerTouch touch;
VL6180Sensor sensor;

/* ------------------------------------------------------- trainer state ------ */

static trainer::EmaFilter depthFilter;
static trainer::HoldTracker holdTracker;
static trainer::Session session;

/* Cached copy of the settings, refreshed by the settings update handler.
 *
 * Two buffers and one pointer: readers (the trainer task, the display task, the
 * HTTP task) get a pointer to a struct nobody writes any more, so a read is
 * consistent and needs no lock, and a settings post on core 0 cannot be seen
 * half applied on core 1. */
static TrainerSettings settingsBuffers[2];
static TrainerSettings *volatile settingsActive = &settingsBuffers[0];
static uint8_t settingsWriteIndex = 1;

static inline const TrainerSettings &cfg()
{
    return *settingsActive;
}

static void publishSettings(const TrainerSettings &value)
{
    settingsBuffers[settingsWriteIndex] = value;
    settingsActive = &settingsBuffers[settingsWriteIndex];
    settingsWriteIndex = (settingsWriteIndex == 0) ? 1 : 0;
}

static trainer::SessionSummary lastSession;
static uint32_t lastSessionAtEpoch = 0;

static float depthMm = 0.0f;
static float depthRawMm = 0.0f;
static float peakMm = 0.0f;
static uint32_t lastValidSampleMs = 0;
static uint32_t lastActivityMs = 0;
static uint32_t lastStatePushMs = 0;
static int lastGoalLevel = 0;
static String notice;
static uint32_t noticeSetMs = 0;
static bool holdCounted = false;

/* Snapshot for the display task: a plain POD written by trainerTask and copied
 * by displayTask under a spinlock. The copy is a few hundred bytes and the lock
 * is held for microseconds, so a spinlock is cheaper than a mutex here. */
static TrainerScreen screenSnapshot;
static portMUX_TYPE snapshotMux = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t trainerTaskHandle = nullptr;
static TaskHandle_t displayTaskHandle = nullptr;

/* ------------------------------------------------------------ prototypes ---- */

static void refreshSettings(bool invalidateDisplay);
static void applyTrainerStateToSession(const String &originId);
static void startSession();
static void stopSession();
static void zeroReference();
static void setNotice(const char *text);
static void buildSnapshot();
static void publishState();
static float readBatteryVolts();
static void handleTouch();
void trainerTask(void *pvParameters);
void displayTask(void *pvParameters);

/* -------------------------------------------------------------- helpers ---- */

static float currentGoalMm()
{
    return trainer::goalForLevel(lastGoalLevel, cfg().level1_mm, cfg().level2_mm, cfg().level3_mm);
}

static void setNotice(const char *text)
{
    notice = text;
    noticeSetMs = millis();
}

static void refreshSettings(bool invalidateDisplay)
{
    TrainerSettings loaded;
    trainerSettingsService.read([&loaded](TrainerSettings &value)
                                { loaded = value; });

    const bool unitsChanged = (loaded.units != cfg().units);
    publishSettings(loaded);

    holdTracker.retarget(cfg().hold_seconds);
    display.setBrightness(cfg().display_brightness);

    if (invalidateDisplay || unitsChanged)
    {
        display.invalidate();
    }
}

static float readBatteryVolts()
{
    /* 100k/100k divider on the board, so the pin sees half the cell voltage.
     * While USB is connected the reading is the charging voltage and means
     * nothing; the UI says so. */
    const uint32_t millivolts = analogReadMilliVolts(TRAINER_BAT_ADC_PIN);
    return (float)millivolts * TRAINER_BAT_DIVIDER / 1000.0f;
}

/* ------------------------------------------------------------- sessions ---- */

static void startSession()
{
    if (session.active())
    {
        return;
    }
    session.begin();
    peakMm = 0.0f;
    depthFilter.reset();
    holdTracker.reset(cfg().hold_seconds);
    holdCounted = false;
    lastValidSampleMs = millis();
    lastActivityMs = millis();
    setNotice("session started");
    esp32sveltekit.getNotificationService()->pushNotification("Session started", PUSHINFO);
    Serial.println("[trainer] session started");
}

static void stopSession()
{
    if (!session.active())
    {
        return;
    }
    session.end();
    lastSession = session.summary();
    const time_t now = time(nullptr);
    lastSessionAtEpoch = (now > 1600000000) ? (uint32_t)now : 0;

    char buffer[96];
    snprintf(buffer, sizeof(buffer), "session: peak %d mm, avg %d mm, %d s, %u holds",
             (int)lroundf(lastSession.peakMm), (int)lroundf(lastSession.averageMm()),
             (int)lroundf(lastSession.elapsedSeconds), (unsigned)lastSession.holds);
    setNotice(buffer);
    esp32sveltekit.getNotificationService()->pushNotification(buffer, PUSHSUCCESS);
    Serial.printf("[trainer] %s\n", buffer);

    session.clear();
    holdTracker.reset(cfg().hold_seconds);
    holdCounted = false;
}

static void zeroReference()
{
    /* depth = reference - (range + calibration), so making the distance in
     * front of the sensor mean depth 0 means calibration = reference - range. */
    if (!sensor.present() || !trainer::rangeIsUsable(sensor.rangeMm()))
    {
        setNotice("zero needs a valid distance");
        esp32sveltekit.getNotificationService()->pushNotification("No usable distance to zero at", PUSHWARNING);
        return;
    }
    const float calibration = constrain(cfg().reference_mm - sensor.rangeMm(),
                                        -TRAINER_CALIBRATION_LIMIT_MM, TRAINER_CALIBRATION_LIMIT_MM);
    trainerSettingsService.update([calibration](TrainerSettings &value)
                                  {
        value.calibration_mm = calibration;
        return StateUpdateResult::CHANGED; },
                                  TRAINER_DEVICE_ORIGIN);

    depthFilter.reset();
    depthMm = 0.0f;
    depthRawMm = 0.0f;
    peakMm = 0.0f;

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "zero set at %d mm", (int)lroundf(sensor.rangeMm()));
    setNotice(buffer);
    esp32sveltekit.getNotificationService()->pushNotification(buffer, PUSHINFO);
    Serial.printf("[trainer] %s (calibration %d mm)\n", buffer, (int)lroundf(calibration));
}

/* -------------------------------------------------------------- commands ---- */

static void applyTrainerStateToSession(const String &originId)
{
    if (originId == TRAINER_DEVICE_ORIGIN)
    {
        return; /* telemetry push, not a command */
    }

    TrainerState command;
    trainerStateService.read([&command](TrainerState &state)
                             { command = state; });

    if (command.sessionStart)
    {
        /* Clear the transient flag first, then act: a client that posts the same
         * body twice must not start a second session. */
        trainerStateService.update([](TrainerState &state)
                                   {
            state.sessionStart = false;
            return StateUpdateResult::CHANGED; },
                                   TRAINER_DEVICE_ORIGIN);
        startSession();
    }

    if (command.sessionStop)
    {
        trainerStateService.update([](TrainerState &state)
                                   {
            state.sessionStop = false;
            return StateUpdateResult::CHANGED; },
                                   TRAINER_DEVICE_ORIGIN);
        stopSession();
    }

    if (command.zero)
    {
        trainerStateService.update([](TrainerState &state)
                                   {
            state.zero = false;
            return StateUpdateResult::CHANGED; },
                                   TRAINER_DEVICE_ORIGIN);
        zeroReference();
    }

    if (command.resetPeak)
    {
        peakMm = 0.0f;
        trainerStateService.update([](TrainerState &state)
                                   {
            state.resetPeak = false;
            return StateUpdateResult::CHANGED; },
                                   TRAINER_DEVICE_ORIGIN);
        setNotice("peak cleared");
    }

    if (command.goalLevel != lastGoalLevel)
    {
        lastGoalLevel = command.goalLevel;
        holdTracker.reset(cfg().hold_seconds);
        holdCounted = false;
        char buffer[48];
        if (lastGoalLevel > 0)
        {
            snprintf(buffer, sizeof(buffer), "goal %s at %d mm", trainer::levelName(lastGoalLevel), (int)lroundf(currentGoalMm()));
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "free play, no goal");
        }
        setNotice(buffer);
    }

    /* display_on is a state field only: the display task reads it, so nothing to
     * do here. */
}

/* -------------------------------------------------------------- snapshot ---- */

static void buildSnapshot()
{
    TrainerScreen screen;

    screen.depthMm = depthMm;
    screen.peakMm = peakMm;
    screen.maxDepthMm = cfg().max_depth_mm;
    screen.gauge = trainer::gaugeFraction(depthMm, cfg().max_depth_mm);
    screen.peakGauge = trainer::gaugeFraction(peakMm, cfg().max_depth_mm);
    screen.level = trainer::levelForDepth(depthMm, cfg().level1_mm, cfg().level2_mm, cfg().level3_mm);
    screen.goalLevel = lastGoalLevel;
    screen.goalMm = currentGoalMm();
    screen.holdProgress = holdTracker.progress();
    screen.holdSeconds = holdTracker.heldSeconds();
    screen.holdNeeded = holdTracker.neededSeconds();
    screen.inZone = holdTracker.inZone();
    screen.holdComplete = holdTracker.complete();
    screen.sessionActive = session.active();
    screen.sessionSeconds = session.summary().elapsedSeconds;
    screen.sessionAvgMm = session.summary().averageMm();
    screen.sessionHolds = session.summary().holds;
    screen.sensorOk = sensor.present();
    screen.batteryKnown = true; /* readBatteryVolts() always returns a value; on USB it is the charging voltage */
    screen.batteryV = readBatteryVolts();
    screen.unit = cfg().units;

    /* The footer is one short line; the panel has room for about 28 characters
     * at this size. */
    char footer[40];
    const char *model = sensor.present() ? sensor.modelName() : "no sensor";
    if (WiFi.status() == WL_CONNECTED)
    {
        snprintf(footer, sizeof(footer), "%s %s", WiFi.localIP().toString().c_str(), model);
    }
    else
    {
        snprintf(footer, sizeof(footer), "AP %s %s", WiFi.softAPIP().toString().c_str(), model);
    }
    strncpy(screen.footer, footer, sizeof(screen.footer) - 1);
    screen.footer[sizeof(screen.footer) - 1] = '\0';

    portENTER_CRITICAL(&snapshotMux);
    screenSnapshot = screen;
    portEXIT_CRITICAL(&snapshotMux);
}

/* --------------------------------------------------------------- publish ---- */

static void publishState()
{
    const float goal = currentGoalMm();
    const bool inZone = holdTracker.inZone();
    const bool holdComplete = holdTracker.complete();
    const float progress = holdTracker.progress();
    const float held = holdTracker.heldSeconds();
    const trainer::SessionSummary summary = session.summary();
    const bool sessionActive = session.active();
    const float range = (sensor.present() && trainer::rangeIsUsable(sensor.rangeMm())) ? sensor.rangeMm() : -1.0f;
    const uint8_t sensorStatus = sensor.present() ? sensor.status() : 0xFF;
    const String sensorStatusText = sensor.present() ? String(sensor.statusText()) : String("no sensor on the I2C bus");
    const String sensorModel = sensor.present() ? String(sensor.modelName()) : String("none");
    const String sensorMode = String(sensor.modeName());
    const float battery = readBatteryVolts();
    const String noticeText = (noticeSetMs != 0 && (millis() - noticeSetMs) > NOTICE_TIMEOUT_MS) ? String("") : notice;

    trainerStateService.update([&](TrainerState &state)
                               {
        bool changed = false;

        /* measured */
        if (state.sensorPresent != sensor.present()) { state.sensorPresent = sensor.present(); changed = true; }
        if (state.sensorModelId != (int)sensor.modelId()) { state.sensorModelId = (int)sensor.modelId(); changed = true; }
        if (state.sensorModel != sensorModel) { state.sensorModel = sensorModel; changed = true; }
        if (state.sensorMode != sensorMode) { state.sensorMode = sensorMode; changed = true; }
        if (state.sensorStatus != (int)sensorStatus) { state.sensorStatus = (int)sensorStatus; changed = true; }
        if (state.sensorStatusText != sensorStatusText) { state.sensorStatusText = sensorStatusText; changed = true; }
        if (fabsf(state.rangeMm - range) >= 0.5f) { state.rangeMm = range; changed = true; }
        if (state.sensorGood != sensor.goodSamples()) { state.sensorGood = sensor.goodSamples(); changed = true; }
        if (state.sensorBad != sensor.badSamples()) { state.sensorBad = sensor.badSamples(); changed = true; }

        /* derived */
        if (fabsf(state.depthMm - depthMm) >= DEPTH_PUSH_THRESHOLD_MM) { state.depthMm = depthMm; changed = true; }
        if (fabsf(state.depthRawMm - depthRawMm) >= 1.0f) { state.depthRawMm = depthRawMm; changed = true; }
        if (fabsf(state.peakMm - peakMm) >= 0.5f) { state.peakMm = peakMm; changed = true; }
        const float gauge = trainer::gaugeFraction(depthMm, cfg().max_depth_mm);
        if (fabsf(state.gauge - gauge) >= 0.005f) { state.gauge = gauge; changed = true; }
        const int level = trainer::levelForDepth(depthMm, cfg().level1_mm, cfg().level2_mm, cfg().level3_mm);
        if (state.level != level) { state.level = level; changed = true; }
        const String levelName = String(trainer::levelName(level));
        if (state.levelName != levelName) { state.levelName = levelName; changed = true; }
        if (state.goalLevel != lastGoalLevel) { state.goalLevel = lastGoalLevel; changed = true; }
        if (fabsf(state.goalMm - goal) >= 0.5f) { state.goalMm = goal; changed = true; }
        if (state.inZone != inZone) { state.inZone = inZone; changed = true; }
        if (state.holdComplete != holdComplete) { state.holdComplete = holdComplete; changed = true; }
        if (fabsf(state.holdProgress - progress) >= 0.01f) { state.holdProgress = progress; changed = true; }
        if (fabsf(state.holdSecondsHeld - held) >= 0.05f) { state.holdSecondsHeld = held; changed = true; }
        if (fabsf(state.holdSecondsNeeded - holdTracker.neededSeconds()) >= 0.05f) { state.holdSecondsNeeded = holdTracker.neededSeconds(); changed = true; }

        /* session */
        if (state.sessionActive != sessionActive) { state.sessionActive = sessionActive; changed = true; }
        if (fabsf(state.sessionSeconds - summary.elapsedSeconds) >= 0.2f) { state.sessionSeconds = summary.elapsedSeconds; changed = true; }
        if (fabsf(state.sessionAvgMm - summary.averageMm()) >= 0.5f) { state.sessionAvgMm = summary.averageMm(); changed = true; }
        if (fabsf(state.sessionDeepSeconds - summary.deepSeconds) >= 0.2f) { state.sessionDeepSeconds = summary.deepSeconds; changed = true; }
        if (state.sessionHolds != summary.holds) { state.sessionHolds = summary.holds; changed = true; }
        if (fabsf(state.lastSessionPeakMm - lastSession.peakMm) >= 0.5f) { state.lastSessionPeakMm = lastSession.peakMm; changed = true; }
        if (fabsf(state.lastSessionAvgMm - lastSession.averageMm()) >= 0.5f) { state.lastSessionAvgMm = lastSession.averageMm(); changed = true; }
        if (fabsf(state.lastSessionSeconds - lastSession.elapsedSeconds) >= 0.2f) { state.lastSessionSeconds = lastSession.elapsedSeconds; changed = true; }
        if (state.lastSessionHolds != lastSession.holds) { state.lastSessionHolds = lastSession.holds; changed = true; }
        if (state.lastSessionAt != lastSessionAtEpoch) { state.lastSessionAt = lastSessionAtEpoch; changed = true; }

        /* board */
        if (state.displayPresent != display.present()) { state.displayPresent = display.present(); changed = true; }
        if (state.displayFrames != display.frames()) { state.displayFrames = display.frames(); changed = true; }
        if (state.displayBrightness != display.brightness()) { state.displayBrightness = display.brightness(); changed = true; }
        if (state.touchPresent != touch.present()) { state.touchPresent = touch.present(); changed = true; }
        if (state.touchPresses != touch.presses()) { state.touchPresses = touch.presses(); changed = true; }
        if (fabsf(state.batteryV - battery) >= 0.05f) { state.batteryV = battery; changed = true; }
        if (!state.batteryKnown) { state.batteryKnown = true; changed = true; }

        /* settings mirror */
        if (state.units != cfg().units) { state.units = cfg().units; changed = true; }
        if (fabsf(state.referenceMm - cfg().reference_mm) >= 0.5f) { state.referenceMm = cfg().reference_mm; changed = true; }
        if (state.autoAdvance != cfg().auto_advance) { state.autoAdvance = cfg().auto_advance; changed = true; }
        if (state.notice != noticeText) { state.notice = noticeText; changed = true; }

        return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED; },
                               TRAINER_DEVICE_ORIGIN);
}

/* ------------------------------------------------------------------ touch ---- */

static void handleTouch()
{
    const TrainerTouch::Event event = touch.takeEvent();
    if (event == TrainerTouch::Event::none)
    {
        return;
    }

    lastActivityMs = millis();

    if (event == TrainerTouch::Event::longPress)
    {
        zeroReference();
        return;
    }

    /* A tap starts a session, and a tap during a session ends it. */
    if (session.active())
    {
        stopSession();
    }
    else
    {
        startSession();
    }
}

/* ---------------------------------------------------------------- tasks ---- */

void trainerTask(void *pvParameters)
{
    (void)pvParameters;

    const TickType_t period = pdMS_TO_TICKS(1000 / TRAINER_SENSOR_TASK_HZ);
    TickType_t lastWake = xTaskGetTickCount();

    uint32_t lastSampleUs = micros();
    uint32_t tick = 0;

    for (;;)
    {
        const uint32_t nowUs = micros();
        float dt = (float)(nowUs - lastSampleUs) / 1000000.0f;
        if (dt > TRAINER_MAX_STEP_SECONDS)
        {
            dt = TRAINER_MAX_STEP_SECONDS;
        }
        lastSampleUs = nowUs;

        /* The sensor and the touch controller share one bus, so both are polled
         * from this task and from nowhere else. */
        const bool newSample = sensor.poll();
        if ((tick % TRAINER_TOUCH_EVERY_N) == 0)
        {
            touch.poll();
        }
        tick++;

        if (newSample)
        {
            if (trainer::rangeIsUsable(sensor.rangeMm()) && sensor.status() == 0)
            {
                lastValidSampleMs = millis();
                depthRawMm = trainer::depthFromRange(sensor.rangeMm(), cfg().calibration_mm,
                                                     cfg().reference_mm, cfg().max_depth_mm);
                depthMm = depthFilter.push(depthRawMm, dt, cfg().filter_tau_s);

                if (session.active())
                {
                    const float goal = currentGoalMm();
                    session.addSample(depthMm, dt, goal);
                }
                if (depthMm > peakMm)
                {
                    peakMm = depthMm;
                }
                if (fabsf(depthMm - depthRawMm) > DEPTH_PUSH_THRESHOLD_MM || depthMm > 5.0f)
                {
                    lastActivityMs = millis();
                }

                /* A goal is held when the depth stays at or above it. The
                 * tracker owns the timing, so a pause in the sample stream
                 * cannot complete a hold on its own. */
                const float goal = currentGoalMm();
                const bool atGoal = session.active() && lastGoalLevel > 0 && goal > 0.0f && depthMm >= goal;
                holdTracker.update(atGoal, dt);

                if (holdTracker.complete() && !holdCounted)
                {
                    holdCounted = true;
                    holdTracker.acknowledge();
                    session.countHold(cfg().hold_seconds);
                    char buffer[64];
                    snprintf(buffer, sizeof(buffer), "%s held for %.1f s", trainer::levelName(lastGoalLevel),
                             (double)cfg().hold_seconds);
                    setNotice(buffer);
                    esp32sveltekit.getNotificationService()->pushNotification(buffer, PUSHSUCCESS);

                    if (cfg().auto_advance && lastGoalLevel < 3)
                    {
                        lastGoalLevel++;
                        holdTracker.reset(cfg().hold_seconds);
                        holdCounted = false;
                    }
                    else if (!cfg().auto_advance)
                    {
                        holdCounted = false;
                    }
                }
                else if (!atGoal)
                {
                    holdCounted = false;
                }
            }
            else
            {
                /* An unusable sample is not a distance. The last depth stays on
                 * the gauge, but no time is added to the session. */
                depthRawMm = 0.0f;
            }
        }

        /* A session ends by itself when nothing is in front of the sensor for
         * session_stop_s, so walking away closes the run and stores its totals. */
        if (session.active() && lastValidSampleMs != 0 &&
            (millis() - lastValidSampleMs) > (uint32_t)(cfg().session_stop_s * 1000.0f))
        {
            setNotice("session ended: nothing in front of the sensor");
            stopSession();
        }

        handleTouch();

        if ((millis() - lastStatePushMs) >= STATE_PUSH_INTERVAL_MS)
        {
            lastStatePushMs = millis();
            buildSnapshot();
            publishState();
        }

        vTaskDelayUntil(&lastWake, period);
    }
}

void displayTask(void *pvParameters)
{
    (void)pvParameters;

    const TickType_t period = pdMS_TO_TICKS(1000 / TRAINER_DISPLAY_TASK_HZ);
    TickType_t lastWake = xTaskGetTickCount();
    bool dimmed = false;

    for (;;)
    {
        TrainerScreen screen;
        portENTER_CRITICAL(&snapshotMux);
        screen = screenSnapshot;
        portEXIT_CRITICAL(&snapshotMux);

        if (!display.present())
        {
            vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));
            continue;
        }

        /* The screen dims when nothing happens, and it refuses to dim while a
         * session runs: the readout is the point of the device then. */
        const uint32_t idleMs = millis() - lastActivityMs;
        const bool shouldDim = cfg().display_timeout_s > 0 &&
                               idleMs > (uint32_t)cfg().display_timeout_s * 1000u &&
                               !session.active();
        if (shouldDim != dimmed)
        {
            dimmed = shouldDim;
            display.setBrightness(shouldDim ? cfg().dim_brightness : cfg().display_brightness);
        }

        display.render(screen);

        vTaskDelayUntil(&lastWake, period);
    }
}

/* ------------------------------------------------------------------ setup ---- */

void setup()
{
    Serial.begin(SERIAL_BAUD_RATE);
    delay(200);

    /* --- the panel first: it brings up the I2C bus the sensor and the touch
     * controller need. --- */
    display.begin();

    Wire.setClock(TRAINER_I2C_SPEED);
    sensor.begin(Wire);
    touch.begin(Wire, display.bus());

    Serial.printf("[trainer] display %s, sensor %s (id 0x%02X) %s, touch %s\n",
                  display.present() ? "ok" : "MISSING",
                  sensor.present() ? sensor.modelName() : "MISSING", sensor.modelId(),
                  sensor.present() ? sensor.modeName() : "-",
                  touch.present() ? "ok" : "MISSING");

    /* --- network, web server, settings --- */
    esp32sveltekit.setMDNSAppName("DTT Trainer");
    esp32sveltekit.begin();

    trainerSettingsService.begin();
    trainerStateService.begin();

    /* The cached copy must exist before the tasks start. */
    refreshSettings(true);

    /* A settings change takes effect at once: brightness, filter and hold target
     * need no restart, and a unit change needs a full repaint. */
    trainerSettingsService.addUpdateHandler([&](const String &originId)
                                            { refreshSettings(false); },
                                            false);

    /* Command applier: runs on every state change a client caused. */
    trainerStateService.addUpdateHandler([&](const String &originId)
                                         { applyTrainerStateToSession(originId); },
                                         false);

    lastGoalLevel = 0;
    lastValidSampleMs = millis();
    lastActivityMs = millis();

    xTaskCreatePinnedToCore(trainerTask, "trainer", 8192, NULL, 3, &trainerTaskHandle, 1);
    xTaskCreatePinnedToCore(displayTask, "display", 4096, NULL, 2, &displayTaskHandle, 1);

    Serial.printf("DTT-Trainer %s on %s ready\n", APP_VERSION, BUILD_TARGET);
}

void loop()
{
    /* The framework runs its own network task and the trainer has its own tasks.
     * The Arduino loop task stays free. */
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}
