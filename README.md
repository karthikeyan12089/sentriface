# SENTRIFACE — Smart Face-Recognition Door Lock

> A capstone project that combines an **ESP32 microcontroller** with a **Python-based face recognition system** to create an intelligent, contactless door access control system.

---

## 📁 Repository Structure

`
sentriface/
├── sentriface ver 6.2.ino   # Arduino sketch for the ESP32
├── python/sentriface_camera.py    # Python script that runs on a PC/laptop
├── faces/                   # Enrolled face images for recognition
│   └── Master/              # One folder per person (folder name = person's name)
│       ├── 1.jpg
│       ├── 2.jpg
│       └── ...
└── requirements.txt         # Python dependencies
`

---

## 🧠 How It Works

`
[Person approaches]
       │
       ▼
[IR Obstacle Sensor detects motion]
       │
       ▼
[ESP32 sends "MOTION_DETECTED" over USB Serial to PC]
       │
       ▼
[Python script opens webcam, scans face using LBPH algorithm]
       │
    ┌──┴──────────────────────────────┐
    │ Face recognised?                │
    │                                 │
   YES                               NO
    │                                 │
    ▼                                 ▼
[PC sends "ACCESS_GRANTED:Name"]  [PC sends "NO_FACE"]
    │                                 │
    ▼                                 ▼
[ESP32 opens servo (door),        [ESP32 shows "Access Denied"
 shows name on LCD,                on LCD, buzzer beeps red LED]
 green LED, welcome tone]
`

---

## 🔧 Hardware Required

| Component             | Quantity | Notes                                  |
|-----------------------|----------|----------------------------------------|
| ESP32 Dev Board       | 1        | Any standard 38-pin ESP32              |
| 16×2 I2C LCD          | 1        | I2C address usually 0x27               |
| 0.96" OLED (SSD1306)  | 1        | I2C, 128×64                            |
| IR Obstacle Sensor    | 1        | 3.3V logic output                      |
| SG90 Servo Motor      | 1        | Simulates door latch                   |
| Green LED             | 1        | Access granted indicator               |
| Red LED               | 1        | Access denied indicator                |
| Passive/Active Buzzer | 1        | Audio feedback                         |
| Push Button           | 1        | Admin override (bypass face scan)      |
| 220Ω Resistors        | 2        | For LEDs                               |
| USB Cable             | 1        | ESP32 ↔ PC serial connection           |
| Laptop/PC with webcam | 1        | Runs the Python face recognition script|

---

## 📌 Hardware Wiring

| Part                 | Pin on Part | ESP32 GPIO | Notes                              |
|----------------------|-------------|------------|------------------------------------|
| 16×2 I2C LCD         | SDA         | GPIO 32    | Uses Wire (primary I2C bus)        |
|                      | SCL         | GPIO 33    |                                    |
|                      | VCC         | 5V (VIN)   | Needs 5V for backlight             |
|                      | GND         | GND        |                                    |
| 0.96" OLED (SSD1306) | SDA         | GPIO 25    | Uses Wire1 (secondary I2C bus)     |
|                      | SCL         | GPIO 26    |                                    |
|                      | VCC         | 3.3V       |                                    |
|                      | GND         | GND        |                                    |
| IR Obstacle Sensor   | OUT         | GPIO 27    | 3.3V logic — do NOT use 5V here    |
|                      | VCC         | 3.3V       |                                    |
|                      | GND         | GND        |                                    |
| SG90 Servo           | Signal      | GPIO 13    |                                    |
|                      | VCC (red)   | 5V (VIN)   | Must be 5V — 3.3V causes jitter    |
|                      | GND (brown) | GND        |                                    |
| Green LED            | Anode (+)   | GPIO 4     | Through 220Ω resistor              |
| Red LED              | Anode (+)   | GPIO 16    | Through 220Ω resistor              |
| Buzzer               | +           | GPIO 2     | Passive or active                  |
| Admin Button         | One leg     | GPIO 15    | Other leg to GND                   |

> ⚠️ **Important:** The LCD and OLED use SEPARATE I2C buses (Wire vs Wire1) because both often share the same I2C address (0x27 / 0x3C). The sketch handles this automatically — just follow the wiring table exactly.

---

## 💻 Software Setup — ESP32 (Arduino Side)

### 1. Install Arduino IDE
Download from: https://www.arduino.cc/en/software

### 2. Add ESP32 Board Support
1. Open Arduino IDE → **File → Preferences**
2. In "Additional Board Manager URLs", paste:
   `
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   `
3. Go to **Tools → Board → Boards Manager**
4. Search for **"esp32 by Espressif Systems"** → Install version **3.x**

### 3. Install Required Libraries
Go to **Tools → Manage Libraries** and install:

| Library Name             | Author                  |
|--------------------------|-------------------------|
| Adafruit SSD1306         | Adafruit                |
| Adafruit GFX Library     | Adafruit                |
| ESP32Servo               | Kevin Harrington        |
| LiquidCrystal I2C        | Frank de Brabander      |

