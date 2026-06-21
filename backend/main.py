# backend/main.py
import asyncio, base64, json, sqlite3, datetime, threading
import paho.mqtt.client as mqtt_client
import firebase_admin
from firebase_admin import credentials, messaging
from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse
from PIL import Image
import io

app = FastAPI(title="NyloNode Backend")

# ── Firebase ──────────────────────────────────────────────
# firebase se encarga de enviar notificaciones push a los usuarios cuando se detecta un evento de movimiento no natural. Se inicializa con las credenciales del servicio para poder usar la API de FCM (Firebase Cloud Messaging)
cred = credentials.Certificate("serviceAccountKey.json")
firebase_admin.initialize_app(cred)

# ── Base de datos ─────────────────────────────────────────
def init_db():
    conn = sqlite3.connect("nylonode.db") # crea o abre la base de datos SQLite local llamada "nylonode.db"
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

# ── Clasificador de movimiento ────────────────────────────
def clasificar_imagen(imagen_bytes: bytes) -> dict:
    """
    En producción: integrar YOLOv8 nano o TFLite.
    Por ahora: lógica por tamaño de objeto en imagen.
    
    Para integrar YOLO real:
        from ultralytics import YOLO
        model = YOLO("yolov8n.pt")
        results = model(imagen_bytes)
        # buscar clases: person, cat, dog, bird
    """
    try:
        img = Image.open(io.BytesIO(imagen_bytes)).convert("RGB")
        w, h = img.size
        # Análisis básico de movimiento por diferencia de brillo
        pixels = list(img.getdata())
        brillo_promedio = sum(sum(p) for p in pixels) / (len(pixels) * 3)
        
        # Umbral provisional — reemplazar con modelo ML real
        if brillo_promedio < 80 or brillo_promedio > 200:
            return {"tipo": "no_natural", "confianza": 0.75,
                    "descripcion": "Objeto detectado en la malla"}
        return {"tipo": "natural", "confianza": 0.82,
                "descripcion": "Movimiento ambiental (viento)"}
    except Exception as e:
        return {"tipo": "no_natural", "confianza": 0.5,
                "descripcion": "No se pudo analizar la imagen"}

def enviar_push(fcm_token: str, tipo: str, descripcion: str, magnitud: float):
    try:
        titulo = "⚠️ Alerta NyloNode" if tipo == "no_natural" else "NyloNode — Sin riesgo"
        cuerpo = f"{descripcion} (magnitud: {magnitud:.1f}g)"
        mensaje = messaging.Message(
            notification=messaging.Notification(title=titulo, body=cuerpo),
            data={"tipo": tipo, "magnitud": str(magnitud)},
            android=messaging.AndroidConfig(
                priority="high",
                notification=messaging.AndroidNotification(sound="default")
            ),
            apns=messaging.APNSConfig(
                payload=messaging.APNSPayload(
                    aps=messaging.Aps(sound="default", badge=1)
                )
            ),
            token=fcm_token,
        )
        messaging.send(mensaje)
        print(f"Push enviado: {titulo}")
    except Exception as e:
        print(f"Error enviando push: {e}")

def guardar_evento(magnitud, tipo, confianza, imagen_b64, usuario):
    conn = sqlite3.connect("nylonode.db")
    conn.execute(
        "INSERT INTO eventos (timestamp,magnitud,tipo,confianza,imagen,usuario) VALUES (?,?,?,?,?,?)",
        (datetime.datetime.now().isoformat(), magnitud, tipo, confianza, imagen_b64, usuario)
    )
    conn.commit()
    conn.close()

# ── MQTT: recibir datos del ESP32 ─────────────────────────
imagen_buffer = bytearray()
imagen_esperada = 0

def on_mqtt_message(client, userdata, msg):
    global imagen_buffer, imagen_esperada
    topic = msg.topic

    if topic == "nylonode/alerta":
        data = json.loads(msg.payload.decode())
        magnitud = data.get("magnitud", 0)
        print(f"Evento recibido: magnitud={magnitud}g")

    elif topic == "nylonode/imagen/meta":
        meta = json.loads(msg.payload.decode())
        imagen_esperada = meta["bytes"]
        imagen_buffer   = bytearray()
        print(f"Esperando imagen: {imagen_esperada} bytes")

    elif topic == "nylonode/imagen/data":
        imagen_buffer.extend(msg.payload)
        if len(imagen_buffer) >= imagen_esperada and imagen_esperada > 0:
            procesar_imagen_completa(bytes(imagen_buffer))
            imagen_buffer   = bytearray()
            imagen_esperada = 0

def procesar_imagen_completa(imagen_bytes):
    resultado  = clasificar_imagen(imagen_bytes)
    imagen_b64 = base64.b64encode(imagen_bytes).decode()
    magnitud   = 0  # vendría del mensaje de alerta previo

    guardar_evento(magnitud, resultado["tipo"],
                   resultado["confianza"], imagen_b64, "usuario_1")

    # Obtener token FCM del usuario
    conn  = sqlite3.connect("nylonode.db")
    fila  = conn.execute(
        "SELECT fcm_token FROM usuarios WHERE id=1"
    ).fetchone()
    conn.close()

    if fila and fila[0] and resultado["tipo"] == "no_natural":
        enviar_push(fila[0], resultado["tipo"],
                    resultado["descripcion"], magnitud)

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

# Arranca MQTT en hilo separado al iniciar FastAPI
threading.Thread(target=iniciar_mqtt, daemon=True).start()

# ── API REST para la app ──────────────────────────────────
@app.get("/estado")
def estado():
    return {"status": "activo", "version": "1.0", "broker": "conectado"}

@app.get("/historial/{usuario_id}")
def historial(usuario_id: str, limite: int = 20):
    conn = sqlite3.connect("nylonode.db")
    rows = conn.execute(
        """SELECT timestamp, magnitud, tipo, confianza
           FROM eventos WHERE usuario=? ORDER BY id DESC LIMIT ?""",
        (usuario_id, limite)
    ).fetchall()
    conn.close()
    return [{"timestamp": r[0], "magnitud": r[1],
             "tipo": r[2], "confianza": r[3]} for r in rows]

@app.post("/registro")
def registrar_usuario(email: str, fcm_token: str):
    conn = sqlite3.connect("nylonode.db")
    try:
        conn.execute(
            "INSERT OR REPLACE INTO usuarios (email, fcm_token) VALUES (?,?)",
            (email, fcm_token)
        )
        conn.commit()
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
    # Publicar nuevo umbral al dispositivo vía MQTT
    # mqtt_client.publish("nylonode/config", json.dumps({"umbral": umbral}))
    return {"status": "ok", "umbral": umbral}

@app.get("/ultimo-evento/{usuario_id}")
def ultimo_evento(usuario_id: str):
    conn = sqlite3.connect("nylonode.db")
    row = conn.execute(
        "SELECT timestamp, magnitud, tipo, imagen FROM eventos WHERE usuario=? ORDER BY id DESC LIMIT 1",
        (usuario_id,)
    ).fetchone()
    conn.close()
    if not row:
        return {"mensaje": "Sin eventos"}
    return {"timestamp": row[0], "magnitud": row[1],
            "tipo": row[2], "tiene_imagen": bool(row[3])}