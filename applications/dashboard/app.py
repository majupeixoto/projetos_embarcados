"""
VitaLink Dashboard — Backend
Faz bridge entre o broker MQTT e o browser via Socket.IO.
"""

import json
import os
import smtplib

from dotenv import load_dotenv
load_dotenv()
import threading
import time
from collections import deque
from datetime import datetime
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

import paho.mqtt.client as mqtt
from flask import Flask, render_template, request
from flask_socketio import SocketIO

# ─── Config ──────────────────────────────────────────────────────────────────

# ─── E-mail (variáveis de ambiente) ──────────────────────────────────────────
# Configure antes de iniciar o servidor:
#   export VITALINK_EMAIL="seu@gmail.com"
#   export VITALINK_PASSWORD="sua_app_password"   # Gmail: Conta → Segurança → Senhas de app
#   export VITALINK_SMTP="smtp.gmail.com"          # opcional, padrão Gmail
#   export VITALINK_SMTP_PORT="587"                # opcional, padrão TLS

EMAIL_SENDER   = os.environ.get("VITALINK_EMAIL",    "")
EMAIL_PASSWORD = os.environ.get("VITALINK_PASSWORD", "")
EMAIL_SMTP     = os.environ.get("VITALINK_SMTP",     "smtp.gmail.com")
EMAIL_PORT     = int(os.environ.get("VITALINK_SMTP_PORT", "587"))

MQTT_BROKER         = "localhost"
MQTT_PORT           = 1883
MQTT_TOPIC_ALERTS   = "elderly/alerts"
MQTT_TOPIC_SAMPLES  = "elderly/samples"
MQTT_TOPIC_BENCH    = "elderly/benchmark"

app      = Flask(__name__)
app.config["SECRET_KEY"] = "vitalink_2025"
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

# Buffers thread-safe
events:        deque[dict] = deque(maxlen=100)   # alertas / online / offline
samples:       deque[dict] = deque(maxlen=500)   # telemetria do benchmark
bench_results: list        = []                  # resultados V1 vs V2 (max 6 entradas)
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
        client.subscribe(MQTT_TOPIC_BENCH,   qos=1)
        print(f"[MQTT] Conectado → '{MQTT_TOPIC_ALERTS}' + '{MQTT_TOPIC_SAMPLES}' + '{MQTT_TOPIC_BENCH}'")
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
        elif msg.topic == MQTT_TOPIC_BENCH:
            _handle_benchmark(raw)
        else:
            _handle_event(raw)
    except Exception as exc:
        print(f"[MQTT] Erro ao processar mensagem: {exc}")

def _handle_event(raw: dict):
    event = make_event(raw)
    events.appendleft(event)
    socketio.emit("mqtt_event", event)
    print(f"[MQTT] {event['time_str']} [{event['status'].upper()}] {event['device_id']}")

def _handle_benchmark(raw: dict):
    key = (raw.get("vertente"), raw.get("n"))
    for i, r in enumerate(bench_results):
        if (r.get("vertente"), r.get("n")) == key:
            bench_results[i] = raw
            break
    else:
        bench_results.append(raw)
    socketio.emit("benchmark_result", raw)

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

@app.route("/api/send-report", methods=["POST"])
def send_report():
    if not EMAIL_SENDER or not EMAIL_PASSWORD:
        return {"error": "E-mail não configurado no servidor. Defina VITALINK_EMAIL e VITALINK_PASSWORD."}, 503

    data     = request.get_json(silent=True) or {}
    to_email = data.get("email", "").strip()
    if not to_email or "@" not in to_email:
        return {"error": "Endereço de e-mail inválido."}, 400

    html = _build_report_html()
    try:
        msg             = MIMEMultipart("alternative")
        msg["Subject"]  = f"VitaLink — Boletim do Dia {datetime.now().strftime('%d/%m/%Y')}"
        msg["From"]     = EMAIL_SENDER
        msg["To"]       = to_email
        msg.attach(MIMEText(html, "html", "utf-8"))

        with smtplib.SMTP(EMAIL_SMTP, EMAIL_PORT) as server:
            server.starttls()
            server.login(EMAIL_SENDER, EMAIL_PASSWORD)
            server.sendmail(EMAIL_SENDER, to_email, msg.as_string())

        print(f"[Email] Boletim enviado para {to_email}")
        return {"ok": True}
    except smtplib.SMTPAuthenticationError as exc:
        print(f"[Email] Falha de autenticação ({EMAIL_SMTP}:{EMAIL_PORT}) conta={EMAIL_SENDER} — {exc}")
        return {"error": "Falha de autenticação. Use uma App Password do Gmail (não a senha normal)."}, 500
    except smtplib.SMTPConnectError as exc:
        print(f"[Email] Não conectou ao servidor SMTP — {exc}")
        return {"error": f"Não foi possível conectar ao servidor {EMAIL_SMTP}:{EMAIL_PORT}."}, 500
    except Exception as exc:
        print(f"[Email] Erro inesperado: {type(exc).__name__}: {exc}")
        return {"error": str(exc)}, 500

