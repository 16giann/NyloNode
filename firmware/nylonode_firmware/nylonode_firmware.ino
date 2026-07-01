#include <WiFi.h>
#include <o-s-c-a-t-project-1_inferencing.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <WebServer.h>
#include "esp_camera.h"
#define sensor_t sensor_adafruit_t
#include <Adafruit_ADXL345_U.h>
#undef sensor_t
#include "esp_sleep.h"
#include <Preferences.h>
#include "FS.h"
#include "SD_MMC.h"

// ── Configuración ──────────────────────────────────────────
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const float UMBRAL_G    = 1.5;
const int   COOLDOWN    = 30000;

// ── Pines ESP32-CAM AI-Thinker ────────────────────────────
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22   
#define PIN_LED         33
#define PIN_BUZZER      12
#define PIN_RESET        0
#define PIN_FLASH        4

// ── Variables globales ─────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);
Preferences  preferences;
WebServer    server(80);

unsigned long ultimaAlerta = 0;
bool sensorDisponible      = false;
bool sdDisponible          = false;
int  contadorImagenes      = 0;

// ── Declaraciones adelantadas ──────────────────────────────
void reconectarMQTT();
void procesarMovimiento(float magnitud);
void enviarImagen(uint8_t* buf, size_t len);
void enviarImagenesPendientes();
bool inicializarCamara();
void alertaLocal();
void recibirConfiguracion(char* topic, byte* payload, unsigned int length);
void servirListado();
void servirImagen();
void tomarFotoYEnviar();
void enviarComoBMP(uint8_t* buf, size_t len);
void servirPaginaLive();     
void servirFrameLive();      
void obtenerUltimoContador(); // <-- Nueva función para escanear la SD

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED,    OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RESET,  INPUT_PULLUP);
  pinMode(PIN_FLASH,  OUTPUT);
  digitalWrite(PIN_FLASH, LOW);

  if (digitalRead(PIN_RESET) == LOW) {
    Serial.println("Reseteando configuración WiFi...");
    WiFiManager wm;
    wm.resetSettings();
    Serial.println("Configuración borrada — reinicia el ESP32");
    while (true) {
      digitalWrite(PIN_LED, HIGH); delay(200);
      digitalWrite(PIN_LED, LOW);  delay(200);
    }
  }

  if (SD_MMC.begin("/sdcard", true)) { 
    sdDisponible = true;
    pinMode(PIN_FLASH, OUTPUT);
    digitalWrite(PIN_FLASH, LOW); 
    Serial.println("SD OK (Modo 1-Bit)");
    if (!SD_MMC.exists("/nylonode")) {
      SD_MMC.mkdir("/nylonode");
      Serial.println("Carpeta /nylonode creada");
    }
    
    // Escanear archivos existentes para no sobrescribir nada anterior
    obtenerUltimoContador();

    uint64_t total = SD_MMC.totalBytes() / (1024 * 1024);
    uint64_t used  = SD_MMC.usedBytes()  / (1024 * 1024);
    Serial.printf("SD: %llu MB usados de %llu MB\n", used, total);
  } else {
    Serial.println("ADVERTENCIA: SD no encontrada");
  }

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  bool conectado = wm.autoConnect("NyloNode-Setup", "nylonode123");

  if (!conectado) {
    Serial.println("Sin WiFi — guardando solo en SD");
    alertaLocal();
  } else {
    Serial.println("WiFi conectado: " + WiFi.localIP().toString());
  }

  Wire.begin(13, 14);

  if (!accel.begin()) {
    Serial.println("ADVERTENCIA: ADXL345 no encontrado - modo simulacion");
  } else {
    accel.setRange(ADXL345_RANGE_16_G);
    sensorDisponible = true;
    Serial.println("ADXL345 OK");
  }

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setBufferSize(10000);
  mqtt.setCallback(recibirConfiguracion);

  server.on("/",           servirListado);
  server.on("/imagen",     servirImagen);
  server.on("/foto",       tomarFotoYEnviar);
  server.on("/live",       servirPaginaLive); 
  server.on("/live-frame", servirFrameLive);  
  server.begin();

  Serial.println("Servidor web: http://" + WiFi.localIP().toString());
  Serial.println("Setup completo");
}

