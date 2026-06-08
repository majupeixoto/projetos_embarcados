#!/usr/bin/env python3
"""
Simulador do ESP32 — Pulseira de Assistência ao Idoso
======================================================
Simula o firmware completo com máquina de estados de 5 fases:
  Queda Livre → Impacto → Imobilidade → Verificação de Postura → Pré-Alerta

Novidades:
  f + ENTER  → Queda completa com janela de cancelamento de 15 s
  c + ENTER  → Cancela o pré-alerta (simula o botão físico do idoso)
  s + ENTER  → Simula sentar/abaixar (mostra por que NÃO dispara alarme)

Dependência:  pip install paho-mqtt
Uso:          python mock_esp32.py
              python mock_esp32.py --broker 192.168.1.100 --device esp32_02
              python mock_esp32.py --auto-fall 60
"""

import argparse
import ctypes
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
TOPIC_SAMPLES  = "elderly/samples"
TOPIC_BENCH    = "elderly/benchmark"

# ─── Benchmark computado em tempo real ───────────────────────────────────────
#
# N escolhidos para o ESP8266 (80KB RAM):
#   N=100  →   400 bytes de heap consumidos pelo V1
#   N=500  → 2.000 bytes de heap consumidos pelo V1
#   N=2000 → 8.000 bytes de heap consumidos pelo V1  ← limite seguro
#
# V1: ctypes.memmove real — cópia física de memória em C, O(n)
# V2: aritmética de ponteiro  head = (head+1) % N    — O(1)
#
# Os tempos são medidos no PC, mas a proporção V1/V2 demonstra o mesmo
# comportamento assintótico que o ESP8266 exibiria no hardware.
# Heap simulado: ~45 KB disponíveis no ESP8266 após inicialização do WiFi.

_BENCH_NS    = (100, 5000, 20000)
_BENCH_REPS  = 100
_ESP32_HEAP  = 298432   # bytes livres simulados (ESP32 após WiFi+MQTT)

# Fator de escala PC → ESP32
# PC roda ~3GHz, ESP32 @ 240MHz → operações de memória ~12.5x mais lentas
_SCALE = 3000 / 240   # ≈ 12.5

def _compute_benchmark_results():
    results    = []
    float_size = ctypes.sizeof(ctypes.c_float)

    for N in _BENCH_NS:
        heap_delta_v1 = N * float_size

        # ── V1: SlidingWindow — memmove real medido no PC, escalado pro ESP32
        arr_v1 = (ctypes.c_float * N)(*[i * 0.01 for i in range(N)])

        t0 = time.perf_counter()
        for i in range(_BENCH_REPS):
            ctypes.memmove(
                arr_v1,
                ctypes.addressof(arr_v1) + float_size,
                (N - 1) * float_size,
            )
            arr_v1[N - 1] = i * 0.01
        lat_pc_v1 = (time.perf_counter() - t0) / _BENCH_REPS * 1_000_000
        lat_v1    = max(1, int(lat_pc_v1 * _SCALE))

        results.append({
            "vertente": "V1", "n": N,
            "lat_us":    lat_v1,
            "heap_livre": _ESP32_HEAP - heap_delta_v1,
            "heap_delta": heap_delta_v1,
        })

        # ── V2: CircularBuffer — ponteiro circular medido no PC, escalado pro ESP32
        arr_v2 = (ctypes.c_float * N)(*[0.0] * N)
        head   = 0
        for i in range(N):      # pré-preenchimento (igual ao benchmark C++)
            arr_v2[head] = i * 0.01
            head = (head + 1) % N

        t0 = time.perf_counter()
        for i in range(_BENCH_REPS):
            arr_v2[head] = i * 0.01
            head = (head + 1) % N
        lat_pc_v2 = (time.perf_counter() - t0) / _BENCH_REPS * 1_000_000
        lat_v2    = max(1, int(lat_pc_v2 * _SCALE))

        results.append({
            "vertente": "V2", "n": N,
            "lat_us":    lat_v2,
            "heap_livre": _ESP32_HEAP,
            "heap_delta": 0,
        })

    return results

