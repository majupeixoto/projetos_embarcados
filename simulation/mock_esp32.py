#!/usr/bin/env python3
"""
Simulador do ESP32 — Pulseira de Assistência ao Idoso
======================================================
Simula o firmware completo: botão de pânico + acelerômetro MPU-6500.
Cada evento replica exatamente o JSON publicado pelo firmware real.

Dependência:  pip install paho-mqtt
Uso:          python mock_esp32.py
              python mock_esp32.py --broker 192.168.1.100 --device esp32_02
              python mock_esp32.py --auto-fall 60   # queda aleatória a cada ~60s
"""

import argparse
import json
import math
import random
import sys
import threading
import time

import paho.mqtt.client as mqtt

# ─── Configuração padrão ──────────────────────────────────────────────────────

DEFAULT_BROKER = "localhost"
DEFAULT_PORT   = 1883
DEFAULT_TOPIC  = "elderly/alerts"
DEFAULT_DEVICE = "esp32_01"

HEARTBEAT_INTERVAL = 30   # segundos — igual ao firmware

# ─── Thresholds espelhados do firmware (somente para referência visual) ───────
# Estes valores NÃO afetam a simulação; servem apenas para exibir nos logs
# a mesma nomenclatura usada no firmware (config.h).
FREEFALL_THRESHOLD_G = 0.40
IMPACT_THRESHOLD_G   = 2.50
FALL_WINDOW_MS       = 500

# ─── Estado do dispositivo simulado ──────────────────────────────────────────

class DeviceState:
    def __init__(self, device_id: str):
        self.device_id  = device_id
        self.start_time = time.time()
        self.connected  = False

    def uptime_ms(self) -> int:
        return int((time.time() - self.start_time) * 1000)

    def build_payload(self, status: str, cause: str = "",
                      accel_g: float = 0.0) -> str:
        doc: dict = {
            "status":    status,
            "device_id": self.device_id,
            "uptime_ms": self.uptime_ms(),
        }
        if cause:
            doc["cause"]   = cause
        if accel_g > 0:
            doc["accel_g"] = round(accel_g, 2)
        return json.dumps(doc)

# ─── Cores ANSI para terminal ─────────────────────────────────────────────────

class C:
    RED    = "\033[91m"
    GREEN  = "\033[92m"
    YELLOW = "\033[93m"
    BLUE   = "\033[94m"
    GRAY   = "\033[90m"
    BOLD   = "\033[1m"
    RESET  = "\033[0m"

def colored(text: str, color: str) -> str:
    return f"{color}{text}{C.RESET}"

# ─── Callbacks MQTT ──────────────────────────────────────────────────────────

def on_connect(client, userdata: DeviceState, flags, rc, properties=None):
    if rc == 0:
        userdata.connected = True
        print(colored("[MQTT] Conectado ao broker.", C.GREEN))
        _publish_online(client, userdata)
    else:
        codes = {1: "protocolo recusado", 2: "ID rejeitado",
                 3: "servidor indisponível", 4: "credenciais inválidas",
                 5: "não autorizado"}
        print(colored(f"[MQTT] Falha: {codes.get(rc, f'rc={rc}')}", C.RED))

def on_disconnect(client, userdata: DeviceState, disconnect_flags, reason_code, properties):
    # paho-mqtt 2.x (VERSION2): asssinatura tem 5 args obrigatórios
    userdata.connected = False
    if reason_code != 0:
        print(colored(f"[MQTT] Desconectado inesperadamente (rc={reason_code}).", C.YELLOW))

def on_publish(client, userdata, mid, reason_code, properties):
    # paho-mqtt 2.x (VERSION2): inclui reason_code e properties após mid
    pass   # confirmação silenciosa — log fica nas funções de publicação

# ─── Funções de publicação ────────────────────────────────────────────────────

def _log_publish(payload: str, label: str, color: str):
    ts = time.strftime("%H:%M:%S")
    print(f"  {C.GRAY}[{ts}]{C.RESET} {colored(label, color + C.BOLD)}  {C.GRAY}{payload}{C.RESET}")

def _publish_raw(client: mqtt.Client, topic: str, payload: str, retained: bool):
    client.publish(topic, payload, qos=1, retain=retained)

def _publish_online(client: mqtt.Client, state: DeviceState):
    payload = state.build_payload("online", cause="heartbeat")
    _publish_raw(client, DEFAULT_TOPIC, payload, retained=False)
    _log_publish(payload, "ONLINE", C.GREEN)

