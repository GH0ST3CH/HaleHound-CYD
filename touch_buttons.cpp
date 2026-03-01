// ═══════════════════════════════════════════════════════════════════════════
// HaleHound-CYD Touch Button Implementation
// Replaces PCF8574 I2C button expander with touchscreen zones
// Uses SOFTWARE BIT-BANGED SPI (CYD28_TouchscreenR by Piotr Zapart / hexefx.com)
// to avoid VSPI conflict with NRF24/CC1101 radios. Proven working approach.
// Created: 2026-02-06
// Updated: 2026-02-11 - Switched to Piotr Zapart's CYD28_TouchscreenR library
// ═══════════════════════════════════════════════════════════════════════════

#include "touch_buttons.h"
#include "icon.h"
#include "shared.h"
#ifndef CYD_35
  #include "CYD28_TouchscreenR.h"
#endif

// ═══════════════════════════════════════════════════════════════════════════
// TOUCH CONTROLLER INSTANCE
// ═══════════════════════════════════════════════════════════════════════════

#ifdef CYD_35
// GT911 capacitive touch (I2C) — returns screen coordinates directly
// No calibration needed — factory calibrated
TAMC_GT911 gt911(CYD_GT911_SDA, CYD_GT911_SCL, CYD_GT911_INT, CYD_GT911_RST,
                  CYD_SCREEN_WIDTH, CYD_SCREEN_HEIGHT);

// GT911 EDGE-TRIGGERED TOUCH
// Simple approach: fire ONCE on finger-down, suppress while held, reset on lift.
// GT911 clears its register after each read — inter-frame gaps (10-30ms) cause
// false "not touched" readings. We use a 50ms gap tolerance so brief gaps don't
// falsely reset the touch state.
//
// _touchFired = true after a touch event is returned to caller.
// Stays true while finger is on screen (suppresses re-fire).
// Clears after 50ms of sustained no-touch (real finger lift).
static uint32_t _gt911CacheTime = 0;
static const uint32_t GT911_CACHE_MS = 20;      // Poll every 20ms
static const uint32_t GT911_LIFT_MS = 50;        // 50ms no-touch = real finger lift
static uint32_t _lastTouchedTime = 0;            // Last time GT911 reported touched
static bool _touchFired = false;                 // true = touch already reported, waiting for lift

void consumeTouch() {
    _touchFired = true;
}

// Block until finger physically lifts off GT911
// Waits for 80ms of sustained no-touch (spans any inter-frame gaps)
void waitForTouchRelease() {
    delay(30);
    uint32_t lastSeen = millis();
    while (millis() - lastSeen < 80) {
        gt911.read();
        if (gt911.isTouched) lastSeen = millis();
        delay(10);
    }
    _touchFired = false;
    _lastTouchedTime = 0;
    _gt911CacheTime = 0;
}

void gt911CachedRead() {
    uint32_t now = millis();
    if (now - _gt911CacheTime < GT911_CACHE_MS) return;

    gt911.read();
    _gt911CacheTime = now;

    if (gt911.isTouched) {
        _lastTouchedTime = now;
        if (_touchFired) {
            // Already reported this touch session — suppress until finger lifts
            gt911.isTouched = false;
            gt911.touches = 0;
        }
        // else: fresh touch after release — let it through
    } else {
        // GT911 says not touched — real release or inter-frame gap?
        if (_lastTouchedTime > 0 && (now - _lastTouchedTime >= GT911_LIFT_MS)) {
            // 50ms of no-touch = finger is genuinely off screen
            _touchFired = false;
            _lastTouchedTime = 0;
        }
        // else: within gap window — don't reset, ride it out
    }
}
#else
// CYD28 resistive touch (software SPI) — needs calibration
// Uses software SPI when begin() is called without SPI parameter
// Portrait mode (240x320) to match TFT rotation 0
CYD28_TouchR touch(CYD_SCREEN_WIDTH, CYD_SCREEN_HEIGHT);

// No edge-trigger on resistive touch — consumeTouch is a no-op
void consumeTouch() {}

// Wait for finger to lift off resistive screen
void waitForTouchRelease() {
    delay(30);
    uint32_t lastSeen = millis();
    while (millis() - lastSeen < 80) {
        if (touch.touched()) lastSeen = millis();
        delay(10);
    }
}
#endif

// ═══════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ═══════════════════════════════════════════════════════════════════════════

static ButtonState buttonStates[BTN_COUNT];
static uint32_t buttonPressTime[BTN_COUNT];
static uint32_t lastUpdateTime = 0;
static uint32_t lastRepeatTime = 0;
static ButtonID lastButton = BTN_NONE;
static ButtonEvent currentEvent;
static bool touchFeedbackEnabled = false;
static bool initialized = false;

#ifndef CYD_35
// Touch calibration globals — loaded from EEPROM by loadSettings()
// Defaults match Jesse's board (rawY→screenX, rawX→screenY)
uint8_t touch_cal_x_source = 1;      // 0=rawX, 1=rawY → screenX
uint16_t touch_cal_x_min = 3780;     // source raw value → screenX=0
uint16_t touch_cal_x_max = 350;      // source raw value → screenX=239
uint8_t touch_cal_y_source = 0;      // 0=rawX, 1=rawY → screenY
uint16_t touch_cal_y_min = 150;      // source raw value → screenY=0
uint16_t touch_cal_y_max = 3700;     // source raw value → screenY=319
bool touch_calibrated = false;        // true if user has run calibration

// Helper: map raw touch point to screenX using calibration
// Uses tft.width() so mapping adapts to current rotation automatically
int touchMapX(CYD28_TS_Point &p) {
    int raw = touch_cal_x_source ? p.y : p.x;
    int maxX = tft.width() - 1;
    int val = map(raw, touch_cal_x_min, touch_cal_x_max, 0, maxX);
    return constrain(val, 0, maxX);
}