# ─── Thresholds espelhados do firmware (config.h) ────────────────────────────
# Mantidos sincronizados com config.h apenas para exibição nos logs.
FREEFALL_THRESHOLD_G = 0.50
IMPACT_THRESHOLD_G   = 2.80
FALL_WINDOW_MS       = 500
IMMOBILITY_THRESHOLD = 0.15
IMMOBILITY_DELAY_S   = 3.0
CANCEL_WINDOW_S      = 15

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
            doc["cause"] = cause
        if accel_g > 0:
            doc["accel_g"] = round(accel_g, 2)
        return json.dumps(doc)

# ─── Cores ANSI ──────────────────────────────────────────────────────────────

class C:
    RED    = "\033[91m"
    GREEN  = "\033[92m"
    YELLOW = "\033[93m"
    BLUE   = "\033[94m"
    CYAN   = "\033[96m"
    GRAY   = "\033[90m"
    BOLD   = "\033[1m"
    RESET  = "\033[0m"

def colored(text: str, *codes: str) -> str:
    return "".join(codes) + text + C.RESET

# ─── Estado global do pré-alerta ─────────────────────────────────────────────
# Compartilhado entre a thread da queda e o loop de comandos principal.

_cancel_event     = threading.Event()   # sinalizado quando o usuário digita 'c'
_pre_alert_active = False               # True durante a janela de 15 s
_pre_alert_lock   = threading.Lock()    # protege _pre_alert_active
_fall_in_progress = threading.Event()   # evita quedas simultâneas no auto-fall

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
    userdata.connected = False
    if reason_code != 0:
        print(colored(f"[MQTT] Desconectado inesperadamente (rc={reason_code}).", C.YELLOW))

def on_publish(client, userdata, mid, reason_code, properties):
    pass

# ─── Funções de publicação ────────────────────────────────────────────────────

def _log_publish(payload: str, label: str, *colors: str):
    ts = time.strftime("%H:%M:%S")
    print(f"  {C.GRAY}[{ts}]{C.RESET} {colored(label, *colors)}  {C.GRAY}{payload}{C.RESET}")

def _publish_raw(client: mqtt.Client, topic: str, payload: str, retained: bool):
    client.publish(topic, payload, qos=1, retain=retained)

def _publish_online(client: mqtt.Client, state: DeviceState):
    payload = state.build_payload("online", cause="online")
    _publish_raw(client, DEFAULT_TOPIC, payload, retained=False)
    _log_publish(payload, "ONLINE", C.GREEN)

def _publish_button(client: mqtt.Client, state: DeviceState):
    payload = state.build_payload("alert", cause="manual", accel_g=0.0)
    _publish_raw(client, DEFAULT_TOPIC, payload, retained=True)
    _log_publish(payload, "ALERTA — BOTÃO DE PÂNICO", C.RED, C.BOLD)

def _publish_pre_alert(client: mqtt.Client, state: DeviceState, accel_g: float):
    payload = state.build_payload("pre_alert", cause="fall", accel_g=accel_g)
    _publish_raw(client, DEFAULT_TOPIC, payload, retained=False)
    _log_publish(payload, f"PRÉ-ALERTA publicado — impacto={accel_g:.2f}g (dashboard notificado)", C.YELLOW)

def _publish_fall(client: mqtt.Client, state: DeviceState, accel_g: float):
    payload = state.build_payload("alert", cause="fall", accel_g=accel_g)
    _publish_raw(client, DEFAULT_TOPIC, payload, retained=True)
    _log_publish(payload, f"ALERTA — QUEDA CONFIRMADA ({accel_g:.2f} g)", C.RED, C.BOLD)

# ─── Simulação física ─────────────────────────────────────────────────────────

def simulate_fall_physics() -> tuple[float, float]:
    """Gera parâmetros físicos realistas para a queda."""
    freefall_duration = random.uniform(0.10, 0.35)
    impact_g          = random.uniform(IMPACT_THRESHOLD_G + 0.3, 7.5)
    return freefall_duration, impact_g

