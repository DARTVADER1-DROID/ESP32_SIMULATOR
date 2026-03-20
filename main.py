"""
ESP32 Pin Simulator — FastAPI Backend
======================================
Connects to:
  - Frontend  : REST polling (GET /api/pin/{id}) + output push (POST /api/pin)
  - ESP32     : REST push (POST /api/esp32/reading) + output pull (GET /api/esp32/output/{id})
  - WebSocket : Real-time broadcast to all connected frontend clients
"""

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field
from typing import Optional, Dict, List
from collections import deque
import asyncio
import json
import time
import logging

logging.basicConfig(level=logging.INFO, format="%(asctime)s  %(levelname)s  %(message)s")
log = logging.getLogger(__name__)

app = FastAPI(title="ESP32 Pin Simulator", version="1.0.0", description="Backend for the ESP32 Pin Signal Simulator")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# ═══════════════════════════════════════════════════════════════
# MODELS
# ═══════════════════════════════════════════════════════════════

class PinConfig(BaseModel):
    """Sent by the frontend when configuring a pin."""
    gpio: str
    label: str = ""
    mode: Optional[str] = None           # "input" | "output"
    signal_type: Optional[str] = None    # "ADC" | "DAC" | "PWM" | "DIGITAL" | "TOUCH" | "PWM_READ"
    value: float = Field(0.0, ge=0.0, le=1.0)  # normalized 0–1
    pull_mode: str = "none"              # "none" | "up" | "down"
    frequency: int = Field(1000, ge=1, le=40_000_000)
    digital_state: bool = False
    touch_state: bool = False
    timestamp: Optional[int] = None


class ESP32Reading(BaseModel):
    """Sent by the ESP32 when it pushes a sensor reading."""
    gpio: str
    signal_type: str                     # "ADC" | "DIGITAL" | "TOUCH" | "PWM_READ"
    value: float = Field(0.0, ge=0.0, le=1.0)  # normalized 0–1
    raw: Optional[float] = None          # raw ADC count, raw touch value, etc.
    unit: Optional[str] = None           # "V", "%", "raw", etc.
    timestamp: Optional[int] = None


class HistoryEntry(BaseModel):
    ts: int
    value: float
    signal_type: str


# ═══════════════════════════════════════════════════════════════
# IN-MEMORY STORE
# ═══════════════════════════════════════════════════════════════

# Current pin configurations (set by frontend)
pin_configs: Dict[str, PinConfig] = {}

# Latest live readings for INPUT pins (set by ESP32)
pin_readings: Dict[str, ESP32Reading] = {}

# Signal history per pin (last 500 readings)
pin_history: Dict[str, deque] = {}

MAX_HISTORY = 500


def get_history(gpio: str) -> deque:
    if gpio not in pin_history:
        pin_history[gpio] = deque(maxlen=MAX_HISTORY)
    return pin_history[gpio]


# ═══════════════════════════════════════════════════════════════
# WEBSOCKET MANAGER
# ═══════════════════════════════════════════════════════════════

class ConnectionManager:
    def __init__(self):
        self.active: List[WebSocket] = []

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self.active.append(ws)
        log.info(f"WS client connected — total: {len(self.active)}")

    def disconnect(self, ws: WebSocket):
        self.active.remove(ws)
        log.info(f"WS client disconnected — total: {len(self.active)}")

    async def broadcast(self, message: dict):
        dead = []
        for client in self.active:
            try:
                await client.send_text(json.dumps(message))
            except Exception:
                dead.append(client)
        for d in dead:
            if d in self.active:
                self.active.remove(d)

    async def send_init(self, ws: WebSocket):
        """Send full current state to a newly connected client."""
        payload = {
            "type": "init",
            "configs": {k: v.dict() for k, v in pin_configs.items()},
            "readings": {k: v.dict() for k, v in pin_readings.items()},
        }
        await ws.send_text(json.dumps(payload))


manager = ConnectionManager()


# ═══════════════════════════════════════════════════════════════
# WEBSOCKET ENDPOINT
# ═══════════════════════════════════════════════════════════════

@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await manager.connect(ws)
    await manager.send_init(ws)
    try:
        while True:
            data = await ws.receive_text()
            try:
                msg = json.loads(data)
                if msg.get("type") == "ping":
                    await ws.send_text(json.dumps({"type": "pong"}))
                elif msg.get("type") == "pin_update":
                    pin = PinConfig(**msg["data"])
                    pin_configs[pin.gpio] = pin
                    await manager.broadcast({"type": "pin_config", "data": pin.dict()})
                    log.info(f"WS pin_update: GPIO {pin.gpio} → {pin.mode}/{pin.signal_type}")
            except Exception as e:
                log.warning(f"WS message error: {e}")
    except WebSocketDisconnect:
        manager.disconnect(ws)


