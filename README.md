# ESP32 Pin Simulator — Full Project

## Project Structure

```
esp32_simulator.html     ← Open this in your browser (the UI)
main.py                  ← FastAPI backend (run this on your PC)
requirements.txt         ← Python dependencies
esp32_firmware.ino       ← Flash this to your ESP32
README.md                ← You are here
```

---

## Quick Start

### 1. Start the Backend

```bash
pip install -r requirements.txt
uvicorn main:app --reload --host 0.0.0.0 --port 8000
```

Backend is now live at `http://localhost:8000`
Swagger docs at `http://localhost:8000/docs`

### 2. Open the Frontend

Just open `esp32_simulator.html` in Chrome/Firefox. No server needed.

### 3. Flash the ESP32 (optional — only if using real hardware)

Open `esp32_firmware.ino` in Arduino IDE.

Edit these two lines at the top:
```cpp
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"
#define BACKEND_HOST     "http://192.168.1.100:8000"  // ← your PC's IP
```

Find your PC's local IP:
- Windows: `ipconfig` → look for IPv4 Address
- Mac/Linux: `ifconfig` → look for inet

Install the ArduinoJson library:
- Arduino IDE → Tools → Manage Libraries → search "ArduinoJson" → install

Select board: **ESP32 Dev Module** → Upload.

---

## How It All Works Together

```
┌─────────────────────────────────────────────────────────────┐
│                        YOUR PC                              │
│                                                             │
│  ┌─────────────────┐         ┌──────────────────────────┐  │
│  │  Browser        │  REST   │  FastAPI Backend          │  │
│  │  (frontend)     │◄───────►│  localhost:8000           │  │
│  │                 │  WS     │                           │  │
│  └─────────────────┘         └──────────┬───────────────┘  │
│                                         │                   │
└─────────────────────────────────────────┼───────────────────┘
                                          │ WiFi (HTTP)
                                          │
                               ┌──────────▼───────────┐
                               │  ESP32 Hardware      │
                               │  (optional)          │
                               └──────────────────────┘
```

### Data Flow — INPUT pins (sensor readings)

```
ESP32 reads sensor
  → POST /api/esp32/readings/batch   (every 200ms)
    → Backend stores latest value
      → Frontend polls GET /api/pin/{id}   (every N ms, user configured)
        → Oscilloscope + Log updates live
```

### Data Flow — OUTPUT pins (control signals)

```
User moves slider in frontend
  → POST /api/pin to backend
    → Backend stores output command
      → ESP32 polls GET /api/esp32/outputs   (every 300ms)
        → ESP32 applies to actual GPIO (DAC/PWM/DIGITAL)
```

---

## Using Without Real ESP32

You can simulate everything without hardware by posting fake readings directly:

```bash
# Simulate ADC reading on GPIO 34 at 1.65V (halfway)
curl -X POST http://localhost:8000/api/esp32/reading \
  -H "Content-Type: application/json" \
  -d '{"gpio":"34","signal_type":"ADC","value":0.5,"raw":2047,"unit":"V"}'

# Simulate digital HIGH on GPIO 32
curl -X POST http://localhost:8000/api/esp32/reading \
  -H "Content-Type: application/json" \
  -d '{"gpio":"32","signal_type":"DIGITAL","value":1.0}'

# Simulate touch on GPIO 33
curl -X POST http://localhost:8000/api/esp32/reading \
  -H "Content-Type: application/json" \
  -d '{"gpio":"33","signal_type":"TOUCH","value":1.0,"raw":12}'
```

Or write a Python script to simulate a sine wave:

```python
import requests, math, time

i = 0
while True:
    v = (math.sin(i * 0.1) + 1) / 2   # 0.0 to 1.0
    requests.post("http://localhost:8000/api/esp32/reading", json={
        "gpio": "34",
        "signal_type": "ADC",
        "value": round(v, 4),
        "raw": round(v * 4095),
        "unit": "V"
    })
    i += 1
    time.sleep(0.1)
```

---

## API Reference

### Frontend Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/pin` | Configure a pin (frontend sends this) |
| GET | `/api/pin/{id}` | Get latest reading for a pin (frontend polls this) |
| GET | `/api/pins` | Get all pin states |
| DELETE | `/api/pin/{id}` | Reset a pin |
| DELETE | `/api/pins` | Reset all pins |
| WS | `/ws` | WebSocket for real-time updates |

### ESP32 Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/esp32/reading` | Push a single sensor reading |
| POST | `/api/esp32/readings/batch` | Push multiple readings at once |
| GET | `/api/esp32/output/{id}` | Get output command for a pin |
| GET | `/api/esp32/outputs` | Get ALL output commands |
| GET | `/api/esp32/inputs` | Get all configured input pins |

---

## Value Normalization

All values are normalized **0.0 to 1.0** across the whole system.

| Signal Type | value = 0.0 | value = 1.0 | Conversion |
|-------------|-------------|-------------|------------|
| ADC | 0V | 3.3V | `voltage = value * 3.3` |
| DAC | 0V | 3.3V | `raw_8bit = value * 255` |
| PWM | 0% duty | 100% duty | `percent = value * 100` |
| DIGITAL | LOW (0V) | HIGH (3.3V) | `state = value > 0.5` |
| TOUCH | Released | Touched | `touched = value > 0.5` |

---

## Troubleshooting

**Frontend shows WS: OFF** — Backend not running. Start uvicorn first.

**Input pin shows "..." forever** — Pin not configured yet or backend not receiving ESP32 data. Try the curl commands above to test.

**ESP32 not connecting** — Check WIFI_SSID/PASSWORD. Check BACKEND_HOST is your PC's actual local IP (not localhost). Make sure PC firewall allows port 8000.

**ADC readings noisy** — This is normal for ESP32 ADC2 pins when WiFi is active. Use ADC1 pins (32–39) for stable readings.
