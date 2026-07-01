import asyncio, base64, json, sqlite3, datetime, threading, requests
import paho.mqtt.client as mqtt_client
import firebase_admin
from firebase_admin import credentials, messaging
from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse
from PIL import Image
import io

app = FastAPI(title="NyloNode Backend")

# ── Firebase ──────────────────────────────────────────────
cred = credentials.Certificate("serviceAccountKey.json")
firebase_admin.initialize_app(cred)

# ── Base de datos ─────────────────────────────────────────
def init_db():
    conn = sqlite3.connect("nylonode.db")
    conn.execute("""CREATE TABLE IF NOT EXISTS eventos (
        id        INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp TEXT,
        magnitud  REAL,
        tipo      TEXT,
        confianza REAL,
        imagen    TEXT,
        usuario   TEXT
    )""")
    conn.execute("""CREATE TABLE IF NOT EXISTS usuarios (
        id        INTEGER PRIMARY KEY AUTOINCREMENT,
        email     TEXT UNIQUE,
        fcm_token TEXT,
        umbral    REAL DEFAULT 1.5
    )""")
    conn.commit()
    conn.close()

init_db()

# ── Enviar notificación push via Expo ─────────────────────
def enviar_push_expo(token: str, titulo: str, cuerpo: str):
    try:
        # Si es token nativo de Apple (APNs) — usar Firebase
        if not token.startswith('ExponentPushToken'):
            mensaje = messaging.Message(
                notification=messaging.Notification(
                    title=titulo,
                    body=cuerpo
                ),
                apns=messaging.APNSConfig(
                    payload=messaging.APNSPayload(
                        aps=messaging.Aps(
                            sound='default',
                            badge=1
                        )
                    )
                ),
                token=token,
            )
            response = messaging.send(mensaje)
            print(f"Push Firebase enviado: {response}")
        else:
            # Token Expo — usar servicio Expo
            response = requests.post(
                'https://exp.host/--/api/v2/push/send',
                json={
                    'to':       token,
                    'title':    titulo,
                    'body':     cuerpo,
                    'sound':    'default',
                    'priority': 'high',
                },
                headers={'Content-Type': 'application/json'}
            )
            print(f"Push Expo enviado: {response.json()}")
    except Exception as e:
        print(f"Error enviando push: {e}")

# ── Guardar evento ────────────────────────────────────────
def guardar_evento(magnitud, tipo, confianza, imagen_b64, usuario):
    conn = sqlite3.connect("nylonode.db")
    conn.execute(
        "INSERT INTO eventos (timestamp,magnitud,tipo,confianza,imagen,usuario) VALUES (?,?,?,?,?,?)",
        (datetime.datetime.now().isoformat(), magnitud, tipo, confianza, imagen_b64, usuario)
    )
    conn.commit()
    conn.close()

# ── Procesar alerta del ESP32 ─────────────────────────────
def procesar_alerta_esp32(data: dict):
    magnitud  = data.get("magnitud", 0)
    tipo      = data.get("tipo", "no_natural")
    confianza = data.get("confianza", 0.8)

    guardar_evento(magnitud, tipo, confianza, "", "usuario_1")
    print(f"Evento guardado: tipo={tipo}, magnitud={magnitud}g")

    # Obtener token del usuario
    conn  = sqlite3.connect("nylonode.db")
    fila  = conn.execute(
        "SELECT fcm_token FROM usuarios WHERE id=1"
    ).fetchone()
    conn.close()

    if not fila or not fila[0]:
        print("No hay token registrado — abre la app primero")
        return

    token = fila[0]

    # Enviar notificación siempre — natural o no natural
    if tipo == "no_natural":
        titulo = "⚠️ Alerta NyloNode"
        cuerpo = f"Movimiento no natural detectado ({magnitud:.1f}g)"
    else:
        titulo = "✓ NyloNode — Sin riesgo"
        cuerpo = f"Movimiento natural detectado ({magnitud:.1f}g)"

    enviar_push_expo(token, titulo, cuerpo)

# ── MQTT ──────────────────────────────────────────────────
imagen_buffer  = bytearray()
imagen_esperada = 0

