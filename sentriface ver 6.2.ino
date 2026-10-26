/*
  ============================================================================
  SENTRIFACE - ESP32 side (ver 6.2)
  ============================================================================
  This file combines:
    - From the "v5 / state-machine" version: a non-blocking main loop (no
      delay() calls that freeze the board), separate I2C buses for the LCD
      and OLED so they never interfere with each other, a sensor re-arm
      cooldown, and a scan timeout so the system never gets stuck waiting
      forever for the PC.
    - From "ver6": the Miku OLED animation (sleep / wink / tilt / awake),
      the green/red LEDs, the buzzer, the physical admin button, and
      showing the recognised person's name on the LCD.

  HOW TO USE THIS FILE
  ---------------------
  This chat delivered it as a plain .txt file. Arduino IDE needs a .ino
  file inside a FOLDER of the exact same name. To use it:
    1. Create a folder called  sentriface_merged
    2. Save this file inside that folder as  sentriface_merged.ino
       (just change the extension from .txt to .ino)
    3. Open sentriface_merged.ino in Arduino IDE.

  LIBRARIES YOU MUST INSTALL (Tools -> Manage Libraries)
  ---------------------------------------------------------
    - Adafruit SSD1306
    - Adafruit GFX Library
    - ESP32Servo
    - LiquidCrystal I2C  (by Frank de Brabander -- other forks of this
                          library may not have begin(cols, rows))
  Board package: "esp32 by Espressif" version 3.x (needed for tone/LEDC
  functions used by the buzzer helper below).

  ============================================================================
  HARDWARE WIRING (this is exactly how YOUR circuit is wired)
  ============================================================================
    Part                 | Pin on part | ESP32 GPIO | Notes
    ---------------------|-------------|------------|---------------------------
    16x2 I2C LCD          SDA          | 32         | "LCD bus" = Wire
                           SCL          | 33         |
                           VCC          | 5V (VIN)   | needs 5V for backlight
                           GND          | GND        |
    ---------------------|-------------|------------|---------------------------
    0.96" OLED (SSD1306)  SDA          | 25         | "OLED bus" = Wire1 (GPIO 25)
                           SCL          | 26         | (GPIO 26)
                           VCC          | 3.3V       |
                           GND          | GND        |
    ---------------------|-------------|------------|---------------------------
    IR obstacle sensor    OUT          | 27         | 3.3V logic, see below
                           VCC          | 3.3V       | NOT 5V -- can exceed the
                           GND          | GND        | ESP32 pin's safe voltage
    ---------------------|-------------|------------|---------------------------
    SG90 servo (the door) signal       | 13         |
                           red (VCC)   | 5V (VIN)   | NOT 3.3V -- too weak,
                           brown (GND) | GND        | causes jitter / resets
    ---------------------|-------------|------------|---------------------------
    Green LED (granted)   anode (+)    | 4          | through a ~220 ohm resistor
    Red LED (denied)      anode (+)    | 16         | through a ~220 ohm resistor
    Buzzer                +            | 2          | passive or active buzzer
    Admin override button  one leg     | 15         | other leg to GND
                                                      (uses the internal pull-up,
                                                       so no resistor needed)

  IMPORTANT: GPIO 2 and GPIO 15 are "strapping pins" on the ESP32 -- the
  chip briefly looks at their voltage at boot/reset to decide how to start
  up. A button or buzzer held LOW at the exact moment of reset can, on some
  boards, interfere with booting. In practice it is usually fine for a
  button, but if you ever see boot problems, this is the first thing to
  suspect and move to a different free GPIO (e.g. 17, 18, 19, 23, 25, 26).

  Why two separate I2C buses (Wire for the LCD, Wire1 for the OLED)?
  Both displays use the I2C protocol, but each has its own fixed address
  (the LCD backpack is usually 0x27, the OLED is usually 0x3C). I2C lets
  many devices share one pair of wires AS LONG AS they are all started on
  that same bus. Putting them on two separate buses instead is simpler to
  reason about and matches exactly how you wired the real board (LCD on
  32/33, OLED on 25/26) -- it also means a problem with one display can
  never block the other.

  ============================================================================
  SERIAL PROTOCOL (how this board and the PC Python script talk)
  ============================================================================
  Baud rate: 115200. Every message is plain text ending with a newline.

    ESP32 -> PC   MOTION_DETECTED        Sensor triggered. PC should open
                                          its webcam now.
    PC -> ESP32   CAMERA_READY           The webcam is open and the PC has
                                          started looking for a face.
    PC -> ESP32   ACCESS_GRANTED         A face was found (no name).
    PC -> ESP32   ACCESS_GRANTED:<name>  A face was found AND matched to a
                                          named person (for a later upgrade
                                          where specific people are
                                          recognised, not just "any face").
    PC -> ESP32   NO_FACE                No face was seen before the PC's
                                          own timeout.
    PC -> ESP32   ACCESS_DENIED          A face was seen but did not match
                                          anyone (also a later-upgrade case).
    ESP32 -> PC   CAMERA_CLOSE           This cycle is finished. PC should
                                          release/close its webcam.

  This board accepts BOTH "ACCESS_GRANTED" and "ACCESS_GRANTED:<name>", so
  it works with the simple "any face opens the door" Python script AND
  with a future script that recognises specific people.

  ============================================================================
  WHAT HAPPENS, STEP BY STEP (the state machine)
  ============================================================================
    SLEEP
      Displays are off. We are only reading the sensor, waiting for it to
      trigger.
          |  sensor triggers
          v
    WAKING
      We send MOTION_DETECTED. The OLED plays the Miku "waking up"
      animation (wink -> tilt -> awake) while the LCD says "Motion
      detected". This is just a visual flourish; it does not block
      anything else from happening.
          |  animation finishes, OR the admin button is pressed, OR
          |  CAMERA_READY / ACCESS_GRANTED arrives early
          v
    WAITING_CAMERA
      Waiting for the PC to say CAMERA_READY. If the PC (or its Python
      script) is not even running, this step would wait forever on its
      own -- so there is a timeout that gives up and goes to DENIED
      after a while.
          |  CAMERA_READY received
          v
    SCANNING
      The OLED shows a "scanning" animation (a moving indicator, since we
      don't know how long the PC will take). We are listening for
      ACCESS_GRANTED / ACCESS_GRANTED:<name> / NO_FACE / ACCESS_DENIED.
      The admin button also works here, and instantly grants access
      without waiting for the camera (useful for testing, or as a manual
      override).
          |  granted                        |  denied / no face / timeout
          v                                  v
    DOOR_OPEN                           DENIED
      LCD + OLED show "MATCH!" / the     LCD + OLED show "DENIED".
      person's name if one was sent.     Red LED + buzzer.
      Green LED + buzzer. The servo
      swings open.
          |  holds open 5 seconds
          v
    DOOR_CLOSING
      Servo returns to closed. LCD shows "Door closed".
          |
          +----------------------<-----------------------+
          v
    COOLDOWN
      We tell the PC CAMERA_CLOSE (release the webcam). We wait a few
      seconds AND wait for the sensor to stop being triggered (so a
      person still standing in front of the sensor doesn't instantly
      re-open everything) before going back to SLEEP.
          |
          v
        SLEEP  (back to the top)

  ============================================================================
*/