// Helper: map raw touch point to screenY using calibration
// Uses tft.height() so mapping adapts to current rotation automatically
int touchMapY(CYD28_TS_Point &p) {
    int raw = touch_cal_y_source ? p.y : p.x;
    int maxY = tft.height() - 1;
    int val = map(raw, touch_cal_y_min, touch_cal_y_max, 0, maxY);
    return constrain(val, 0, maxY);
}
#else
// GT911 doesn't need calibration — stubs for EEPROM compatibility
uint8_t touch_cal_x_source = 0;
uint16_t touch_cal_x_min = 0;
uint16_t touch_cal_x_max = CYD_SCREEN_WIDTH;
uint8_t touch_cal_y_source = 0;
uint16_t touch_cal_y_min = 0;
uint16_t touch_cal_y_max = CYD_SCREEN_HEIGHT;
bool touch_calibrated = true;
#endif

// ═══════════════════════════════════════════════════════════════════════════
// TOUCH ZONE DEFINITIONS (from cyd_config.h)
// ═══════════════════════════════════════════════════════════════════════════

struct TouchZone {
    ButtonID id;
    uint16_t x1, y1, x2, y2;
};

static const TouchZone touchZones[] = {
    { BTN_UP,     TOUCH_BTN_UP_X1,   TOUCH_BTN_UP_Y1,   TOUCH_BTN_UP_X2,   TOUCH_BTN_UP_Y2 },
    { BTN_DOWN,   TOUCH_BTN_DOWN_X1, TOUCH_BTN_DOWN_Y1, TOUCH_BTN_DOWN_X2, TOUCH_BTN_DOWN_Y2 },
    { BTN_SELECT, TOUCH_BTN_SEL_X1,  TOUCH_BTN_SEL_Y1,  TOUCH_BTN_SEL_X2,  TOUCH_BTN_SEL_Y2 },
    { BTN_BACK,   TOUCH_BTN_BACK_X1, TOUCH_BTN_BACK_Y1, TOUCH_BTN_BACK_X2, TOUCH_BTN_BACK_Y2 },
};

static const int NUM_TOUCH_ZONES = sizeof(touchZones) / sizeof(TouchZone);

// ═══════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ═══════════════════════════════════════════════════════════════════════════

void touchButtonsSetup() {
    // Initialize button states
    for (int i = 0; i < BTN_COUNT; i++) {
        buttonStates[i] = BTN_STATE_IDLE;
        buttonPressTime[i] = 0;
    }

    // Setup hardware BOOT button (GPIO0)
    pinMode(BOOT_BUTTON, INPUT_PULLUP);

#ifdef CYD_35
    // GT911 capacitive touch — I2C, no SPI conflict
    // Manual Wire init before GT911 — isolates I2C bus setup from GT911 driver
    #if CYD_DEBUG
    Serial.printf("[TOUCH] GT911 I2C init: SDA=%d SCL=%d INT=%d RST=%d\n",
                  CYD_GT911_SDA, CYD_GT911_SCL, CYD_GT911_INT, CYD_GT911_RST);
    Serial.println("[TOUCH] Calling Wire.begin()...");
    Serial.flush();
    #endif

    // Reset the GT911 — set I2C address via INT pin before Wire starts
    // R25 not populated on most 3248S035C boards so INT is disconnected (pin = -1)
    // When INT is available: hold LOW during reset to select addr 0x5D
    pinMode(CYD_GT911_RST, OUTPUT);
    #if CYD_GT911_INT >= 0
    pinMode(CYD_GT911_INT, OUTPUT);
    digitalWrite(CYD_GT911_INT, LOW);   // INT=LOW during reset → addr 0x5D
    #endif
    digitalWrite(CYD_GT911_RST, LOW);
    delay(10);
    digitalWrite(CYD_GT911_RST, HIGH);
    delay(10);
    #if CYD_GT911_INT >= 0
    pinMode(CYD_GT911_INT, INPUT);      // Release INT — GT911 takes over
    #endif
    delay(100);                          // GT911 needs time to boot after reset

    #if CYD_DEBUG
    Serial.println("[TOUCH] GT911 hardware reset done, starting I2C...");
    Serial.flush();
    #endif

    Wire.begin(CYD_GT911_SDA, CYD_GT911_SCL);
    delay(10);

    #if CYD_DEBUG
    Serial.println("[TOUCH] Wire.begin() OK — scanning for GT911...");

    // I2C scan to verify GT911 is responding
    Wire.beginTransmission(0x5D);
    uint8_t i2cErr = Wire.endTransmission();
    if (i2cErr == 0) {
        Serial.println("[TOUCH] GT911 found at 0x5D");
    } else {
        Serial.printf("[TOUCH] GT911 NOT found at 0x5D (err=%d), trying 0x14...\n", i2cErr);
        Wire.beginTransmission(0x14);
        i2cErr = Wire.endTransmission();
        if (i2cErr == 0) {
            Serial.println("[TOUCH] GT911 found at 0x14");
        } else {
            Serial.printf("[TOUCH] GT911 NOT found at 0x14 either (err=%d)\n", i2cErr);
        }
    }
    Serial.flush();
    #endif

    // Now let TAMC_GT911 do its config read — Wire is already running
    // gt911.begin() will call Wire.begin() again but it'll see bus is started and skip
    gt911.begin();
    gt911.setRotation(ROTATION_INVERTED);  // Raw passthrough — no axis flip

    #if CYD_DEBUG
    Serial.println("[TOUCH] GT911 capacitive touch initialized (I2C)");
    #endif
#else
    // XPT2046 resistive touch — SOFTWARE BIT-BANGED SPI
    // Calling begin() without SPI parameter = software SPI mode
    touch.begin();
    touch.setRotation(1);  // Rotation 1 - direct mapping

    #if CYD_DEBUG
    Serial.println("[TOUCH] CYD28_TouchR initialized with SOFTWARE SPI");
    Serial.println("[TOUCH] Pins - CLK:25 MOSI:32 MISO:39 CS:33 IRQ:36");
    #endif
#endif

    // Clear current event
    currentEvent.button = BTN_NONE;
    currentEvent.state = BTN_STATE_IDLE;

    initialized = true;

    #if CYD_DEBUG
    Serial.println("[TOUCH] BOOT button on GPIO " + String(BOOT_BUTTON));
    Serial.println("[TOUCH] Touch zones defined: " + String(NUM_TOUCH_ZONES));
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// VISUAL TOUCH TEST - Shows raw values on screen for debugging
// ═══════════════════════════════════════════════════════════════════════════

void runTouchTest() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 5);
#ifdef CYD_35
    tft.println("TOUCH TEST 3.5\"");