void loop() {
  server.handleClient();

  if (!mqtt.connected()) reconectarMQTT();
  mqtt.loop();

  if (mqtt.connected()) enviarImagenesPendientes();

  if (sensorDisponible) {
    sensors_event_t evento;
    accel.getEvent(&evento);

    float magnitud = sqrt(
      pow(evento.acceleration.x, 2) +
      pow(evento.acceleration.y, 2) +
      pow(evento.acceleration.z, 2)
    ) / 9.81;

    unsigned long ahora = millis();
    if (magnitud > UMBRAL_G && (ahora - ultimaAlerta) > COOLDOWN) {
      Serial.printf("Movimiento: %.2fg\n", magnitud);
      ultimaAlerta = ahora;
      procesarMovimiento(magnitud);
    }
  }
  delay(10);
}

// ── Escanear SD para prevenir sobrescritura de archivos ──
void obtenerUltimoContador() {
  File dir = SD_MMC.open("/nylonode");
  if (!dir) return;

  int maxIndex = 0;
  File archivo = dir.openNextFile();
  
  while (archivo) {
    String nombre = String(archivo.name());
    int indiceActual = 0;

    // Buscar patrones tanto de tus capturas manuales ("foto_X.raw") como de movimiento ("img_X.raw")
    if (nombre.startsWith("foto_") && nombre.endsWith(".raw")) {
      indiceActual = nombre.substring(5, nombre.length() - 4).toInt();
    } else if (nombre.startsWith("img_") && nombre.endsWith(".raw")) {
      indiceActual = nombre.substring(4, nombre.length() - 4).toInt();
    }

    if (indiceActual > maxIndex) {
      maxIndex = indiceActual;
    }
    archivo = dir.openNextFile();
  }
  dir.close();

  contadorImagenes = maxIndex;
  Serial.printf("Escaneo de SD finalizado. El contador continuará desde: %d\n", contadorImagenes + 1);
}