#include <Wire.h>
#include <ESP32Servo.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Forward declaration of State enum for Arduino preprocessor
enum State {
  ST_SLEEP,            // displays off, only watching the sensor
  ST_WAKING,            // Miku wink -> tilt -> awake animation playing
  ST_WAITING_CAMERA,    // sent MOTION_DETECTED, waiting for CAMERA_READY
  ST_SCANNING,          // camera is live, waiting for a face result
  ST_VERIFYING,         // short pause + loading bar after a grant, for effect
  ST_DOOR_OPEN,         // servo open, holding
  ST_DOOR_CLOSING,      // "Door closed" message showing
  ST_DENIED,            // red LED + buzzer + DENIED message
  ST_COOLDOWN           // sent CAMERA_CLOSE, waiting before re-arming
};

// ============================================================================
// PIN AND HARDWARE CONSTANTS -- matches the wiring table at the top of this
// file. If you rewire something, this is the only place you should need to
// change a pin number.
// ============================================================================

// --- I2C bus pins ---
const int SDA_LCD  = 32, SCL_LCD  = 33;   // LCD lives on the "Wire" bus
const int SDA_OLED = 25, SCL_OLED = 26;   // OLED lives on the "Wire1" bus (GPIO 25/26)

// --- I2C device addresses ---
// If your LCD or OLED doesn't show anything, run the separate
// i2c_scanner sketch to find the real addresses and change these two
// lines. 0x27 and 0x3C are the most common, 0x3F and 0x3D are the next
// most common alternates.
const uint8_t LCD_ADDR  = 0x27;
const uint8_t OLED_ADDR = 0x3C;

// --- Other component pins ---
const int SERVO_PIN   = 13;
const int BUZZER_PIN  = 2;
const int LED_GREEN    = 4;
const int LED_RED      = 16;
const int BUTTON_PIN   = 15;   // admin override button, wired to GND, uses
                                // the internal pull-up resistor (see setup())
const int SENSOR_PIN   = 27;
const int BUZZER_LEDC_CH = 2;    // LEDC channel for buzzer on Core 2.x -- must NOT match BUTTON_PIN (15)

// --- Sensor polarity ---
// Different sensor modules report "something detected" differently:
//   - A PIR motion sensor (and the Wokwi simulator's PIR) outputs HIGH
//     when it sees motion.
//   - Most small IR obstacle-avoidance modules (the kind with a trim-pot
//     for sensitivity) do the OPPOSITE: they output LOW when an object is
//     in front of them, and HIGH when the path is clear.
// Your real hardware is the IR obstacle module, so this should be LOW.
// If you test in the Wokwi simulator (which uses a PIR), change this
// back to HIGH for that test, then back to LOW again for real hardware.
const int SENSOR_ACTIVE_LEVEL = LOW;

// ============================================================================
// TIMING CONSTANTS -- all the "how long" numbers in one place, in
// milliseconds, so you can tune the system's feel without hunting through
// the code.
// ============================================================================
const unsigned long MIKU_FRAME_MS      = 650;    // how long each Miku wake frame shows
const unsigned long CAMERA_WAIT_MS     = 10000;  // give up waiting for CAMERA_READY after this long
const unsigned long SCAN_TIMEOUT_MS    = 20000;  // give up waiting for a face result after this long
const unsigned long VERIFY_MS          = 1200;   // short "verifying" pause before showing MATCH!
const unsigned long DOOR_OPEN_MS       = 5000;   // how long the servo stays open
const unsigned long DOOR_CLOSE_PAUSE_MS= 1200;   // "Door closed" message hang time
const unsigned long DENIED_MS          = 2000;   // how long the DENIED screen / buzzer last
const unsigned long COOLDOWN_MS        = 3000;   // minimum wait before re-arming
const unsigned long IDLE_TIMEOUT_MS    = 30000;  // if awake this long with nothing happening, go to sleep

const int SERVO_CLOSED = 15;   // 15 deg avoids hitting physical end stop & motor stall
const int SERVO_OPEN   = 90;    // servo angle = door open (90 is gentler on an
                                 // SG90 than a full 180 -- it still clearly
                                 // reads as "open" for a demo)

