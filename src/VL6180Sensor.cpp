#include "VL6180Sensor.h"

/* Register addresses both parts share (VL6180X datasheet, table 5). The data
 * path is plain register access on purpose: the Adafruit driver keeps read8()
 * and write8() private in the released version, and a VL6180 without the X never
 * passes its begin() check. The library is still used for what it is good at -
 * detecting the part and writing the tuning registers ST recommends for a
 * VL6180X. */
#define REG_IDENTIFICATION_MODEL_ID 0x000
#define REG_SYSRANGE_START 0x018
#define REG_RESULT_RANGE_STATUS 0x04D
#define REG_RESULT_INTERRUPT_STATUS_GPIO 0x04F
#define REG_RESULT_RANGE_VAL 0x062
#define REG_SYSTEM_INTERRUPT_CLEAR 0x015

#define VL6180X_MODEL_ID 0xB4
#define VL6180_MODEL_ID 0xB3

/* A measurement that has not finished in this long is treated as lost, so the
 * driver can start over instead of waiting forever. */
#define MEASUREMENT_TIMEOUT_MS 250
/* Continuous mode that stops delivering samples is abandoned after this. */
#define CONTINUOUS_STALL_MS 1500
/* How often a missing sensor is looked for again. */
#define DETECT_RETRY_MS 5000

/* ------------------------------------------------------------------ public -- */

bool VL6180Sensor::begin(TwoWire &wire, uint8_t address)
{
    _wire = &wire;
    _address = address;
    _started = true;
    return detect();
}

void VL6180Sensor::setSamplePeriodMs(uint16_t periodMs)
{
    _periodMs = (periodMs < 10) ? 10 : periodMs;
    if (_present && _mode == Mode::continuous && _useLibrary)
    {
        _chip.startRangeContinuous(_periodMs);
    }
}

void VL6180Sensor::accountHostGap(uint32_t gapMs)
{
    if (gapMs == 0)
    {
        return;
    }
    /* Both references move: the stall check compares against the sample time, and
     * the single shot timeout compares against the measurement start. */
    _lastSampleMs += gapMs;
    _startedMs += gapMs;
}

bool VL6180Sensor::poll()
{
    /* A bus read that fails is not a measurement: count it, and after a few in a
     * row declare the sensor gone and let the slow retry look for it again. One
     * NACK is not a lost sensor, and a lost sensor must not turn every poll into
     * a NACK either. */
    _busError = false;
    const bool sampled = pollInternal();
    if (_busError)
    {
        if (_readFailures < 255)
        {
            _readFailures++;
        }
        if (_readFailures >= 5 && _present)
        {
            _present = false;
            _mode = Mode::absent;
            _rangeMm = NAN;
            _status = 0xFF;
            Serial.println("[sensor] lost the sensor on the I2C bus, looking for it again every 5 s");
        }
    }
    else
    {
        _readFailures = 0;
    }
    return sampled;
}

bool VL6180Sensor::pollInternal()
{
    const uint32_t now = millis();

    /* A sensor that is not there yet: look again now and then. Everything on
     * this bus is hot pluggable, and a device flashed with the sensor unplugged
     * should start to work when it is plugged in. */
    if (!_present)
    {
        if (_started && (now - _lastDetectMs) >= DETECT_RETRY_MS)
        {
            detect();
        }
        return false;
    }

    if (_mode == Mode::continuous)
    {
        if (interruptFlagSet())
        {
            return readResult();
        }
        /* No sample for a long time: this chip does not run the continuous
         * sequence. Take one measurement at a time instead. */
        if (_lastSampleMs != 0 && (now - _lastSampleMs) > CONTINUOUS_STALL_MS)
        {
            if (_useLibrary)
            {
                _chip.stopRangeContinuous();
            }
            _mode = Mode::singleShot;
            _measurementRunning = false;
            Serial.println("[sensor] continuous mode stalled, single shot polling from now on");
        }
        return false;
    }

    /* Single shot: start a measurement, then read it on a later call. Nothing
     * here blocks, so the task keeps serving the touch controller too. */
    if (!_measurementRunning)
    {
        if (_lastSampleMs != 0 && (now - _lastSampleMs) < _periodMs)
        {
            return false;
        }
        startSingleShot();
        return false;
    }

    if (interruptFlagSet())
    {
        _measurementRunning = false;
        return readResult();
    }

    if ((now - _startedMs) > MEASUREMENT_TIMEOUT_MS)
    {
        /* Lost the measurement. Count it as bad, clear the interrupt and start
         * again on the next call. */
        _measurementRunning = false;
        _badSamples++;
        _status = 0xFE; /* 0xFE: the driver timed out, not the sensor */
        _rangeMm = NAN;
        regWrite(REG_SYSTEM_INTERRUPT_CLEAR, 0x07);
    }
    return false;
}