// ── Servidor web: listado de imágenes ────────────────────
void servirListado() {
  if (!sdDisponible) {
    server.send(404, "text/plain", "SD no disponible");
    return;
  }

  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<title>NyloNode</title>"
                "<style>"
                "body{background:#0A0E1A;color:#F1F5F9;font-family:sans-serif;padding:20px}"
                "h2{color:#3B82F6}"
                "a{color:#60A5FA;text-decoration:none;display:block;padding:8px 0;border-bottom:1px solid #1E2D45}"
                ".btn{background:#3B82F6;color:white;padding:12px 20px;border-radius:8px;"
                "display:inline-block;margin:10px 0;text-decoration:none}"
                ".btn-live{background:#EF4444;margin-left:10px;}"
                "</style></head><body>"
                "<h2>NyloNode — Imágenes TinyML (96x96 Grayscale)</h2>"
                "<a class='btn' href='/foto'>📸 Tomar foto ahora</a>"
                "<a class='btn btn-live' href='/live'>🔴 Ver En Vivo</a><br><br>";
  File dir = SD_MMC.open("/nylonode");
  File archivo = dir.openNextFile();
  int count = 0;
  while (archivo) {
    String nombre = String(archivo.name());
    if (nombre.endsWith(".raw")) {
      html += "<a href='/imagen?archivo=" + nombre + "'>📷 " + nombre + "</a>";
      count++;
    }
    archivo = dir.openNextFile();
  }
  if (count == 0) {
    html += "<p>No hay imágenes guardadas todavía</p>";
  }
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ── Servidor web: ver imagen ──────────────────────────────
void servirImagen() {
  if (!sdDisponible) {
    server.send(404, "text/plain", "SD no disponible");
    return;
  }
  String ruta = "/nylonode/";
  if (server.hasArg("archivo")) {
    ruta += server.arg("archivo");
  } else {
    ruta += "prueba.raw";
  }
  File archivo = SD_MMC.open(ruta);
  if (!archivo) {
    server.send(404, "text/plain", "Archivo no encontrado: " + ruta);
    return;
  }
  size_t tam = archivo.size();
  uint8_t* buf = (uint8_t*)malloc(tam);
  if (!buf) {
    archivo.close();
    server.send(500, "text/plain", "Error de asignación de memoria");
    return;
  }
  archivo.read(buf, tam);
  archivo.close();
  enviarComoBMP(buf, tam);
  free(buf);
}

// ── Servidor web: tomar foto ──────────────────────────────
void tomarFotoYEnviar() {
  Serial.println("Foto solicitada desde app...");
  digitalWrite(PIN_LED, HIGH);
  if (!inicializarCamara()) {
    server.send(500, "text/plain", "Error iniciando cámara");
    digitalWrite(PIN_LED, LOW);
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Error capturando imagen");
    esp_camera_deinit();
    digitalWrite(PIN_LED, LOW);
    return;
  }

  if (sdDisponible) {
    contadorImagenes++;
    String ruta = "/nylonode/foto_" + String(contadorImagenes) + ".raw";
    File archivo = SD_MMC.open(ruta, FILE_WRITE);
    if (archivo) {
      archivo.write(fb->buf, fb->len);
      archivo.close();
      Serial.println("Foto guardada en SD: " + ruta);
    }
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");
  enviarComoBMP(fb->buf, fb->len);

  esp_camera_fb_return(fb);
  esp_camera_deinit();
  digitalWrite(PIN_LED, LOW);
  Serial.println("Foto enviada");
}

// ── Interfaz HTML del En Vivo (Refresco por JS) ──
void servirPaginaLive() {
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<title>NyloNode Live</title>"
                "<style>"
                "body{background:#0A0E1A;color:#F1F5F9;font-family:sans-serif;text-align:center;padding:20px;}"
                "h2{color:#EF4444;}"
                ".stream-container{position:relative;display:inline-block;background:#000;padding:10px;border-radius:12px;border:2px solid #1E2D45;}"
                "img{width:288px;height:288px;image-rendering:pixelated;display:block;border-radius:6px;}"
                ".btn{background:#3B82F6;color:white;padding:12px 24px;border-radius:8px;display:inline-block;margin:15px 5px;text-decoration:none;border:none;font-size:16px;cursor:pointer;}"
                ".btn-back{background:#475569;}"
                "#status{margin-top:10px;color:#10B981;font-weight:bold;}"
                "</style></head><body>"
                "<h2>🔴 Transmisión en Vivo (96x96)</h2>"
                "<div class='stream-container'>"
                "  <img id='videoStream' src='/live-frame' alt='Cargando streaming...'>"
                "</div><br>"
                "<button class='btn' onclick='capturarFoto()'>📸 Capturar y Guardar en SD</button>"
                "<a class='btn btn-back' href='/'>Volver al Inicio</a>"
                "<div id='status'></div>"
                "<script>"
                "var camView = document.getElementById('videoStream');"
                "var streamActivo = true;"
                
                "function actualizarVideo() {"
                "  if(!streamActivo) return;"
                "  var imgTmp = new Image();"
                "  imgTmp.onload = function() {"
                "    camView.src = this.src;"
                "    setTimeout(actualizarVideo, 30);" 
                "  };"
                "  imgTmp.onerror = function() {"
                "    setTimeout(actualizarVideo, 200);"
                "  };"
                "  imgTmp.src = '/live-frame?t=' + new Date().getTime();" 
                "}"
                
                "function capturarFoto(){"
                "  streamActivo = false;" 
                "  var statusDiv = document.getElementById('status');"
                "  statusDiv.innerText = 'Capturando frame actual y guardando en SD...';"
                "  statusDiv.style.color = '#F59E0B';"
                "  fetch('/foto')"
                "    .then(res => {"
                "       if(res.ok) { "
                "         statusDiv.innerText = '¡OK! Captura almacenada con éxito en la SD.';" 
                "         statusDiv.style.color = '#10B981'; "
                "       } else { "
                "         statusDiv.innerText = 'Error al procesar la foto en la SD'; "
                "         statusDiv.style.color = '#EF4444'; "
                "       }"
                "       streamActivo = true; actualizarVideo();" 
                "    })"
                "    .catch(() => {"
                "       statusDiv.innerText = 'Error de red al intentar capturar';"
                "       statusDiv.style.color = '#EF4444';"
                "       streamActivo = true; actualizarVideo();"
                "    });"
                "}"
                
                "actualizarVideo();" 
                "</script></body></html>";
  server.send(200, "text/html", html);
}

// ── Entrega de un frame individual BMP en vivo (No bloqueante) ──
void servirFrameLive() {
  if (!inicializarCamara()) {
    server.send(500, "text/plain", "Cámara ocupada");
    return;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    esp_camera_deinit();
    server.send(500, "text/plain", "Fallo captura");
    return;
  }
  
  server.sendHeader("Access-Control-Allow-Origin", "*");
  enviarComoBMP(fb->buf, fb->len);

  esp_camera_fb_return(fb);
  esp_camera_deinit();
}

// ── Procesar movimiento ──────────────────────────────────
void procesarMovimiento(float magnitud) {
  digitalWrite(PIN_LED, HIGH);
  Serial.println("Procesando movimiento...");
  if (!inicializarCamara()) {
    Serial.println("Error iniciando cámara");
    digitalWrite(PIN_LED, LOW);
    return;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Error capturando imagen");
    esp_camera_deinit();
    digitalWrite(PIN_LED, LOW);
    return;
  }
  Serial.printf("Imagen capturada: %d bytes\n", fb->len);
  if (sdDisponible) {
    contadorImagenes++;
    String ruta    = "/nylonode/img_" + String(contadorImagenes) + ".raw";
    String rutaMeta = "/nylonode/img_" + String(contadorImagenes) + ".txt";
    File archivo = SD_MMC.open(ruta, FILE_WRITE);
    if (archivo) {
      archivo.write(fb->buf, fb->len);
      archivo.close();
      Serial.println("Imagen guardada en SD: " + ruta);
    }
    File meta = SD_MMC.open(rutaMeta, FILE_WRITE);
    if (meta) {
      meta.println("magnitud:" + String(magnitud, 2));
      meta.println("timestamp:" + String(millis()));
      meta.println("enviado:false");
      meta.close();
    }
  }
  if (mqtt.connected()) {
    String payload = "{\"evento\":\"movimiento\","
                     "\"magnitud\":" + String(magnitud, 2) + ","
                     "\"timestamp\":" + String(millis()) + "}";
    mqtt.publish("nylonode/alerta", payload.c_str());
    Serial.println("Alerta MQTT enviada");
    enviarImagen(fb->buf, fb->len);
  } else {
    Serial.println("Sin internet — guardado en SD para envío posterior");
    alertaLocal();
  }
  esp_camera_fb_return(fb);
  esp_camera_deinit();
  digitalWrite(PIN_LED, LOW);
}

void enviarImagenesPendientes() {
  if (!sdDisponible) return;
  File dir = SD_MMC.open("/nylonode");
  if (!dir) return;
  File archivo = dir.openNextFile();
  while (archivo) {
    String nombre = String(archivo.name());
    if (nombre.endsWith(".txt")) {
      String contenido = "";
      while (archivo.available()) contenido += (char)archivo.read();
      if (contenido.indexOf("enviado:false") >= 0) {
        String nombreImg = nombre;
        nombreImg.replace(".txt", ".raw");
        File img = SD_MMC.open("/nylonode/" + nombreImg);
        if (img) {
          size_t tam = img.size();
          uint8_t* buf = (uint8_t*)malloc(tam);
          if (buf) {
            img.read(buf, tam);
            img.close();
            enviarImagen(buf, tam);
            free(buf);
            Serial.println("Pendiente enviada: " + nombreImg);
            SD_MMC.remove(("/nylonode/" + nombreImg).c_str());
            SD_MMC.remove(("/nylonode/" + nombre).c_str());
          }
        }
      }
    }
    archivo = dir.openNextFile();
  }
}

void enviarImagen(uint8_t* buf, size_t len) {
  String meta = "{\"tipo\":\"raw_grayscale\",\"bytes\":" + String(len) + "}";
  mqtt.publish("nylonode/imagen/meta", meta.c_str());
  delay(100);
  const int CHUNK = 2048;
  int partes = (len + CHUNK - 1) / CHUNK;
  for (int i = 0; i < partes; i++) {
    int inicio = i * CHUNK;
    int fin    = min(inicio + CHUNK, (int)len);
    mqtt.publish("nylonode/imagen/data", buf + inicio, fin - inicio, false);
    delay(20);
  }
  Serial.println("Imagen enviada por MQTT");
}

void recibirConfiguracion(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println("Config recibida: " + msg);
}

void reconectarMQTT() {
  int intentos = 0;
  while (!mqtt.connected() && intentos < 3) {
    Serial.print("Conectando MQTT...");
    if (mqtt.connect("nylonode-esp32")) {
      Serial.println("OK");
      mqtt.subscribe("nylonode/config");
    } else {
      Serial.printf("Error: %d\n", mqtt.state());
      delay(2000);
      intentos++;
    }
  }
}

bool inicializarCamara() {
  camera_config_t config;
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = CAM_PIN_D0;
  config.pin_d1        = CAM_PIN_D1;
  config.pin_d2        = CAM_PIN_D2;
  config.pin_d3        = CAM_PIN_D3;
  config.pin_d4        = CAM_PIN_D4;
  config.pin_d5        = CAM_PIN_D5;
  config.pin_d6        = CAM_PIN_D6;
  config.pin_d7        = CAM_PIN_D7;
  config.pin_xclk      = CAM_PIN_XCLK;
  config.pin_pclk      = CAM_PIN_PCLK;
  config.pin_vsync     = CAM_PIN_VSYNC;
  config.pin_href      = CAM_PIN_HREF;
  config.pin_sscb_sda  = CAM_PIN_SIOD;
  config.pin_sscb_scl  = CAM_PIN_SIOC;
  config.pin_pwdn      = CAM_PIN_PWDN;
  config.pin_reset     = CAM_PIN_RESET;
  config.xclk_freq_hz  = 20000000;
  config.pixel_format = PIXFORMAT_GRAYSCALE;
  config.frame_size    = FRAMESIZE_96X96;
  config.jpeg_quality  = 0;
  config.fb_count      = 1;
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) return false;

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);  
  }
  return true;
}