// ============================================================================
// MIKU BITMAP FRAMES (64x64 pixels each)
// ============================================================================
// These are just raw image data for drawMiku() to display -- you do not
// need to read or edit these arrays. They were carried over unchanged
// from the earlier version of this project.
// ============================================================================
const unsigned char mikuSleep[] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xFC, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x3C, 0x38, 0x03, 0x80, 0x00, 0x00, 0x00, 0x00, 0x36, 0xC0, 0x00, 0x6F, 0xE0, 0x00,
  0x00, 0x07, 0xE7, 0x00, 0x00, 0x1E, 0x60, 0x00, 0x00, 0x08, 0x6C, 0x00, 0x00, 0x06, 0x3F, 0x00,
  0x00, 0x30, 0x48, 0x00, 0x00, 0x07, 0x3F, 0x00, 0x00, 0x20, 0xD0, 0x00, 0x00, 0x03, 0x30, 0x80,
  0x00, 0x20, 0xC8, 0x00, 0x00, 0x01, 0x30, 0x40, 0x00, 0x41, 0x98, 0x00, 0x00, 0x01, 0xB0, 0x40,
  0x00, 0x43, 0x20, 0x00, 0x00, 0x00, 0x98, 0x40, 0x00, 0xC2, 0x60, 0x00, 0x00, 0x00, 0x58, 0x60,
  0x00, 0x84, 0xC0, 0x00, 0x00, 0x00, 0x58, 0x30, 0x01, 0x8F, 0x80, 0x42, 0x00, 0x00, 0x58, 0x30,
  0x01, 0x8B, 0x80, 0xC2, 0x00, 0x08, 0x38, 0x30, 0x01, 0x81, 0x80, 0x86, 0x00, 0x04, 0x38, 0x30,
  0x03, 0x03, 0x01, 0x8E, 0x04, 0x04, 0x3C, 0x30, 0x03, 0x02, 0x01, 0x0A, 0x06, 0x06, 0x1C, 0x38,
  0x03, 0x06, 0x03, 0x1A, 0x0D, 0xC6, 0x16, 0x18, 0x03, 0x06, 0x03, 0xE2, 0x0D, 0x3E, 0x16, 0x18,
  0x03, 0x04, 0x06, 0x02, 0x0C, 0x8E, 0x16, 0x18, 0x02, 0x04, 0x06, 0x43, 0x0C, 0xC6, 0x16, 0x18,
  0x03, 0x0C, 0x06, 0x81, 0x0C, 0x46, 0x16, 0x18, 0x06, 0x0C, 0x04, 0x81, 0x0C, 0x26, 0x1E, 0x18,
  0x06, 0x0C, 0x07, 0x00, 0x8C, 0x16, 0x08, 0x18, 0x06, 0x0C, 0x06, 0x00, 0x8C, 0x0E, 0x08, 0x18,
  0x06, 0x0C, 0x84, 0x00, 0x4C, 0x06, 0x08, 0x0C, 0x06, 0x0D, 0x84, 0x00, 0x48, 0x06, 0x08, 0x0C,
  0x04, 0x0D, 0x84, 0x00, 0x2C, 0x06, 0x08, 0x0C, 0x04, 0x03, 0x84, 0xF8, 0x19, 0xF6, 0x18, 0x0C,
  0x04, 0x03, 0x87, 0x8C, 0x03, 0x1E, 0x18, 0x0C, 0x04, 0x03, 0xC6, 0x04, 0x00, 0x0C, 0x18, 0x0C,
  0x04, 0x03, 0x44, 0x00, 0x00, 0x04, 0x58, 0x0C, 0x0C, 0x03, 0x46, 0x00, 0x00, 0x04, 0x58, 0x0C,
  0x08, 0x03, 0x26, 0x00, 0x00, 0x04, 0xF8, 0x0C, 0x08, 0x03, 0x26, 0x00, 0x00, 0x0C, 0xA8, 0x06,
  0x08, 0x03, 0x37, 0x00, 0x90, 0x0D, 0x88, 0x06, 0x08, 0x03, 0x17, 0x80, 0x00, 0x39, 0x08, 0x06,
  0x08, 0x03, 0x0E, 0xE0, 0x00, 0xEB, 0x08, 0x06, 0x08, 0x03, 0x06, 0x3E, 0x0F, 0x8E, 0x08, 0x06,
  0x08, 0x03, 0x02, 0x0F, 0xFE, 0x1C, 0x08, 0x06, 0x08, 0x03, 0x00, 0x19, 0x13, 0x18, 0x08, 0x02,
  0x18, 0x03, 0x00, 0x30, 0xE1, 0x80, 0x10, 0x02, 0x10, 0x01, 0x80, 0xE0, 0x40, 0xC0, 0x10, 0x06,
  0x10, 0x01, 0x81, 0xE0, 0x40, 0xF0, 0x10, 0x03, 0x10, 0x01, 0x82, 0x90, 0xA1, 0x78, 0x10, 0x01,
  0x10, 0x01, 0x86, 0xD1, 0x11, 0x6C, 0x10, 0x03, 0x10, 0x00, 0x84, 0xCB, 0x1A, 0x6C, 0x10, 0x01,
  0x10, 0x00, 0xCC, 0x85, 0xF4, 0x64, 0x20, 0x01, 0x11, 0x00, 0xC8, 0x81, 0x10, 0x66, 0x20, 0x01,
  0x11, 0x00, 0xD8, 0x83, 0x10, 0x26, 0x20, 0x03, 0x11, 0x00, 0xD1, 0x83, 0x10, 0x23, 0x20, 0x13,
  0x13, 0x00, 0xD0, 0x83, 0x10, 0x23, 0x60, 0x11, 0x13, 0x80, 0x7F, 0x82, 0x08, 0x3F, 0xC0, 0x13,
  0x0B, 0x80, 0x61, 0x82, 0x08, 0x38, 0xC0, 0x32, 0x0A, 0xC0, 0x61, 0x82, 0x08, 0x31, 0xC0, 0x76,
  0x0E, 0x60, 0x7F, 0x82, 0x08, 0x3F, 0x80, 0xDC, 0x04, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x98,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const unsigned char mikuAwake[] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFF, 0x80, 0x00, 0x00,
  0x00, 0x00, 0x0F, 0x0E, 0x00, 0x70, 0xF8, 0x00, 0x00, 0x00, 0x0C, 0xF0, 0x00, 0x0D, 0xFC, 0x00,
  0x00, 0x01, 0xFC, 0xE0, 0x00, 0x03, 0xCC, 0x00, 0x00, 0x02, 0x19, 0x80, 0x00, 0x01, 0xCF, 0xE0,
  0x00, 0x0C, 0x1B, 0x00, 0x00, 0x00, 0xCC, 0x20, 0x00, 0x08, 0x3A, 0x00, 0x00, 0x00, 0x6C, 0x10,
  0x00, 0x08, 0x38, 0x00, 0x00, 0x00, 0x64, 0x08, 0x00, 0x18, 0x22, 0x00, 0x00, 0x00, 0x36, 0x08,
  0x00, 0x10, 0x44, 0x00, 0x00, 0x00, 0x12, 0x08, 0x00, 0x10, 0x48, 0x00, 0x00, 0x00, 0x12, 0x0C,
  0x00, 0x20, 0x98, 0x00, 0x00, 0x00, 0x0A, 0x04, 0x00, 0x61, 0xF0, 0x18, 0x40, 0x00, 0x0B, 0x04,
  0x00, 0x61, 0x60, 0x10, 0xC0, 0x01, 0x87, 0x04, 0x00, 0x60, 0x60, 0x20, 0xC0, 0x00, 0x85, 0x84,
  0x00, 0x60, 0x40, 0x21, 0xC1, 0x80, 0x85, 0x84, 0x00, 0x40, 0xC0, 0x42, 0x41, 0xC0, 0xC7, 0x86,
  0x00, 0xC0, 0xC0, 0x46, 0x41, 0xF8, 0xC3, 0xC2, 0x00, 0xC0, 0x80, 0x78, 0x41, 0x27, 0xC2, 0xC2,
  0x00, 0xC0, 0x80, 0xC8, 0x41, 0x11, 0xC2, 0xC2, 0x00, 0xC1, 0x80, 0x90, 0x41, 0x00, 0xC2, 0xC2,
  0x00, 0x81, 0x80, 0x90, 0x41, 0x08, 0xC2, 0xC2, 0x01, 0x81, 0x80, 0xA0, 0x61, 0x04, 0xC3, 0xC2,
  0x01, 0x81, 0x80, 0xC0, 0x21, 0x02, 0xC3, 0x82, 0x01, 0x81, 0x80, 0x9E, 0x21, 0x3F, 0x82, 0x02,
  0x01, 0x81, 0x90, 0xEF, 0x11, 0x7B, 0x82, 0x01, 0x01, 0x81, 0xA0, 0xC7, 0x91, 0xDD, 0x82, 0x01,
  0x01, 0x81, 0xB0, 0xD7, 0x09, 0x1C, 0x82, 0x01, 0x01, 0x81, 0xF0, 0x9F, 0x07, 0x7C, 0x82, 0x01,
  0x01, 0x80, 0x70, 0x9F, 0x02, 0x7C, 0x83, 0x01, 0x01, 0x80, 0x70, 0x99, 0x00, 0x6D, 0x8B, 0x01,
  0x01, 0x00, 0x58, 0x8A, 0x00, 0x29, 0x8B, 0x01, 0x03, 0x00, 0x58, 0x80, 0x00, 0x01, 0x9B, 0x01,
  0x03, 0x00, 0x48, 0x80, 0x00, 0x01, 0x9F, 0x01, 0x03, 0x00, 0x4C, 0xC0, 0x00, 0x01, 0x17, 0x00,
  0x03, 0x00, 0x44, 0xE0, 0x04, 0x03, 0x33, 0x00, 0x03, 0x00, 0x42, 0xB0, 0x00, 0x07, 0x23, 0x00,
  0x03, 0x00, 0x43, 0x9C, 0x00, 0x19, 0x43, 0x00, 0x03, 0x00, 0x41, 0xC7, 0x81, 0xE3, 0xC3, 0x00,
  0x03, 0x00, 0x40, 0xC1, 0xFF, 0x83, 0x83, 0x00, 0x03, 0x00, 0x60, 0x03, 0x66, 0xC3, 0x06, 0x00,
  0x06, 0x00, 0x20, 0x06, 0x3C, 0x60, 0x06, 0x00, 0x06, 0x00, 0x20, 0x1C, 0x18, 0x38, 0x06, 0x00,
  0x86, 0x00, 0x20, 0x7C, 0x18, 0x3C, 0x06, 0x00, 0x86, 0x00, 0x20, 0xD4, 0x24, 0x2A, 0x06, 0x00,
  0x86, 0x00, 0x20, 0x92, 0x66, 0x4B, 0x06, 0x00, 0x86, 0x00, 0x20, 0x93, 0xE7, 0xC9, 0x84, 0x00,
  0x86, 0x00, 0x11, 0x91, 0xFF, 0x88, 0x8C, 0x00, 0x86, 0x00, 0x11, 0x10, 0x46, 0x08, 0xC8, 0x00,
  0x06, 0x40, 0x13, 0x30, 0x46, 0x08, 0xC8, 0x00, 0x06, 0x40, 0x12, 0x20, 0x46, 0x0C, 0x58, 0x04,
  0x06, 0x40, 0x16, 0x20, 0x42, 0x0C, 0x50, 0x04, 0x06, 0x40, 0x1F, 0xE0, 0xC2, 0x0F, 0xF0, 0x0C,
  0x02, 0x60, 0x08, 0x60, 0x82, 0x0C, 0x30, 0x0C, 0x03, 0x60, 0x0C, 0x20, 0x82, 0x0C, 0x30, 0x0D,
  0x01, 0xF0, 0x0F, 0xE0, 0x82, 0x0F, 0xF0, 0x17, 0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const unsigned char mikuWink[] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xE0, 0x00, 0x00,
  0x00, 0x00, 0x03, 0xC3, 0x80, 0x1C, 0x7E, 0x00, 0x00, 0x00, 0x03, 0x7C, 0x00, 0x03, 0x7F, 0x00,
  0x00, 0x00, 0xFF, 0x38, 0x00, 0x01, 0xF3, 0x00, 0x00, 0x01, 0x06, 0x60, 0x00, 0x00, 0x73, 0xF8,
  0x00, 0x03, 0x06, 0x40, 0x00, 0x00, 0x33, 0x88, 0x00, 0x02, 0x0E, 0x80, 0x00, 0x00, 0x3B, 0x04,
  0x00, 0x06, 0x0E, 0x00, 0x00, 0x00, 0x19, 0x82, 0x00, 0x04, 0x08, 0x80, 0x00, 0x00, 0x0D, 0x82,
  0x00, 0x04, 0x11, 0x00, 0x00, 0x00, 0x05, 0x82, 0x00, 0x08, 0x12, 0x00, 0x00, 0x00, 0x04, 0x83,
  0x00, 0x08, 0x26, 0x00, 0x00, 0x00, 0x02, 0x81, 0x00, 0x08, 0x7C, 0x02, 0x10, 0x00, 0x02, 0xC1,
  0x00, 0x08, 0x78, 0x04, 0x10, 0x00, 0x41, 0xC1, 0x00, 0x08, 0x18, 0x0C, 0x30, 0x00, 0x61, 0x61,
  0x00, 0x10, 0x10, 0x08, 0x70, 0x60, 0x21, 0x61, 0x00, 0x10, 0x30, 0x18, 0xD0, 0x60, 0x21, 0xE1,
  0x00, 0x10, 0x30, 0x10, 0x90, 0x5E, 0x21, 0xB0, 0x00, 0x10, 0x20, 0x13, 0x90, 0x49, 0xB1, 0xB0,
  0x00, 0x10, 0x20, 0x3A, 0x10, 0x48, 0x60, 0xB0, 0x00, 0x10, 0x60, 0x22, 0x10, 0x44, 0x30, 0xB0,
  0x00, 0x10, 0x60, 0x24, 0x18, 0x42, 0x30, 0xF0, 0x00, 0x20, 0x60, 0x24, 0x18, 0x41, 0x20, 0xE0,
  0x00, 0x60, 0x60, 0x38, 0x08, 0x40, 0xA0, 0x80, 0x00, 0x20, 0x60, 0x30, 0x0C, 0x40, 0x60, 0x80,
  0x00, 0x60, 0x64, 0x20, 0x04, 0x47, 0xE0, 0x80, 0x00, 0x60, 0x6C, 0x20, 0x06, 0x5E, 0xE0, 0x80,
  0x00, 0x60, 0x6C, 0x20, 0x02, 0x57, 0x60, 0x80, 0x00, 0x60, 0x3C, 0x20, 0x01, 0xD7, 0x20, 0x80,
  0x00, 0x60, 0x3C, 0x2F, 0xC0, 0xDF, 0x20, 0xC0, 0x00, 0x40, 0x3C, 0x38, 0x60, 0x1F, 0x62, 0xC0,
  0x00, 0xC0, 0x36, 0x20, 0x00, 0x19, 0x62, 0xC0, 0x00, 0xC0, 0x26, 0x20, 0x00, 0x0A, 0x42, 0xC0,
  0x00, 0xC0, 0x22, 0x20, 0x00, 0x00, 0x47, 0xC0, 0x80, 0xC0, 0x23, 0x30, 0x00, 0x00, 0x45, 0xC0,
  0x80, 0xC0, 0x21, 0x30, 0x0D, 0x80, 0xCC, 0x40, 0x80, 0xC0, 0x21, 0xA8, 0x05, 0x01, 0xC8, 0x40,
  0x80, 0xC0, 0x20, 0xA6, 0x00, 0x07, 0x58, 0xC0, 0x80, 0x80, 0x20, 0x73, 0x80, 0x1C, 0xF0, 0xC0,
  0x81, 0x80, 0x20, 0x70, 0xFF, 0xF0, 0xE0, 0xC0, 0x81, 0x80, 0x10, 0x30, 0xD9, 0xB0, 0xC0, 0xC0,
  0x81, 0x80, 0x10, 0x01, 0x8B, 0x18, 0x00, 0x80, 0x41, 0x80, 0x10, 0x03, 0x06, 0x0C, 0x01, 0x80,
  0x41, 0x80, 0x10, 0x0F, 0x06, 0x0F, 0x01, 0x80, 0x41, 0x80, 0x10, 0x1D, 0x0E, 0x0B, 0x81, 0x80,
  0x41, 0x80, 0x10, 0x34, 0x89, 0x12, 0xC1, 0x80, 0x41, 0x80, 0x18, 0x24, 0x91, 0xB2, 0x41, 0x80,
  0x41, 0x00, 0x0C, 0x64, 0x7F, 0xE2, 0x41, 0x00, 0x41, 0x00, 0x0C, 0x44, 0x19, 0x82, 0x23, 0x00,
  0x41, 0x00, 0x0C, 0xC4, 0x11, 0x82, 0x22, 0x00, 0x41, 0x10, 0x0C, 0x88, 0x11, 0x83, 0x12, 0x00,
  0x41, 0x10, 0x06, 0x88, 0x10, 0x83, 0x16, 0x01, 0x81, 0x30, 0x03, 0xE8, 0x30, 0x83, 0x7C, 0x01,
  0x80, 0xB8, 0x07, 0x18, 0x20, 0x83, 0xCC, 0x01, 0x80, 0xB8, 0x03, 0x08, 0x20, 0x83, 0x0C, 0x03,
  0x00, 0xF6, 0x07, 0xF8, 0x20, 0x83, 0xFC, 0x07, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const unsigned char mikuTilt[] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x03, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0xE7, 0x7F, 0xE0, 0x00, 0x00,
  0x00, 0x00, 0x08, 0x67, 0xC0, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x10, 0x4E, 0x00, 0x03, 0x00, 0x00,
  0x00, 0x00, 0x20, 0xD8, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x60, 0xF0, 0x00, 0x00, 0x40, 0x00,
  0x00, 0x00, 0x41, 0x88, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0xC3, 0x10, 0x00, 0x00, 0x17, 0xC0,
  0x00, 0x00, 0x84, 0x60, 0x00, 0x00, 0x0F, 0xC0, 0x00, 0x01, 0x8F, 0x80, 0x00, 0x00, 0x0E, 0x60,
  0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x06, 0x60, 0x00, 0x03, 0x03, 0x00, 0xC0, 0x00, 0x06, 0x70,
  0x00, 0x02, 0x02, 0x01, 0x88, 0x00, 0x02, 0x6C, 0x00, 0x02, 0x04, 0x03, 0x18, 0x00, 0x02, 0x46,
  0x00, 0x02, 0x0C, 0x02, 0x38, 0x00, 0x02, 0x63, 0x00, 0x06, 0x18, 0x07, 0xF0, 0x00, 0x02, 0x41,
  0x00, 0x04, 0x10, 0x0D, 0x30, 0x20, 0x41, 0x61, 0x00, 0x0C, 0x10, 0x0B, 0x30, 0x60, 0x41, 0x41,
  0x00, 0x08, 0x20, 0x0C, 0x30, 0x7C, 0x41, 0x41, 0x00, 0x08, 0x24, 0x18, 0x30, 0xDA, 0x41, 0x61,
  0x00, 0x18, 0x7C, 0x30, 0x20, 0x88, 0xC1, 0x61, 0x00, 0x10, 0x7C, 0x20, 0x30, 0x88, 0x41, 0x61,
  0x00, 0x10, 0x34, 0x7E, 0x11, 0x84, 0x41, 0x61, 0x00, 0x20, 0x3C, 0x7F, 0x11, 0x04, 0xC1, 0x62,
  0x00, 0x20, 0x2C, 0xE3, 0x91, 0x02, 0x81, 0x62, 0x00, 0x60, 0x6C, 0xCB, 0x8B, 0x03, 0x83, 0x62,
  0x00, 0x40, 0x6C, 0x8F, 0x0A, 0x01, 0x82, 0x62, 0x00, 0x40, 0x4C, 0x8F, 0x06, 0x79, 0x83, 0xE2,
  0x00, 0xC0, 0x4C, 0x89, 0x06, 0x7D, 0x83, 0x02, 0x00, 0x80, 0x44, 0x8A, 0x00, 0x1B, 0x02, 0x02,
  0x00, 0x80, 0x47, 0x80, 0x00, 0xFB, 0x06, 0x02, 0x01, 0x00, 0x43, 0xC0, 0x00, 0xFB, 0x06, 0x02,
  0x01, 0x00, 0x43, 0x40, 0x00, 0x92, 0x06, 0x02, 0x81, 0x00, 0x43, 0x20, 0x00, 0x32, 0x26, 0x02,
  0x83, 0x00, 0x81, 0x10, 0x40, 0x06, 0x2E, 0x02, 0x82, 0x00, 0x80, 0x0C, 0x00, 0x04, 0x7A, 0x01,
  0x82, 0x00, 0x80, 0x07, 0x00, 0x0C, 0xBA, 0x01, 0x82, 0x00, 0x80, 0x0F, 0xF8, 0xFD, 0xB2, 0x01,
  0xC2, 0x00, 0x80, 0x3B, 0x3F, 0x8B, 0x02, 0x01, 0xC2, 0x00, 0xC0, 0x61, 0xE6, 0x1E, 0x02, 0x01,
  0xC2, 0x00, 0x41, 0xE0, 0x83, 0x18, 0x02, 0x01, 0x42, 0x20, 0x42, 0xA1, 0x81, 0x80, 0x02, 0x01,
  0x62, 0x20, 0x44, 0x92, 0xC3, 0xC0, 0x04, 0x00, 0x63, 0x20, 0x44, 0x96, 0x47, 0xA0, 0x04, 0x00,
  0x61, 0x60, 0x4C, 0x9C, 0x65, 0x90, 0x04, 0x00, 0x41, 0x60, 0x68, 0x87, 0xF9, 0xB0, 0x04, 0x00,
  0x41, 0x70, 0x39, 0x04, 0x61, 0x18, 0x04, 0x08, 0x40, 0xF8, 0x31, 0x04, 0x41, 0x08, 0x0C, 0x08,
  0x40, 0xD8, 0x31, 0x0C, 0x41, 0x8C, 0x08, 0x0C, 0x40, 0x4C, 0x3F, 0x0C, 0x41, 0x8C, 0x08, 0x1C,
  0x40, 0x04, 0x2F, 0x0C, 0x41, 0x9C, 0x18, 0x1C, 0xC0, 0x03, 0x63, 0x08, 0x41, 0xE4, 0x10, 0x3D,
  0x80, 0x01, 0xFF, 0x08, 0x61, 0x86, 0x30, 0x21, 0x80, 0x00, 0xFF, 0x18, 0x21, 0xFE, 0x60, 0x4E,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ============================================================================
