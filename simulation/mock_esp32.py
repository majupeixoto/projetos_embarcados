#!/usr/bin/env python3
"""
Simulador do ESP32 - Pulseira de Assistência ao Idoso
======================================================
Simula o firmware do ESP32 enviando pacotes JSON via MQTT para o broker local.
Use este script para testar o Dashboard Web sem o hardware físico.

Dependência:
    pip install paho-mqtt

Uso:
    python mock_esp32.py
    python mock_esp32.py --broker 192.168.1.100 --device esp32_02
"""

import argparse
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt

# ─── Configuração padrão ──────────────────────────────────────────────────────

DEFAULT_BROKER = "localhost"
DEFAULT_PORT   = 1883
DEFAULT_TOPIC  = "elderly/alerts"
DEFAULT_DEVICE = "esp32_01"

HEARTBEAT_INTERVAL = 30   # segundos, igual ao firmware


# ─── Estado do simulador ──────────────────────────────────────────────────────

class DeviceState:
    def __init__(self, device_id: str):
        self.device_id  = device_id
        self.start_time = time.time()
        self.connected  = False

    def uptime_ms(self) -> int:
        return int((time.time() - self.start_time) * 1000)

    def build_payload(self, status: str) -> str:
        doc = {
            "status":    status,
            "device_id": self.device_id,
            "uptime_ms": self.uptime_ms(),
        }
        return json.dumps(doc)


# ─── Callbacks MQTT ──────────────────────────────────────────────────────────

def on_connect(client, userdata: DeviceState, flags, rc, properties=None):
    if rc == 0:
        userdata.connected = True
        print(f"[MQTT] Conectado ao broker.")
        _publish(client, userdata, "online")
    else:
        codes = {
            1: "versão de protocolo recusada",
            2: "identificador rejeitado",
            3: "servidor indisponível",
            4: "usuário/senha inválidos",
            5: "não autorizado",
        }
        print(f"[MQTT] Falha na conexão: {codes.get(rc, f'rc={rc}')}")


def on_disconnect(client, userdata: DeviceState, rc, properties=None):
    userdata.connected = False
    if rc != 0:
        print(f"[MQTT] Desconectado inesperadamente (rc={rc}). Reconectando...")


def on_publish(client, userdata, mid, properties=None):
    pass   # confirmação silenciosa; o log fica em _publish()


# ─── Helpers de publicação ────────────────────────────────────────────────────

def _publish(client: mqtt.Client, state: DeviceState, status: str, retained: bool = False):
    payload = state.build_payload(status)
    result  = client.publish(DEFAULT_TOPIC, payload, qos=1, retain=retained)

    color = {"online": "\033[92m", "alert": "\033[91m", "offline": "\033[90m"}
    reset = "\033[0m"
    tag   = color.get(status, "") + status.upper() + reset

    print(f"[Simulador] [{tag}] {payload}")
    return result


# ─── Threads de suporte ───────────────────────────────────────────────────────

def heartbeat_thread(client: mqtt.Client, state: DeviceState):
    """Envia heartbeat periódico, igual ao firmware."""
    while True:
        time.sleep(HEARTBEAT_INTERVAL)
        if state.connected:
            _publish(client, state, "online")


def panic_button_thread(client: mqtt.Client, state: DeviceState):
    """
    Simula o botão de pânico via ENTER.
    Comportamento espelhado ao firmware:
      1. Publica 'alert' (retained=True)
      2. Aguarda 3 segundos (LED vermelho no firmware)
      3. Publica 'online'
    """
    print("\n" + "─" * 52)
    print("  Pressione [ENTER] para simular o botão de pânico")
    print("  Pressione [Ctrl+C] para encerrar")
    print("─" * 52 + "\n")

    try:
        while True:
            input()
            if not state.connected:
                print("[Simulador] Aguardando conexão com o broker...")
                continue

            print("[Simulador] *** BOTÃO DE PÂNICO PRESSIONADO! ***")
            _publish(client, state, "alert", retained=True)
            time.sleep(3)
            _publish(client, state, "online")

    except (KeyboardInterrupt, EOFError):
        pass


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Simulador ESP32 — Pulseira Idoso")
    parser.add_argument("--broker", default=DEFAULT_BROKER, help="IP do broker MQTT")
    parser.add_argument("--port",   default=DEFAULT_PORT,   type=int)
    parser.add_argument("--device", default=DEFAULT_DEVICE, help="ID do dispositivo")
    args = parser.parse_args()

    # Atualiza tópico global se necessário
    global DEFAULT_TOPIC
    DEFAULT_TOPIC = "elderly/alerts"

    state  = DeviceState(args.device)
    client = mqtt.Client(
        client_id          = args.device,
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    )
    client.user_data_set(state)
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_publish    = on_publish

    # LWT (Last Will Testament) — broker publica 'offline' se a conexão cair
    lwt = state.build_payload("offline")
    client.will_set(DEFAULT_TOPIC, lwt, qos=1, retain=True)

    print("=" * 52)
    print("  ESP32 Pulseira Idoso — Simulador MQTT")
    print("=" * 52)
    print(f"  Broker : {args.broker}:{args.port}")
    print(f"  Tópico : {DEFAULT_TOPIC}")
    print(f"  Device : {args.device}")
    print("=" * 52)

    try:
        client.connect(args.broker, args.port, keepalive=60)
    except (ConnectionRefusedError, OSError) as exc:
        print(f"\n[Erro] Não foi possível conectar ao broker: {exc}")
        print("[Dica] Verifique se o Mosquitto está rodando:")
        print("       Windows : net start mosquitto")
        print("       Linux   : sudo systemctl start mosquitto")
        sys.exit(1)

    client.loop_start()

    # Thread de heartbeat (daemon — encerra junto com o processo)
    t_hb = threading.Thread(target=heartbeat_thread, args=(client, state), daemon=True)
    t_hb.start()

    try:
        panic_button_thread(client, state)
    except KeyboardInterrupt:
        pass

    print("\n[Simulador] Encerrando...")
    if state.connected:
        _publish(client, state, "offline")
        time.sleep(0.5)

    client.loop_stop()
    client.disconnect()
    print("[Simulador] Desconectado.")


if __name__ == "__main__":
    main()
