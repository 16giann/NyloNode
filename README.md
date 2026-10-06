# NyloNode

Sistema IoT de detección de movimiento no natural en mallas de balcón, diseñado para proteger a niños y mascotas en departamentos en altura.

Proyecto desarrollado para el curso **Proyecto en TICs I** — Universidad Diego Portales.

# Descripción

NyloNode detecta vibraciones anómalas en la malla de seguridad de un balcón mediante un acelerómetro, captura una imagen del evento, clasifica si el movimiento es natural (viento) o no natural (una persona o mascota tocando la malla) usando un modelo de **inteligencia artificial embebida (TinyML)**, y notifica al usuario en tiempo real a través de una aplicación móvil.

# Características

- Detección de movimiento en tiempo real (100 lecturas por segundo)
- Clasificación de imágenes con TinyML (Edge Impulse) directamente en el microcontrolador
- Captura automática de imagen ante cada evento detectado
- Notificaciones push al celular en menos de 5 segundos
- Aplicación móvil con historial de eventos, configuración de sensibilidad y visualización de la cámara
- Almacenamiento local en tarjeta SD como respaldo sin conexión
- Comunicación a través de internet (no depende de estar en la misma red WiFi)

# Arquitectura del sistema

```
┌──────────────┐     ┌───────────┐     ┌──────────────┐     ┌─────────────┐     ┌───────────┐
│  ADXL345     │────▶│ ESP32-CAM │────▶│   HiveMQ     │────▶│  Backend    │────▶│    App    │
│ (acelerómetro)│     │ + TinyML  │     │   (MQTT)     │     │  (FastAPI)  │     │  (Expo)   │
└──────────────┘     └───────────┘     └──────────────┘     └─────────────┘     └───────────┘
                                                                     │
                                                                     ▼
                                                              ┌─────────────┐
                                                              │   SQLite    │
                                                              └─────────────┘
```

# Stack tecnológico

| Componente | Tecnología |
|------------|-----------|
| Firmware | Arduino C++ |
| Hardware | ESP32-CAM (AI-Thinker) + Sensor ADXL345 |
| Inteligencia artificial | Edge Impulse (TinyML) |
| Comunicación | MQTT vía HiveMQ |
| Backend | Python + FastAPI |
| Base de datos | SQLite |
| Notificaciones | Firebase Cloud Messaging / Expo Notifications |
| Aplicación móvil | React Native + Expo |

# Estructura del proyecto

```
nylonode/
├── backend/
│   ├── main.py              # Servidor FastAPI, lógica MQTT y notificaciones
│   ├── nylonode.db          # Base de datos SQLite (se genera automáticamente)
│   └── serviceAccountKey.json
├── firmware/
│   └── nylonode_firmware_live.ino   # Firmware del ESP32-CAM
└── NyloNodeApp/
    └── src/app/index.tsx    # Aplicación móvil React Native
```

# Instalación y uso

# Requisitos previos

- [Python 3.9+](https://python.org)
- [Node.js (LTS)](https://nodejs.org)
- [Arduino IDE](https://arduino.cc/en/software) (solo para modificar el firmware)
- App **Expo Go** en tu celular (App Store / Play Store)

# 1. Clonar el repositorio

```bash
git clone https://github.com/16giann/NyloNode.git
cd NyloNode
```

# 2. Backend

```bash
cd backend
python3 -m venv venv
source venv/bin/activate
pip3 install fastapi uvicorn paho-mqtt firebase-admin pillow python-multipart requests

python3 -m uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

> Necesitas tu propio `serviceAccountKey.json` de Firebase para las notificaciones push. Colócalo en `backend/`.

# 3. Aplicación móvil

```bash
cd NyloNodeApp
npm install
npx expo start
```

Escanea el código QR con la app **Expo Go**.

# 4. Firmware del ESP32-CAM

1. Abre `firmware/nylonode_firmware_live.ino` en Arduino IDE
2. Instala las librerías: `WiFiManager`, `PubSubClient`, `Adafruit ADXL345`
3. Selecciona la placa **AI Thinker ESP32-CAM**
4. Sube el firmware (modo programación: mantén **FLASH**, presiona **RST**, suelta **FLASH**, luego sube)
5. Configura el WiFi conectándote a la red `NyloNode-Setup` (contraseña: `nylonode123`)

# Conexión de pines — ADXL345 al ESP32-CAM

| ADXL345 | ESP32-CAM |
|---------|-----------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 13 |
| SCL | GPIO 14 |

# Broker MQTT

El proyecto utiliza **HiveMQ** (`broker.hivemq.com:1883`), un broker público y gratuito, por lo que no depende de la IP local de ningún computador — el ESP32 y el backend se conectan desde cualquier red con acceso a internet.

# Probar sin hardware físico

Para simular una alerta sin necesidad del ESP32 conectado:

```bash
curl -X POST "http://localhost:8000/simular-alerta?tipo=no_natural&magnitud=2.5"
```

---

# Equipo

- Gianfranco Caleni
- Ricardo Vargas
- Oscar Rodriguez
- Matias Neira

**Profesor guía:** Miguel Carrasco — Proyecto en TICs I

# Licencia

Proyecto académico desarrollado para la Universidad Diego Portales.