// I2C LCD DRIVER (Auto-Detects 0x27 vs 0x3F using LiquidCrystal_I2C)
// ============================================================================
#include <LiquidCrystal_I2C.h>

class LCDDisplay {
  LiquidCrystal_I2C *driver = nullptr;
  uint8_t addr = 0x27;
public:
  void begin() {
    Wire.begin(SDA_LCD, SCL_LCD);
    delay(100);
    // Auto-detect backpack address: check 0x27, 0x3F, 0x20, etc.
    const uint8_t possibleAddrs[] = { LCD_ADDR, 0x27, 0x3F, 0x20, 0x26, 0x38 };
    for (uint8_t a : possibleAddrs) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        addr = a;
        break;
      }
    }
    Serial.print("[LCD] Found I2C address: 0x");
    Serial.println(addr, HEX);

    driver = new LiquidCrystal_I2C(addr, 16, 2);
    driver->init();

    // CRITICAL FIX: LiquidCrystal_I2C's begin()/init() internally calls Wire.begin() with no args,
    // which silently resets the ESP32 Wire bus back to default GPIO 21/22!
    // We immediately restore Wire to pins 32 & 33:
    Wire.end();
    delay(20);
    Wire.begin(SDA_LCD, SCL_LCD);
    delay(50);

    // Force HD44780 4-bit initialization directly over pins 32/33
    Wire.beginTransmission(addr);
    Wire.write(0x38); Wire.write(0x3C); Wire.write(0x38);
    Wire.endTransmission();
    delay(5);

    Wire.beginTransmission(addr);
    Wire.write(0x38); Wire.write(0x3C); Wire.write(0x38);
    Wire.endTransmission();
    delayMicroseconds(150);

    Wire.beginTransmission(addr);
    Wire.write(0x38); Wire.write(0x3C); Wire.write(0x38);
    Wire.endTransmission();
    delayMicroseconds(150);

    Wire.beginTransmission(addr);
    Wire.write(0x28); Wire.write(0x2C); Wire.write(0x28); // 4-bit mode
    Wire.endTransmission();
    delay(5);

    driver->backlight();
    driver->clear();
  }

  void clear() {
    if (driver) driver->clear();
  }

  void backlight(bool on) {
    if (!driver) return;
    if (on) driver->backlight();
    else driver->noBacklight();
  }

  void show(const char *l1, const char *l2 = "") {
    if (!driver) return;
    char b1[17], b2[17];
    snprintf(b1, sizeof(b1), "%-16.16s", l1);
    snprintf(b2, sizeof(b2), "%-16.16s", l2);
    driver->setCursor(0, 0);
    driver->print(b1);
    driver->setCursor(0, 1);
    driver->print(b2);
  }
};

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================
LCDDisplay lcd;                                     // LCD on Wire bus (32/33) with auto-address
Adafruit_SSD1306 oled(128, 64, &Wire1, -1);        // OLED on Wire1 bus (25/26)
Servo doorServo;

