"""
SENTRIFACE - PC Camera Bridge  |  camera version 6.1.py
=======================================================
Best-of-both merge:
  From camera_ver6.py : LBPH face RECOGNITION (knows WHO), cascade auto-download,
                        ACCESS_GRANTED:<name>, NO_FACE, per-user confidence label
  From Jules           : auto-reconnect loop, graceful port retry, try/except
                        around every serial call, no sys.exit on port miss

Serial protocol (115200 baud):
  ESP32 -> PC  : MOTION_DETECTED         IR triggered -> open camera
  PC -> ESP32  : CAMERA_READY            camera live, scanning started
  PC -> ESP32  : ACCESS_GRANTED:<name>   enrolled face matched
  PC -> ESP32  : NO_FACE                 nobody matched before timeout
  ESP32 -> PC  : CAMERA_CLOSE           cycle done -> release camera

Install : pip install opencv-contrib-python pyserial numpy
Run     : python "camera version 6.1.py"
          Close Arduino Serial Monitor first (it locks the COM port).
          Press q in the camera window or Ctrl+C to quit.
"""

import os
import sys
import time
import csv
import urllib.request
import ctypes
from datetime import datetime

def is_space_pressed():
    """Check if SPACEBAR is pressed for manual scan trigger."""
    if sys.platform == "win32":
        return bool(ctypes.windll.user32.GetAsyncKeyState(0x20) & 0x8000)
    return False

def force_foreground(win_name):
    """Bring preview window to the front."""
    if sys.platform == "win32":
        try:
            hwnd = ctypes.windll.user32.FindWindowW(None, win_name)
            if hwnd:
                ctypes.windll.user32.ShowWindow(hwnd, 5)
                ctypes.windll.user32.SetForegroundWindow(hwnd)
                ctypes.windll.user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002)
        except Exception:
            pass

from collections import deque

import cv2
import numpy as np
import serial
from serial.tools import list_ports

# ──────────────────────────── SETTINGS ────────────────────────────────────────
PORT             = None     # None = auto-detect; set "COM5" to force
BAUD             = 115200
CAMERA_INDEX     = 0        # try 1 if the wrong camera opens
FACES_DIR        = os.path.join(os.path.dirname(os.path.abspath(__file__)), "faces")
SCAN_TIMEOUT_S   = 15       # send NO_FACE after this many seconds
CAMERA_HOLD_MAX  = 40       # safety release if CAMERA_CLOSE never arrives
SHOW_PREVIEW     = True
ATTENDANCE_CSV   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "attendance.csv")

CONFIDENCE_MAX      = 70    # LBPH: lower = stricter; raise to 80 if denied
FACE_WINDOW         = 5     # look at the last N frames...
FACE_FRAMES_NEEDED  = 3     # ...require this many matches for same person

DETECT_WIDTH     = 320      # resize frame for faster detection
DETECT_MIN_FACE  = 40       # minimum face size in resized image
FACE_SIZE        = (200, 200)

XML_URL = (
    "https://raw.githubusercontent.com/opencv/opencv/master"
    "/data/haarcascades/haarcascade_frontalface_default.xml"
)

# ──────────────────────────── ATTENDANCE LOG ──────────────────────────────────
def log_attendance(name, status="GRANTED"):
    """Append one row to attendance.csv next to this script. Never raises."""
    try:
        write_header = (not os.path.isfile(ATTENDANCE_CSV)
                        or os.path.getsize(ATTENDANCE_CSV) == 0)
        with open(ATTENDANCE_CSV, "a", newline="", encoding="utf-8-sig") as f:
            writer = csv.writer(f)
            if write_header:
                writer.writerow(["Name", "Date", "Time", "Status"])
            now = datetime.now()
            writer.writerow([name, now.strftime("%Y-%m-%d"),
                             now.strftime("%H:%M:%S"), status])
        print(f"[LOG] Attendance: {name} ({status})")
    except Exception as e:
        print(f"[WARN] Could not write attendance log: {e}")

# ──────────────────────────── CASCADE ─────────────────────────────────────────
def load_cascade():
    """Load Haar Cascade; auto-download if the bundled cv2 path is broken."""
    xml = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
    if not os.path.exists(xml):
        xml = "haarcascade_frontalface_default.xml"
        if not os.path.exists(xml):
            print("[SETUP] Downloading haarcascade XML...")
            urllib.request.urlretrieve(XML_URL, xml)
            print("[SETUP] Done.")
    cascade = cv2.CascadeClassifier(xml)
    if cascade.empty():
        sys.exit("[ERROR] Could not load face detector. Check OpenCV install.")
    return cascade

