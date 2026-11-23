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
#include <esp_log.h>
#include <esp_wifi.h>

#include <ESP32SvelteKit.h>
#include <NotificationService.h>
#include <PsychicHttpServer.h>

#include "TrainerConfig.h"
#include "TrainerDisplay.h"
#include "TrainerConsole.h"
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

/* Set from the WiFi event task when a driver session starts or ends, so the radio
 * settings are applied from this project's own task and not inside a callback. */
static volatile bool radioReportDone = false;
static uint32_t lastStatusMs = 0;
static uint32_t lastRadioAttemptMs = 0;

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

/* Rep mode: a rep is the goal held for the hold time, and the depth has to come back
 * to the start before the next rep counts. The state machine itself is pure logic in
 * trainer_math.h, so the host tests cover it. This sits after cfg() because the mode
 * lives in the settings. */
static trainer::RepCount repCounter;
/* The rest between reps, timed from the moment the depth is back out and the next rep is armed. */
static trainer::RepBreak repBreak;

static bool playModeReps()
{
    return cfg().play_mode == 1;
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
static char notice[96] = "";
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

/* The palette, out of the settings and into the display. A change repaints the whole panel in the
 * new colours, which is what makes a colour picker in the browser land on the trainer. */
static TrainerColours g_appliedColours;
static bool g_coloursApplied = false;

static void applyColours()
{
    /* The compiled-in values are the fallback, so an empty or unparseable string leaves that part
     * of the panel looking the way it did rather than turning it black. */
    const TrainerColours fallback;
    TrainerColours wanted = fallback;
    trainer::parseHexColour(cfg().colour_bg, &wanted.bg, fallback.bg);
    trainer::parseHexColour(cfg().colour_track, &wanted.track, fallback.track);
    trainer::parseHexColour(cfg().colour_accent, &wanted.accent, fallback.accent);
    trainer::parseHexColour(cfg().colour_zone, &wanted.zone, fallback.zone);
    trainer::parseHexColour(cfg().colour_done, &wanted.done, fallback.done);
    trainer::parseHexColour(cfg().colour_goal, &wanted.goal, fallback.goal);
    trainer::parseHexColour(cfg().colour_peak, &wanted.peak, fallback.peak);
    trainer::parseHexColour(cfg().colour_text, &wanted.text, fallback.text);
    trainer::parseHexColour(cfg().colour_dim, &wanted.dim, fallback.dim);
    trainer::parseHexColour(cfg().colour_warn, &wanted.warn, fallback.warn);

    if (!g_coloursApplied || memcmp(&wanted, &g_appliedColours, sizeof(wanted)) != 0)
    {
        g_appliedColours = wanted;
        g_coloursApplied = true;
        display.setColours(wanted);
    }
}

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
/* Reads an integer out of the request's query string. The query arrives without its "?" - "x=12
 * and y=34" - so a key is a match at the start or right after an ampersand. A plain indexOf would
 * find the "x=" inside "max=". A missing or malformed key gives the fallback. */
static int queryInt(const String &query, const char *key, int fallback)
{
    const String head = String(key) + "=";
    int from = -1;
    if (query.startsWith(head))
    {
        from = head.length();
    }
    else
    {
        const String needle = String("&") + head;
        const int at = query.indexOf(needle);
        if (at < 0)
        {
            return fallback;
        }
        from = at + needle.length();
    }
    const int to = query.indexOf('&', from);
    const String value = (to < 0) ? query.substring(from) : query.substring(from, to);
    if (value.length() == 0)
    {
        return fallback;
    }
    return value.toInt();
}

static void openMenu();
static void closeMenu();
static void pressMenuChip();
static void applyMenuPress(int row, float dx);
static void refuseWhilePlaying(const char *what);

/* The panel menu. Its open flag is published in the state, so a client can see what the
 * glass is doing and can drive the same code path a finger uses - which is how the menu is
 * tested without a person standing at the board. The notice is the answer a refused tap
 * gets; the panel shows it for a couple of seconds. */
#define TRAINER_PANEL_ORIGIN "trainerPanel"
static bool menuOpen = false;
static char menuNotice[28] = "";
static uint32_t menuNoticeMs = 0;

/* The calibration guide: which step it is on, the two captured distances, and the window of
 * recent readings that decides whether the head has stopped moving. */
static uint8_t calStep = trainer::kCalStepOff;
static float calZeroMm = -1.0f;
static float calFullMm = -1.0f;
static float calSamples[trainer::kCalibrationSamples] = {0};
static int calSampleCount = 0;
static float calRangeMm = -1.0f;
static bool calStable = false;
static void startCalibration();
static void cancelCalibration();
static void captureCalibrationPoint();
static void calibrationSample();
static bool applyRadioSettings();
static bool radioSettingsApplied = false;
static void tryPlainJoin();
static void logRadioStatus();
static void printStatusLine();
static void i2cBusScan();
static void bootRadioScan();
void trainerTask(void *pvParameters);
void displayTask(void *pvParameters);

/* -------------------------------------------------------------- helpers ---- */

/* The goals as the pure maths wants them: the settings keep them as arrays, and the count
 * says how many of the ten are in use. */
static void levelBands(float lows[trainer::kMaxLevels], float highs[trainer::kMaxLevels])
{
    cfg().levels(lows, highs);
}

static int levelCount()
{
    return trainer::levelCountInUse(cfg().level_count);
}

/* The band of the level the user picked. Level 0 (free play) gives an empty band, which is
 * never in range, so free play counts nothing. */
static trainer::LevelBand currentBand()
{
    float lows[trainer::kMaxLevels];
    float highs[trainer::kMaxLevels];
    levelBands(lows, highs);
    return trainer::bandForLevel(lastGoalLevel, lows, highs, levelCount(), TRAINER_MIN_BAND_MM);
}

static float currentGoalMm()
{
    return currentBand().low;
}

static float currentGoalLimitMm()
{
    return currentBand().high;
}

static void setNotice(const char *text)
{
    /* A fixed array, not a String: this is written from the touch handler (trainer
     * task) and from the command handler (network task) and read by both as well. */
    snprintf(notice, sizeof(notice), "%s", (text != nullptr) ? text : "");
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

    /* The visible round window is not always centred on the panel, so the whole layout is
     * moved to match. setCentre decides for itself whether anything changed. */
    TrainerDisplay::setCentre(cfg().panel_centre_x, cfg().panel_centre_y);

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

/* ------------------------------------------------------------------ radio ---- */

/*
 * How far this device reaches is decided by three PHY settings, and all three
 * can be set in software:
 *
 *   - the country code. An ESP32 that was never given one runs as country "01",
 *     the worldwide safe set, which is the lowest common denominator of every
 *     regulation: less power and fewer channels than a real country allows.
 *   - the transmit power. Set it explicitly to the ceiling of the radio instead
 *     of trusting a default that another component could have lowered.
 *   - the channel width. HT40 doubles the occupied spectrum and the throughput,
 *     and costs link margin. This device sends a small JSON now and then, so HT20
 *     is the better trade at the edge of a network.
 *
 * What these do NOT fix: the T-RGB carries a small printed antenna on a board
 * full of a 480x480 display, and its radio is the weak part of the design. If the
 * scan below shows the home network at -85 dBm, no software will make that a good
 * link; move the device, or put a repeater between it and the router.
 */
/* Applies the PHY settings. Returns true when they took effect, so the caller can
 * tell "the driver is not up yet" from "done".
 *
 * The first attempt in this project's history ran once, right after
 * esp32sveltekit.begin(), and failed with ESP_ERR_WIFI_NOT_INIT: the framework
 * disconnects the station with the radio switched off while it starts, which
 * de-initializes the driver, and every call below needs a started WiFi driver.
 * The settings are therefore applied from a retry loop that runs while the driver
 * is up, once per driver session. */
static bool applyRadioSettings()
{
    if (WiFi.getMode() == WIFI_MODE_NULL)
    {
        return false; /* the driver is not initialized yet */
    }

    const esp_err_t country = esp_wifi_set_country_code(TRAINER_WIFI_COUNTRY, true);
    const esp_err_t power = esp_wifi_set_max_tx_power(TRAINER_WIFI_TX_POWER_QUARTER_DBM);
    const esp_err_t bandwidth = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);

    /* Modem sleep wakes the radio only on the access point's beacon, which is
     * good for a battery and bad for a round trip. Off here: the trainer is on a
     * desk, and the web UI is used while the session runs. */
    WiFi.setSleep(false);

    const bool ok = (country == ESP_OK) && (power == ESP_OK) && (bandwidth == ESP_OK);
    Serial.printf("[wifi] radio: country %s %s, 19.5 dBm %s, HT20 %s, sleep off\n",
                  TRAINER_WIFI_COUNTRY,
                  country == ESP_OK ? "ok" : esp_err_to_name(country),
                  power == ESP_OK ? "ok" : esp_err_to_name(power),
                  bandwidth == ESP_OK ? "ok" : esp_err_to_name(bandwidth));
    return ok;
}

/* What a disconnect reason means in practice, for the log line below. */
static const char *joinReasonHint(uint8_t reason)
{
    switch (reason)
    {
    case 15:
    case 204:
        return " (handshake timeout: most often a wrong password, or a signal too weak for the handshake)";
    case 201:
        return " (access point not found: check that the network is 2.4 GHz and in range)";
    case 202:
        return " (authentication refused: wrong password)";
    case 203:
        return " (association refused by the access point)";
    case 205:
        return " (the access point dropped the connection)";
    case 2:
        return " (authentication expired)";
    case 200:
        return " (beacon timeout: the access point stopped answering)";
    default:
        return "";
    }
}

/* After a few failed joins the framework keeps pinning the channel and the BSSID
 * from its own scan. A plain join lets the station choose for itself, which is what
 * helps against a weak signal, a mesh or band steering. Tried rarely, and only while
 * the framework has not got the link up either. */
static uint8_t joinFailures = 0;
static uint32_t lastPlainJoinMs = 0;

/* The same last resort, and it alternates between two ways of joining:
 *
 *   - a plain join, which makes the station scan and pick an access point itself,
 *   - a join pinned to the channel and BSSID the framework stored from the last good
 *     scan, which skips the scan completely.
 *
 * On a link as weak as this board's, the scan is the expensive half of the work: it
 * takes the radio away from the join, and a failed scan is what leaves the framework
 * reporting "no suitable network". Trying both means a bad scan cannot block the join
 * for ever. */
static uint8_t plainJoinPinned = 0;

static void tryPlainJoin()
{
    if (WiFi.status() == WL_CONNECTED || joinFailures < 2)
    {
        return;
    }
    if ((millis() - lastPlainJoinMs) < 30000u)
    {
        return;
    }

    WiFiSettings stored;
    esp32sveltekit.getWiFiSettingsService()->read([&stored](WiFiSettings &value)
                                                  { stored = value; });
    if (stored.wifiSettings.empty())
    {
        return;
    }

    lastPlainJoinMs = millis();
    const wifi_settings_t &network = stored.wifiSettings.front();

    /* A stored channel and BSSID are usable when the framework has joined this
     * network at least once since it was saved. */
    uint8_t bssidSum = 0;
    for (size_t i = 0; i < sizeof(network.bssid); i++)
    {
        bssidSum = (uint8_t)(bssidSum | network.bssid[i]);
    }
    const bool pinned = (network.channel > 0) && (bssidSum != 0);

    plainJoinPinned = (uint8_t)((plainJoinPinned + 1) & 1);

    /* disconnect without switching the radio off: this also cancels a scan that is
     * stuck in the running state, which is what keeps the framework from finding a
     * network again after a failed handshake. */
    WiFi.disconnect(false, false);
    delay(50);

    if (pinned && plainJoinPinned == 1)
    {
        Serial.printf("[wifi] %u joins failed, joining \"%s\" directly on channel %d at %02X:%02X:..:%02X, without a scan\n",
                      (unsigned)joinFailures, network.ssid.c_str(), (int)network.channel,
                      network.bssid[0], network.bssid[1], network.bssid[5]);
        WiFi.begin(network.ssid.c_str(), network.password.c_str(), network.channel, network.bssid, true);
        return;
    }

    Serial.printf("[wifi] %u joins failed, trying a plain join to \"%s\" without the pinned access point\n",
                  (unsigned)joinFailures, network.ssid.c_str());
    WiFi.begin(network.ssid.c_str(), network.password.c_str());
}

/* One report, a few seconds after boot: either the joined network with its signal
 * strength, or a scan of what the radio can hear at all. The second case is the
 * one that explains a device which will not join: the ESP32 has no 5 GHz radio, so
 * a 5 GHz-only SSID never appears in this list, and neither does a network that is
 * simply too far away. */
static void logRadioStatus()
{
    radioReportDone = true;

    /* What the user stored, so a wrong password length or a typo in the SSID shows
     * up here. The password itself is never printed. */
    {
        WiFiSettings stored;
        esp32sveltekit.getWiFiSettingsService()->read([&stored](WiFiSettings &value)
                                                      { stored = value; });
        Serial.printf("[wifi] %u stored network(s):", (unsigned)stored.wifiSettings.size());
        for (auto &network : stored.wifiSettings)
        {
            /* A password that was pasted with a trailing space, or with a character
             * outside printable ASCII, fails the handshake and looks exactly like a
             * wrong password. Report the shape, never the password. */
            bool space = false;
            bool control = false;
            for (size_t i = 0; i < network.password.length(); i++)
            {
                const char c = network.password[i];
                if (c == ' ')
                {
                    space = true;
                }
                if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E)
                {
                    control = true;
                }
            }
            Serial.printf(" \"%s\" (%u character password%s%s)", network.ssid.c_str(),
                          (unsigned)network.password.length(),
                          space ? ", contains a space" : "",
                          control ? ", contains a non-printable character" : "");
        }
        Serial.println();
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.printf("[wifi] connected to \"%s\", %d dBm, channel %d, IP %s\n",
                      WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.channel(),
                      WiFi.localIP().toString().c_str());
        return;
    }

    /* A negative return is a failed scan, not an empty band. Saying "nothing at
     * all" for a scan that never ran would send the reader after the antenna for
     * no reason. The framework scans every five seconds while it cannot join, so a
     * second scan from here usually loses the race: one attempt, then report the
     * state without the list. */
    const int found = WiFi.scanNetworks(false /* async */, false /* show hidden */, false /* passive */, 300 /* ms per channel */);
    if (found < 0)
    {
        Serial.printf("[wifi] not connected, and this scan returned %d (%s); the framework scans as well, so this is usually just a collision\n",
                      found, (found == -1) ? "still running" : "failed");
        return;
    }

    Serial.printf("[wifi] not connected. %d network(s) in range:\n", found);
    for (int i = 0; i < found && i < 12; i++)
    {
        Serial.printf("[wifi]   %4d dBm  channel %2d  %s%s\n", (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                      WiFi.SSID(i).c_str(), (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "  (open)" : "");
    }
    if (found == 0)
    {
        Serial.println("[wifi] the radio hears nothing. Check that the network is 2.4 GHz and in range: this radio cannot see 5 GHz.");
    }
    WiFi.scanDelete();
}

/* The same look, once, before the framework brings up its own access point: with
 * nothing else running, an empty result means the radio really hears nothing. */
static void bootRadioScan()
{
    WiFi.mode(WIFI_STA);
    delay(150);

    const int found = WiFi.scanNetworks(false, false, false, 300);
    if (found < 0)
    {
        Serial.printf("[wifi] boot scan failed with code %d (-1 running, -2 failed)\n", found);
    }
    else
    {
        Serial.printf("[wifi] boot scan, before any access point of this device: %d network(s)\n", found);
        for (int i = 0; i < found && i < 12; i++)
        {
            Serial.printf("[wifi]   %4d dBm  channel %2d  %s\n",
                          (int)WiFi.RSSI(i), (int)WiFi.channel(i), WiFi.SSID(i).c_str());
        }
        if (found == 0)
        {
            Serial.println("[wifi] nothing at all. The radio does not hear a single access point, before anything of this device is transmitting.");
        }
    }
    WiFi.scanDelete();
}

/* Which devices answer on the one I2C bus. Printed at start-up because that is
 * the first thing to check when the sensor or the touch does nothing: it says
 * whether the part is on the bus at all, instead of leaving a guess. */
static void i2cBusScan()
{
    Serial.printf("[i2c] scan of SDA %d / SCL %d:", TRAINER_I2C_SDA, TRAINER_I2C_SCL);
    uint8_t found = 0;
    for (uint8_t address = 0x08; address < 0x78; address++)
    {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() != 0)
        {
            continue;
        }
        const char *who = "";
        if (address == TRAINER_XL9535_ADDR)
        {
            who = " XL9535 (panel expander)";
        }
        else if (address == TRAINER_SENSOR_ADDR)
        {
            who = " VL6180X / VL6180 (distance sensor)";
        }
        else if (address == TRAINER_TOUCH_ADDR)
        {
            who = " CST820 / CST816 (touch)";
        }
        else if (address == 0x38)
        {
            who = " FT3267 (touch, half circle board)";
        }
        Serial.printf(" 0x%02X%s;", address, who);
        found++;
    }
    if (found == 0)
    {
        Serial.print(" nothing answered");
    }
    Serial.println();
}

/* One line with everything the board knows about itself. Printed at the end of
 * the radio report and then every minute: the USB serial port is opened by hand,
 * so anything printed during the first seconds is usually lost. */
static void printStatusLine()
{
    char wifi[64];
    const wifi_mode_t mode = WiFi.getMode();
    const char *modeName = (mode == WIFI_MODE_APSTA) ? "AP+STA" : ((mode == WIFI_MODE_AP) ? "AP" : "STA");
    if (WiFi.status() == WL_CONNECTED)
    {
        snprintf(wifi, sizeof(wifi), "%s %s %d dBm", modeName, WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    else
    {
        snprintf(wifi, sizeof(wifi), "%s %s", modeName, WiFi.softAPIP().toString().c_str());
    }

    char sensorText[80];
    if (sensor.present())
    {
        snprintf(sensorText, sizeof(sensorText), "%s 0x%02X %s range %d mm depth %d mm",
                 sensor.modelName(), sensor.modelId(), sensor.modeName(),
                 (int)lroundf(trainer::rangeIsUsable(sensor.rangeMm()) ? sensor.rangeMm() : -1.0f),
                 (int)lroundf(depthMm));
    }
    else
    {
        snprintf(sensorText, sizeof(sensorText), "not on the bus");
    }

    Serial.printf("[status] up %u s | display %s | sensor %s | touch %s | wifi %s | radio %s | peak %d mm | session %s %d s, %u holds\n",
                  (unsigned)(millis() / 1000),
                  display.present() ? "ok" : "not started",
                  sensorText,
                  touch.stateName(),
                  wifi,
                  radioSettingsApplied ? "DE 19.5 dBm HT20" : "pending",
                  (int)lroundf(peakMm),
                  session.active() ? "on" : "off",
                  (int)lroundf(session.summary().elapsedSeconds),
                  (unsigned)session.summary().holds);
}

/* ------------------------------------------------------------- sessions ---- */

static void startSession()
{
    /* A play always measures, so the sensor comes back before the first sample is wanted. */
    sensor.resume();
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
    repCounter.reset(cfg().reps_target); /* rep mode starts a new set */
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

/* A client command arrives from the network task, and only its flags are read here -
 * never a copy of the whole state. The framework calls this handler AFTER it has
 * released the service lock, and TrainerState carries Arduino Strings that the
 * trainer task writes at the same moment. A String shares its buffer by reference
 * count, and that count is not thread safe, so copying the state here corrupts a
 * buffer and the next free() aborts inside the heap:
 *
 *     free() -> poison_allocated_region -> esp_psram_check_ptr_addr   (assert)
 *
 * which is the crash that killed the httpd task twice. Every flag below is a plain
 * value, so reading them one by one under the lock is safe. */
struct TrainerCommand
{
    bool sessionStart;
    bool sessionStop;
    bool zero;
    bool resetPeak;
    int goalLevel;
    bool menuOpen;
    int menuPress;
};

static void applyTrainerStateToSession(const String &originId)
{
    if (originId == TRAINER_DEVICE_ORIGIN)
    {
        return; /* telemetry push, not a command */
    }

    TrainerCommand command = {};
    trainerStateService.read([&command](TrainerState &state)
                             {
        command.sessionStart = state.sessionStart;
        command.sessionStop = state.sessionStop;
        command.zero = state.zero;
        command.resetPeak = state.resetPeak;
        command.goalLevel = state.goalLevel;
        command.menuOpen = state.menuOpen;
        command.menuPress = state.menuPress; });

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
            snprintf(buffer, sizeof(buffer), "goal %s: %d-%d mm", trainer::levelName(lastGoalLevel),
                     (int)lroundf(currentGoalMm()), (int)lroundf(currentGoalLimitMm()));
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "free play, no goal");
        }
        setNotice(buffer);
    }

    /* The panel menu. The panel sets this flag itself; a client may set it too, which is
     * how the menu is exercised without a finger on the glass. Both go through the same
     * open, which refuses while a session runs. */
    if (command.menuOpen != menuOpen)
    {
        if (command.menuOpen)
        {
            openMenu();
        }
        else
        {
            closeMenu();
        }
        /* openMenu() may have refused. Publish what really happened rather than leaving a
         * client believing the menu came up. */
        const bool wanted = menuOpen;
        trainerStateService.update([wanted](TrainerState &state)
                                   {
            state.menuOpen = wanted;
            return StateUpdateResult::CHANGED; },
                                   TRAINER_DEVICE_ORIGIN);
    }

    /* A row press, consumed the same way session_start is: cleared first, then acted on, so
     * the same press cannot land twice. A row means nothing unless the menu is on the
     * screen, exactly as it does under a finger: both inputs run the same path. */
    if (command.menuPress >= 0)
    {
        const int press = command.menuPress;
        trainerStateService.update([](TrainerState &state)
                                   {
            state.menuPress = -1;
            return StateUpdateResult::CHANGED; },
                                   TRAINER_DEVICE_ORIGIN);
        if (press == trainer::kMenuPressCapture)
        {
            /* The guide's capture. A finger reaches it by tapping while the guide is up; this
             * is the same call, so the wizard can be exercised without one. */
            captureCalibrationPoint();
        }
        else if (press == trainer::kMenuPressChip)
        {
            pressMenuChip();
        }
        else if (menuOpen && press < trainer::kMenuRowCount)
        {
            applyMenuPress(press, 0.0f);
        }
    }

    /* display_on is a state field only: the display task reads it, so nothing to
     * do here. */
}

/* -------------------------------------------------------------- snapshot ---- */

/* Start a panel animation. The trainer task calls this when a rep counts and when the
 * set is finished; a client can call it through the state endpoint too, which is how the
 * animation is checked without doing a rep. Only the kind, the number and the words are
 * passed in: the start time comes from the device clock. */
static void startPanelEffect(uint8_t kind, uint16_t count, const char *text)
{
    trainerStateService.update([&](TrainerState &state)
                               {
        state.fx = kind;
        state.fxStartMs = millis();
        state.fxCount = count;
        snprintf(state.fxText, sizeof(state.fxText), "%s", (text != nullptr) ? text : "");
        return StateUpdateResult::CHANGED; },
                               TRAINER_DEVICE_ORIGIN);
}

static void buildSnapshot()
{
    TrainerScreen screen;

    /* A play that is running owns the glass. The menu closes itself the moment a set
     * starts, whichever way it was started - a tap on the panel, the web interface, or a
     * level that climbed into the next one. */
    if (menuOpen && session.active())
    {
        closeMenu();
        setNotice("menu closed - a play is running");
    }

    screen.depthMm = depthMm;
    screen.peakMm = peakMm;
    screen.maxDepthMm = cfg().max_depth_mm;
    screen.gauge = trainer::gaugeFraction(depthMm, cfg().max_depth_mm);
    screen.peakGauge = trainer::gaugeFraction(peakMm, cfg().max_depth_mm);
    float lows[trainer::kMaxLevels];
    float highs[trainer::kMaxLevels];
    levelBands(lows, highs);
    screen.level = trainer::levelForDepth(depthMm, lows, levelCount());
    screen.goalLevel = lastGoalLevel;
    screen.goalMm = currentGoalMm();
    screen.goalLimitMm = currentGoalLimitMm();
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

    /* Rep mode: the count replaces the level name on the panel. */
    screen.repMode = playModeReps();
    screen.repCount = repCounter.count;
    screen.repBreakS = repBreak.seconds(millis());
    screen.repTarget = cfg().reps_target;

    /* The panel menu. Its own flag and notice come from this task; the mode and the level
     * it shows are the ones already in use, so the menu never displays a second truth. */
    screen.menuOpen = menuOpen;
    screen.menuLocked = !trainer::menuUsable(session.active());
    screen.menuMode = (uint8_t)trainer::clampPlayMode((int)cfg().play_mode);
    screen.menuLevel = lastGoalLevel;
    screen.levelCount = (uint8_t)levelCount();
    strncpy(screen.menuNotice, menuNotice, sizeof(screen.menuNotice) - 1);
    screen.menuNotice[sizeof(screen.menuNotice) - 1] = '\0';
    screen.menuNoticeMs = menuNoticeMs;

    /* The calibration guide, when it runs. It needs the same snapshot the other screens do. */
    screen.calStep = (uint8_t)calStep;
    screen.calRangeMm = calRangeMm;
    screen.calZeroMm = calZeroMm;
    screen.calFullMm = calFullMm;
    screen.calStable = calStable;

    /* A panel animation in flight. Its start time lives in the state, so it survives
     * this snapshot being rebuilt ten times a second and ends on its own clock. */
    trainerStateService.read([&screen](TrainerState &state)
                             {
        screen.fx = state.fx;
        screen.fxStartMs = state.fxStartMs;
        screen.fxCount = state.fxCount;
        strncpy(screen.fxText, state.fxText, sizeof(screen.fxText) - 1);
        screen.fxText[sizeof(screen.fxText) - 1] = '\0';
    });

    /* The footer is one short line; the panel has room for about 28 characters
     * at this size. The signal strength earns its place there: a weak link is the
     * usual reason this device is hard to reach. */
    char footer[40];
    if (!sensor.present())
    {
        snprintf(footer, sizeof(footer), "no sensor on I2C");
    }
    else if (WiFi.status() == WL_CONNECTED)
    {
        snprintf(footer, sizeof(footer), "%s %ddBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    else
    {
        snprintf(footer, sizeof(footer), "AP %s", WiFi.softAPIP().toString().c_str());
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
    const float goalLimit = currentGoalLimitMm();
    const bool inZone = holdTracker.inZone();
    const bool holdComplete = holdTracker.complete();
    const float progress = holdTracker.progress();
    const float held = holdTracker.heldSeconds();
    const trainer::SessionSummary summary = session.summary();
    const bool sessionActive = session.active();
    const float range = (sensor.present() && trainer::rangeIsUsable(sensor.rangeMm())) ? sensor.rangeMm() : -1.0f;
    const uint8_t sensorStatus = sensor.present() ? sensor.status() : 0xFF;
    const char *sensorStatusText = sensor.present() ? sensor.statusText() : "no sensor on the I2C bus";
    const char *sensorModel = sensor.present() ? sensor.modelName() : "none";
    const char *sensorMode = sensor.modeName();
    const float battery = readBatteryVolts();

    /* A notice clears itself after a while. */
    if (noticeSetMs != 0 && (millis() - noticeSetMs) > NOTICE_TIMEOUT_MS)
    {
        notice[0] = '\0';
    }

    trainerStateService.update([&](TrainerState &state)
                               {
        bool changed = false;

        /* measured */
        if (state.sensorPresent != sensor.present()) { state.sensorPresent = sensor.present(); changed = true; }
        if (state.sensorModelId != (int)sensor.modelId()) { state.sensorModelId = (int)sensor.modelId(); changed = true; }
        if (strcmp(state.sensorModel, sensorModel) != 0) { snprintf(state.sensorModel, sizeof(state.sensorModel), "%s", sensorModel); changed = true; }
        if (strcmp(state.sensorMode, sensorMode) != 0) { snprintf(state.sensorMode, sizeof(state.sensorMode), "%s", sensorMode); changed = true; }
        if (state.sensorStatus != (int)sensorStatus) { state.sensorStatus = (int)sensorStatus; changed = true; }
        if (strcmp(state.sensorStatusText, sensorStatusText) != 0) { snprintf(state.sensorStatusText, sizeof(state.sensorStatusText), "%s", sensorStatusText); changed = true; }
        if (fabsf(state.rangeMm - range) >= 0.5f) { state.rangeMm = range; changed = true; }
        if (state.sensorGood != sensor.goodSamples()) { state.sensorGood = sensor.goodSamples(); changed = true; }
        if (state.sensorBad != sensor.badSamples()) { state.sensorBad = sensor.badSamples(); changed = true; }

        /* derived */
        if (fabsf(state.depthMm - depthMm) >= DEPTH_PUSH_THRESHOLD_MM) { state.depthMm = depthMm; changed = true; }
        if (fabsf(state.depthRawMm - depthRawMm) >= 1.0f) { state.depthRawMm = depthRawMm; changed = true; }
        if (fabsf(state.peakMm - peakMm) >= 0.5f) { state.peakMm = peakMm; changed = true; }
        const float gauge = trainer::gaugeFraction(depthMm, cfg().max_depth_mm);
        if (fabsf(state.gauge - gauge) >= 0.005f) { state.gauge = gauge; changed = true; }
        float goalLows[trainer::kMaxLevels];
        float goalHighs[trainer::kMaxLevels];
        levelBands(goalLows, goalHighs);
        const int level = trainer::levelForDepth(depthMm, goalLows, levelCount());
        if (state.level != level) { state.level = level; changed = true; }
        const char *levelName = trainer::levelName(level);
        if (strcmp(state.levelName, levelName) != 0) { snprintf(state.levelName, sizeof(state.levelName), "%s", levelName); changed = true; }
        if (state.goalLevel != lastGoalLevel) { state.goalLevel = lastGoalLevel; changed = true; }
        if (fabsf(state.goalMm - goal) >= 0.5f) { state.goalMm = goal; changed = true; }
        if (fabsf(state.goalLimitMm - goalLimit) >= 0.5f) { state.goalLimitMm = goalLimit; changed = true; }
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
        if (state.playMode != cfg().play_mode) { state.playMode = cfg().play_mode; changed = true; }
        /* The menu's own view: whether it is up, and whether a set is running that makes it
         * refuse. Both are published so the glass can be read from the web interface. */
        if (state.menuOpen != menuOpen) { state.menuOpen = menuOpen; changed = true; }
        const bool menuLocked = !trainer::menuUsable(sessionActive);
        if (state.menuLocked != menuLocked) { state.menuLocked = menuLocked; changed = true; }
        if (state.repTarget != cfg().reps_target) { state.repTarget = cfg().reps_target; changed = true; }
        if (state.repCount != repCounter.count) { state.repCount = repCounter.count; changed = true; }
        if (state.repArmed != repCounter.armed) { state.repArmed = repCounter.armed; changed = true; }
        const uint16_t restSeconds = repBreak.seconds(millis());
        if (state.repBreakS != restSeconds) { state.repBreakS = restSeconds; changed = true; }
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

        /* The raw register diagnostics are gone now that the controller is proven to
         * work: four hex strings in every state push is work on the event path for
         * nothing in normal use. What stays is what a fault needs - the chip id,
         * whether it answers, and the two counters that separate a dead interrupt
         * line from a controller that answers but reports no point. */
        state.touchChipId = touch.chipId();
        state.touchIrq = touch.irqLevel();
        state.touchAutoSleep = touch.autoSleepDisabled();
        state.touchX = touch.x();
        state.touchY = touch.y();
        state.touchReads = touch.reads();
        state.touchReadFailures = touch.readFailures();
        state.touchIrqPulses = touch.irqPulses();
        state.touchFrames = touch.touchFrames();
        if (state.sensorSuspended != sensor.suspended()) { state.sensorSuspended = sensor.suspended(); changed = true; }
        if (state.calStep != calStep) { state.calStep = calStep; changed = true; }
        if (fabsf(state.calRangeMm - calRangeMm) >= 0.5f) { state.calRangeMm = calRangeMm; changed = true; }
        if (fabsf(state.calZeroMm - calZeroMm) >= 0.5f) { state.calZeroMm = calZeroMm; changed = true; }
        if (fabsf(state.calFullMm - calFullMm) >= 0.5f) { state.calFullMm = calFullMm; changed = true; }
        if (state.calStable != calStable) { state.calStable = calStable; changed = true; }
        if (fabsf(state.batteryV - battery) >= 0.05f) { state.batteryV = battery; changed = true; }
        if (!state.batteryKnown) { state.batteryKnown = true; changed = true; }

        /* settings mirror */
        if (state.units != cfg().units) { state.units = cfg().units; changed = true; }
        if (fabsf(state.referenceMm - cfg().reference_mm) >= 0.5f) { state.referenceMm = cfg().reference_mm; changed = true; }
        if (state.autoAdvance != cfg().auto_advance) { state.autoAdvance = cfg().auto_advance; changed = true; }
        if (strcmp(state.notice, notice) != 0) { snprintf(state.notice, sizeof(state.notice), "%s", notice); changed = true; }

        return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED; },
                               TRAINER_DEVICE_ORIGIN);
}

/* ------------------------------------------------------------------- menu ---- */

/* Anything the finger can change lands here while a set runs: the menu rows, the chip that
 * opens them, and the long press that zeros the depth scale. All of it decides what the set
 * counts, so all of it is fixed for the length of a set - changing the goal or the zero under
 * a running play makes the numbers meaningless, and the zero silently moves every reading. */
static void refuseWhilePlaying(const char *what)
{
    snprintf(menuNotice, sizeof(menuNotice), "PLAYING - %s LOCKED", what);
    menuNoticeMs = millis();
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s locked while a play is running", what);
    setNotice(buffer);
    Serial.printf("[menu] refused: a session is running (%s)\n", what);
}

static void closeMenu()
{
    /* Nothing to repaint by hand: the display draws the menu only while the snapshot says
     * it is open, and it repaints the whole frame when that flag goes away. */
    menuOpen = false;
}

/* What the menu chip does, for a finger and for a client that drives it: leave the calibration
 * guide if it is up, otherwise toggle the menu. One function, so the two cannot drift. */
static void pressMenuChip()
{
    if (calStep != trainer::kCalStepOff)
    {
        cancelCalibration();
        return;
    }
    if (menuOpen)
    {
        closeMenu();
        setNotice("menu closed");
    }
    else
    {
        openMenu();
    }
}

static void openMenu()
{
    if (!trainer::menuUsable(session.active()))
    {
        refuseWhilePlaying("MENU");
        return;
    }
    if (menuOpen)
    {
        return;
    }
    menuOpen = true;
    setNotice("menu open");
    Serial.println("[menu] open");
}

static void applyMenuPress(int row, float dx)
{
    if (!trainer::menuUsable(session.active()))
    {
        refuseWhilePlaying("MENU");
        return;
    }

    switch (row)
    {
    case trainer::kMenuRowMode:
    {
        const int mode = trainer::nextPlayMode((int)cfg().play_mode);
        trainerSettingsService.update([mode](TrainerSettings &value)
                                      {
            value.play_mode = (uint8_t)mode;
            return StateUpdateResult::CHANGED; },
                                      TRAINER_PANEL_ORIGIN);
        char buffer[48];
        snprintf(buffer, sizeof(buffer), "play: %s", trainer::playModeName(mode));
        setNotice(buffer);
        Serial.printf("[menu] %s\n", buffer);
        break;
    }
    case trainer::kMenuRowLevel:
    {
        /* The left third of the row steps back, the rest steps forward, so a level can be
         * reached from either side without walking the whole way round. */
        const int next = trainer::menuLevelStep(lastGoalLevel, levelCount(), trainer::menuStepDirection(dx));
        /* A distinct origin: the panel is a local controller, and the command handler
         * ignores the origin the device publishes its own telemetry under. */
        trainerStateService.update([next](TrainerState &state)
                                   {
            state.goalLevel = next;
            return StateUpdateResult::CHANGED; },
                                   TRAINER_PANEL_ORIGIN);
        char buffer[48];
        if (next == 0)
        {
            snprintf(buffer, sizeof(buffer), "level: free play");
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "level %d of %d", next, levelCount());
        }
        setNotice(buffer);
        Serial.printf("[menu] %s\n", buffer);
        break;
    }
    case trainer::kMenuRowCalibrate:
        /* Two points, and the guide takes the panel from here. */
        startCalibration();
        break;

    case trainer::kMenuRowClose:
        closeMenu();
        setNotice("menu closed");
        Serial.println("[menu] closed");
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------- calibration ---- */

/* The guide, as Basti asked for it: "put the head at 0, press touch; then put the head to full,
 * press touch". Those two readings are the whole depth scale - the first is the distance that
 * reads 0, the difference between them is the travel that reaches full scale - so the two
 * numbers the trainer uses come out of the two positions rather than out of a slider.
 *
 * A capture is only taken from a reading that has stopped moving. A head in motion gives a
 * number it was passing through, and a scale built on one of those is wrong everywhere. */

static void calibrationSample()
{
    const float range = sensor.rangeMm();
    if (!trainer::rangeIsUsable(range))
    {
        return; /* a missing sample must not enter the window as a number */
    }
    for (int i = trainer::kCalibrationSamples - 1; i > 0; --i)
    {
        calSamples[i] = calSamples[i - 1];
    }
    calSamples[0] = range;
    if (calSampleCount < trainer::kCalibrationSamples)
    {
        calSampleCount++;
    }
    calRangeMm = range; /* the newest first: what the sensor sees now */
    calStable = trainer::readingIsStable(calSamples, calSampleCount);
}

static void startCalibration()
{
    sensor.resume(); /* the whole point is to read the sensor at each end */
    calStep = trainer::kCalStepZero;
    calZeroMm = -1.0f;
    calFullMm = -1.0f;
    calSampleCount = 0;
    calRangeMm = sensor.rangeMm();
    calStable = false;
    closeMenu(); /* the guide takes the panel */
    setNotice("calibration: put the head at the 0 position");
    Serial.println("[cal] step 1: capture the 0 position");
}

static void cancelCalibration()
{
    if (calStep == trainer::kCalStepOff)
    {
        return;
    }
    calStep = trainer::kCalStepOff;
    setNotice("calibration cancelled");
    Serial.println("[cal] cancelled");
}

static void captureCalibrationPoint()
{
    if (calStep == trainer::kCalStepOff)
    {
        return;
    }

    if (calStep == trainer::kCalStepDone)
    {
        cancelCalibration();
        setNotice("calibration finished");
        return;
    }

    if (!calStable)
    {
        snprintf(menuNotice, sizeof(menuNotice), "HOLD STILL");
        menuNoticeMs = millis();
        setNotice("wait for the reading to stop moving");
        return;
    }

    const float captured = trainer::readingAverage(calSamples, calSampleCount);

    if (calStep == trainer::kCalStepZero)
    {
        calZeroMm = captured;
        calStep = trainer::kCalStepFull;
        calSampleCount = 0;
        calStable = false;
        setNotice("calibration: put the head at the full position");
        Serial.printf("[cal] 0 position captured at %d mm\n", (int)lroundf(calZeroMm));
        return;
    }

    /* The full position. */
    calFullMm = captured;
    float travel = 0.0f;
    if (!trainer::calibrationTravel(calZeroMm, calFullMm, &travel))
    {
        char buffer[72];
        snprintf(buffer, sizeof(buffer), "refused: travel %d mm, capture the 0 point again",
                 (int)lroundf(travel));
        setNotice(buffer);
        snprintf(menuNotice, sizeof(menuNotice), "TRAVEL %d mm - RETRY", (int)lroundf(travel));
        menuNoticeMs = millis();
        calStep = trainer::kCalStepZero;
        calSampleCount = 0;
        calStable = false;
        Serial.printf("[cal] %s\n", buffer);
        return;
    }

    /* Write it. The reference IS the captured 0 distance now, the travel is the scale, and the
     * old trim is meaningless against a new reference, so it goes to zero. */
    const float zero = calZeroMm;
    trainerSettingsService.update([zero, travel](TrainerSettings &value)
                                  {
        value.reference_mm = zero;
        value.max_depth_mm = travel;
        value.calibration_mm = 0.0f;
        return StateUpdateResult::CHANGED; },
                                  TRAINER_PANEL_ORIGIN);

    char buffer[80];
    snprintf(buffer, sizeof(buffer), "calibrated: 0 at %d mm, full at %d mm, travel %d mm",
             (int)lroundf(zero), (int)lroundf(calFullMm), (int)lroundf(travel));
    setNotice(buffer);
    Serial.printf("[cal] %s\n", buffer);
    esp32sveltekit.getNotificationService()->pushNotification(buffer, PUSHSUCCESS);
    calStep = trainer::kCalStepDone;
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

    /* Any touch wakes the sensor: the panel is being used, so the depth readout should be live
     * again rather than showing --. */
    sensor.resume();

    const float dx = (float)touch.x() - (float)cfg().panel_centre_x;
    const float dy = (float)touch.y() - (float)cfg().panel_centre_y;

    /* The chip answers over its whole target, whatever the press's length. A finger that rests
     * on a button a moment too long used to fall through to the long press, which solves the
     * depth zero - a hidden way to move the whole scale while aiming at a menu. */
    if (trainer::menuButtonHit(dx, dy))
    {
        pressMenuChip();
        return;
    }

    /* The calibration guide: a tap anywhere captures the reading on the glass, and a long press
     * gives up. Both are what the guide's own text says. */
    if (calStep != trainer::kCalStepOff)
    {
        if (event == TrainerTouch::Event::longPress)
        {
            cancelCalibration();
            return;
        }
        captureCalibrationPoint();
        return;
    }

    if (event == TrainerTouch::Event::longPress)
    {
        if (!trainer::menuUsable(session.active()))
        {
            refuseWhilePlaying("ZERO");
            return;
        }
        zeroReference();
        return;
    }

    if (menuOpen)
    {
        const int row = trainer::menuRowAt(dy);
        if (row >= 0)
        {
            applyMenuPress(row, dx);
        }
        return; /* a tap that misses every row leaves the menu up */
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
    uint32_t lastLoopMs = millis();
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

        /* A blocked host is not a broken sensor: a WiFi scan or a panel repaint can
         * hold this task for seconds, and the sensor must not conclude from that
         * silence that continuous mode does not work. */
        const uint32_t loopGapMs = millis() - lastLoopMs;
        lastLoopMs = millis();
        if (loopGapMs > 500)
        {
            sensor.accountHostGap(loopGapMs);
        }

        /* Reps need something to hold. With free play selected there is no goal, so a
         * set would count nothing forever: rep mode takes the first level of the walk
         * instead, which is the last level when the levels count down. */
        if (playModeReps() && lastGoalLevel == 0)
        {
            lastGoalLevel = trainer::firstLevel(levelCount(), cfg().level_count_down);
            repCounter.reset(cfg().reps_target);
            holdTracker.reset(cfg().hold_seconds);
            holdCounted = false;
            setNotice("reps: taking the first level");
        }

        /* The radio: the PHY settings need a started WiFi driver, and the framework
         * switches the radio off while it starts, so this retries until they take
         * effect. The flag is re-armed by every station start and disconnect. */
        if (!radioSettingsApplied && (millis() - lastRadioAttemptMs) >= 1000)
        {
            lastRadioAttemptMs = millis();
            if (WiFi.status() != WL_CONNECTED && applyRadioSettings())
            {
                radioSettingsApplied = true;
            }
        }
        /* A last resort for a network this station cannot join with the channel and
         * access point the framework pinned from its scan. */
        tryPlainJoin();
        /* One report a few seconds after boot, but never while the framework is still
         * trying to join: a scan takes the radio away from the join attempt, and at a
         * weak signal that is the difference between joining and not. A connected
         * device reports at once; a disconnected one waits for the grace period, so
         * the framework gets an uninterrupted run of attempts first. */
        if (!radioReportDone && millis() > TRAINER_WIFI_REPORT_MS &&
            (WiFi.status() == WL_CONNECTED || millis() > TRAINER_WIFI_REPORT_MS + TRAINER_WIFI_REPORT_GRACE_MS))
        {
            logRadioStatus();
            printStatusLine();
        }
        if ((millis() - lastStatusMs) >= TRAINER_STATUS_INTERVAL_MS)
        {
            lastStatusMs = millis();
            printStatusLine();
        }

        /* The sensor sleeps while nothing runs: no samples, no I2C traffic, and the panel shows
         * -- instead of a number nobody is measuring. A touch or a session wakes it. The guide
         * needs it awake for as long as it runs. */
        if (calStep != trainer::kCalStepOff)
        {
            calibrationSample();
        }
        else if (!session.active() && !menuOpen && !sensor.suspended() &&
                 (millis() - lastActivityMs) > TRAINER_SENSOR_IDLE_MS)
        {
            sensor.suspend();
        }

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

                /* A goal counts while the depth is inside its band: at or past the level,
                 * and not past the level's upper limit. The tracker owns the timing, so a
                 * pause in the sample stream cannot complete a hold on its own. */
                const trainer::LevelBand band = currentBand();
                const bool atGoal = session.active() && trainer::inBand(depthMm, band);
                holdTracker.update(atGoal, dt);

                if (holdTracker.complete() && !holdCounted)
                {
                    holdCounted = true;
                    holdTracker.acknowledge();
                    session.countHold(cfg().hold_seconds);

                    if (playModeReps())
                    {
                        /* One rep for each completed hold. The next rep can only count
                         * after the depth came back to the start, which the release
                         * check below arms, so this is a set of real repetitions and
                         * not one long hold. */
                        char buffer[64];
                        const uint16_t target = cfg().reps_target;
                        if (repCounter.countRep(target))
                        {
                            if (target > 0)
                            {
                                snprintf(buffer, sizeof(buffer), "rep %u of %u", (unsigned)repCounter.count, (unsigned)target);
                            }
                            else
                            {
                                snprintf(buffer, sizeof(buffer), "rep %u", (unsigned)repCounter.count);
                            }
                            setNotice(buffer);
                            esp32sveltekit.getNotificationService()->pushNotification(buffer, PUSHSUCCESS);

                            /* The big one on the panel: the count, and the reminder to
                             * come all the way back out before the next rep. */
                            startPanelEffect(1, repCounter.count, nullptr);

                            if (repCounter.targetReached)
                            {
                                const uint16_t done = repCounter.count;
                                /* With climbing on, the set is followed by the next level
                                 * and another set; the run ends on the last level, with the
                                 * praise. Without climbing, every set is the whole thing. */
                                const int next = cfg().auto_advance
                                                     ? trainer::nextLevel(lastGoalLevel, levelCount(), cfg().level_count_down)
                                                     : 0;
                                if (next > 0)
                                {
                                    lastGoalLevel = next;
                                    repCounter.reset(cfg().reps_target);
                                    holdTracker.reset(cfg().hold_seconds);
                                    holdCounted = false;
                                    snprintf(buffer, sizeof(buffer), "set done - %u reps, next %s",
                                             (unsigned)done, trainer::levelName(next));
                                    setNotice(buffer);
                                    esp32sveltekit.getNotificationService()->pushNotification(buffer, PUSHSUCCESS);
                                }
                                else
                                {
                                    snprintf(buffer, sizeof(buffer), "set done - %u reps", (unsigned)done);
                                    setNotice(buffer);
                                    esp32sveltekit.getNotificationService()->pushNotification(buffer, PUSHSUCCESS);
                                    stopSession();
                                    /* The praise takes the panel after the session has been
                                     * closed, so the count is read before that. */
                                    startPanelEffect(2, done, cfg().praise);
                                }
                            }
                        }
                        holdCounted = false;
                    }
                    else
                    {
                        char buffer[64];
                        snprintf(buffer, sizeof(buffer), "%s held for %.1f s", trainer::levelName(lastGoalLevel),
                                 (double)cfg().hold_seconds);
                        setNotice(buffer);
                        esp32sveltekit.getNotificationService()->pushNotification(buffer, PUSHSUCCESS);

                        if (cfg().auto_advance)
                        {
                            /* Climb, or step down, after every completed hold. When the
                             * walk is finished the last level stays. */
                            const int next = trainer::nextLevel(lastGoalLevel, levelCount(), cfg().level_count_down);
                            if (next > 0)
                            {
                                lastGoalLevel = next;
                                holdTracker.reset(cfg().hold_seconds);
                                holdCounted = false;
                            }
                        }
                        else
                        {
                            holdCounted = false;
                        }
                    }
                }
                else if (!atGoal)
                {
                    holdCounted = false;
                }

                /* Rep mode: the return to the start arms the next rep. "Fully back to
                 * 0" means the depth is at or below the release threshold again. */
                if (playModeReps())
                {
                    repCounter.release(depthMm, TRAINER_REP_RELEASE_MM);
                }
                /* The rest between reps: it runs while the next rep is armed and at least one rep
                 * is done, which is exactly the pause the operator is taking. */
                repBreak.update(playModeReps() && repCounter.armed, session.active(), repCounter.count,
                                millis());
            }
            else
            {
                /* An unusable sample is not a distance: the raw value is marked as
                 * absent instead of being reported as 0 mm, which would look like a
                 * real reading. */
                depthRawMm = -1.0f;
            }

            /* The serial console, polled here because this task already holds the live values and
             * runs often enough to keep up with somebody typing. Costs a look at the port per pass
             * when nothing arrives. */
            TrainerConsoleState consoleState;
            consoleState.levelName = trainer::levelName(lastGoalLevel);
            consoleState.depthMm = depthMm;
            consoleState.peakMm = peakMm;
            consoleState.sessionActive = session.active();
            consoleState.reps = repCounter.count;
            consoleState.repTarget = cfg().reps_target;
            consoleState.repArmed = repCounter.armed;
            consoleState.breakS = repBreak.seconds(millis());
            trainerConsole(display, consoleState);
        }

        /* A depth is a measurement, not a state. After a moment without a usable
         * sample the readout falls back to 0, so the ring shows what the sensor
         * really knows instead of freezing on a number from a minute ago. */
        if (lastValidSampleMs != 0 && (millis() - lastValidSampleMs) > TRAINER_DEPTH_HOLD_MS)
        {
            lastValidSampleMs = 0;
            depthMm = 0.0f;
            depthRawMm = -1.0f;
            depthFilter.reset();
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

    /* A touch controller that has gone to sleep does not answer, and that NACK is
     * expected traffic, not a fault. The core prints five lines for each one, which
     * would hide everything else on this port; this firmware reports the same
     * facts itself in [i2c], [touch] and [status] lines. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_log_level_set("Wire", ESP_LOG_NONE);
    esp_log_level_set("esp32-hal-i2c-ng", ESP_LOG_NONE);

    /* --- the panel first: it brings up the I2C bus the sensor and the touch
     * controller need. --- */
    /* The bus first, then a look at who answers on it: the sensor, the touch
     * controller and the panel's expander all live on that one bus. */
    Wire.begin(TRAINER_I2C_SDA, TRAINER_I2C_SCL, TRAINER_I2C_SPEED);
    i2cBusScan();

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
    /* One clean look at the band while nothing of this device is transmitting. */
    bootRadioScan();

    esp32sveltekit.setMDNSAppName("DTT Trainer");
    esp32sveltekit.begin();

    /* The radio settings need a started station interface, and the framework
     * switches the radio off and on while it starts, so they are not applied here
     * but from the retry loop in trainerTask(). Only a fresh driver session needs
     * them again: re-applying them on every reconnect disturbed a running scan and
     * made the framework report "WiFi scan failed". */
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
        (void)event;
        (void)info;
        radioSettingsApplied = false; },
                 ARDUINO_EVENT_WIFI_STA_START);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
        (void)event;
        (void)info;
        radioSettingsApplied = false; },
                 ARDUINO_EVENT_WIFI_AP_START);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
                 {
        (void)event;
        (void)info;
        const uint8_t reason = info.wifi_sta_disconnected.reason;
        if (joinFailures < 255)
        {
            joinFailures++;
        }
        Serial.printf("[wifi] join attempt %u failed, reason %u%s\n",
                      (unsigned)joinFailures, (unsigned)reason, joinReasonHint(reason)); },
                 ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    trainerSettingsService.begin();
    trainerStateService.begin();

    /* ------------------------------------------------------------------ /rest/frame -------
     * A crop of the panel's own pixels as raw RGB565, row major, little endian, with the geometry
     * in headers. It exists so that drawing questions can be settled by looking at the panel
     * instead of by reasoning about a scanline fill: a one pixel seam in an arc is invisible in
     * the source and obvious in a crop.
     *
     * The coordinates come out of the query string by hand, because the handler must not depend on
     * a parameter API that may differ between framework versions. The buffer is copied, not
     * borrowed: the panel is scanning it out while this runs. */
    esp32sveltekit.getServer()->on("/rest/frame", HTTP_GET, [](PsychicRequest *request)
                                   {
        uint16_t *frame = display.frameBuffer();
        if (frame == nullptr)
        {
            return request->reply(503, "text/plain", "no panel");
        }

        const String url = request->query();
        int x = queryInt(url, "x", 0);
        int y = queryInt(url, "y", 0);
        int w = queryInt(url, "w", 64);
        int h = queryInt(url, "h", 64);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (w > 240) w = 240;   /* keep one dump inside a few tens of kB */
        if (h > 240) h = 240;
        if (x + w > TRAINER_TFT_WIDTH) w = TRAINER_TFT_WIDTH - x;
        if (y + h > TRAINER_TFT_HEIGHT) h = TRAINER_TFT_HEIGHT - y;
        if (w < 1 || h < 1)
        {
            return request->reply(400, "text/plain", "crop outside the panel");
        }

        const size_t bytes = (size_t)w * (size_t)h * 2u;
        uint8_t *copy = (uint8_t *)malloc(bytes);
        if (copy == nullptr)
        {
            return request->reply(503, "text/plain", "no room for the crop");
        }
        for (int row = 0; row < h; ++row)
        {
            memcpy(copy + (size_t)row * (size_t)w * 2u,
                   frame + (size_t)(y + row) * (size_t)TRAINER_TFT_WIDTH + (size_t)x,
                   (size_t)w * 2u);
        }

        PsychicResponse response(request);
        response.setCode(200);
        response.setContentType("application/octet-stream");
        response.addHeader("X-Frame-W", String(w).c_str());
        response.addHeader("X-Frame-H", String(h).c_str());
        response.addHeader("X-Frame-X", String(x).c_str());
        response.addHeader("X-Frame-Y", String(y).c_str());
        response.setContent(copy, bytes);
        const esp_err_t sent = response.send();
        free(copy);
        return sent; });

    /* The cached copy must exist before the tasks start. */
    refreshSettings(true);
    applyColours();

    /* A settings change takes effect at once: brightness, filter and hold target
     * need no restart, and a unit change needs a full repaint. */
    trainerSettingsService.addUpdateHandler([&](const String &originId)
                                            { refreshSettings(false); applyColours(); },
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