#else
    tft.println("TOUCH TEST 2.8\"");
#endif
    tft.setTextSize(1);
    tft.setCursor(10, 30);
    tft.println("Touch corners to see values");
    tft.setCursor(10, 45);
    tft.println("BOOT=exit, dots track touch");

    // Draw corner markers — positions adapt to current rotation dimensions
    int sw = tft.width();
    int sh = tft.height();
    tft.fillCircle(10, 80, 5, TFT_RED);          // Top-left marker
    tft.fillCircle(sw - 10, 80, 5, TFT_GREEN);   // Top-right marker
    tft.fillCircle(10, sh - 10, 5, TFT_BLUE);    // Bottom-left marker
    tft.fillCircle(sw - 10, sh - 10, 5, TFT_YELLOW); // Bottom-right marker

    tft.drawRect(0, 60, sw, sh - 60, TFT_CYAN);  // Touch area box

    while (digitalRead(BOOT_BUTTON) == HIGH) {
        // Clear info area at top
        tft.fillRect(0, 0, sw, 55, TFT_BLACK);

#ifdef CYD_35
        gt911CachedRead();
        tft.setTextSize(1);
        tft.setTextColor(TFT_YELLOW);
        tft.setCursor(5, 5);

        if (gt911.isTouched) {
            int screenX = gt911.points[0].x;
            int screenY = gt911.points[0].y;

            tft.printf("GT911 X:%3d Y:%3d  pts:%d", screenX, screenY, gt911.touches);

            screenX = constrain(screenX, 0, sw - 1);
            screenY = constrain(screenY, 0, sh - 1);

            tft.setTextColor(TFT_GREEN);
            tft.setCursor(5, 18);
            tft.printf("SCREEN X:%3d Y:%3d", screenX, screenY);

            tft.setCursor(5, 35);
            tft.setTextColor(TFT_MAGENTA);
            tft.print("TOUCHED! Drawing dot...");

            tft.fillCircle(screenX, screenY, 4, TFT_MAGENTA);
        } else {
            tft.printf("GT911 — no touch");
            tft.setTextColor(TFT_RED);
            tft.setCursor(5, 18);
            tft.print("NO TOUCH - tap screen");
        }
#else
        CYD28_TS_Point p = touch.getPointRaw();

        tft.setTextSize(1);
        tft.setTextColor(TFT_YELLOW);
        tft.setCursor(5, 5);
        tft.printf("RAW X:%4d Y:%4d Z:%4d", p.x, p.y, p.z);

        if (p.z > 100) {
            // Use calibrated touch mapping (saved in EEPROM)
            int screenX = touchMapX(p);
            int screenY = touchMapY(p);

            // Clamp to screen
            screenX = constrain(screenX, 0, sw - 1);
            screenY = constrain(screenY, 0, sh - 1);

            tft.setTextColor(TFT_GREEN);
            tft.setCursor(5, 18);
            tft.printf("SCREEN X:%3d Y:%3d", screenX, screenY);

            tft.setCursor(5, 35);
            tft.setTextColor(TFT_MAGENTA);
            tft.print("TOUCHED! Drawing dot...");

            // Draw dot with OUR mapping
            tft.fillCircle(screenX, screenY, 4, TFT_MAGENTA);
        } else {
            tft.setTextColor(TFT_RED);
            tft.setCursor(5, 18);
            tft.print("NO TOUCH - tap screen");
        }
#endif

        delay(30);
    }

    while (digitalRead(BOOT_BUTTON) == LOW) delay(10);
    tft.fillScreen(TFT_BLACK);
}

// ═══════════════════════════════════════════════════════════════════════════
// SPI BUS MANAGEMENT - NO LONGER NEEDED
// Software bit-banged SPI doesn't conflict with hardware VSPI
// ═══════════════════════════════════════════════════════════════════════════

void touchReinitSPI() {
    // No-op - software SPI doesn't need reinitialization
    // Kept for API compatibility
}

// ═══════════════════════════════════════════════════════════════════════════
// RAW TOUCH FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

bool isTouched() {
#ifdef CYD_35
    gt911CachedRead();
    return gt911.isTouched;
#else
    return touch.touched();
#endif
}

// Raw hardware check — bypasses edge-trigger, does NOT affect state.
// Use ONLY for long-press detection loops.
bool isStillTouched() {
#ifdef CYD_35
    gt911.read();
    return gt911.isTouched;
#else
    return touch.touched();
#endif
}

// Peek at touch coordinates WITHOUT consuming (no edge-trigger).
// Use this when you need to CHECK the position before deciding to act.
// Call consumeTouch() after your action to prevent re-fire.
bool peekTouchPoint(uint16_t *x, uint16_t *y) {
#ifdef CYD_35
    gt911CachedRead();
    if (!gt911.isTouched) return false;
    *x = (uint16_t)constrain(gt911.points[0].x, 0, CYD_SCREEN_WIDTH - 1);
    *y = (uint16_t)constrain(gt911.points[0].y, 0, CYD_SCREEN_HEIGHT - 1);
    return true;
#else
    return getTouchPoint(x, y);  // 2.8" has no edge-trigger
#endif
}

bool getTouchPoint(uint16_t *x, uint16_t *y) {
#ifdef CYD_35
    gt911CachedRead();
    if (!gt911.isTouched) return false;

    // GT911 returns screen coordinates directly — no calibration needed
    *x = (uint16_t)constrain(gt911.points[0].x, 0, CYD_SCREEN_WIDTH - 1);
    *y = (uint16_t)constrain(gt911.points[0].y, 0, CYD_SCREEN_HEIGHT - 1);
    _touchFired = true;  // Edge-trigger: one finger-down = one event
    return true;
#else
    if (!touch.touched()) {
        return false;
    }

    // Use raw values and OUR calibrated mapping (X/Y swapped, rawY inverted)
    CYD28_TS_Point p = touch.getPointRaw();

    // Check if pressure is sufficient
    if (p.z < TOUCH_MIN_PRESSURE) {
        return false;
    }

    // Apply calibrated touch mapping (saved in EEPROM)
    int16_t screenX = touchMapX(p);
    int16_t screenY = touchMapY(p);

    *x = (uint16_t)screenX;
    *y = (uint16_t)screenY;

    return true;
#endif
}

