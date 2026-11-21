#include "TrainerTouch.h"

/* CST816 / CST820 register map (the family shares it; the CST820 datasheet leaves
 * it undocumented and every driver, LilyGo's included, follows the CST816 one):
 *   0x01 gesture id, 0x02 number of fingers,
 *   0x03/0x04 touch point 1 x (high nibble first), 0x05/0x06 touch point 1 y,
 *   0xA7 chip id (0xB7 is this board's CST820), 0xFA interrupt control,
 *   0xFE auto sleep. */
/* The touch frame, as the maintained drivers read it: one burst starting at 0x00,
 * with the touch count in byte 2 and the coordinates in bytes 3..6. This is the
 * same layout as esp_lcd_touch_cst816s (DATA_START_REG 0x02, five bytes) and
 * ESPHome's cst816 (13 bytes from 0x00), which index the same fields. */
#define REG_FRAME 0x00
#define FRAME_LEN 13
#define DUMP_LEN 16   /* the state prints 16 bytes; the frame is 13 and the rest stay 0 */
#define FRAME_OFFSET_TOUCHES 2
#define FRAME_OFFSET_X_HIGH 3
#define FRAME_OFFSET_X_LOW 4
#define FRAME_OFFSET_Y_HIGH 5
#define FRAME_OFFSET_Y_LOW 6

#define COORDINATE_NONE 0x0FFF

#define REG_CHIP_ID 0xA7
#define REG_IRQ_CTL 0xFA
#define REG_DIS_AUTO_SLEEP 0xFE

/* Gesture ids. 0x0B is the DOUBLE click, not the long press: a driver that reads
 * 0x0B as a long press fires on every double tap and never on a real hold. */
#define GESTURE_SINGLE_CLICK 0x05
#define GESTURE_LONG_PRESS 0x0C

/* Interrupt control: a low pulse on each touch (EnTouch), on each change of the
 * touch state (EnChange) and on a gesture (EnMotion). ESPHome writes this value to
 * this register and it is the only one with a proven effect on this family. The
 * line is a hint here: this controller reports a pulse, not a level that stays low
 * while a finger is on the panel, so the frame has to be sampled. */
#define IRQ_CTL_EN_MOTION 0x70

/* Non-zero in 0xFE stops the controller entering low power mode on its own. This
 * driver does not write it (see configure()), but the value is reported, because a
 * board that was left with auto sleep disabled by an older build keeps that setting
 * until a reset. */
#define DIS_AUTO_SLEEP_OFF 0x01

/* Reset timing from the CST816S datasheet: hold low at least 5 ms, then wait
 * 50 ms before the first I2C access. */
#define TOUCH_RESET_LOW_MS 10
#define TOUCH_RESET_SETTLE_MS 60

void IRAM_ATTR TrainerTouch::onIrq()
{
    _irqPulses = _irqPulses + 1; /* not ++: that is deprecated on a volatile in C++20 */
    _irqSeen = true;
}

/* The interrupt handler must be a plain function, and it must live in IRAM: a flash
 * write (an OTA, a settings save) makes flash inaccessible and an ISR in flash would
 * crash. It only sets flags, so no I2C happens in interrupt context. */
static TrainerTouch *touchInstance = nullptr;

static void IRAM_ATTR touchIrqTrampoline()
{
    if (touchInstance)
    {
        touchInstance->onIrq();
    }
}

bool TrainerTouch::begin(TwoWire &wire, Arduino_XL9535SWSPI *xl9535, uint8_t address)
{
    _wire = &wire;
    _xl9535 = xl9535;
    _address = address;

    /* The interrupt line is read as a level and as an edge. The edge catches the
     * short pulse this controller emits on a touch, which a poll would usually miss;
     * the level keeps a board with no interrupt working. */
    pinMode(TRAINER_TOUCH_IRQ, INPUT_PULLUP);
    touchInstance = this;
    attachInterrupt(digitalPinToInterrupt(TRAINER_TOUCH_IRQ), touchIrqTrampoline, FALLING);

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

/* A reset, a chip id read and the configuration. Used at start-up and as the last
 * resort for a controller that answers nothing. It must NOT be used to read a
 * touch: the reset clears the very touch that made the caller look. */
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
    configure();
    return true;
}

/* Writes the interrupt control register only. The auto sleep register (0xFE) is
 * deliberately NOT written: the maintained driver for this family leaves it alone,
 * and its comment records that some CST8xx parts answer an all-0xFF frame once auto
 * sleep is disabled. A controller that naps between touches is handled here by the
 * interrupt line instead. */
void TrainerTouch::configure()
{
    writeRegister(REG_IRQ_CTL, IRQ_CTL_EN_MOTION);

    uint8_t control = 0;
    uint8_t sleep = 0;
    readRegisters(REG_IRQ_CTL, &control, 1);
    readRegisters(REG_DIS_AUTO_SLEEP, &sleep, 1);
    _autoSleepDisabled = sleep;
    Serial.printf("[touch] chip id 0x%02X, IrqCtl 0x%02X, auto sleep 0x%02X (left as the driver found it)\n",
                  _chipId, control, sleep);
}

/* One failed read means the controller does not answer, and on this chip family
 * that is what sleep looks like. */
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
    /* Once is enough: a line for every nap would drown the rest of the log. */
    if (!_sleepLogged)
    {
        _sleepLogged = true;
        Serial.printf("[touch] chip id 0x%02X stops answering (its sleep, if 0xFE did not take). "
                      "Reads pause until the interrupt line or the %d s fallback says otherwise.\n",
                      _chipId, (int)(TRAINER_TOUCH_ASLEEP_PROBE_MS / 1000));
    }
}

