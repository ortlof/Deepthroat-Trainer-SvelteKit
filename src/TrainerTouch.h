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
 *   T-RGB has no free ESP32 pin for it. GPIO1 is the interrupt line.
 *
 *   Two behaviours of this chip family shape the driver:
 *
 *   1. The controller falls asleep when nobody touches the panel for a while,
 *      and a sleeping chip stops answering I2C. Every poll of a sleeping chip is
 *      a NACK, and the ESP32 core logs five error lines per NACK, so a plain
 *      12 Hz poll fills the serial port with noise and hides everything else.
 *      This driver notices the sleep, wakes the controller with a reset through
 *      the expander, and keeps quiet while it is asleep.
 *   2. The interrupt line (GPIO1) goes low for a touch, so the I2C traffic is
 *      gated on it. A slow poll stays in place as a fallback for a board whose
 *      interrupt line is not connected.
 *
 *   Only two gestures are used by the trainer, and each is reported once per
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

    /* One non-blocking read of the controller, gated on the interrupt line. */
    void poll();

    bool present() const { return _present; }
    bool asleep() const { return _asleep; }
    bool pressed() const { return _pressed; }
    int16_t x() const { return _x; }
    int16_t y() const { return _y; }
    uint32_t presses() const { return _presses; }
    uint8_t chipId() const { return _chipId; }
    uint32_t wakeups() const { return _wakeups; }
    const char *stateName() const;

    /* Returns the pending gesture and clears it, so each press is acted on once. */
    Event takeEvent();

private:
    bool readRegisters(uint8_t reg, uint8_t *buffer, size_t len);
    bool probe(); /* reset the controller, then read its chip id */
    void reset();
    void noteSleep();

    TwoWire *_wire = nullptr;
    Arduino_XL9535SWSPI *_xl9535 = nullptr;
    uint8_t _address = TRAINER_TOUCH_ADDR;
    bool _present = false;
    bool _asleep = false;
    bool _sleepLogged = false;
    bool _pressed = false;
    bool _longFired = false;
    int16_t _x = 0;
    int16_t _y = 0;
    uint32_t _pressStartMs = 0;
    uint32_t _presses = 0;
    uint32_t _lastReadMs = 0;
    uint32_t _nextProbeMs = 0;
    uint32_t _wakeups = 0;
    uint8_t _chipId = 0;
    Event _pending = Event::none;
};