ButtonID getTouchZone(uint16_t x, uint16_t y) {
    for (int i = 0; i < NUM_TOUCH_ZONES; i++) {
        if (x >= touchZones[i].x1 && x <= touchZones[i].x2 &&
            y >= touchZones[i].y1 && y <= touchZones[i].y2) {
            return touchZones[i].id;
        }
    }
    return BTN_NONE;
}

// Get screen X coordinate (returns -1 if not touched)
int getTouchX() {
#ifdef CYD_35
    gt911CachedRead();
    if (!gt911.isTouched) return -1;
    return constrain(gt911.points[0].x, 0, CYD_SCREEN_WIDTH - 1);
#else
    if (!touch.touched()) return -1;

    CYD28_TS_Point p = touch.getPointRaw();
    if (p.z < TOUCH_MIN_PRESSURE) return -1;

    return touchMapX(p);
#endif
}

// Get screen Y coordinate (returns -1 if not touched)
int getTouchY() {
#ifdef CYD_35
    gt911CachedRead();
    if (!gt911.isTouched) return -1;
    return constrain(gt911.points[0].y, 0, CYD_SCREEN_HEIGHT - 1);
#else
    if (!touch.touched()) return -1;

    CYD28_TS_Point p = touch.getPointRaw();
    if (p.z < TOUCH_MIN_PRESSURE) return -1;

    return touchMapY(p);
#endif
}

// Get which menu item was tapped (-1 if none or not touched)
int getTouchedMenuItem(int startY, int itemHeight, int itemCount) {
#ifdef CYD_35
    gt911CachedRead();
    if (!gt911.isTouched) return -1;
    int screenY = constrain(gt911.points[0].y, 0, CYD_SCREEN_HEIGHT - 1);
#else
    if (!touch.touched()) return -1;

    CYD28_TS_Point p = touch.getPointRaw();
    if (p.z < TOUCH_MIN_PRESSURE) return -1;

    int screenY = touchMapY(p);
#endif

    // Check if touch is in menu area
    if (screenY < startY) return -1;

    int item = (screenY - startY) / itemHeight;

    if (item >= 0 && item < itemCount) {
        consumeTouch();  // One tap = one action
        return item;
    }

    return -1;
}

// Back button position - MATCHES ORIGINAL ESP32-DIV (icon at x=10, y=20)
#define BACK_ICON_X  10
#define BACK_ICON_Y  20
#define BACK_ICON_SIZE 16

// Draw visible BACK button - bitmap icon at x=10 like original ESP32-DIV
void drawBackButton() {
    tft.drawBitmap(BACK_ICON_X, BACK_ICON_Y, bitmap_icon_go_back, BACK_ICON_SIZE, BACK_ICON_SIZE, HALEHOUND_MAGENTA);
}

// Check if BACK button tapped - checks icon area x=10-26, y=20-36
bool isBackButtonTapped() {
    static uint32_t lastTap = 0;

    if (millis() - lastTap < 300) return false;

#ifdef CYD_35
    gt911CachedRead();
    if (!gt911.isTouched) return false;
    int screenX = constrain(gt911.points[0].x, 0, CYD_SCREEN_WIDTH - 1);
    int screenY = constrain(gt911.points[0].y, 0, CYD_SCREEN_HEIGHT - 1);
#else
    if (!touch.touched()) return false;

    CYD28_TS_Point p = touch.getPointRaw();
    if (p.z < TOUCH_MIN_PRESSURE) return false;

    int screenX = touchMapX(p);
    int screenY = touchMapY(p);
#endif

    // Check icon area at x=10-26, y=20-36 (16x16 icon)
    if (screenX >= BACK_ICON_X && screenX <= BACK_ICON_X + BACK_ICON_SIZE &&
        screenY >= BACK_ICON_Y && screenY <= BACK_ICON_Y + BACK_ICON_SIZE) {
        lastTap = millis();
        consumeTouch();  // One tap = one action
        return true;
    }

    return false;
}

// Check if touch is within a rectangular area
bool isTouchInArea(int x, int y, int w, int h) {
    static uint32_t lastTap = 0;

    if (millis() - lastTap < 200) return false;

#ifdef CYD_35
    gt911CachedRead();
    if (!gt911.isTouched) return false;
    int screenX = constrain(gt911.points[0].x, 0, CYD_SCREEN_WIDTH - 1);
    int screenY = constrain(gt911.points[0].y, 0, CYD_SCREEN_HEIGHT - 1);
#else
    if (!touch.touched()) return false;

    CYD28_TS_Point p = touch.getPointRaw();
    if (p.z < TOUCH_MIN_PRESSURE) return false;

    int screenX = touchMapX(p);
    int screenY = touchMapY(p);
#endif

    if (screenX >= x && screenX <= x + w &&
        screenY >= y && screenY <= y + h) {
        lastTap = millis();
        consumeTouch();  // One tap = one action
        return true;
    }

    return false;
}

// Check if BOOT button (GPIO0) is pressed
bool isBootButtonPressed() {
    return (digitalRead(BOOT_BUTTON) == LOW);
}

// ═══════════════════════════════════════════════════════════════════════════
// CORE UPDATE FUNCTION
// ═══════════════════════════════════════════════════════════════════════════