def on_mqtt_message(client, userdata, msg):
    global imagen_buffer, imagen_esperada
    topic = msg.topic
    try:
        if topic == "nylonode/alerta":
            data = json.loads(msg.payload.decode())
            print(f"Alerta MQTT recibida: {data}")
            procesar_alerta_esp32(data)

        elif topic == "nylonode/imagen/meta":
            meta = json.loads(msg.payload.decode())
            imagen_esperada = meta["bytes"]
            imagen_buffer   = bytearray()
            print(f"Esperando imagen: {imagen_esperada} bytes")

        elif topic == "nylonode/imagen/data":
            imagen_buffer.extend(msg.payload)
            if len(imagen_buffer) >= imagen_esperada and imagen_esperada > 0:
                print(f"Imagen recibida completa: {len(imagen_buffer)} bytes")
                imagen_buffer   = bytearray()
                imagen_esperada = 0

    except Exception as e:
        print(f"Error procesando mensaje MQTT: {e}")

def iniciar_mqtt():
    client = mqtt_client.Client()
    client.on_message = on_mqtt_message
    print("Conectando a HiveMQ...")
    client.connect("broker.hivemq.com", 1883)
    client.subscribe([
        ("nylonode/alerta",      0),
        ("nylonode/imagen/meta", 0),
        ("nylonode/imagen/data", 0),
    ])
    print("MQTT conectado y suscrito ✓")
    client.loop_forever()

threading.Thread(target=iniciar_mqtt, daemon=True).start()

# ── Endpoints API ─────────────────────────────────────────
@app.get("/estado")
def estado():
    return {"status": "activo", "version": "1.0", "broker": "hivemq"}

@app.get("/historial/{usuario_id}")
def historial(usuario_id: str, limite: int = 20):
    conn = sqlite3.connect("nylonode.db")
    rows = conn.execute(
        "SELECT timestamp, magnitud, tipo, confianza FROM eventos WHERE usuario=? ORDER BY id DESC LIMIT ?",
        (usuario_id, limite)
    ).fetchall()
    conn.close()
    return [{"timestamp": r[0], "magnitud": r[1], "tipo": r[2], "confianza": r[3]} for r in rows]

@app.post("/registro")
def registrar_usuario(email: str, fcm_token: str):
    conn = sqlite3.connect("nylonode.db")
    try:
        conn.execute(
            "INSERT OR REPLACE INTO usuarios (email, fcm_token) VALUES (?,?)",
            (email, fcm_token)
        )
        conn.commit()
        print(f"Token registrado: {fcm_token[:30]}...")
    finally:
        conn.close()
    return {"status": "ok", "mensaje": "Usuario registrado"}

@app.put("/umbral/{usuario_id}")
def actualizar_umbral(usuario_id: int, umbral: float):
    if not 0.5 <= umbral <= 4.0:
        raise HTTPException(400, "Umbral debe estar entre 0.5g y 4.0g")
    conn = sqlite3.connect("nylonode.db")
    conn.execute("UPDATE usuarios SET umbral=? WHERE id=?", (umbral, usuario_id))
    conn.commit()
    conn.close()
    return {"status": "ok", "umbral": umbral}

@app.get("/ultimo-evento/{usuario_id}")
def ultimo_evento(usuario_id: str):
    conn = sqlite3.connect("nylonode.db")
    row = conn.execute(
        "SELECT timestamp, magnitud, tipo FROM eventos WHERE usuario=? ORDER BY id DESC LIMIT 1",
        (usuario_id,)
    ).fetchone()
    conn.close()
    if not row:
        return {"mensaje": "Sin eventos"}
    return {"timestamp": row[0], "magnitud": row[1], "tipo": row[2]}

@app.post("/test-notificacion")
def test_notificacion():
    """Endpoint para probar notificaciones sin el ESP32"""
    conn  = sqlite3.connect("nylonode.db")
    fila  = conn.execute("SELECT fcm_token FROM usuarios WHERE id=1").fetchone()
    conn.close()

    if not fila or not fila[0]:
        return {"error": "No hay token — abre la app primero"}

    enviar_push_expo(
        fila[0],
        "⚠️ Alerta NyloNode",
        "Prueba de notificación — sistema funcionando"
    )
    return {"status": "ok", "mensaje": "Notificación enviada"}

@app.post("/simular-alerta")
def simular_alerta(tipo: str = "no_natural", magnitud: float = 2.5):
    """Simula una alerta del ESP32 — para probar sin hardware"""
    procesar_alerta_esp32({
        "magnitud":  magnitud,
        "tipo":      tipo,
        "confianza": 0.87
    })
    return {"status": "ok", "tipo": tipo, "magnitud": magnitud}