#include "TrainerTouch.h"

/* CST816S / CST820 register map (the CST8xx family shares it):
 *   0x01 gesture id, 0x02 number of touch points,
 *   0x03/0x04 touch point 1 x (high/low), 0x05/0x06 touch point 1 y (high/low),
 *   0xA7 chip id. */
#define REG_TOUCH_BLOCK 0x01
#define REG_CHIP_ID 0xA7

#define GESTURE_LONG_PRESS 0x0B /* reported by the controller itself on some parts */

/* Reset timing from the CST816S datasheet: hold low at least 5 ms, then wait
 * 50 ms before the first I2C access. */
#define TOUCH_RESET_LOW_MS 10
#define TOUCH_RESET_SETTLE_MS 60

bool TrainerTouch::begin(TwoWire &wire, Arduino_XL9535SWSPI *xl9535, uint8_t address)
{
    _wire = &wire;
    _xl9535 = xl9535;
    _address = address;

    /* The interrupt line is only read, and a pull-up keeps it defined on a board
     * where nothing drives it. */
    pinMode(TRAINER_TOUCH_IRQ, INPUT_PULLUP);

    if (!probe())
    {
        _present = false;
        _asleep = true;
        _nextProbeMs = millis() + TRAINER_TOUCH_ASLEEP_PROBE_MS;
        return false;
    }

    _present = true;
    _asleep = false;
    _lastReadMs = millis();
    _nextProbeMs = millis();
    return true;
}

void TrainerTouch::reset()
{
    if (!_xl9535)
    {
        return;
    }
    _xl9535->pinMode(TRAINER_XL9535_PIN_TOUCH_RST, OUTPUT);
    _xl9535->digitalWrite(TRAINER_XL9535_PIN_TOUCH_RST, 0);
    delay(TOUCH_RESET_LOW_MS);
    _xl9535->digitalWrite(TRAINER_XL9535_PIN_TOUCH_RST, 1);
    delay(TOUCH_RESET_SETTLE_MS);
}

/* A reset plus a chip id read. Used both at start-up and to wake a controller
 * that went to sleep. */
bool TrainerTouch::probe()
{
    reset();

    /* The chip id tells a real controller from a floating bus. 0x00 and 0xFF are
     * both "nothing answered", everything else is taken as a controller. */
    uint8_t id = 0;
    if (!readRegisters(REG_CHIP_ID, &id, 1))
    {
        _chipId = 0;
        return false;
    }
    if (id == 0x00 || id == 0xFF)
    {
        _chipId = 0;
        return false;
    }
    _chipId = id;
    return true;
}

/* One failed read means the controller does not answer, and on this chip family
 * that is what sleep looks like. Waking it with a reset is quiet on the bus;
 * hammering it with a NACK on every poll is not. */
void TrainerTouch::noteSleep()
{
    if (_asleep)
    {
        return;
    }
    _asleep = true;
    _present = false;
    _pressed = false;
    _longFired = false;
    _nextProbeMs = millis() + TRAINER_TOUCH_ASLEEP_PROBE_MS;
    /* Once is enough: this chip goes quiet between touches by design, and a line
     * for every nap would drown the rest of the log. */
    if (!_sleepLogged)
    {
        _sleepLogged = true;
        Serial.printf("[touch] chip id 0x%02X stops answering between touches (that is its sleep). "
                      "Polling pauses until the interrupt line or the %d s fallback says otherwise.\n",
                      _chipId, (int)(TRAINER_TOUCH_ASLEEP_PROBE_MS / 1000));
    }
}

void TrainerTouch::poll()
{
    if (!_wire)
    {
        return;
    }

    const uint32_t now = millis();

    if (_asleep)
    {
        /* Wake it the way that costs nothing on the bus: a reset through the
         * expander. The expander always answers, while a sleeping controller does
         * not, so resetting first and reading afterwards removes the NACK that a
         * probe-first order produces on every wake. */
        const bool interrupt = (digitalRead(TRAINER_TOUCH_IRQ) == LOW);
        if (!interrupt && (int32_t)(now - _nextProbeMs) < 0)
        {
            return;
        }
        reset();
        uint8_t id = 0;
        if (readRegisters(REG_CHIP_ID, &id, 1) && id != 0x00 && id != 0xFF)
        {
            _chipId = id;
            _asleep = false;
            _present = true;
            _lastReadMs = now;
            _nextProbeMs = now;
            if (interrupt)
            {
                _wakeups++;
                Serial.printf("[touch] awake for a touch (chip id 0x%02X), %u wakeups\n",
                              _chipId, (unsigned)_wakeups);
            }
        }
        else
        {
            /* Back off as well: an interrupt line that stays low forever must not
             * turn this into a poll loop. */
            _nextProbeMs = now + TRAINER_TOUCH_ASLEEP_BACKOFF_MS;
        }
        return;
    }

    /* The controller asserts its interrupt line for a touch. While nothing
     * touches the panel a slow poll is enough; it also keeps a board whose
     * interrupt line is not connected working. */
    const bool interrupt = (digitalRead(TRAINER_TOUCH_IRQ) == LOW);
    if (!interrupt && (now - _lastReadMs) < TRAINER_TOUCH_IDLE_POLL_MS)
    {
        return;
    }

    /* gesture, finger count, x high/low, y high/low */
    uint8_t block[6] = {0};
    if (!readRegisters(REG_TOUCH_BLOCK, block, sizeof(block)))
    {
        noteSleep();
        return;
    }
    _lastReadMs = now;

    const uint8_t gesture = block[0];
    const uint8_t points = block[1] & 0x0F;
    const bool down = (points > 0) || (gesture == GESTURE_LONG_PRESS);
    if (down)
    {
        _x = (int16_t)(((uint16_t)(block[2] & 0x0F) << 8) | block[3]);
        _y = (int16_t)(((uint16_t)(block[4] & 0x0F) << 8) | block[5]);
    }

    if (down && !_pressed)
    {
        _pressed = true;
        _pressStartMs = now;
        _longFired = false;
        _presses++;
        return;
    }

    if (down && _pressed)
    {
        const uint32_t held = now - _pressStartMs;
        if (!_longFired && held >= TRAINER_TOUCH_LONGPRESS_MS)
        {
            _longFired = true;
            _pending = Event::longPress;
        }
        return;
    }

    if (!down && _pressed)
    {
        const uint32_t held = now - _pressStartMs;
        _pressed = false;
        if (!_longFired && held <= TRAINER_TOUCH_TAP_MS)
        {
            _pending = Event::tap;
        }
    }
}

TrainerTouch::Event TrainerTouch::takeEvent()
{
    const Event event = _pending;
    _pending = Event::none;
    return event;
}

const char *TrainerTouch::stateName() const
{
    if (_asleep)
    {
        return "asleep";
    }
    return _present ? "ok" : "absent";
}

bool TrainerTouch::readRegisters(uint8_t reg, uint8_t *buffer, size_t len)
{
    if (!_wire)
    {
        return false;
    }
    _wire->beginTransmission(_address);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0)
    {
        return false;
    }
    if (_wire->requestFrom((int)_address, (int)len) != (int)len)
    {
        return false;
    }
    for (size_t i = 0; i < len; i++)
    {
        buffer[i] = (uint8_t)_wire->read();
    }
    return true;
}