# ──────────────────────────── FACE RECOGNISER ─────────────────────────────────
def load_recognizer():
    """Train LBPH on the enrolled faces/ folder. Returns (recognizer, names)."""
    if not os.path.isdir(FACES_DIR):
        sys.exit(
            f"[ERROR] Faces folder not found: {FACES_DIR}\n"
            "        Run enroll_ver6.py (option 1) first."
        )
    images, labels, names = [], [], {}
    for label_id, person in enumerate(sorted(os.listdir(FACES_DIR))):
        folder = os.path.join(FACES_DIR, person)
        if not os.path.isdir(folder):
            continue
        names[label_id] = person
        for fname in os.listdir(folder):
            img = cv2.imread(os.path.join(folder, fname), cv2.IMREAD_GRAYSCALE)
            if img is not None:
                images.append(cv2.resize(img, FACE_SIZE))
                labels.append(label_id)
    if not images:
        sys.exit(
            f"[ERROR] No images in {FACES_DIR}.\n"
            "        Run enroll_ver6.py (option 1) to enroll faces."
        )
    recognizer = cv2.face.LBPHFaceRecognizer_create()
    recognizer.train(images, np.array(labels))
    print(f"[INIT] Enrolled: {[n.replace('_',' ') for n in names.values()]}")
    return recognizer, names

# ──────────────────────────── SERIAL (Jules pattern) ──────────────────────────
def find_port():
    """Auto-detect ESP32 COM port. Returns None if not found (no crash)."""
    if PORT:
        return PORT
    ports = list(list_ports.comports())
    keywords = ("cp210", "ch340", "ch910", "silicon labs",
                 "usb serial", "usb-serial", "esp32")
    for p in ports:
        if any(k in (p.description or "").lower() for k in keywords):
            return p.device
    if len(ports) == 1:
        return ports[0].device
    return None   # caller will retry instead of crashing

def open_serial(port):
    ser = serial.Serial()
    ser.port     = port
    ser.baudrate = BAUD
    ser.timeout  = 0
    ser.dtr      = False    # avoids resetting ESP32 on connect
    ser.rts      = False
    ser.open()
    return ser

def send(ser, text):
    """Send a line to ESP32 with error swallowing (Jules pattern)."""
    try:
        ser.write((text + "\n").encode())
        print(f"PC  -> ESP32: {text}")
    except Exception as e:
        print(f"[WARN] Serial send failed: {e}")

def read_lines(ser, buf):
    """Non-blocking read; returns complete lines, keeps partial data in buf."""
    data = ser.read(ser.in_waiting or 1)
    if data:
        buf.extend(data)
    lines = []
    while b"\n" in buf:
        i = buf.index(b"\n")
        line = bytes(buf[:i]).decode(errors="ignore").strip()
        del buf[:i + 1]
        if line:
            lines.append(line)
    return lines

# ──────────────────────────── CAMERA ──────────────────────────────────────────
def open_camera():
    """Open webcam with DirectShow first (faster on Windows), fallback default."""
    cap = cv2.VideoCapture(CAMERA_INDEX, cv2.CAP_DSHOW)
    if not cap.isOpened():
        cap.release()
        cap = cv2.VideoCapture(CAMERA_INDEX)
    if not cap.isOpened():
        return None
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    for _ in range(5):      # discard dark auto-exposure warmup frames
        cap.read()
    return cap