void touchButtonsUpdate() {
    if (!initialized) return;

    uint32_t now = millis();

    // Debounce check
    if (now - lastUpdateTime < TOUCH_DEBOUNCE_MS) {
        return;
    }
    lastUpdateTime = now;

    // Clear event from previous frame
    currentEvent.button = BTN_NONE;
    currentEvent.state = BTN_STATE_IDLE;

    // Check touch screen — peek without consuming so raw getTouchPoint()
    // callers in module loops still see the touch if no zone matches here
    uint16_t touchX, touchY;
    ButtonID touchedButton = BTN_NONE;

    if (peekTouchPoint(&touchX, &touchY)) {
        touchedButton = getTouchZone(touchX, touchY);
        currentEvent.x = touchX;
        currentEvent.y = touchY;
        if (touchedButton != BTN_NONE) {
            consumeTouch();  // Zone matched — consume so modules don't double-fire
        }
    }

    // Check hardware BOOT button
    if (digitalRead(BOOT_BUTTON) == LOW) {
        touchedButton = BTN_BOOT;
    }

    // Update button states
    for (int i = 1; i < BTN_COUNT; i++) {  // Skip BTN_NONE (index 0)
        ButtonID btn = (ButtonID)i;
        bool isPressed = (btn == touchedButton);

        switch (buttonStates[i]) {
            case BTN_STATE_IDLE:
                if (isPressed) {
                    buttonStates[i] = BTN_STATE_PRESSED;
                    buttonPressTime[i] = now;
                    currentEvent.button = btn;
                    currentEvent.state = BTN_STATE_PRESSED;
                    currentEvent.pressTime = now;
                    currentEvent.holdTime = 0;
                    lastButton = btn;
                }
                break;

            case BTN_STATE_PRESSED:
                if (isPressed) {
                    // Transition to held after threshold
                    if (now - buttonPressTime[i] > TOUCH_HOLD_THRESHOLD_MS) {
                        buttonStates[i] = BTN_STATE_HELD;
                        currentEvent.button = btn;
                        currentEvent.state = BTN_STATE_HELD;
                        currentEvent.holdTime = now - buttonPressTime[i];
                    }
                } else {
                    // Released quickly - normal press
                    buttonStates[i] = BTN_STATE_RELEASED;
                    currentEvent.button = btn;
                    currentEvent.state = BTN_STATE_RELEASED;
                    currentEvent.holdTime = now - buttonPressTime[i];
                }
                break;

            case BTN_STATE_HELD:
                if (isPressed) {
                    // Still held - generate repeat events
                    if (now - lastRepeatTime > TOUCH_REPEAT_MS) {
                        currentEvent.button = btn;
                        currentEvent.state = BTN_STATE_HELD;
                        currentEvent.holdTime = now - buttonPressTime[i];
                        lastRepeatTime = now;
                    }
                } else {
                    // Released after hold
                    buttonStates[i] = BTN_STATE_RELEASED;
                    currentEvent.button = btn;
                    currentEvent.state = BTN_STATE_RELEASED;
                    currentEvent.holdTime = now - buttonPressTime[i];
                }
                break;

            case BTN_STATE_RELEASED:
                // Always transition back to idle
                buttonStates[i] = BTN_STATE_IDLE;
                if (btn == lastButton) {
                    lastButton = BTN_NONE;
                }
                break;
        }
    }
}

ButtonEvent touchButtonsGetEvent() {
    return currentEvent;
}

// ═══════════════════════════════════════════════════════════════════════════
// SIMPLE BUTTON CHECKS
// ═══════════════════════════════════════════════════════════════════════════

bool buttonPressed(ButtonID btn) {
    if (btn == BTN_NONE || btn >= BTN_COUNT) return false;
    return buttonStates[btn] == BTN_STATE_PRESSED;
}

bool buttonHeld(ButtonID btn) {
    if (btn == BTN_NONE || btn >= BTN_COUNT) return false;
    return buttonStates[btn] == BTN_STATE_HELD;
}

bool buttonReleased(ButtonID btn) {
    if (btn == BTN_NONE || btn >= BTN_COUNT) return false;
    return buttonStates[btn] == BTN_STATE_RELEASED;
}

bool anyButtonPressed() {
    for (int i = 1; i < BTN_COUNT; i++) {
        if (buttonStates[i] == BTN_STATE_PRESSED ||
            buttonStates[i] == BTN_STATE_HELD) {
            return true;
        }
    }
    return false;
}

ButtonID getCurrentButton() {
    for (int i = 1; i < BTN_COUNT; i++) {
        if (buttonStates[i] == BTN_STATE_PRESSED ||
            buttonStates[i] == BTN_STATE_HELD) {
            return (ButtonID)i;
        }
    }
    return BTN_NONE;
}

// ═══════════════════════════════════════════════════════════════════════════
// PCF8574 COMPATIBILITY LAYER
// ═══════════════════════════════════════════════════════════════════════════

bool isUpPressed() {
    return buttonStates[BTN_UP] == BTN_STATE_PRESSED ||
           buttonStates[BTN_UP] == BTN_STATE_HELD;
}

bool isDownPressed() {
    return buttonStates[BTN_DOWN] == BTN_STATE_PRESSED ||
           buttonStates[BTN_DOWN] == BTN_STATE_HELD;
}

bool isLeftPressed() {
    return buttonStates[BTN_LEFT] == BTN_STATE_PRESSED ||
           buttonStates[BTN_LEFT] == BTN_STATE_HELD;
}

bool isRightPressed() {
    return buttonStates[BTN_RIGHT] == BTN_STATE_PRESSED ||
           buttonStates[BTN_RIGHT] == BTN_STATE_HELD;
}

bool isSelectPressed() {
    return buttonStates[BTN_SELECT] == BTN_STATE_PRESSED ||
           buttonStates[BTN_SELECT] == BTN_STATE_HELD;
}

bool isBackPressed() {
    // Back = tap bottom 40 pixels of screen OR BOOT button
    static uint32_t lastBackTouch = 0;

    if (millis() - lastBackTouch > 300) {
#ifdef CYD_35
        gt911CachedRead();
        if (gt911.isTouched) {
            int screenY = constrain(gt911.points[0].y, 0, CYD_SCREEN_HEIGHT - 1);
            if (screenY > tft.height() - 40) {
                lastBackTouch = millis();
                return true;
            }
        }
#else
        if (touch.touched()) {
            CYD28_TS_Point p = touch.getPointRaw();
            if (p.z >= TOUCH_MIN_PRESSURE) {
                int screenY = touchMapY(p);
                if (screenY > tft.height() - 40) {
                    lastBackTouch = millis();
                    return true;
                }
            }
        }
#endif
    }

    // BOOT button also works as back
    if (digitalRead(BOOT_BUTTON) == LOW) {
        return true;
    }

    return false;
}