uint32_t VL6180Sensor::sampleAgeMs() const
{
    if (_lastSampleMs == 0)
    {
        return 0xFFFFFFFFu;
    }
    return millis() - _lastSampleMs;
}

const char *VL6180Sensor::statusText() const
{
    switch (_status)
    {
    case 0:
        return "ok";
    case 1:
        return "signal too weak";
    case 2:
        return "phase out of limits";
    case 3:
        return "signal too strong";
    case 4:
        return "outside the range";
    case 5:
        return "sensor error";
    case 0xFE:
        return "measurement timeout";
    case 0xFF:
        return "no sample yet";
    default:
        return "unusable sample";
    }
}

const char *VL6180Sensor::modeName() const
{
    switch (_mode)
    {
    case Mode::continuous:
        return "continuous";
    case Mode::singleShot:
        return "single shot";
    default:
        return "absent";
    }
}

const char *VL6180Sensor::modelName() const
{
    switch (_modelId)
    {
    case VL6180X_MODEL_ID:
        return "VL6180X";
    case VL6180_MODEL_ID:
        return "VL6180";
    default:
        return "unknown";
    }
}

/* ----------------------------------------------------------------- private -- */

uint8_t VL6180Sensor::regRead(uint16_t reg)
{
    if (!_wire)
    {
        _busError = true;
        return 0xFF;
    }
    _wire->beginTransmission(_address);
    _wire->write((uint8_t)(reg >> 8));
    _wire->write((uint8_t)(reg & 0xFF));
    if (_wire->endTransmission(false) != 0)
    {
        _busError = true;
        return 0xFF;
    }
    if (_wire->requestFrom((int)_address, 1) != 1)
    {
        _busError = true;
        return 0xFF;
    }
    return (uint8_t)_wire->read();
}

void VL6180Sensor::regWrite(uint16_t reg, uint8_t value)
{
    if (!_wire)
    {
        _busError = true;
        return;
    }
    _wire->beginTransmission(_address);
    _wire->write((uint8_t)(reg >> 8));
    _wire->write((uint8_t)(reg & 0xFF));
    _wire->write(value);
    if (_wire->endTransmission() != 0)
    {
        _busError = true;
    }
}

/* Bit 2 of the GPIO interrupt status means "a range measurement is ready". */
bool VL6180Sensor::interruptFlagSet()
{
    return (regRead(REG_RESULT_INTERRUPT_STATUS_GPIO) & 0x04) != 0;
}

bool VL6180Sensor::detect()
{
    _lastDetectMs = millis();

    if (!_wire)
    {
        return false;
    }

    /* Model id first, with plain register access: the library refuses anything
     * that is not a VL6180X, and the report should say what is really there. */
    _modelId = regRead(REG_IDENTIFICATION_MODEL_ID);

    if (_modelId == VL6180X_MODEL_ID && _chip.begin(_wire))
    {
        /* The library writes ST's recommended tuning registers here, which is
         * exactly what a VL6180X needs; the measurements below are read from the
         * registers directly. */
        _useLibrary = true;
        _mode = Mode::continuous;
        _measurementRunning = false;
        _lastSampleMs = 0;
        _present = true;
        _chip.startRangeContinuous(_periodMs);
        Serial.printf("[sensor] found %s (id 0x%02X), %s\n", modelName(), _modelId, modeName());
        return true;
    }

    /* Any other chip that answered: use the plain register sequence both parts
     * support. This covers a VL6180 (no X) and an X whose tuning registers were
     * refused. */
    if (_modelId != 0x00 && _modelId != 0xFF)
    {
        _useLibrary = false;
        _mode = Mode::singleShot;
        _measurementRunning = false;
        _lastSampleMs = 0;
        _present = true;
        _badSamples = 0;
        _goodSamples = 0;
        Serial.printf("[sensor] found %s (id 0x%02X) without the VL6180X tuning, %s\n",
                      modelName(), _modelId, modeName());
        return true;
    }

    _present = false;
    _mode = Mode::absent;
    _modelId = 0;
    return false;
}

void VL6180Sensor::startSingleShot()
{
    _startedMs = millis();
    _measurementRunning = true;
    regWrite(REG_SYSRANGE_START, 0x01);
}

bool VL6180Sensor::readResult()
{
    const uint8_t status = (uint8_t)(regRead(REG_RESULT_RANGE_STATUS) >> 4);
    const uint8_t range = regRead(REG_RESULT_RANGE_VAL);
    regWrite(REG_SYSTEM_INTERRUPT_CLEAR, 0x07);

    _status = status;
    if (status == 0)
    {
        _rangeMm = (float)range;
        _goodSamples++;
    }
    else
    {
        /* A sample the chip rejects is not a distance: declare it invalid so the
         * maths skips it, and let the state service report the reason. */
        _rangeMm = NAN;
        _badSamples++;
    }
    _lastSampleMs = millis();
    return true;
}