# ──────────────────────────── MAIN (Jules reconnect + our LBPH) ───────────────
def main():
    cascade           = load_cascade()
    recognizer, names = load_recognizer()

    print("[INIT] Camera version 6.1 ready.")
    print("       Waiting for ESP32 to connect...\n")

    try:
        # ── Outer reconnect loop (Jules pattern) ──────────────────────────────
        while True:
            port = find_port()
            if not port:
                print("[WAIT] No ESP32 port found. Retrying in 2s...")
                time.sleep(2)
                continue

            try:
                ser = open_serial(port)
            except Exception as e:
                print(f"[WARN] Could not open {port}: {e}. Retrying in 2s...")
                time.sleep(2)
                continue

            print(f"[CONN] Connected on {port} @ {BAUD}.")
            print("       Waiting for MOTION_DETECTED from ESP32...")
            print("       Press q in camera window or Ctrl+C to quit.\n")

            rx         = bytearray()
            cap        = None
            scanning   = False
            window_popped = False
            scan_start = cam_start = 0.0
            recent     = deque(maxlen=FACE_WINDOW)

            def release():
                nonlocal cap, scanning, window_popped
                if cap is not None:
                    cap.release()
                    cap = None
                scanning = False
                window_popped = False
                if SHOW_PREVIEW:
                    cv2.destroyAllWindows()
                    cv2.waitKey(1)

            try:
                # ── Inner camera + recognition loop ───────────────────────────
                while True:
                    # 1) Read serial from ESP32
                    try:
                        lines = read_lines(ser, rx)
                    except Exception as e:
                        print(f"[ERROR] Serial read: {e}. Reconnecting...")
                        break   # break inner loop -> outer loop reconnects

                    for line in lines:
                        print(f"ESP32 -> PC: {line}")
                        if line == "MOTION_DETECTED" and cap is None:
                            cap = open_camera()
                            if cap is None:
                                print("[WARN] Camera failed to open.")
                                send(ser, "NO_FACE")
                            else:
                                recent.clear()
                                scanning   = True
                                window_popped = False
                                scan_start = cam_start = time.time()
                                send(ser, "CAMERA_READY")
                        elif line == "CAMERA_CLOSE":
                            release()

                    # Manual SPACEBAR trigger (test camera without waiting for IR)
                    if cap is None and is_space_pressed():
                        print("[TRIGGER] Spacebar pressed -> Opening camera...")
                        cap = open_camera()
                        if cap is not None:
                            recent.clear()
                            scanning   = True
                            window_popped = False
                            scan_start = cam_start = time.time()
                            send(ser, "CAMERA_READY")
                        time.sleep(0.3)

                    # 2) Camera frame + LBPH recognition
                    if cap is not None:
                        ok, frame = cap.read()

                        if ok and scanning:
                            gray  = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                            scale = gray.shape[1] / DETECT_WIDTH
                            small = cv2.resize(gray, None,
                                               fx=1/scale, fy=1/scale)
                            faces = cascade.detectMultiScale(
                                small, scaleFactor=1.1, minNeighbors=4,
                                minSize=(DETECT_MIN_FACE, DETECT_MIN_FACE)
                            )

                            seen_this_frame = set()
                            for (sx, sy, sw, sh) in faces:
                                x, y = int(sx*scale), int(sy*scale)
                                w, h = int(sw*scale), int(sh*scale)
                                x, y = max(0, x), max(0, y)
                                crop = gray[y:y+h, x:x+w]
                                if crop.size == 0:
                                    continue
                                face       = cv2.resize(crop, FACE_SIZE)
                                label, conf = recognizer.predict(face)
                                print(f"  conf={conf:.1f}  "
                                      f"label={names.get(label,'?')}")

                                if conf < CONFIDENCE_MAX:
                                    person = names[label]
                                    seen_this_frame.add(person)
                                    color = (0, 255, 0)
                                    text  = (f"{person.replace('_',' ')} "
                                             f"({conf:.0f})")
                                else:
                                    color = (0, 0, 255)
                                    text  = f"Unknown ({conf:.0f})"

                                cv2.rectangle(frame,
                                              (x, y), (x+w, y+h), color, 2)
                                cv2.putText(frame, text, (x, y-10),
                                            cv2.FONT_HERSHEY_SIMPLEX,
                                            0.7, color, 2)

                            recent.append(seen_this_frame)

                            # Grant if confirmed across enough recent frames
                            granted = None
                            if recent:
                                for person in set().union(*recent):
                                    count = sum(1 for r in recent
                                                if person in r)
                                    if count >= FACE_FRAMES_NEEDED:
                                        granted = person
                                        break

                            if granted:
                                name_clean = granted.replace("_", " ")
                                print(f"[MATCH] Recognised: {name_clean}")
                                send(ser, f"ACCESS_GRANTED:{name_clean}")
                                log_attendance(name_clean)
                                scanning = False
                            elif time.time() - scan_start > SCAN_TIMEOUT_S:
                                print("[TIMEOUT] No enrolled face found.")
                                send(ser, "NO_FACE")
                                scanning = False

                        if ok and SHOW_PREVIEW:
                            status = ("Scanning..."
                                      if scanning
                                      else "Done - waiting for ESP32")
                            cv2.putText(frame, status, (10, 30),
                                        cv2.FONT_HERSHEY_SIMPLEX,
                                        0.8, (0, 255, 255), 2)
                            cv2.imshow("SENTRIFACE v6.1", frame)
                            if not window_popped:
                                window_popped = True
                                force_foreground("SENTRIFACE v6.1")

                        if time.time() - cam_start > CAMERA_HOLD_MAX:
                            print("[SAFETY] Camera hold timeout. Releasing.")
                            release()

                    # 3) Keyboard quit & CPU sleep
                    if cap is not None and SHOW_PREVIEW:
                        if cv2.waitKey(1) & 0xFF == ord("q"):
                            return
                    else:
                        time.sleep(0.02)

            except Exception as e:
                print(f"[ERROR] Main loop: {e}")
            finally:
                release()
                try:
                    ser.close()
                except Exception:
                    pass
                print("[RECONN] Disconnected. Reconnecting in 1s...")
                time.sleep(1)

    except KeyboardInterrupt:
        print("\n[EXIT] Stopped by user.")


if __name__ == "__main__":
    main()
