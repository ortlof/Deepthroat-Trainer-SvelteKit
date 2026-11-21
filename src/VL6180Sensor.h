#pragma once

/*
 *   VL6180X / VL6180 time-of-flight sensor on the shared T-RGB I2C bus.
 *
 *   The sensor reports a distance in millimetres over I2C at 0x29; it does not
 *   report a depth. "Depth" is the trainer's own idea and lives in
 *   src/trainer_math.h.
 *
 *   Two chips are sold under almost the same name and both answer at 0x29:
 *     - VL6180X, the newer and much more common breakout. Model id 0xB4.
 *     - VL6180 (no X), the older part. Model id 0xB3, shorter range, no support
 *       for every register the X uses.
 *
 *   The model id is read with raw register access, before any library decides
 *   anything, and it is reported on the serial port, on the display and on the
 *   Settings page. That way the part on the board can be told apart without a
 *   datasheet in hand.
 *
 *   For a VL6180X the Adafruit driver is used to detect the part and to write the
 *   tuning registers that ST recommends for it. Every measurement is then read
 *   from the result registers directly, because the driver keeps its register
 *   access private and a VL6180 without the X never passes its detection.
 *   Continuous mode is used where the chip takes it; when no sample arrives, the
 *   driver switches to single shot instead of hanging. poll() never blocks.
 */

#include <Arduino.h>
#include <Wire.h>

#include <Adafruit_VL6180X.h>

#include "TrainerConfig.h"

class VL6180Sensor
{
public:
    enum class Mode : uint8_t
    {
        absent,     /* nothing answered on the bus */
        continuous, /* free running at the inter-measurement period */
        singleShot  /* one measurement per poll, non blocking */
    };

    /* Takes the bus and detects the chip. Returns false when nothing answered;
     * poll() keeps retrying, so a sensor connected later starts to work. */
    bool begin(TwoWire &wire, uint8_t address = TRAINER_SENSOR_ADDR);

    /* One non-blocking step. Call it from a single task, at the sample rate.
     * Returns true when this call produced a new sample. */
    bool poll();

    bool present() const { return _present; }
    VL6180Sensor::Mode mode() const { return _mode; }
    const char *modeName() const;

    /* Range of the newest sample in millimetres; NAN when there is none yet. */
    float rangeMm() const { return _rangeMm; }
    /* Status of the newest sample, 0 = the chip is happy with the measurement. */
    uint8_t status() const { return _status; }
    const char *statusText() const;
    /* 0xB4 on a VL6180X, 0xB3 on a VL6180; 0 when nothing answered. */
    uint8_t modelId() const { return _modelId; }
    const char *modelName() const;

    uint32_t goodSamples() const { return _goodSamples; }
    uint32_t badSamples() const { return _badSamples; }
    /* Milliseconds since the newest sample. */
    uint32_t sampleAgeMs() const;

    void setSamplePeriodMs(uint16_t periodMs);

    /* A host task that was blocked (a panel repaint, a WiFi scan) is not a sensor
     * that stopped delivering. Shift the stall reference by the gap so continuous
     * mode is not abandoned because of it. */
    void accountHostGap(uint32_t gapMs);

private:
    bool detect();
    bool readResult();
    void startSingleShot();
    bool interruptFlagSet();
    bool pollInternal();

    void regWrite(uint16_t reg, uint8_t value);
    uint8_t regRead(uint16_t reg);

    Adafruit_VL6180X _chip;
    TwoWire *_wire = nullptr;
    bool _useLibrary = false;
    bool _started = false;
    /* A read that fails sets this, so poll() can tell a lost bus from a sample
     * the sensor rejected. */
    bool _busError = false;
    uint8_t _readFailures = 0;

    uint8_t _address = TRAINER_SENSOR_ADDR;
    bool _present = false;
    Mode _mode = Mode::absent;

    float _rangeMm = NAN;
    uint8_t _status = 0xFF;
    uint8_t _modelId = 0;

    uint32_t _goodSamples = 0;
    uint32_t _badSamples = 0;
    uint32_t _lastSampleMs = 0;
    uint32_t _lastDetectMs = 0;
    uint32_t _startedMs = 0;
    bool _measurementRunning = false;
    uint16_t _periodMs = 25;
};