# ═══════════════════════════════════════════════════════════════
# FRONTEND → BACKEND ENDPOINTS
# ═══════════════════════════════════════════════════════════════

@app.post("/api/pin", summary="Frontend: configure or update a pin")
async def set_pin(pin: PinConfig):
    """
    Called by the frontend when the user configures a pin (output mode, values, etc.).
    Also broadcasts the update to all WebSocket clients.
    """
    pin_configs[pin.gpio] = pin
    await manager.broadcast({"type": "pin_config", "data": pin.dict()})
    log.info(f"POST /api/pin  GPIO {pin.gpio}  {pin.mode}/{pin.signal_type}  value={pin.value:.3f}")
    return {"status": "ok", "gpio": pin.gpio, "message": f"GPIO {pin.gpio} configured"}


@app.get("/api/pin/{gpio_id}", summary="Frontend: get current state of a pin (INPUT polling)")
async def get_pin(gpio_id: str):
    """
    Called by the frontend to poll live readings for INPUT pins.
    Returns the latest reading pushed by the ESP32 (or the config if no reading yet).

    Response shape the frontend expects:
      { gpio, value, signal_type, raw, unit, timestamp }
    """
    # If ESP32 has pushed a reading, return that
    if gpio_id in pin_readings:
        reading = pin_readings[gpio_id]
        return {
            "gpio": gpio_id,
            "value": reading.value,
            "signal_type": reading.signal_type,
            "raw": reading.raw,
            "unit": reading.unit,
            "timestamp": reading.timestamp or int(time.time() * 1000),
        }

    # No reading yet — return zeros with config info
    cfg = pin_configs.get(gpio_id)
    return {
        "gpio": gpio_id,
        "value": 0.0,
        "signal_type": cfg.signal_type if cfg else None,
        "raw": None,
        "unit": None,
        "timestamp": int(time.time() * 1000),
    }


@app.get("/api/pins", summary="Frontend: get all pin states")
async def get_all_pins():
    """Returns both configs and latest readings for all pins."""
    return {
        "configs": {k: v.dict() for k, v in pin_configs.items()},
        "readings": {k: v.dict() for k, v in pin_readings.items()},
    }


@app.delete("/api/pin/{gpio_id}", summary="Frontend: reset a pin")
async def reset_pin(gpio_id: str):
    pin_configs.pop(gpio_id, None)
    pin_readings.pop(gpio_id, None)
    pin_history.pop(gpio_id, None)
    await manager.broadcast({"type": "pin_reset", "gpio": gpio_id})
    return {"status": "ok", "gpio": gpio_id}


@app.delete("/api/pins", summary="Frontend: reset all pins")
async def reset_all_pins():
    pin_configs.clear()
    pin_readings.clear()
    pin_history.clear()
    await manager.broadcast({"type": "reset_all"})
    return {"status": "ok", "message": "All pins reset"}


# ═══════════════════════════════════════════════════════════════
# ESP32 → BACKEND ENDPOINTS
# ═══════════════════════════════════════════════════════════════

@app.post("/api/esp32/reading", summary="ESP32: push a sensor reading")
async def esp32_push_reading(reading: ESP32Reading):
    """
    Called by the ESP32 to push a live sensor reading.
    The frontend will pick this up on its next poll of GET /api/pin/{id}.
    Also broadcasts via WebSocket so connected frontends update instantly.

    Payload from ESP32:
      {
        "gpio": "34",
        "signal_type": "ADC",
        "value": 0.75,        // normalized 0.0–1.0
        "raw": 3071,          // raw ADC count (0–4095)
        "unit": "V"
      }
    """
    if reading.timestamp is None:
        reading.timestamp = int(time.time() * 1000)

    pin_readings[reading.gpio] = reading

    # Push to history
    hist = get_history(reading.gpio)
    hist.appendleft({
        "ts": reading.timestamp,
        "value": reading.value,
        "signal_type": reading.signal_type,
    })

    # Broadcast to frontend via WebSocket
    await manager.broadcast({
        "type": "pin_state",
        "data": {
            "gpio": reading.gpio,
            "value": reading.value,
            "signal_type": reading.signal_type,
            "raw": reading.raw,
            "unit": reading.unit,
            "timestamp": reading.timestamp,
        }
    })

    return {"status": "ok", "gpio": reading.gpio}


@app.post("/api/esp32/readings/batch", summary="ESP32: push multiple readings at once")
async def esp32_push_batch(readings: List[ESP32Reading]):
    """
    ESP32 can push multiple pin readings in a single HTTP request.
    More efficient than one request per pin.
    """
    updated = []
    for reading in readings:
        if reading.timestamp is None:
            reading.timestamp = int(time.time() * 1000)
        pin_readings[reading.gpio] = reading
        hist = get_history(reading.gpio)
        hist.appendleft({"ts": reading.timestamp, "value": reading.value, "signal_type": reading.signal_type})
        updated.append(reading.gpio)

    # Single broadcast for all readings
    await manager.broadcast({
        "type": "batch_readings",
        "readings": [r.dict() for r in readings],
    })

    return {"status": "ok", "updated": updated}