# ─── Gerador do boletim HTML ──────────────────────────────────────────────────

def _build_report_html() -> str:
    today  = datetime.now().strftime("%d/%m/%Y")
    now_s  = datetime.now().strftime("%H:%M")

    today_evs   = [e for e in events if e["date_str"] == today]
    evs_sorted  = sorted(today_evs, key=lambda e: e["time_str"])

    alerts_n = sum(1 for e in today_evs if e["status"] == "alert")
    total_n  = len(today_evs)
    last_t   = today_evs[0]["time_str"] if today_evs else "—"
    device   = today_evs[0]["device_id"] if today_evs else "—"

    # ── Linhas da tabela de eventos ───────────────────────────────────────────
    def _row(e):
        s, c = e["status"], e.get("cause", "")
        if s == "alert" and c == "fall":
            icon, label, color, bg = "🚨", "Queda Confirmada", "#dc2626", "#fef2f2"
        elif s == "alert":
            icon, label, color, bg = "🚨", "Botão de Pânico",  "#dc2626", "#fef2f2"
        elif s == "pre_alert":
            icon, label, color, bg = "⚠️", "Pré-Alerta",       "#d97706", "#fffbeb"
        elif s == "online":
            icon, label, color, bg = "✅", "Online",            "#16a34a", "#f0fdf4"
        else:
            icon, label, color, bg = "❌", "Offline",           "#64748b", "#f8fafc"

        accel = e.get("accel_g", 0)
        detail = (f"<span style='color:#94a3b8;font-size:11px;'> · {float(accel):.2f}g</span>"
                  if accel and float(accel) > 0 else "")

        return f"""
        <tr>
          <td style="padding:10px 16px;border-bottom:1px solid #f1f5f9;font-family:monospace;font-size:12px;color:#64748b;white-space:nowrap;">{e['time_str']}</td>
          <td style="padding:10px 16px;border-bottom:1px solid #f1f5f9;">
            <span style="background:{bg};color:{color};padding:3px 10px;border-radius:5px;font-size:11px;font-weight:700;">{icon} {label}</span>{detail}
          </td>
          <td style="padding:10px 16px;border-bottom:1px solid #f1f5f9;font-size:12px;color:#94a3b8;font-family:monospace;">{e['device_id']}</td>
          <td style="padding:10px 16px;border-bottom:1px solid #f1f5f9;font-size:12px;color:#94a3b8;">{e.get('uptime_str','—')}</td>
        </tr>"""

    rows = "".join(_row(e) for e in evs_sorted) if evs_sorted else """
        <tr><td colspan="4" style="padding:32px;text-align:center;color:#94a3b8;font-size:13px;">
          Nenhum evento registrado hoje.
        </td></tr>"""

    return f"""<!DOCTYPE html>
<html lang="pt-BR">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"></head>
<body style="margin:0;padding:0;background:#f1f5f9;font-family:Arial,Helvetica,sans-serif;">
<table width="100%" cellpadding="0" cellspacing="0" style="background:#f1f5f9;">
<tr><td align="center" style="padding:40px 16px;">
<table width="580" cellpadding="0" cellspacing="0" style="background:#fff;border-radius:16px;overflow:hidden;box-shadow:0 4px 24px rgba(0,0,0,0.08);">

  <!-- HEADER -->
  <tr><td style="background:linear-gradient(135deg,#0f172a 0%,#1a3254 100%);padding:36px 40px;text-align:center;">
    <table cellpadding="0" cellspacing="0" align="center">
      <tr>
        <td style="background:#3b82f6;border-radius:12px;width:48px;height:48px;text-align:center;vertical-align:middle;font-size:22px;line-height:48px;">🛡️</td>
        <td style="padding-left:14px;text-align:left;">
          <p style="margin:0;font-size:22px;font-weight:800;color:#fff;letter-spacing:-0.5px;">VitaLink</p>
          <p style="margin:2px 0 0;font-size:12px;color:#94a3b8;">Monitoramento 24h</p>
        </td>
      </tr>
    </table>
    <p style="margin:20px 0 0;font-size:14px;color:#cbd5e1;">Boletim Diário — <strong style="color:#fff;">{today}</strong></p>
    <p style="margin:6px 0 0;font-size:12px;color:#64748b;">Dispositivo: <span style="color:#94a3b8;font-family:monospace;">{device}</span></p>
  </td></tr>

  <!-- STATS -->
  <tr><td style="padding:28px 40px 16px;">
    <table width="100%" cellpadding="0" cellspacing="0"><tr>
      <td width="33%" style="padding:8px;">
        <div style="background:#fef2f2;border-radius:12px;padding:18px 8px;text-align:center;">
          <p style="margin:0;font-size:32px;font-weight:900;color:#dc2626;">{alerts_n}</p>
          <p style="margin:4px 0 0;font-size:10px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;">Alertas</p>
        </div>
      </td>
      <td width="33%" style="padding:8px;">
        <div style="background:#f0f9ff;border-radius:12px;padding:18px 8px;text-align:center;">
          <p style="margin:0;font-size:32px;font-weight:900;color:#0284c7;">{total_n}</p>
          <p style="margin:4px 0 0;font-size:10px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;">Eventos</p>
        </div>
      </td>
      <td width="33%" style="padding:8px;">
        <div style="background:#f0fdf4;border-radius:12px;padding:18px 8px;text-align:center;">
          <p style="margin:0;font-size:26px;font-weight:900;color:#16a34a;font-family:monospace;">{last_t}</p>
          <p style="margin:4px 0 0;font-size:10px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;">Último Reg.</p>
        </div>
      </td>
    </tr></table>
  </td></tr>

  <!-- TABELA DE EVENTOS -->
  <tr><td style="padding:0 24px 8px;">
    <p style="margin:0 16px 12px;font-size:13px;font-weight:700;color:#1e293b;">Histórico de Eventos</p>
    <table width="100%" cellpadding="0" cellspacing="0" style="border:1px solid #f1f5f9;border-radius:10px;overflow:hidden;">
      <tr style="background:#f8fafc;">
        <th style="padding:10px 16px;text-align:left;font-size:10px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;font-weight:600;">Horário</th>
        <th style="padding:10px 16px;text-align:left;font-size:10px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;font-weight:600;">Evento</th>
        <th style="padding:10px 16px;text-align:left;font-size:10px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;font-weight:600;">Dispositivo</th>
        <th style="padding:10px 16px;text-align:left;font-size:10px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;font-weight:600;">Uptime</th>
      </tr>
      {rows}
    </table>
  </td></tr>

  <!-- FOOTER -->
  <tr><td style="background:#f8fafc;padding:20px 40px;border-top:1px solid #f1f5f9;text-align:center;">
    <p style="margin:0;font-size:11px;color:#94a3b8;">Gerado em {today} às {now_s} · VitaLink Dashboard</p>
    <p style="margin:6px 0 0;font-size:11px;color:#cbd5e1;">Este é um e-mail automático. Não responda.</p>
  </td></tr>

</table>
</td></tr>
</table>
</body>
</html>"""

# ─── Socket.IO ────────────────────────────────────────────────────────────────

@socketio.on("connect")
def handle_connect():
    socketio.emit("broker_status", {"connected": broker_connected})
    if samples:
        history = list(reversed(list(samples)[:100]))
        socketio.emit("telem_history", history)
    if bench_results:
        socketio.emit("bench_history", bench_results)

# ─── Entry point ─────────────────────────────────────────────────────────────

if __name__ == "__main__":
    t = threading.Thread(target=mqtt_worker, daemon=True)
    t.start()
    print("\n" + "=" * 52)
    print("  VitaLink Dashboard rodando em http://localhost:5000")
    print("=" * 52 + "\n")
    socketio.run(app, host="0.0.0.0", port=5000, debug=False, use_reloader=False)