uint8_t readButtonMask() {
    // Returns inverted bitmask (0 = pressed) to match PCF8574 behavior
    uint8_t mask = 0xFF;

    if (isUpPressed())     mask &= ~(1 << 0);
    if (isDownPressed())   mask &= ~(1 << 1);
    if (isLeftPressed())   mask &= ~(1 << 2);
    if (isRightPressed())  mask &= ~(1 << 3);
    if (isSelectPressed()) mask &= ~(1 << 4);
    if (isBackPressed())   mask &= ~(1 << 5);
    if (buttonStates[BTN_BOOT] != BTN_STATE_IDLE) mask &= ~(1 << 6);

    return mask;
}

// ═══════════════════════════════════════════════════════════════════════════
// MENU NAVIGATION HELPERS
// ═══════════════════════════════════════════════════════════════════════════

ButtonID waitForButton() {
    clearButtonEvents();

    while (true) {
        touchButtonsUpdate();

        if (currentEvent.state == BTN_STATE_PRESSED) {
            return currentEvent.button;
        }

        delay(10);
    }
}

ButtonID waitForButtonTimeout(uint32_t timeoutMs) {
    clearButtonEvents();
    uint32_t startTime = millis();

    while (millis() - startTime < timeoutMs) {
        touchButtonsUpdate();

        if (currentEvent.state == BTN_STATE_PRESSED) {
            return currentEvent.button;
        }

        delay(10);
    }

    return BTN_NONE;
}

void waitForRelease() {
    while (anyButtonPressed()) {
        touchButtonsUpdate();
        delay(10);
    }
}

void clearButtonEvents() {
    for (int i = 0; i < BTN_COUNT; i++) {
        buttonStates[i] = BTN_STATE_IDLE;
        buttonPressTime[i] = 0;
    }
    currentEvent.button = BTN_NONE;
    currentEvent.state = BTN_STATE_IDLE;
    lastButton = BTN_NONE;
}

// ═══════════════════════════════════════════════════════════════════════════
// VISUAL FEEDBACK
// ═══════════════════════════════════════════════════════════════════════════

void setTouchFeedback(bool enabled) {
    touchFeedbackEnabled = enabled;
}

void drawTouchZones(uint16_t color) {
    for (int i = 0; i < NUM_TOUCH_ZONES; i++) {
        tft.drawRect(
            touchZones[i].x1,
            touchZones[i].y1,
            touchZones[i].x2 - touchZones[i].x1,
            touchZones[i].y2 - touchZones[i].y1,
            color
        );
    }
}

