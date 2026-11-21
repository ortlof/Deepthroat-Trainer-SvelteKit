#include "TrainerTouch.h"

/* CST816S / CST820 register map (the CST8xx family shares it):
 *   0x01 gesture id, 0x02 number of touch points,
 *   0x03/0x04 touch point 1 x (high/low), 0x05/0x06 touch point 1 y (high/low),
 *   0xA7 chip id. */
#define REG_GESTURE 0x01
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
    _address = address;

    reset(xl9535);

    /* The chip id tells a real controller from a floating bus. 0x00 and 0xFF
     * are both "no answer", everything else is taken as a controller. */
    uint8_t id = 0;
    if (readRegisters(REG_CHIP_ID, &id, 1))
    {
        _chipId = id;
        _present = (id != 0x00 && id != 0xFF);
    }
    else
    {
        _chipId = 0;
        _present = false;
    }
    return _present;
}

void TrainerTouch::reset(Arduino_XL9535SWSPI *xl9535)
{
    if (xl9535)
    {
        xl9535->pinMode(TRAINER_XL9535_PIN_TOUCH_RST, OUTPUT);
        xl9535->digitalWrite(TRAINER_XL9535_PIN_TOUCH_RST, 0);
        delay(TOUCH_RESET_LOW_MS);
        xl9535->digitalWrite(TRAINER_XL9535_PIN_TOUCH_RST, 1);
        delay(TOUCH_RESET_SETTLE_MS);
    }
}

void TrainerTouch::poll()
{
    if (!_present || !_wire)
    {
        return;
    }

    const uint32_t now = millis();

    /* gesture, finger count, x high/low, y high/low */
    uint8_t block[6] = {0};
    if (!readRegisters(REG_TOUCH_BLOCK, block, sizeof(block)))
    {
        /* One lost read is not a missing controller; only a long silence is. */
        if (_lastGoodMs != 0 && (now - _lastGoodMs) > 30000u)
        {
            _present = false;
        }
        return;
    }
    _lastGoodMs = now;

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
