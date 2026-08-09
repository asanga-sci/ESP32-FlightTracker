import serial
import os
import re
import struct
import time
from datetime import datetime
from PIL import Image

PORT = "COM4"          # Change this
BAUDRATE = 115200

os.makedirs("images", exist_ok=True)

try:
    ser = serial.Serial(
        PORT,
        BAUDRATE,
        timeout=1
    )
except serial.SerialException as exc:
    if "PermissionError" in repr(exc) or "Access is denied" in str(exc):
        raise SystemExit(
            f"Cannot open {PORT}: it is already in use. "
            "Close PlatformIO Monitor, Arduino Serial Monitor, or any other "
            "terminal connected to this port, then try again."
        ) from None
    raise

print(f"Connected to {PORT}")
print("Requesting screenshot...")

# The firmware screenshot task waits for this line before calling
# take_screenshot(). Send it after opening the port so the request is not
# lost during board reset or while another monitor owns the port.
time.sleep(2.0)  # Opening USB CDC can reset the ESP32; let tasks start.
ser.reset_input_buffer()
ser.write(b"ss\n")
ser.flush()

print("Waiting for screenshot...")

request_deadline = time.monotonic() + 30
next_retry = time.monotonic() + 3
while True:
    line = ser.readline().decode("ascii", errors="ignore").strip()

    if line != "BEGIN_SCREENSHOT":
        if time.monotonic() >= next_retry and time.monotonic() < request_deadline:
            ser.write(b"ss\n")
            ser.flush()
            next_retry = time.monotonic() + 3
        if time.monotonic() >= request_deadline:
            ser.close()
            raise SystemExit("Timed out waiting for BEGIN_SCREENSHOT")
        continue

    print("Screenshot started")

    # Synchronize on the metadata line. Serial output from boot/log messages
    # can precede or partially surround this line.
    deadline = time.monotonic() + 10
    match = None
    while time.monotonic() < deadline:
        line = ser.readline().decode("ascii", errors="ignore").strip()
        match = re.search(r"SCREENSHOT_INFO (\d+) (\d+) (\d+)", line)
        if match:
            break

    if match is None:
        print("Timed out waiting for SCREENSHOT_INFO")
        continue

    width = int(match.group(1))
    height = int(match.group(2))
    data_size = int(match.group(3))

    print(f"Resolution: {width}x{height}")
    print(f"Expected data: {data_size} bytes")

    # Read exactly the pixel data
    data = bytearray()

    while len(data) < data_size:
        chunk = ser.read(data_size - len(data))

        if not chunk:
            print("Timeout while receiving screenshot")
            break

        data.extend(chunk)

    if len(data) != data_size:
        print(
            f"Incomplete screenshot: "
            f"{len(data)}/{data_size} bytes"
        )
        continue

    print("Screenshot received")

    # lv_draw_buf_t::data_size can include LVGL's alignment padding. The
    # actual RGB565 image is exactly width * height * 2 bytes.
    pixel_size = width * height * 2
    if data_size < pixel_size:
        print(f"Invalid screenshot size: {data_size} < {pixel_size}")
        continue

    pixel_data = data[:pixel_size]

    # LVGL stores RGB565 as native little-endian 16-bit words on ESP32.
    # Decode explicitly instead of relying on Pillow's platform-dependent
    # RGB;16 decoder.
    rgb = bytearray(width * height * 3)
    out = 0
    for (value,) in struct.iter_unpack("<H", pixel_data):
        rgb[out] = ((value >> 11) & 0x1F) * 255 // 31
        rgb[out + 1] = ((value >> 5) & 0x3F) * 255 // 63
        rgb[out + 2] = (value & 0x1F) * 255 // 31
        out += 3

    image = Image.frombytes("RGB", (width, height), bytes(rgb))

    filename = datetime.now().strftime(
        "images/screenshot_%Y%m%d_%H%M%S.png"
    )

    image.save(filename)

    print(f"Saved: {filename}")
    ser.close()
    break