@app.get("/api/esp32/output/{gpio_id}", summary="ESP32: get the current output command for a pin")
async def esp32_get_output(gpio_id: str):
    """
    Called by the ESP32 to get what value it should output on a pin.
    The frontend sets this via POST /api/pin when the user moves a slider.

    Response the ESP32 should parse:
      {
        "gpio": "25",
        "mode": "output",
        "signal_type": "DAC",
        "value": 0.75,          // normalized 0.0–1.0
        "voltage": 2.475,       // convenience: value * 3.3
        "raw_8bit": 191,        // convenience: value * 255  (for DAC/PWM 8-bit)
        "raw_12bit": 3071,      // convenience: value * 4095 (for ADC reference)
        "duty_percent": 75.0,   // convenience: value * 100  (for PWM)
        "digital_state": true,  // for DIGITAL mode
        "frequency": 1000,      // for PWM
        "pull_mode": "none",
        "configured": true
      }
    """
    cfg = pin_configs.get(gpio_id)
    if not cfg:
        return {
            "gpio": gpio_id,
            "configured": False,
            "mode": None,
            "value": 0.0,
        }

    return {
        "gpio": gpio_id,
        "configured": True,
        "mode": cfg.mode,
        "signal_type": cfg.signal_type,
        "value": cfg.value,
        "voltage": round(cfg.value * 3.3, 4),
        "raw_8bit": round(cfg.value * 255),
        "raw_12bit": round(cfg.value * 4095),
        "duty_percent": round(cfg.value * 100, 2),
        "digital_state": cfg.digital_state,
        "frequency": cfg.frequency,
        "pull_mode": cfg.pull_mode,
    }


@app.get("/api/esp32/outputs", summary="ESP32: get ALL output pin commands at once")
async def esp32_get_all_outputs():
    """
    ESP32 polls this single endpoint to get all configured output pins.
    More efficient than polling each pin individually.
    """
    outputs = {}
    for gpio_id, cfg in pin_configs.items():
        if cfg.mode == "output":
            outputs[gpio_id] = {
                "gpio": gpio_id,
                "signal_type": cfg.signal_type,
                "value": cfg.value,
                "voltage": round(cfg.value * 3.3, 4),
                "raw_8bit": round(cfg.value * 255),
                "duty_percent": round(cfg.value * 100, 2),
                "digital_state": cfg.digital_state,
                "frequency": cfg.frequency,
            }
    return {"outputs": outputs}


@app.get("/api/esp32/inputs", summary="ESP32: get all configured INPUT pins")
async def esp32_get_all_inputs():
    """
    ESP32 calls this to know which pins it should be reading and sending back.
    Returns the list of input pins with their signal type and pull mode.
    """
    inputs = {}
    for gpio_id, cfg in pin_configs.items():
        if cfg.mode == "input":
            inputs[gpio_id] = {
                "gpio": gpio_id,
                "signal_type": cfg.signal_type,
                "pull_mode": cfg.pull_mode,
            }
    return {"inputs": inputs}


# ═══════════════════════════════════════════════════════════════
# HISTORY ENDPOINT
# ═══════════════════════════════════════════════════════════════

@app.get("/api/pin/{gpio_id}/history", summary="Get signal history for a pin")
async def get_pin_history(gpio_id: str, limit: int = 100):
    hist = list(get_history(gpio_id))
    return {"gpio": gpio_id, "history": hist[:limit], "total": len(hist)}


# ═══════════════════════════════════════════════════════════════
# HEALTH
# ═══════════════════════════════════════════════════════════════

@app.get("/health", summary="Health check")
async def health():
    return {
        "status": "ok",
        "ws_clients": len(manager.active),
        "configured_pins": len(pin_configs),
        "active_readings": len(pin_readings),
        "uptime_ms": int(time.time() * 1000),
    }


@app.get("/", summary="API info")
async def root():
    return {
        "name": "ESP32 Pin Simulator Backend",
        "version": "1.0.0",
        "docs": "/docs",
        "health": "/health",
        "endpoints": {
            "frontend": ["POST /api/pin", "GET /api/pin/{id}", "GET /api/pins", "DELETE /api/pin/{id}", "WS /ws"],
            "esp32":    ["POST /api/esp32/reading", "POST /api/esp32/readings/batch", "GET /api/esp32/output/{id}", "GET /api/esp32/outputs", "GET /api/esp32/inputs"],
        }
    }