// ============================================================================
// STATE MACHINE
// ============================================================================
// The whole program is "we are currently in exactly one of these states,
// and each pass through loop() checks: has enough time passed, or has a
// message arrived, that should move us to the NEXT state?" Nothing in
// loop() ever calls delay() for longer than a few microseconds, so the
// board can keep reading the sensor and the serial port at all times --
// this is what makes the system "non-blocking".
// ============================================================================

State state = ST_SLEEP;
unsigned long stateStart = 0;     // millis() timestamp of when we entered this state
unsigned long lastFrameTime = 0;  // used by the Miku wake animation and the scan animation
int mikuFrameIndex = 0;           // which Miku wake frame we're currently showing
int scanAnimFrame = 0;            // which frame of the "scanning" indicator we're on

String pendingName = "";          // name to show, if ACCESS_GRANTED:<name> was received
String rxLine = "";                // buffer for assembling one incoming serial line
bool cameraReadyReceived = false;  // tracks if CAMERA_READY arrived during wake animation
char denyReason[17] = "Unknown user";      // LCD line 2 on the DENIED screen (bounded buffer, no dangling ptr)
bool stuckWarned = false;                  // shown the "sensor stuck" hint this cooldown?
unsigned long buzzerOffAt = 0;             // millis() when the current beep should stop (0 = silent)
const unsigned long SENSOR_STUCK_MS = 10000;  // sensor still active this long after cooldown -> hint