def _publish_button(client: mqtt.Client, state: DeviceState):
    """Simula o botão de pânico físico (ALERT_BUTTON no firmware)."""
    payload = state.build_payload("alert", cause="manual", accel_g=0.0)
    _publish_raw(client, DEFAULT_TOPIC, payload, retained=True)
    _log_publish(payload, "ALERTA — BOTÃO DE PÂNICO", C.RED)

def _publish_fall(client: mqtt.Client, state: DeviceState, accel_g: float):
    """
    Simula uma queda detectada pelo MPU-6500 (ALERT_FALL no firmware).

    O valor accel_g representa a magnitude do impacto no momento da detecção.
    O firmware usa a fórmula: sqrt(ax² + ay² + az²) em unidades 'g'.

    Faixa realista de impacto:
      Queda leve (sentado no chão) : ~2.5 – 3.5 g
      Queda média (escorregão)     : ~3.5 – 5.0 g
      Queda forte (desmaio)        : ~5.0 – 8.0 g
    """
    payload = state.build_payload("alert", cause="fall", accel_g=accel_g)
    _publish_raw(client, DEFAULT_TOPIC, payload, retained=True)
    _log_publish(payload, f"ALERTA — QUEDA DETECTADA ({accel_g:.2f} g)", C.RED)

# ─── Simulação física da queda ────────────────────────────────────────────────

def simulate_fall_physics() -> tuple[float, float]:
    """
    Reproduz as fases física de uma queda que o firmware detectaria:

    Fase 1 — FREEFALL  : magnitude cai abaixo de FREEFALL_THRESHOLD_G.
                         Simulada por FREEFALL_SAMPLES * MPU_SAMPLE_MS = ~100 ms.
    Fase 2 — IMPACT    : magnitude dispara acima de IMPACT_THRESHOLD_G.
                         Valor gerado aleatoriamente dentro de uma faixa realista.

    Retorna: (duração_freefall_s, accel_impacto_g)
    """
    freefall_duration = random.uniform(0.10, 0.35)   # segundos em queda livre
    impact_g          = random.uniform(IMPACT_THRESHOLD_G + 0.3, 7.5)
    return freefall_duration, impact_g

def run_fall_sequence(client: mqtt.Client, state: DeviceState):
    """Executa a sequência completa de queda com logs de cada fase."""
    if not state.connected:
        print(colored("  [!] Aguardando conexão com o broker...", C.YELLOW))
        return

    freefall_s, impact_g = simulate_fall_physics()

    # ── Fase 1: freefall ──────────────────────────────────────────────────
    mag_freefall = random.uniform(0.02, FREEFALL_THRESHOLD_G - 0.05)
    print(f"\n  {colored('↓ FREEFALL', C.YELLOW)}  mag={mag_freefall:.2f} g  "
          f"(duração simulada: {freefall_s*1000:.0f} ms)")
    time.sleep(freefall_s)

    # ── Fase 2: impacto ───────────────────────────────────────────────────
    print(f"  {colored('💥 IMPACTO', C.RED)}   mag={impact_g:.2f} g  "
          f"(threshold: {IMPACT_THRESHOLD_G} g)")
    _publish_fall(client, state, impact_g)

    # Firmware: LED vermelho por ALERT_LED_DURATION_MS, depois volta ao verde
    time.sleep(3)
    _publish_online(client, state)
    print()

# ─── Threads de suporte ───────────────────────────────────────────────────────

def heartbeat_thread(client: mqtt.Client, state: DeviceState):
    """Heartbeat periódico — espelha o timer de 30s do firmware."""
    while True:
        time.sleep(HEARTBEAT_INTERVAL)
        if state.connected:
            _publish_online(client, state)

def auto_fall_thread(client: mqtt.Client, state: DeviceState, interval_s: int):
    """
    Gera quedas aleatórias automaticamente.
    Útil para estresse do dashboard sem interação manual.
    Intervalo real = interval_s ± 20% (jitter para parecer mais realista).
    """
    print(colored(f"  [AUTO-FALL] Ativo — queda a cada ~{interval_s}s", C.YELLOW))
    while True:
        jitter = interval_s * random.uniform(0.8, 1.2)
        time.sleep(jitter)
        if state.connected:
            print(colored("\n  [AUTO-FALL] Disparando queda automática...", C.YELLOW))
            run_fall_sequence(client, state)