# ─── Sequência de queda (implementação bloqueante) ───────────────────────────

def _fall_sequence_impl(client: mqtt.Client, state: DeviceState):
    """
    Executa as 5 fases da detecção de queda com logs detalhados.
    Bloqueante — destinada a rodar em sua própria thread ou no auto-fall thread.
    """
    global _pre_alert_active

    _fall_in_progress.set()
    try:
        freefall_s, impact_g = simulate_fall_physics()

        # ── Fase 1: Queda Livre ───────────────────────────────────────────
        mag_ff = random.uniform(0.02, FREEFALL_THRESHOLD_G - 0.05)
        print(f"\n  {colored('↓ QUEDA LIVRE', C.YELLOW, C.BOLD)}"
              f"    mag={mag_ff:.2f}g  (threshold: <{FREEFALL_THRESHOLD_G}g)"
              f"  duração≈{freefall_s*1000:.0f}ms")
        time.sleep(freefall_s)

        # ── Fase 2: Impacto ───────────────────────────────────────────────
        print(f"  {colored('💥 IMPACTO', C.RED, C.BOLD)}"
              f"         mag={impact_g:.2f}g  (threshold: >{IMPACT_THRESHOLD_G}g)")
        time.sleep(0.3)

        # ── Fase 3: Imobilidade ───────────────────────────────────────────
        print(f"  {colored('⏳ IMOBILIDADE', C.BLUE)}"
              f"     aguardando {IMMOBILITY_DELAY_S:.0f}s para o corpo sossegar...")
        time.sleep(IMMOBILITY_DELAY_S)

        # ── Fase 4: Verificação de Postura (produto escalar) ──────────────
        # Simula vetor após queda (horizontal) vs. vetor estável (vertical)
        simulated_cosine = random.uniform(0.20, 0.65)   # ângulo > 45°
        simulated_var    = random.uniform(0.02, IMMOBILITY_THRESHOLD - 0.02)
        print(f"  {colored('📐 POSTURA', C.CYAN)}"
              f"          cosθ={simulated_cosine:.3f}"
              f"  (< 0.707 → ângulo > 45°)   var={simulated_var:.2f}g"
              f"  {colored('→ QUEDA VALIDADA', C.YELLOW, C.BOLD)}")

        # ── Pré-Alerta: janela de cancelamento ────────────────────────────
        _cancel_event.clear()
        with _pre_alert_lock:
            _pre_alert_active = True

        _publish_pre_alert(client, state, impact_g)

        print(colored(f"\n  ⚠  PRÉ-ALERTA ATIVO — impacto={impact_g:.2f}g", C.YELLOW, C.BOLD))
        print(colored(f"     [LED AMARELO PISCANDO] Digite 'c' + Enter para CANCELAR.\n",
                      C.YELLOW))

        cancelled = False
        for remaining in range(CANCEL_WINDOW_S, 0, -1):
            print(f"\r  ⏱  {remaining:2d}s restantes...", end="", flush=True)
            if _cancel_event.wait(timeout=1.0):
                cancelled = True
                break

        print()  # newline após o countdown

        with _pre_alert_lock:
            _pre_alert_active = False

        if cancelled:
            print(colored("  ✓ Alarme CANCELADO — falso positivo confirmado.", C.GREEN, C.BOLD))
            print(colored("    [LED VERDE] Monitoramento retomado normalmente.\n", C.GREEN))
        else:
            print(colored("  ⏱ Tempo esgotado. Enviando QUEDA CONFIRMADA ao MQTT.", C.RED, C.BOLD))
            print(colored("    [LED VERMELHO 3s]\n", C.RED))
            _publish_fall(client, state, impact_g)
            time.sleep(3)
            _publish_online(client, state)
            print()

    finally:
        _fall_in_progress.clear()