// ----------------------------------------------------------------------------
// BUZZER -- non-blocking beep. Does NOT use tone(): some ESP32Servo versions
// ship their own tone() that blocks and can fight the servo for PWM channels.
// beepStart() begins a beep and returns at once; buzzerUpdate() (called every
// loop pass) silences it when the time is up.
// ----------------------------------------------------------------------------
void buzzerInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER_PIN, 2000, 8);
#else
  ledcSetup(BUZZER_LEDC_CH, 2000, 8);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
#endif
}
void buzzerTone(unsigned int freq) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER_PIN, freq);
#else
  ledcWriteTone(BUZZER_LEDC_CH, freq);
#endif
}
void beepStart(unsigned int freq, unsigned long ms) {
  buzzerTone(freq);
  buzzerOffAt = millis() + ms;
}
void buzzerUpdate() {
  if (buzzerOffAt && (long)(millis() - buzzerOffAt) >= 0) {
    buzzerTone(0);
    buzzerOffAt = 0;
  }
}

// Reads the sensor and applies SENSOR_ACTIVE_LEVEL, so the rest of the code
// never has to think about HIGH vs LOW again.
bool sensorActive() {
  return digitalRead(SENSOR_PIN) == SENSOR_ACTIVE_LEVEL;
}

// ----------------------------------------------------------------------------
// DISPLAY HELPERS
// ----------------------------------------------------------------------------

