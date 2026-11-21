#pragma once

/*
 *   CST820 / CST816 capacitive touch on the T-RGB.
 *
 *   The touch controller shares the I2C bus with the sensor and the XL9535
 *   expander, and it must be polled from the same task as the sensor: the
 *   Arduino Wire library has no locking, so two tasks on one bus corrupt each
 *   other's transactions.
 *
 *   The controller is reset through the XL9535 expander (pin 1), because the
 *   T-RGB has no free ESP32 pin for it. GPIO1 is the interrupt line only.
 *
 *   Only two gestures are used by the trainer, and they are reported once per
 *   press: a tap toggles a session, a long press sets the zero reference at the
 *   distance that is in front of the sensor at that moment.
 */

#include <Arduino.h>
#include <Wire.h>

#include <Arduino_GFX_Library.h>

#include "TrainerConfig.h"

class TrainerTouch
{
public:
    enum class Event : uint8_t
    {
        none,
        tap,
        longPress
    };

    /* xl9535 may be null: then the controller is used without a reset, which
     * works on a board that has just powered up. The concrete expander type is
     * used, not the Arduino_DataBus base: the base class has no pinMode(). */
    bool begin(TwoWire &wire, Arduino_XL9535SWSPI *xl9535, uint8_t address = TRAINER_TOUCH_ADDR);

    /* One non-blocking read of the controller. */
    void poll();

    bool present() const { return _present; }
    bool pressed() const { return _pressed; }
    int16_t x() const { return _x; }
    int16_t y() const { return _y; }
    uint32_t presses() const { return _presses; }
    uint8_t chipId() const { return _chipId; }

    /* Returns the pending gesture and clears it, so each press is acted on once. */
    Event takeEvent();

private:
    bool readRegisters(uint8_t reg, uint8_t *buffer, size_t len);
    void reset(Arduino_XL9535SWSPI *xl9535);

    TwoWire *_wire = nullptr;
    uint8_t _address = TRAINER_TOUCH_ADDR;
    bool _present = false;
    bool _pressed = false;
    bool _longFired = false;
    int16_t _x = 0;
    int16_t _y = 0;
    uint32_t _pressStartMs = 0;
    uint32_t _presses = 0;
    uint32_t _lastGoodMs = 0;
    uint8_t _chipId = 0;
    Event _pending = Event::none;
};