void enviarComoBMP(uint8_t* buf, size_t len) {
  size_t bmp_size = 54 + 1024 + len;
  uint8_t* bmp = (uint8_t*)malloc(bmp_size);
  if (!bmp) {
    server.send(500, "text/plain", "Falta memoria RAM para procesamiento");
    return;
  }
  memset(bmp, 0, bmp_size);

  bmp[0] = 'B'; bmp[1] = 'M';
  *(uint32_t*)(&bmp[2])  = bmp_size;
  *(uint32_t*)(&bmp[10]) = 54 + 1024;
  *(uint32_t*)(&bmp[14]) = 40;
  *(int32_t*)(&bmp[18])  = 96;
  *(int32_t*)(&bmp[22])  = -96;
  *(uint16_t*)(&bmp[26]) = 1;
  *(uint16_t*)(&bmp[28]) = 8;
  *(uint32_t*)(&bmp[34]) = len;
  uint8_t* paleta = bmp + 54;
  for (int i = 0; i < 256; i++) {
    paleta[i * 4 + 0] = i;
    paleta[i * 4 + 1] = i;
    paleta[i * 4 + 2] = i;
    paleta[i * 4 + 3] = 0;
  }
  memcpy(bmp + 54 + 1024, buf, len);
  server.send_P(200, "image/bmp", (const char*)bmp, bmp_size);
  free(bmp);
}

void alertaLocal() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, HIGH);
    tone(PIN_BUZZER, 1000, 200);
    delay(300);
    digitalWrite(PIN_LED, LOW);
    delay(200);
  }
}