// Draws one full-screen Miku bitmap, centred at x=32 (since the bitmaps are
// 64px wide and the OLED is 128px wide, this centres it).
void drawMiku(const unsigned char *bitmap) {
  oled.clearDisplay();
  oled.drawBitmap(32, 0, bitmap, 64, 64, SSD1306_WHITE);
  oled.display();
}

// Big centred text, used for MATCH! / DENIED / CLOSED etc.
void oledBigText(const char *text, int x, int y) {
  oled.clearDisplay();
  oled.setTextSize(2);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(x, y);
  oled.print(text);
  oled.display();
}

// An indeterminate "still working" animation: a block that slides back and
// forth. We use this instead of a 0-100% bar during SCANNING because we
// genuinely don't know how long the PC will take to find a face -- a fixed
// duration bar would either finish too early (and look broken) or too late.
void drawScanAnimation(int frame) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(16, 10);
  oled.print("Scanning Face...");
  oled.drawRect(14, 30, 100, 10, SSD1306_WHITE);
  int pos = frame % 20;
  if (pos > 10) pos = 20 - pos;         // 0..10..0 ping-pong motion
  int blockX = 14 + 2 + pos * 8;
  oled.fillRect(blockX, 32, 14, 6, SSD1306_WHITE);
  oled.display();
}

// A real, fixed-duration 0-100% bar, used only for the brief "verifying"
// pause right after a grant -- mostly for visual polish before "MATCH!".
void drawLoadingBar(unsigned long elapsedMs, unsigned long totalMs) {
  int pct = (int)min(100UL, elapsedMs * 100UL / totalMs);
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(22, 14);
  oled.print("Verifying...");
  oled.drawRect(14, 30, 100, 12, SSD1306_WHITE);
  oled.fillRect(14, 30, pct, 12, SSD1306_WHITE);
  oled.display();
}

// ----------------------------------------------------------------------------
// STATE TRANSITIONS
// ----------------------------------------------------------------------------
// setState() is the ONLY place that changes `state`. Putting all the "what
// to show / what to send" logic for entering a state in one place (instead
// of scattered through loop()) makes it much easier to follow what happens
// when, and avoids accidentally running "enter this state" code more than
// once per visit.
// ----------------------------------------------------------------------------
void setState(State s) {
  state = s;
  stateStart = millis();

  switch (s) {

    case ST_SLEEP:
      // Standby state: OLED rests, LCD displays ready message, LEDs off
      oled.ssd1306_command(SSD1306_DISPLAYOFF);
      lcd.backlight(true);
      lcd.show("SENTRIFACE", "Waiting Motion..");
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, LOW);
      break;

    case ST_WAKING:
      // Sensor just triggered. Turn everything back on and tell the PC.
      oled.ssd1306_command(SSD1306_DISPLAYON);
      lcd.backlight(true);
      lcd.show("Motion detected", "Camera opening..");
      mikuFrameIndex = 0;
      cameraReadyReceived = false;
      lastFrameTime = millis();
      drawMiku(mikuWink);             // first wake frame shows immediately
      Serial.println("MOTION_DETECTED");
      break;

    case ST_WAITING_CAMERA:
      lcd.show("Waiting on PC..", "Opening camera");
      drawMiku(mikuAwake);
      break;

    case ST_SCANNING:
      lcd.show("Scanning face...", "Look at camera");
      scanAnimFrame = 0;
      lastFrameTime = 0;   // forces an immediate first draw in loop()
      break;

    case ST_VERIFYING:
      lcd.show("Face recognized", "Verifying...");
      break;

    case ST_DOOR_OPEN:
      // Show the name if the PC sent one, otherwise a generic message.
      if (pendingName.length() > 0) {
        lcd.show("Welcome:", pendingName.c_str());
        // Size-2 text is 12px per character and the screen is 128px wide,
        // so "Hi <name>!" fits only for names up to 6 characters.
        if (pendingName.length() <= 6) {
          char line[12];
          snprintf(line, sizeof(line), "Hi %s!", pendingName.c_str());
          int x = (128 - (int)strlen(line) * 12) / 2;   // centre it
          oledBigText(line, x, 24);
        } else {
          oledBigText("MATCH!", 22, 24);   // name still shows in full on the LCD
        }
      } else {
        lcd.show("Admin recognized", "Access granted");
        oledBigText("MATCH!", 22, 24);
      }
      digitalWrite(LED_GREEN, HIGH);
      beepStart(2000, 150);              // non-blocking, see buzzerUpdate()
      doorServo.write(SERVO_OPEN);
      break;

    case ST_DOOR_CLOSING:
      doorServo.write(SERVO_CLOSED);
      lcd.show("Door closed", "");
      oledBigText("CLOSED", 20, 24);
      digitalWrite(LED_GREEN, LOW);
      break;

    case ST_DENIED:
      lcd.show("Access Denied", denyReason);
      oledBigText("DENIED", 18, 24);
      digitalWrite(LED_GREEN, LOW);    // clear green in case deny() was called from ST_DOOR_OPEN
      digitalWrite(LED_RED, HIGH);
      beepStart(500, 500);
      break;

    case ST_COOLDOWN:
      Serial.println("CAMERA_CLOSE");
      stuckWarned = false;
      digitalWrite(LED_RED, LOW);
      lcd.show("Please wait...", "");
      oled.clearDisplay();
      oled.display();
      pendingName = "";   // clear for the next cycle
      break;
  }
}

// Shared helper: whichever state we're in, jump straight to granting
// access. Used both by a real ACCESS_GRANTED from the PC and by the
// physical admin button (which bypasses the camera entirely).
void deny(const char *reason) {
  // If the door is currently open, physically close it before showing DENIED
  // so the door never stays open during an error/timeout.
  if (state == ST_DOOR_OPEN) {
    doorServo.write(SERVO_CLOSED);
  }
  strncpy(denyReason, reason, sizeof(denyReason) - 1);
  denyReason[sizeof(denyReason) - 1] = '\0';   // always null-terminate
  setState(ST_DENIED);
}

void grantAccess(const String &name) {
  pendingName = name;
  setState(ST_VERIFYING);
}

// ----------------------------------------------------------------------------
// SERIAL INPUT
// ----------------------------------------------------------------------------
// Reads whatever bytes are available WITHOUT blocking (unlike
// Serial.readStringUntil, which can pause the whole program for up to a
// second if a line is incomplete). Each full line is handed to
// handleCommand() as soon as it is complete.
// ----------------------------------------------------------------------------
void readSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxLine.length()) {
        handleCommand(rxLine);
        rxLine = "";
      }
    } else if (rxLine.length() < 64) {
      rxLine += c;
    }
  }
}

