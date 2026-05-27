"""
VitaLink Dashboard — Backend
Faz bridge entre o broker MQTT e o browser via Socket.IO.
"""

import json
import threading
import time
from collections import deque
from datetime import datetime

import paho.mqtt.client as mqtt
from flask import Flask, render_template
from flask_socketio import SocketIO

# ─── Config ──────────────────────────────────────────────────────────────────

MQTT_BROKER         = "localhost"
MQTT_PORT           = 1883
MQTT_TOPIC_ALERTS   = "elderly/alerts"
MQTT_TOPIC_SAMPLES  = "elderly/samples"

app      = Flask(__name__)
app.config["SECRET_KEY"] = "vitalink_2025"
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

# Buffers thread-safe
events:  deque[dict] = deque(maxlen=100)   # alertas / online / offline
samples: deque[dict] = deque(maxlen=500)   # telemetria do benchmark
broker_connected = False

# ─── Helpers ─────────────────────────────────────────────────────────────────

def format_uptime(ms: int) -> str:
    s = ms // 1000
    h, rem = divmod(s, 3600)
    m, s   = divmod(rem, 60)
    if h:   return f"{h}h {m}m"
    if m:   return f"{m}m {s}s"
    return f"{s}s"

def make_event(payload: dict) -> dict:
    now = datetime.now()
    return {
        "status":     payload.get("status", "unknown"),
        "cause":      payload.get("cause", ""),          # "manual", "fall"
        "accel_g":    payload.get("accel_g", 0.0),       # magnitude do impacto (queda)
        "device_id":  payload.get("device_id", "?"),
        "uptime_ms":  payload.get("uptime_ms", 0),
        "uptime_str": format_uptime(payload.get("uptime_ms", 0)),
        "time_str":   now.strftime("%H:%M:%S"),
        "date_str":   now.strftime("%d/%m/%Y"),
        "iso":        now.isoformat(),
    }

# ─── MQTT callbacks ──────────────────────────────────────────────────────────

def on_connect(client, userdata, flags, rc, properties=None):
    global broker_connected
    if rc == 0:
        broker_connected = True
        client.subscribe(MQTT_TOPIC_ALERTS,  qos=1)
        client.subscribe(MQTT_TOPIC_SAMPLES, qos=0)
        print(f"[MQTT] Conectado → '{MQTT_TOPIC_ALERTS}' + '{MQTT_TOPIC_SAMPLES}'")
        socketio.emit("broker_status", {"connected": True})
    else:
        print(f"[MQTT] Falha na conexão (rc={rc})")

def on_disconnect(client, userdata, disconnect_flags, reason_code, properties=None):
    global broker_connected
    broker_connected = False
    print("[MQTT] Desconectado do broker")
    socketio.emit("broker_status", {"connected": False})

def on_message(client, userdata, msg):
    try:
        raw = json.loads(msg.payload.decode())
        if msg.topic == MQTT_TOPIC_SAMPLES:
            _handle_sample(raw)
        else:
            _handle_event(raw)
    except Exception as exc:
        print(f"[MQTT] Erro ao processar mensagem: {exc}")

def _handle_event(raw: dict):
    event = make_event(raw)
    events.appendleft(event)
    socketio.emit("mqtt_event", event)
    print(f"[MQTT] {event['time_str']} [{event['status'].upper()}] {event['device_id']}")

def _handle_sample(raw: dict):
    sample = {
        "seq":      raw.get("seq",      0),
        "value":    round(float(raw.get("value", 0.0)), 2),
        "buf_size": int(raw.get("buf_size", 0)),
        "buf_cap":  int(raw.get("buf_cap",  512)),
        "produced": int(raw.get("produced", 0)),
    }
    samples.appendleft(sample)
    socketio.emit("telemetry_sample", sample)

# ─── MQTT worker (thread separada com reconexão) ──────────────────────────────

def mqtt_worker():
    client = mqtt.Client(
        client_id            = "vitalink_dashboard_01",
        callback_api_version = mqtt.CallbackAPIVersion.VERSION2,
    )
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    while True:
        try:
            print(f"[MQTT] Conectando a {MQTT_BROKER}:{MQTT_PORT}...")
            client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
            client.loop_forever()
        except Exception as exc:
            print(f"[MQTT] Erro: {exc} — tentando novamente em 5s")
            time.sleep(5)

# ─── Rotas Flask ──────────────────────────────────────────────────────────────

@app.route("/")
def index():
    today = datetime.now().strftime("%d/%m/%Y")
    alerts_today = sum(1 for e in events if e["status"] == "alert" and e["date_str"] == today)
    history      = list(events)[:30]
    latest       = history[0] if history else None
    return render_template(
        "index.html",
        history        = history,
        latest         = latest,
        alerts_today   = alerts_today,
        broker_host    = MQTT_BROKER,
        broker_port    = MQTT_PORT,
        broker_ok      = broker_connected,
        telem_count    = len(samples),
    )

@app.route("/api/events")
def api_events():
    return {"events": list(events)[:50]}

@app.route("/api/samples")
def api_samples():
    return {"samples": list(samples)[:100]}

# ─── Socket.IO ────────────────────────────────────────────────────────────────

@socketio.on("connect")
def handle_connect():
    socketio.emit("broker_status", {"connected": broker_connected})
    # Hidrata o gráfico com as últimas amostras já recebidas
    if samples:
        history = list(reversed(list(samples)[:100]))
        socketio.emit("telem_history", history)

# ─── Entry point ─────────────────────────────────────────────────────────────

if __name__ == "__main__":
    t = threading.Thread(target=mqtt_worker, daemon=True)
    t.start()
    print("\n" + "=" * 52)
    print("  VitaLink Dashboard rodando em http://localhost:5000")
    print("=" * 52 + "\n")
    socketio.run(app, host="0.0.0.0", port=5000, debug=False, use_reloader=False)
