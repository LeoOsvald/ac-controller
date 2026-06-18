#!/usr/bin/env python3
import serial
import serial.tools.list_ports
import threading
import time
from datetime import datetime
from flask import Flask
from flask_socketio import SocketIO, emit

PORTA_ARDUINO = "/dev/ttyACM0" # Pode variar, a função detectar_arduino tenta encontrar
BAUD_RATE     = 9600

app      = Flask(__name__)
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

estado = {
    "temperatura": None, "umidade": None,
    "alunos": 0, "ventilador": False,
    "ultimo_uid": None, "temp_limite": 28.0,
}
serial_conn = None

def detectar_arduino():
    portas = serial.tools.list_ports.comports()
    for p in portas:
        if "ttyACM" in p.device or "ttyUSB" in p.device:
            print(f"[SERIAL] Arduino em {p.device}")
            return p.device
    return PORTA_ARDUINO

def verificar_e_controlar_ventilador():
    global estado, serial_conn
    if estado["temperatura"] is not None and estado["alunos"] is not None:
        temp_atual = estado["temperatura"]
        temp_limite = estado["temp_limite"]
        alunos_presentes = estado["alunos"]
        ventilador_ligado = estado["ventilador"]

        # Regra: Ligar se Temperatura > Limite E Alunos > 1
        deve_ligar = (temp_atual > temp_limite) and (alunos_presentes > 1)

        if deve_ligar and not ventilador_ligado:
            if serial_conn and serial_conn.is_open:
                serial_conn.write(b"VENTILADOR:LIGAR\n")
                print("[CONTROLE] Enviando comando: VENTILADOR:LIGAR")
        elif not deve_ligar and ventilador_ligado:
            if serial_conn and serial_conn.is_open:
                serial_conn.write(b"VENTILADOR:DESLIGAR\n")
                print("[CONTROLE] Enviando comando: VENTILADOR:DESLIGAR")

def parsear_linha(linha):
    global estado
    linha = linha.strip()
    if not linha: return None
    evento = {"tipo":"log","msg":linha,"ts":datetime.now().strftime("%H:%M:%S")}

    if linha.startswith("ENTRADA:"):
        uid = linha.split(":",1)[1]; estado["ultimo_uid"] = uid
        evento.update({"tipo":"entrada","uid":uid,"msg":f"Entrada — cartão {uid}"})
    elif linha.startswith("SAIDA:"):
        uid = linha.split(":",1)[1]; estado["ultimo_uid"] = uid
        evento.update({"tipo":"saida","uid":uid,"msg":f"Saída — cartão {uid}"})
    elif linha.startswith("ALUNOS:"):
        n = int(linha.split(":",1)[1]); estado["alunos"] = n
        evento.update({"tipo":"alunos","valor":n,"msg":f"Alunos presentes: {n}"})
    elif linha.startswith("TEMP:"):
        partes = linha.split(";")
        temp = float(partes[0].split(":",1)[1])
        umid = float(partes[1].split(":",1)[1]) if len(partes)>1 else None
        estado["temperatura"] = temp
        if umid is not None: estado["umidade"] = umid
        evento.update({"tipo":"temp","temp":temp,"umid":umid,
            "msg":f"Temp: {temp:.1f}°C"+(f" · Umidade: {umid:.1f}%" if umid else "")})
    elif linha == "VENTILADOR:LIGADO":
        estado["ventilador"] = True
        evento.update({"tipo":"ventilador","ligado":True,"msg":"Ventilador ligado"})
    elif linha == "VENTILADOR:DESLIGADO":
        estado["ventilador"] = False
        evento.update({"tipo":"ventilador","ligado":False,"msg":"Ventilador desligado"})
    elif linha.startswith("TEMP_LIMITE_ATUALIZADO:"):
        val = float(linha.split(":",1)[1]); estado["temp_limite"] = val
        evento.update({"tipo":"temp_limite","valor":val,"msg":f"Limite atualizado: {val:.1f}°C"})
    elif linha.startswith("TEMP_LIMITE:"):
        val = float(linha.split(":",1)[1]); estado["temp_limite"] = val
        evento.update({"tipo":"temp_limite","valor":val,"msg":f"Limite atual: {val:.1f}°C"})
    elif linha.startswith("CARTAO_INVALIDO:"):
        uid = linha.split(":",1)[1]
        evento.update({"tipo":"erro","msg":f"Cartão não cadastrado: {uid}"})
    elif linha.startswith("ERRO"):
        evento["tipo"] = "erro"

    evento["estado"] = dict(estado)
    return evento

def loop_serial():
    global serial_conn
    porta = detectar_arduino()
    while True:
        try:
            print(f"[SERIAL] Conectando em {porta}...")
            serial_conn = serial.Serial(porta, BAUD_RATE, timeout=2)
            time.sleep(2)
            print("[SERIAL] Conectado!")
            # Solicitar o limite de temperatura atual do Arduino ao conectar
            serial_conn.write(b"GET_TEMP_LIMITE\n")
            while True:
                linha = serial_conn.readline().decode("utf-8", errors="replace").strip()
                if linha:
                    print(f"[ARDUINO] {linha}")
                    evento = parsear_linha(linha)
                    if evento:
                        socketio.emit("evento", evento)
                        # Verificar e controlar o ventilador após cada atualização de estado
                        verificar_e_controlar_ventilador()
        except Exception as e:
            serial_conn = None
            print(f"[SERIAL] Erro: {e}. Reconectando em 5s...")
            time.sleep(5)

@app.route("/")
def index():
    with open("painel.html", "r", encoding="utf-8") as f:
        return f.read()

@socketio.on("connect")
def on_connect():
    print("[WS] Cliente conectado")
    emit("evento", {
        "tipo":"estado_inicial",
        "estado":dict(estado),
        "ts":datetime.now().strftime("%H:%M:%S")
    })

@socketio.on("settemp")
def on_settemp(data):
    global serial_conn
    try:
        val = float(data["valor"])
        if serial_conn:
            serial_conn.write(f"SETTEMP:{val:.1f}\n".encode())
            print(f"[SERIAL] Enviado: SETTEMP:{val:.1f}")
    except Exception as e:
        print(f"[WS] Erro settemp: {e}")

if __name__ == "__main__":
    threading.Thread(target=loop_serial, daemon=True).start()
    print("=" * 50)
    print("  Painel em http://localhost:8080")
    print("=" * 50)
    socketio.run(app, host="0.0.0.0", port=8765, debug=False, allow_unsafe_werkzeug=True)