void handleCommand(String cmd) {
  cmd.trim();

  // Accept camera-protocol messages while waking, waiting, or scanning
  bool listening = (state == ST_WAKING || state == ST_WAITING_CAMERA || state == ST_SCANNING);

  if (cmd == "CAMERA_READY") {
    cameraReadyReceived = true;
    if (state == ST_WAITING_CAMERA) setState(ST_SCANNING);

  } else if (cmd == "ACCESS_GRANTED") {
    if (listening) grantAccess("Face matched");   // bare grant (no name) -- avoids "Admin recognized" text

  } else if (cmd.startsWith("ACCESS_GRANTED:")) {
    if (listening) grantAccess(cmd.substring(15));   // text after the colon (len 15)

  } else if (cmd == "NO_FACE") {
    if (listening) deny("No face found");

  } else if (cmd == "ACCESS_DENIED") {
    if (listening) deny("Unknown user");
  }
}

// ============================================================================
// SETUP -- runs once at power-on / reset
// ============================================================================
void setup() {
  Serial.begin(115200);

  // Start each I2C bus on its OWN pins, exactly as wired (see the big
  // comment block at the top of this file for why this matters).
  Wire.begin(SDA_LCD, SCL_LCD);
  Wire1.begin(SDA_OLED, SCL_OLED);
  Wire1.setClock(400000);   // 400kHz "fast mode" -- the OLED library likes this

  lcd.begin();
  lcd.backlight(true);
  lcd.show("SENTRIFACE", "Booting...");

  // The last two arguments to begin() are important:
  //   reset   = true  -> perform the normal reset sequence
  //   periphBegin = false -> do NOT call Wire1.begin() again internally,
  //                          since we already started it on the correct
  //                          pins above. If this were left as default
  //                          (true), the library would silently reset
  //                          Wire1 back to its default pins.
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    lcd.show("OLED not found", "check wiring");
    Serial.println("OLED_INIT_FAILED");
    for (;;) delay(1000);   // stop here -- nothing else can safely continue
  }
  oled.clearDisplay();
  oled.display();

  // 50Hz is the standard signal frequency SG90 servos expect. The pulse
  // width range (500-2400 microseconds) covers the servo's full sweep;
  // these are safe, commonly-used defaults for a generic SG90.
  ESP32PWM::allocateTimer(0);   // reserve timers for the servo (ESP32Servo requirement)
  ESP32PWM::allocateTimer(1);
  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN, 500, 2400);
  doorServo.write(SERVO_CLOSED);

  buzzerInit();   // sets up the buzzer pin for PWM tones
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);   // button reads LOW when pressed
  pinMode(SENSOR_PIN, INPUT);

  delay(500);   // let the sensor's own power-up settle before we trust it
  setState(ST_COOLDOWN);   // start in cooldown so we don't instantly react
                           // to a sensor that is still warming up
  Serial.println("SENTRIFACE_READY");
}

// ============================================================================
// MAIN LOOP -- runs continuously, must never be blocked for long
// ============================================================================
void loop() {
  readSerial();                       // always check for PC messages
  buzzerUpdate();                     // stop any finished beep
  unsigned long now = millis();
  unsigned long elapsed = now - stateStart;

  // ---- Admin button: works from any "awake and not already resolved"
  // state, and immediately grants access without needing the camera at
  // all. Useful for demos, testing, or as a manual backup entry method.
  bool buttonPressed = (digitalRead(BUTTON_PIN) == LOW);
  bool buttonUsableNow =
      (state == ST_WAKING || state == ST_WAITING_CAMERA || state == ST_SCANNING);
  if (buttonPressed && buttonUsableNow) {
    grantAccess("Admin");
    return;   // skip the rest of this pass; we just changed state
  }

  switch (state) {

    case ST_SLEEP:
      if (sensorActive()) setState(ST_WAKING);
      break;

    case ST_WAKING:
      // Play wink -> tilt -> awake, then move on automatically. Each frame
      // is timed by MIKU_FRAME_MS so this never blocks.
      if (now - lastFrameTime >= MIKU_FRAME_MS) {
        lastFrameTime = now;
        mikuFrameIndex++;
        if (mikuFrameIndex == 1) {
          drawMiku(mikuTilt);
        } else if (mikuFrameIndex == 2) {
          drawMiku(mikuAwake);
        } else {
          setState(cameraReadyReceived ? ST_SCANNING : ST_WAITING_CAMERA);
        }
      }
      break;

    case ST_WAITING_CAMERA:
      // CAMERA_READY normally arrives via handleCommand(). This timeout
      // is the safety net for "the PC / Python script isn't running at
      // all" so the board doesn't wait here forever.
      if (elapsed >= CAMERA_WAIT_MS) deny("PC not responding");
      break;

    case ST_SCANNING:
      // Keep the "still scanning" indicator moving.
      if (now - lastFrameTime >= 80) {
        lastFrameTime = now;
        drawScanAnimation(scanAnimFrame++);
      }
      // Safety net: if the PC never sends a result at all.
      if (elapsed >= SCAN_TIMEOUT_MS) deny("Scan timed out");
      break;

    case ST_VERIFYING:
      drawLoadingBar(elapsed, VERIFY_MS);
      if (elapsed >= VERIFY_MS) setState(ST_DOOR_OPEN);
      break;

    case ST_DOOR_OPEN:
      if (elapsed >= DOOR_OPEN_MS) setState(ST_DOOR_CLOSING);
      break;

    case ST_DOOR_CLOSING:
      if (elapsed >= DOOR_CLOSE_PAUSE_MS) setState(ST_COOLDOWN);
      break;

    case ST_DENIED:
      if (elapsed >= DENIED_MS) setState(ST_COOLDOWN);
      break;

    case ST_COOLDOWN:
      // Re-arm only once BOTH a minimum time has passed AND the sensor
      // has actually cleared. Without the "sensor cleared" check, a
      // person standing in front of an IR obstacle sensor (which reports
      // "detected" for as long as they're there, not just once) would
      // make the system immediately wake up again the instant it went
      // to sleep.
      if (elapsed >= COOLDOWN_MS && !sensorActive()) setState(ST_SLEEP);
      // If the sensor never clears (sensitivity pot too high, or something
      // sitting in front of it), say so instead of looking frozen.
      else if (!stuckWarned && elapsed >= COOLDOWN_MS + SENSOR_STUCK_MS) {
        stuckWarned = true;
        lcd.show("Sensor stuck?", "Adjust IR pot");
        Serial.println("SENSOR_STUCK");
      }
      break;
  }

  // ---- General idle safety net: if we somehow got stuck "awake" with
  // nothing happening for a long time (should not normally occur, since
  // every awake state above has its own timeout, but this is a backstop),
  // force everything back to a clean sleep.
  bool awakeState = (state != ST_SLEEP && state != ST_COOLDOWN);
  if (awakeState && elapsed >= IDLE_TIMEOUT_MS) {
    deny("Timed out");
  }
}