def run_fall_sequence(client: mqtt.Client, state: DeviceState, threaded: bool = True):
    """
    Inicia a sequência de queda.
    threaded=True  → background (não bloqueia o loop de comandos).
    threaded=False → bloqueante (usado pelo auto-fall thread).
    """
    if not state.connected:
        print(colored("  [!] Aguardando conexão com o broker...", C.YELLOW))
        return
    if _fall_in_progress.is_set():
        print(colored("  [!] Sequência de queda já em andamento.", C.GRAY))
        return
    if threaded:
        threading.Thread(target=_fall_sequence_impl, args=(client, state),
                         daemon=True).start()
    else:
        _fall_sequence_impl(client, state)

# ─── Simulação de sentar/abaixar ─────────────────────────────────────────────

def simulate_sit_down():
    """
    Exibe o perfil de aceleração de um sentar/abaixar gradual.
    NÃO publica no MQTT — demonstra por que o firmware descarta este evento.
    """
    print(f"\n  {colored('🪑 SENTAR / ABAIXAR (simulação de sensor)', C.BLUE, C.BOLD)}")
    print(f"  {'─' * 54}")

    phases = [
        (0.15, random.uniform(1.05, 1.15), "Início do movimento — magnitude estável (~1.0g)"),
        (0.20, random.uniform(1.20, 1.40), "Flexão do joelho — leve elevação"),
        (0.25, random.uniform(0.80, 1.00), "Descida controlada — redução suave"),
        (0.15, random.uniform(1.50, 1.78), "Assentamento — pico suave (< 2.8g)"),
        (0.20, random.uniform(0.95, 1.05), "Postura sentada — magnitude estabiliza em ~1.0g"),
    ]

    for duration, mag, desc in phases:
        bar_len  = int(mag * 10)
        bar_fill = "█" * bar_len
        color    = C.YELLOW if mag > 1.3 else C.GREEN
        print(f"  {colored(f'{mag:.2f}g', color)} [{bar_fill:<20}] {C.GRAY}{desc}{C.RESET}")
        time.sleep(duration)

    print(f"\n  {colored('✓ Resultado no firmware:', C.GREEN, C.BOLD)}")
    print(f"    • Sem queda livre  (mag nunca caiu abaixo de {FREEFALL_THRESHOLD_G}g)")
    print(f"    • Sem impacto alto (mag nunca superou {IMPACT_THRESHOLD_G}g)")
    print(f"    • Estado permanece em IDLE — nenhum alerta disparado\n")

# ─── Threads de suporte ───────────────────────────────────────────────────────

def telem_thread(client: mqtt.Client, state: DeviceState):
    """Simula leituras do acelerômetro MPU-6500: ~1g em repouso, queda visível."""
    seq      = 0
    produced = 0
    BUF_CAP  = 64
    while True:
        if state.connected:
            produced += 1
            buf_size  = min(produced - seq, BUF_CAP)

            # Fase da sequência de queda afeta o valor simulado
            if _fall_in_progress.is_set():
                with _pre_alert_lock:
                    in_pre = _pre_alert_active
                if in_pre:
                    value = round(random.uniform(0.05, 0.25), 2)   # queda livre
                else:
                    value = round(random.uniform(2.5, 4.5), 2)     # impacto
            else:
                # Repouso: ~1g + pequena oscilação (respiração/movimento)
                value = round(1.0 + 0.12 * math.sin(seq * 0.18)
                              + random.uniform(-0.04, 0.04), 2)

            payload = json.dumps({
                "seq":      seq,
                "value":    value,
                "buf_size": buf_size,
                "buf_cap":  BUF_CAP,
                "produced": produced,
            })
            client.publish(TOPIC_SAMPLES, payload, qos=0)
            seq += 1
        time.sleep(0.2)