# ─── Loop de comandos (thread principal) ─────────────────────────────────────

MENU = f"""
  {C.BOLD}Comandos disponíveis:{C.RESET}
  {C.BOLD}[ENTER]{C.RESET}    → Botão de pânico (cause: manual)
  {C.BOLD}f[ENTER]{C.RESET}   → Simular queda com física realista (cause: fall)
  {C.BOLD}h[ENTER]{C.RESET}   → Enviar heartbeat manual
  {C.BOLD}q[ENTER]{C.RESET}   → Sair
"""

def command_loop(client: mqtt.Client, state: DeviceState):
    print(MENU)
    try:
        while True:
            cmd = input("  > ").strip().lower()

            if cmd == "" :   # ENTER = botão de pânico
                if not state.connected:
                    print(colored("  [!] Sem conexão com o broker.", C.YELLOW))
                    continue
                print(colored("\n  *** BOTÃO DE PÂNICO PRESSIONADO! ***\n", C.RED + C.BOLD))
                _publish_button(client, state)
                time.sleep(3)
                _publish_online(client, state)
                print()

            elif cmd == "f":  # queda simulada
                run_fall_sequence(client, state)

            elif cmd == "h":  # heartbeat manual
                if state.connected:
                    _publish_online(client, state)

            elif cmd == "q":  # sair
                break

            else:
                print(colored("  Comando desconhecido. Use ENTER, f, h ou q.", C.GRAY))

    except (KeyboardInterrupt, EOFError):
        pass

# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Simulador ESP32 — Pulseira Idoso")
    parser.add_argument("--broker",    default=DEFAULT_BROKER, help="IP do broker MQTT")
    parser.add_argument("--port",      default=DEFAULT_PORT,   type=int)
    parser.add_argument("--device",    default=DEFAULT_DEVICE, help="Device ID")
    parser.add_argument("--auto-fall", default=0,              type=int,
                        metavar="SEG",
                        help="Gera quedas automáticas a cada N segundos (0 = desativado)")
    args = parser.parse_args()

    state  = DeviceState(args.device)
    client = mqtt.Client(
        client_id            = args.device,
        callback_api_version = mqtt.CallbackAPIVersion.VERSION2,
    )
    client.user_data_set(state)
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_publish    = on_publish

    # LWT: broker publica "offline" automaticamente se a conexão cair
    lwt = state.build_payload("offline", cause="lwt")
    client.will_set(DEFAULT_TOPIC, lwt, qos=1, retain=True)

    print(f"\n{C.BOLD}{'═'*54}{C.RESET}")
    print(f"  {C.BOLD}VitaLink — Simulador ESP32 + MPU-6500{C.RESET}")
    print(f"{'═'*54}")
    print(f"  Broker  : {args.broker}:{args.port}")
    print(f"  Tópico  : {DEFAULT_TOPIC}")
    print(f"  Device  : {args.device}")
    print(f"  Auto-fall: {'cada ~' + str(args.auto_fall) + 's' if args.auto_fall else 'desativado'}")
    print(f"{'═'*54}\n")

    try:
        client.connect(args.broker, args.port, keepalive=60)
    except (ConnectionRefusedError, OSError) as exc:
        print(colored(f"[Erro] Não foi possível conectar: {exc}", C.RED))
        print("  Windows: & 'C:\\Program Files\\mosquitto\\mosquitto.exe' -v")
        print("  Linux  : sudo systemctl start mosquitto")
        sys.exit(1)

    client.loop_start()

    # Thread de heartbeat (daemon)
    threading.Thread(target=heartbeat_thread, args=(client, state),
                     daemon=True).start()

    # Thread de quedas automáticas (se solicitado)
    if args.auto_fall > 0:
        threading.Thread(target=auto_fall_thread,
                         args=(client, state, args.auto_fall),
                         daemon=True).start()

    command_loop(client, state)

    print(colored("\n[Simulador] Encerrando...", C.GRAY))
    if state.connected:
        payload = state.build_payload("offline", cause="shutdown")
        _publish_raw(client, DEFAULT_TOPIC, payload, retained=True)
        time.sleep(0.5)

    client.loop_stop()
    client.disconnect()
    print(colored("[Simulador] Desconectado.", C.GRAY))

if __name__ == "__main__":
    main()