void drawTouchLabels(uint16_t color) {
    tft.setTextColor(color);
    tft.setTextSize(1);

    for (int i = 0; i < NUM_TOUCH_ZONES; i++) {
        uint16_t centerX = (touchZones[i].x1 + touchZones[i].x2) / 2;
        uint16_t centerY = (touchZones[i].y1 + touchZones[i].y2) / 2;

        String label = getButtonName(touchZones[i].id);
        int16_t textWidth = label.length() * 6;  // Approximate

        tft.setCursor(centerX - textWidth/2, centerY - 4);
        tft.print(label);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// CALIBRATION
// ═══════════════════════════════════════════════════════════════════════════

void setTouchCalibration(uint16_t minX, uint16_t maxX, uint16_t minY, uint16_t maxY) {
#ifdef CYD_35
    // GT911 capacitive touch — no calibration needed
    (void)minX; (void)maxX; (void)minY; (void)maxY;
#else
    // Legacy function — kept for compatibility
    touch_cal_x_source = 1;  // rawY→screenX
    touch_cal_x_min = minX;
    touch_cal_x_max = maxX;
    touch_cal_y_source = 0;  // rawX→screenY
    touch_cal_y_min = minY;
    touch_cal_y_max = maxY;
#endif
}

// Forward declare saveSettings from utils.cpp
extern void saveSettings();

void runTouchCalibration() {
#ifdef CYD_35
    // GT911 capacitive touch — factory calibrated, no user calibration needed
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(2);
    tft.setCursor(20, CYD_SCREEN_HEIGHT / 2 - 30);
    tft.println("GT911 CAPACITIVE");
    tft.setCursor(20, CYD_SCREEN_HEIGHT / 2);
    tft.println("No calibration needed");
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(20, CYD_SCREEN_HEIGHT / 2 + 40);
    tft.println("Factory calibrated - touch is ready!");
    tft.setCursor(20, CYD_SCREEN_HEIGHT / 2 + 60);
    tft.println("BOOT button = exit");
    while (digitalRead(BOOT_BUTTON) == HIGH) delay(10);
    while (digitalRead(BOOT_BUTTON) == LOW) delay(10);
    tft.fillScreen(TFT_BLACK);
#else
    // 4-corner calibration — captures raw values, computes mapping, saves to EEPROM
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(0xF81F);  // HALEHOUND_PINK
    tft.setTextSize(2);
    tft.setCursor(15, 100);
    tft.println("TOUCH CALIBRATION");
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(15, 140);
    tft.println("Touch the crosshairs when");
    tft.setCursor(15, 155);
    tft.println("they appear on screen.");
    tft.setCursor(15, 180);
    tft.println("Hold steady until it turns green.");
    tft.setCursor(15, 210);
    tft.setTextColor(TFT_YELLOW);
    tft.println("BOOT button = cancel");

    delay(2500);

    // Collect raw touch data at 4 known screen corners
    // Using inset positions so crosshairs are visible
    // Positions adapt to current rotation dimensions
    int calW = tft.width();
    int calH = tft.height();
    uint16_t rawPoints[4][2];  // [corner][0=rawX, 1=rawY]
    const char* cornerNames[] = {"TOP-LEFT", "TOP-RIGHT", "BOT-LEFT", "BOT-RIGHT"};
    int crossX[] = {20, calW - 20, 20, calW - 20};   // screen X positions
    int crossY[] = {20, 20, calH - 20, calH - 20};   // screen Y positions

    for (int i = 0; i < 4; i++) {
        tft.fillScreen(TFT_BLACK);

        // Draw crosshair at target position
        tft.drawLine(crossX[i] - 15, crossY[i], crossX[i] + 15, crossY[i], 0xF81F);
        tft.drawLine(crossX[i], crossY[i] - 15, crossX[i], crossY[i] + 15, 0xF81F);
        tft.drawCircle(crossX[i], crossY[i], 10, 0xF81F);

        // Label
        tft.setTextColor(TFT_WHITE);
        tft.setTextSize(2);
        tft.setCursor(40, 150);
        tft.print("Touch ");
        tft.println(cornerNames[i]);

        // Wait for touch with BOOT button cancel
        bool cancelled = false;
        while (!touch.touched()) {
            if (digitalRead(BOOT_BUTTON) == LOW) {
                cancelled = true;
                break;
            }
            delay(10);
        }
        if (cancelled) {
            // Keep existing/default cal values — they're good enough
            touch_calibrated = true;
            saveSettings();
            tft.fillScreen(TFT_BLACK);
            tft.setTextColor(TFT_RED);
            tft.setTextSize(2);
            tft.setCursor(40, 150);
            tft.println("CANCELLED");
            tft.setTextColor(TFT_WHITE);
            tft.setTextSize(1);
            tft.setCursor(40, 180);
            tft.println("Using default calibration");
            delay(1500);
            tft.fillScreen(TFT_BLACK);
            return;
        }

        // Debounce — take average of 5 readings
        delay(100);
        uint32_t sumX = 0, sumY = 0;
        int samples = 0;
        for (int s = 0; s < 5; s++) {
            CYD28_TS_Point p = touch.getPointRaw();
            if (p.z > 100) {
                sumX += p.x;
                sumY += p.y;
                samples++;
            }
            delay(30);
        }

        if (samples == 0) {
            // Bad read — retry this corner
            i--;
            continue;
        }

        rawPoints[i][0] = sumX / samples;  // average rawX
        rawPoints[i][1] = sumY / samples;  // average rawY

        // Show success
        tft.drawCircle(crossX[i], crossY[i], 10, TFT_GREEN);
        tft.drawLine(crossX[i] - 15, crossY[i], crossX[i] + 15, crossY[i], TFT_GREEN);
        tft.drawLine(crossX[i], crossY[i] - 15, crossX[i], crossY[i] + 15, TFT_GREEN);

        tft.setTextColor(TFT_GREEN);
        tft.setTextSize(1);
        tft.setCursor(40, 180);
        tft.printf("Raw X:%d Y:%d", rawPoints[i][0], rawPoints[i][1]);

        // Wait for release
        while (touch.touched()) delay(10);
        delay(400);
    }

    // ── Compute mapping from captured corners ──
    // TL=0, TR=1, BL=2, BR=3
    // Screen X varies: TL(20)→TR(220) = left→right
    // Screen Y varies: TL(20)→BL(300) = top→bottom

    // Figure out which RAW axis maps to screen X (horizontal)
    // Compare TL vs TR — whichever raw axis has biggest difference = screen X source
    int diffRawX_horiz = abs((int)rawPoints[1][0] - (int)rawPoints[0][0]);  // TR.rawX - TL.rawX
    int diffRawY_horiz = abs((int)rawPoints[1][1] - (int)rawPoints[0][1]);  // TR.rawY - TL.rawY

    uint8_t xSrc, ySrc;
    uint16_t xMin, xMax, yMin, yMax;

    // Inset is 20px from edges, so span between crosshairs:
    int xSpan = calW - 40;   // horizontal span (was hardcoded 200)
    int ySpan = calH - 40;   // vertical span (was hardcoded 280)
    int maxScreenX = calW - 1;  // max screen X coordinate (was hardcoded 239)
    int maxScreenY = calH - 1;  // max screen Y coordinate (was hardcoded 319)

    if (diffRawX_horiz > diffRawY_horiz) {
        // rawX varies horizontally → rawX controls screenX
        xSrc = 0;  // rawX → screenX
        ySrc = 1;  // rawY → screenY

        // TL is screenX=20, TR is screenX=(calW-20)
        // Extrapolate to 0 and maxScreenX
        int rawLeft = ((int)rawPoints[0][0] + (int)rawPoints[2][0]) / 2;   // avg TL+BL rawX
        int rawRight = ((int)rawPoints[1][0] + (int)rawPoints[3][0]) / 2;  // avg TR+BR rawX
        xMin = rawLeft + (rawLeft - rawRight) * 20 / xSpan;                // extrapolate to screenX=0
        xMax = rawRight + (rawRight - rawLeft) * (maxScreenX - (calW - 20)) / xSpan;  // to screenX=max

        int rawTop = ((int)rawPoints[0][1] + (int)rawPoints[1][1]) / 2;    // avg TL+TR rawY
        int rawBot = ((int)rawPoints[2][1] + (int)rawPoints[3][1]) / 2;    // avg BL+BR rawY
        yMin = rawTop + (rawTop - rawBot) * 20 / ySpan;                    // extrapolate to screenY=0
        yMax = rawBot + (rawBot - rawTop) * (maxScreenY - (calH - 20)) / ySpan;       // to screenY=max
    } else {
        // rawY varies horizontally → rawY controls screenX
        xSrc = 1;  // rawY → screenX
        ySrc = 0;  // rawX → screenY

        int rawLeft = ((int)rawPoints[0][1] + (int)rawPoints[2][1]) / 2;   // avg TL+BL rawY
        int rawRight = ((int)rawPoints[1][1] + (int)rawPoints[3][1]) / 2;  // avg TR+BR rawY
        xMin = rawLeft + (rawLeft - rawRight) * 20 / xSpan;
        xMax = rawRight + (rawRight - rawLeft) * (maxScreenX - (calW - 20)) / xSpan;

        int rawTop = ((int)rawPoints[0][0] + (int)rawPoints[1][0]) / 2;    // avg TL+TR rawX
        int rawBot = ((int)rawPoints[2][0] + (int)rawPoints[3][0]) / 2;    // avg BL+BR rawX
        yMin = rawTop + (rawTop - rawBot) * 20 / ySpan;
        yMax = rawBot + (rawBot - rawTop) * (maxScreenY - (calH - 20)) / ySpan;
    }

    // Apply to globals
    touch_cal_x_source = xSrc;
    touch_cal_x_min = xMin;
    touch_cal_x_max = xMax;
    touch_cal_y_source = ySrc;
    touch_cal_y_min = yMin;
    touch_cal_y_max = yMax;
    touch_calibrated = true;

    // Save to EEPROM
    saveSettings();

    #if CYD_DEBUG
    Serial.println("[TOUCH] Calibration computed and saved:");
    Serial.printf("  screenX: %s raw %d→%d\n", xSrc ? "rawY" : "rawX", xMin, xMax);
    Serial.printf("  screenY: %s raw %d→%d\n", ySrc ? "rawY" : "rawX", yMin, yMax);
    for (int i = 0; i < 4; i++) {
        Serial.printf("  %s: rawX=%d rawY=%d\n", cornerNames[i], rawPoints[i][0], rawPoints[i][1]);
    }
    #endif

    // Show results
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(2);
    tft.setCursor(20, 40);
    tft.println("CALIBRATION SAVED!");

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(20, 80);
    tft.printf("X: %s %d -> %d", xSrc ? "rawY" : "rawX", xMin, xMax);
    tft.setCursor(20, 100);
    tft.printf("Y: %s %d -> %d", ySrc ? "rawY" : "rawX", yMin, yMax);

    tft.setCursor(20, 140);
    tft.setTextColor(TFT_YELLOW);
    tft.println("Touch anywhere to test...");
    tft.setCursor(20, 155);
    tft.println("BOOT button = exit");

    // Quick test loop — let user verify calibration works
    while (digitalRead(BOOT_BUTTON) == HIGH) {
        if (touch.touched()) {
            CYD28_TS_Point p = touch.getPointRaw();
            if (p.z > 100) {
                int sx = touchMapX(p);
                int sy = touchMapY(p);
                tft.fillCircle(sx, sy, 4, 0xF81F);

                tft.fillRect(20, 180, SCREEN_WIDTH - 40, 20, TFT_BLACK);
                tft.setTextColor(TFT_CYAN);
                tft.setCursor(20, 180);
                tft.printf("Screen: %d, %d", sx, sy);
            }
        }
        delay(30);
    }
    while (digitalRead(BOOT_BUTTON) == LOW) delay(10);

    tft.fillScreen(TFT_BLACK);
#endif
}

// ═══════════════════════════════════════════════════════════════════════════
// DEBUG
// ═══════════════════════════════════════════════════════════════════════════

String getButtonName(ButtonID btn) {
    switch (btn) {
        case BTN_NONE:   return "NONE";
        case BTN_UP:     return "UP";
        case BTN_DOWN:   return "DOWN";
        case BTN_LEFT:   return "LEFT";
        case BTN_RIGHT:  return "RIGHT";
        case BTN_SELECT: return "SELECT";
        case BTN_BACK:   return "BACK";
        case BTN_BOOT:   return "BOOT";
        default:         return "?";
    }
}

void printTouchDebug() {
    #if CYD_DEBUG
    Serial.println("═══════════════════════════════════════");
#ifdef CYD_35
    Serial.println("         TOUCH DEBUG (GT911)");
#else
    Serial.println("         TOUCH DEBUG (XPT2046)");
#endif
    Serial.println("═══════════════════════════════════════");

#ifdef CYD_35
    gt911CachedRead();
    bool touched = gt911.isTouched;
    Serial.println("Touched:    " + String(touched ? "YES" : "NO"));
    if (touched) {
        Serial.println("X:          " + String(gt911.points[0].x));
        Serial.println("Y:          " + String(gt911.points[0].y));
        Serial.println("Points:     " + String(gt911.touches));
        Serial.println("Zone:       " + getButtonName(getTouchZone(gt911.points[0].x, gt911.points[0].y)));
    }
#else
    bool touched = touch.touched();
    Serial.println("Touched:    " + String(touched ? "YES" : "NO"));

    if (touched) {
        CYD28_TS_Point p = touch.getPointScaled();
        Serial.println("Raw X:      " + String(p.x));
        Serial.println("Raw Y:      " + String(p.y));
        Serial.println("Raw Z:      " + String(p.z));

        uint16_t screenX, screenY;
        if (getTouchPoint(&screenX, &screenY)) {
            Serial.println("Screen X:   " + String(screenX));
            Serial.println("Screen Y:   " + String(screenY));
            Serial.println("Zone:       " + getButtonName(getTouchZone(screenX, screenY)));
        }
    }

    Serial.println("───────────────────────────────────────");
    Serial.println("Calibration:");
    Serial.printf("  screenX: %s %d -> %d\n", touch_cal_x_source ? "rawY" : "rawX", touch_cal_x_min, touch_cal_x_max);
    Serial.printf("  screenY: %s %d -> %d\n", touch_cal_y_source ? "rawY" : "rawX", touch_cal_y_min, touch_cal_y_max);
    Serial.printf("  calibrated: %s\n", touch_calibrated ? "YES" : "NO (defaults)");
#endif

    Serial.println("BOOT btn:   " + String(digitalRead(BOOT_BUTTON) == LOW ? "PRESSED" : "released"));
    Serial.println("───────────────────────────────────────");
    Serial.println("Button states:");

    for (int i = 1; i < BTN_COUNT; i++) {
        String state;
        switch (buttonStates[i]) {
            case BTN_STATE_IDLE:     state = "idle"; break;
            case BTN_STATE_PRESSED:  state = "PRESSED"; break;
            case BTN_STATE_HELD:     state = "HELD"; break;
            case BTN_STATE_RELEASED: state = "released"; break;
        }
        Serial.println("  " + getButtonName((ButtonID)i) + ": " + state);
    }

    Serial.println("═══════════════════════════════════════");
    #endif
}