def _publish_benchmark(client: mqtt.Client, state: DeviceState):
    """Computa e publica os resultados do benchmark para o dashboard."""
    if not state.connected:
        print(colored("  [!] Sem conexão com o broker.", C.YELLOW))
        return
    print(colored("\n  [BENCHMARK] Calculando V1 vs V2 (memmove real)...", C.CYAN))
    results = _compute_benchmark_results()
    for result in results:
        v      = result["vertente"]
        n      = result["n"]
        lat    = result["lat_us"]
        color  = C.RED if v == "V1" else C.BLUE
        print(colored(f"  [{v}] N={n:>5}  →  Latência: {lat} µs  |  "
                      f"Heap delta: {result['heap_delta']} bytes", color))
        client.publish(TOPIC_BENCH, json.dumps(result), qos=1)
        time.sleep(0.1)
    print(colored("  [BENCHMARK] Concluído. Verifique o gráfico no dashboard.\n", C.GREEN))

def auto_fall_thread(client: mqtt.Client, state: DeviceState, interval_s: int):
    """
    Gera quedas automáticas para stress test do dashboard.
    Usa threaded=False para aguardar cada sequência completa (incluindo os 15s
    de pré-alerta) antes de agendar a próxima queda.
    """
    print(colored(f"  [AUTO-FALL] Ativo — queda a cada ~{interval_s}s", C.YELLOW))
    while True:
        jitter = interval_s * random.uniform(0.8, 1.2)
        time.sleep(jitter)
        if state.connected and not _fall_in_progress.is_set():
            print(colored("\n  [AUTO-FALL] Disparando queda automática...", C.YELLOW))
            run_fall_sequence(client, state, threaded=False)

# ─── Loop de comandos (thread principal) ─────────────────────────────────────

MENU = f"""
  {C.BOLD}Comandos disponíveis:{C.RESET}
  {C.BOLD}[ENTER]{C.RESET}    → Botão de pânico (alerta imediato — cause: manual)
  {C.BOLD}f[ENTER]{C.RESET}   → Simular queda completa (4 fases + pré-alerta de 15s)
  {C.BOLD}c[ENTER]{C.RESET}   → Cancelar pré-alerta ativo (simula botão do firmware)
  {C.BOLD}s[ENTER]{C.RESET}   → Simular sentar/abaixar (mostra por que não dispara alarme)
  {C.BOLD}b[ENTER]{C.RESET}   → Publicar resultados do benchmark no dashboard
  {C.BOLD}q[ENTER]{C.RESET}   → Sair
"""

def command_loop(client: mqtt.Client, state: DeviceState):
    print(MENU)
    try:
        while True:
            cmd = input("  > ").strip().lower()

            if cmd == "":   # ENTER = botão de pânico imediato (sem pré-alerta)
                if not state.connected:
                    print(colored("  [!] Sem conexão com o broker.", C.YELLOW))
                    continue
                print(colored("\n  *** BOTÃO DE PÂNICO PRESSIONADO! ***\n", C.RED, C.BOLD))
                _publish_button(client, state)
                time.sleep(3)
                _publish_online(client, state)
                print()

            elif cmd == "f":   # queda completa com pré-alerta
                run_fall_sequence(client, state, threaded=True)

            elif cmd == "c":   # cancela o pré-alerta ativo
                with _pre_alert_lock:
                    active = _pre_alert_active
                if active:
                    _cancel_event.set()
                    print(colored("  [Botão físico] Sinal de cancelamento enviado.", C.GREEN))
                else:
                    print(colored("  [!] Nenhum pré-alerta ativo para cancelar.", C.GRAY))

            elif cmd == "s":   # simula sentar
                simulate_sit_down()

            elif cmd == "b":   # publica benchmark
                _publish_benchmark(client, state)

            elif cmd == "q":   # sair
                break

            else:
                print(colored("  Comando desconhecido. Use ENTER, f, c, s, b ou q.", C.GRAY))

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

    lwt = state.build_payload("offline", cause="lwt")
    client.will_set(DEFAULT_TOPIC, lwt, qos=1, retain=True)

    print(f"\n{C.BOLD}{'═'*54}{C.RESET}")
    print(f"  {C.BOLD}VitaLink — Simulador ESP32 + MPU-6500{C.RESET}")
    print(f"{'═'*54}")
    print(f"  Broker   : {args.broker}:{args.port}")
    print(f"  Tópico   : {DEFAULT_TOPIC}")
    print(f"  Device   : {args.device}")
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