void TrainerTouch::applyBlock(uint32_t now, const uint8_t *block)
{
    const uint8_t points = block[1] & 0x0F;
    const uint16_t rawX = (uint16_t)(((uint16_t)(block[2] & 0x0F) << 8) | block[3]);
    const uint16_t rawY = (uint16_t)(((uint16_t)(block[4] & 0x0F) << 8) | block[5]);
    /* Two guards, both from the maintained driver for this family, and both needed:
     * only a count of 1 or 2 is a real point, and a coordinate of 0xFFF is the
     * "no point" sentinel. This part reports an all-0xFF frame while idle, in which
     * the count lands at 15 - counting that as a press is how the first version
     * invented taps that never happened. */
    const bool havePoint = (rawX != COORDINATE_NONE) && (rawY != COORDINATE_NONE);
    const bool down = (points >= 1 && points <= 2) && havePoint;

    if (down)
    {
        _x = (int16_t)rawX;
        _y = (int16_t)rawY;
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

void TrainerTouch::poll()
{
    if (!_wire)
    {
        return;
    }

    const uint32_t now = millis();
    _irqLevel = (digitalRead(TRAINER_TOUCH_IRQ) == LOW) ? 1 : 0;
    const bool irqSeen = _irqSeen;
    _irqSeen = false;

    /* An interrupt pulse means a touch happened even when the line is already high
     * again, which is the normal case for this controller. While it answers, the
     * frame is sampled at the poll rate as well. */
    const bool due = _asleep ? (irqSeen || _irqLevel != 0 || (int32_t)(now - _nextProbeMs) >= 0)
                             : (irqSeen || (int32_t)(now - _lastReadMs) >= (int32_t)TRAINER_TOUCH_POLL_MS);
    if (!due)
    {
        return;
    }

    _lastReadMs = now;

    /* One burst for the whole frame, the way the maintained drivers read it. The
     * frame doubles as the diagnostic window, so nothing extra is put on the bus. */
    uint8_t frame[FRAME_LEN] = {0};
    const bool answered = readRegisters(REG_FRAME, frame, sizeof(frame));
    _reads++;

    if (!answered)
    {
        _readFailures++;
        /* A failed read means "no finger now", never "keep the last state": the
         * controller sleeps between touches and a stale press would stay pressed. */
        _pressed = false;
        _longFired = false;

        if (++_failStreak < TRAINER_TOUCH_FAIL_LIMIT)
        {
            return;
        }
        _failStreak = 0;

        if (!_asleep)
        {
            noteSleep();
            return;
        }

        /* Asleep and still silent: reset it through the expander. The expander
         * always answers while a sleeping controller does not, and no touch can be
         * in flight while nothing answers. */
        if (probe())
        {
            _asleep = false;
            _present = true;
            _wakeups++;
            Serial.printf("[touch] awake again (chip id 0x%02X), %u wakeups\n", _chipId, (unsigned)_wakeups);
        }
        else
        {
            _nextProbeMs = millis() + TRAINER_TOUCH_ASLEEP_PROBE_MS;
        }
        return;
    }

    _failStreak = 0;
    if (_asleep)
    {
        /* It answered: either a touch is in progress, or the auto sleep write took
         * and the controller never slept in the first place. */
        _asleep = false;
        _present = true;
        _wakeups++;
        Serial.printf("[touch] answered (chip id 0x%02X), %u wakeups\n", _chipId, (unsigned)_wakeups);
    }

    /* The six bytes that the state and the diagnostic have always carried, taken
     * from the frame: gesture, touch count, x high/low, y high/low. */
    const uint8_t block[6] = {frame[1], frame[FRAME_OFFSET_TOUCHES], frame[FRAME_OFFSET_X_HIGH],
                              frame[FRAME_OFFSET_X_LOW], frame[FRAME_OFFSET_Y_HIGH], frame[FRAME_OFFSET_Y_LOW]};
    for (size_t i = 0; i < sizeof(block); i++)
    {
        _lastBlock[i] = block[i];
    }
    applyBlock(now, block);

    /* A frame with a real point is kept: it is the proof that the controller reports
     * touches at all, and it survives the idle frames that follow it. */
    const uint8_t frameCount = frame[FRAME_OFFSET_TOUCHES] & 0x0F;
    const uint16_t frameX = (uint16_t)(((uint16_t)(frame[FRAME_OFFSET_X_HIGH] & 0x0F) << 8) | frame[FRAME_OFFSET_X_LOW]);
    const uint16_t frameY = (uint16_t)(((uint16_t)(frame[FRAME_OFFSET_Y_HIGH] & 0x0F) << 8) | frame[FRAME_OFFSET_Y_LOW]);
    if (frameCount >= 1 && frameCount <= 2 && frameX != COORDINATE_NONE && frameY != COORDINATE_NONE)
    {
        memcpy(_touchFrame, frame, FRAME_LEN);
        _touchFrames++;
    }

    /* The frame is the diagnostic window, and the previous one is kept for the
     * sample in which the registers changed. */
    if (memcmp(frame, _dump, FRAME_LEN) != 0)
    {
        memset(_dumpChanged, 0, DUMP_LEN);
        memcpy(_dumpChanged, frame, FRAME_LEN);
        _dumpChanges++;
    }
    memset(_dump, 0, DUMP_LEN);
    memcpy(_dump, frame, FRAME_LEN);
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

bool TrainerTouch::writeRegister(uint8_t reg, uint8_t value)
{
    if (!_wire)
    {
        return false;
    }
    _wire->beginTransmission(_address);
    _wire->write(reg);
    _wire->write(value);
    return _wire->endTransmission(true) == 0;
}