> ⚠️ For LiquidCrystal I2C, install the one by **Frank de Brabander** specifically — other forks may be missing the egin(cols, rows) method.

### 4. Flash the ESP32
1. Open sentriface ver 6.2.ino in Arduino IDE
2. Select your board: **Tools → Board → ESP32 Arduino → ESP32 Dev Module**
3. Select the correct COM port: **Tools → Port → COMx** (check Device Manager if unsure)
4. Click **Upload** (▶)
5. The LCD should display **"SENTRIFACE / Booting..."** on success

---

## 🐍 Software Setup — Python (PC/Laptop Side)

### 1. Install Python
Download Python 3.10 or newer from: https://www.python.org/downloads/
- ✅ During install, check **"Add Python to PATH"**

### 2. Install Dependencies
Open a terminal (Command Prompt / PowerShell) in this repo folder and run:
`ash
pip install -r requirements.txt
`

This installs:
- **opencv-contrib-python** — camera access + face detection (Haar Cascade) + LBPH recognition
- **pyserial** — serial communication with the ESP32 over USB
- **numpy** — numerical operations used by OpenCV

### 3. Enrolled Faces (aces/ folder)
The aces/ folder already contains enrolled face images used for recognition.

**Structure:**
`
faces/
└── PersonName/         ← folder name becomes the displayed name
    ├── 1.jpg
    ├── 2.jpg
    └── ...             ← at least 10–30 photos recommended
`

**To add a new person:**
1. Create a new folder inside aces/ named after the person (e.g., aces/John)
2. Add 15–30 close-up grayscale or colour photos of their face (JPG/PNG)
3. Photos should be taken in varied lighting and angles for best accuracy
4. Save and restart the Python script — it retrains automatically on every run

**To remove a person:**
- Simply delete their folder from aces/

### 4. Run the Python Script
`ash
python "python/sentriface_camera.py"
`

> ⚠️ **Close the Arduino IDE Serial Monitor first** — it locks the COM port and will prevent the Python script from connecting.

The script will:
1. Auto-detect which COM port the ESP32 is on
2. Train the LBPH face recogniser from the aces/ folder
3. Wait for MOTION_DETECTED signal from the ESP32
4. Open the webcam, scan for a face, send result back

**Manual test (no hardware needed):** Press **SPACEBAR** while the Python script is running to trigger the camera scan manually.

---

## 🔁 Serial Protocol (ESP32 ↔ PC)

| Direction    | Message                    | Meaning                            |
|--------------|----------------------------|------------------------------------|
| ESP32 → PC   | MOTION_DETECTED          | IR sensor triggered, open camera   |
| PC → ESP32   | CAMERA_READY             | Camera opened, scanning started    |
| PC → ESP32   | ACCESS_GRANTED:Name      | Face matched, name of person       |
| PC → ESP32   | NO_FACE                  | Nobody recognised before timeout   |
| ESP32 → PC   | CAMERA_CLOSE             | Cycle done, release the camera     |

---

## ⚙️ Configurable Settings (in python/sentriface_camera.py)

| Setting            | Default | Description                                           |
|--------------------|---------|-------------------------------------------------------|
| PORT             | None  | None = auto-detect; set "COM5" to force a port    |
| BAUD             | 115200  | Must match Serial.begin() in the Arduino sketch     |
| CAMERA_INDEX     |       | Change to 1 if the wrong camera opens               |
| SCAN_TIMEOUT_S   | 15    | Seconds before sending NO_FACE if no match found   |
| CONFIDENCE_MAX   | 70    | LBPH threshold — lower = stricter. Raise to 80 if falsely denied |
| FACE_FRAMES_NEEDED | 3   | How many consecutive frames must match before granting |
| SHOW_PREVIEW     | True  | Show live camera window (False for headless mode)   |

---

## 🛠️ Troubleshooting

| Problem                            | Fix                                                                      |
|------------------------------------|--------------------------------------------------------------------------|
| Python can't find COM port         | Unplug/replug ESP32, close Arduino Serial Monitor, check Device Manager  |
| "Faces folder not found" error     | Make sure the aces/ folder is in the same directory as the .py file   |
| Always says "Unknown"              | Raise CONFIDENCE_MAX to 80; add more/better training images            |
| OLED not initialising              | Check wiring on GPIO 25/26; verify SSD1306 I2C address is  x3C        |
| LCD shows garbage or blank         | Check wiring on GPIO 32/33; adjust contrast pot on back of LCD module   |
| Servo jitters                      | Power servo from 5V (VIN), NOT 3.3V                                      |
| Face recognised but door won't open| Check servo signal wire on GPIO 13; check SERVO_CLOSED/OPEN angle values |
| IR sensor always triggered         | Adjust the sensitivity potentiometer on the IR sensor module             |

---

## 📜 License
This project is built for academic/capstone purposes. Feel free to use, modify, and learn from it.
