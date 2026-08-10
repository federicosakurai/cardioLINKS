#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// ============================================================
// LCD 20x4 I2C
// ============================================================

#define LCD_SDA 22
#define LCD_SCL 21

#define LCD_ADDRESS 0x27

hd44780_I2Cexp lcd(LCD_ADDRESS);

// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID = "TeleCentro-78e2";
const char* WIFI_PASSWORD = "EZM2MGYMGZMN";

// ============================================================
// FIREBASE
// ============================================================

const char* FIREBASE_URL =
  "https://firestore.googleapis.com/v1/projects/cardiolink-f186f/databases/(default)/documents/ecgs";

// ============================================================
// BUFFER EN PSRAM
// ============================================================

#define BUFFER_SIZE (1024 * 1024)

char* hl7Buffer = nullptr;

// ============================================================
// LCD - COMUNICACION ENTRE CORES
// ============================================================

struct MensajeLCD {
  String l1, l2, l3, l4;
  int numLineas;
};

QueueHandle_t colaLCD;
TaskHandle_t taskLCDHandle;

// ============================================================
// PROTOTIPOS
// ============================================================

void conectarWifi();
void recibirHL7Serial();
void procesarTrama();

String extraerCampo(
  const char* segmento,
  int campoNum);

char* extraerBase64PDF(
  char* hl7);

void enviarAFirebase(
  char* pdfBase64,
  String paciente,
  String ecgId,
  String fecha);

void tareaLCD(void* parametro);

// ============================================================
// LCD - FUNCIONES DE BAJO NIVEL (solo usadas dentro de tareaLCD)
// ============================================================

void limpiarLineaLCD(int linea) {

  lcd.setCursor(0, linea);
  lcd.print("                    ");
}

// ============================================================

void escribirLineaLCD(
  int linea,
  String texto) {

  limpiarLineaLCD(linea);

  lcd.setCursor(0, linea);

  if (texto.length() > 20) {
    texto = texto.substring(0, 20);
  }

  lcd.print(texto);
}

// ============================================================
// LCD - FUNCIONES PUBLICAS (encolan mensajes, no bloquean)
// ============================================================

void mostrarLCD(String linea1) {
  MensajeLCD msg = { linea1, "", "", "", 1 };
  xQueueSend(colaLCD, &msg, 0);
}

void mostrarLCD(
  String linea1,
  String linea2) {
  MensajeLCD msg = { linea1, linea2, "", "", 2 };
  xQueueSend(colaLCD, &msg, 0);
}

void mostrarLCD(
  String linea1,
  String linea2,
  String linea3) {
  MensajeLCD msg = { linea1, linea2, linea3, "", 3 };
  xQueueSend(colaLCD, &msg, 0);
}

void mostrarLCD(
  String linea1,
  String linea2,
  String linea3,
  String linea4) {
  MensajeLCD msg = { linea1, linea2, linea3, linea4, 4 };
  xQueueSend(colaLCD, &msg, 0);
}

// ============================================================
// TAREA LCD - CORRE EN CORE 0
// ============================================================

void tareaLCD(void* parametro) {

  MensajeLCD msg;

  for (;;) {

    if (xQueueReceive(colaLCD, &msg, portMAX_DELAY) == pdTRUE) {

      lcd.clear();

      escribirLineaLCD(0, msg.l1);

      if (msg.numLineas >= 2) escribirLineaLCD(1, msg.l2);
      if (msg.numLineas >= 3) escribirLineaLCD(2, msg.l3);
      if (msg.numLineas >= 4) escribirLineaLCD(3, msg.l4);
    }
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(2000);

  Serial.println();
  Serial.println("=== PULSONET - MULTI ECG ===");

  // ==========================================================
  // LCD
  // ==========================================================

  Wire.begin(
    LCD_SDA,
    LCD_SCL);

  int status = lcd.begin(20, 4);

  if (status) {
    Serial.println("ERROR: Fallo al iniciar LCD (hd44780)");
    hd44780::fatalError(status);  // el backlight parpadea indicando error
  }

  lcd.backlight();

  Serial.println("LCD I2C iniciado");

  // ==========================================================
  // CREAR COLA Y TAREA LCD EN CORE 0
  // ==========================================================

  colaLCD = xQueueCreate(10, sizeof(MensajeLCD));

  xTaskCreatePinnedToCore(
    tareaLCD,
    "TareaLCD",
    4096,
    NULL,
    1,
    &taskLCDHandle,
    0  // Core 0
  );

  mostrarLCD(
    "PULSONET",
    "Iniciando sistema...",
    "LCD 20x4 I2C");

  delay(2000);

  // ==========================================================
  // PSRAM
  // ==========================================================

  mostrarLCD(
    "PULSONET",
    "Comprobando PSRAM...");

  Serial.println(
    "Comprobando PSRAM...");

  if (!psramFound()) {

    Serial.println(
      "ERROR: PSRAM no detectada");

    mostrarLCD(
      "ERROR",
      "PSRAM NO DETECTADA",
      "Sistema detenido");

    while (true) {
      delay(1000);
    }
  }

  hl7Buffer =
    (char*)ps_malloc(
      BUFFER_SIZE);

  if (hl7Buffer == nullptr) {

    Serial.println(
      "ERROR: No se pudo alocar PSRAM");

    mostrarLCD(
      "ERROR",
      "NO SE PUDO ALOCAR",
      "LA PSRAM");

    while (true) {
      delay(1000);
    }
  }

  Serial.printf(
    "PSRAM OK - %d KB\n",
    BUFFER_SIZE / 1024);

  mostrarLCD(
    "PSRAM OK",
    "Buffer HL7:",
    String(BUFFER_SIZE / 1024) + " KB");

  delay(2000);

  // ==========================================================
  // WIFI
  // ==========================================================

  conectarWifi();

  // ==========================================================
  // SISTEMA LISTO
  // ==========================================================

  Serial.println();
  Serial.println("Sistema listo.");

  mostrarLCD(
    "PULSONET",
    "SISTEMA LISTO",
    "Esperando ECG...",
    "--------------------");

  delay(2000);
}

// ============================================================
// LOOP - CORRE EN CORE 1
// ============================================================

void loop() {

  recibirHL7Serial();

  procesarTrama();

  Serial.println();
  Serial.println(
    "=================================");

  Serial.println(
    "Listo para recibir otro ECG");

  Serial.println(
    "=================================");

  Serial.println();

  mostrarLCD(
    "PULSONET",
    "SISTEMA LISTO",
    "Esperando otro ECG...",
    "--------------------");

  delay(1000);
}

// ============================================================
// RECIBIR HL7
// ============================================================

void recibirHL7Serial() {

  Serial.println();
  Serial.println(
    "Esperando HL7 por Serial...");

  Serial.println(
    "Pegar HL7 completo");

  Serial.println(
    "Finaliza automaticamente");

  mostrarLCD(
    "ESPERANDO HL7",
    "Ingrese ECG por",
    "Serial...",
    "--------------------");

  memset(
    hl7Buffer,
    0,
    BUFFER_SIZE);

  int pos = 0;

  while (!Serial.available()) {
    delay(10);
  }

  Serial.println(
    "Recibiendo datos...");

  mostrarLCD(
    "RECIBIENDO HL7",
    "Datos en proceso...",
    "Espere...",
    "--------------------");

  unsigned long ultimoDato =
    millis();

  while (true) {

    while (Serial.available()) {

      char c =
        Serial.read();

      if (
        pos < BUFFER_SIZE - 1) {

        hl7Buffer[pos++] =
          c;

      } else {

        Serial.println(
          "ERROR: Buffer PSRAM lleno");

        mostrarLCD(
          "ERROR",
          "BUFFER PSRAM LLENO",
          "ECG demasiado grande",
          "Proceso detenido");

        return;
      }

      ultimoDato =
        millis();
    }

    if (
      millis() - ultimoDato > 3000) {

      hl7Buffer[pos] =
        '\0';

      Serial.printf(
        "HL7 recibido en PSRAM: %d bytes\n",
        pos);

      mostrarLCD(
        "HL7 RECIBIDO",
        String(pos) + " bytes",
        "Procesando...",
        "--------------------");

      delay(1500);

      return;
    }

    delay(1);
  }
}

// ============================================================
// PROCESAR HL7
// ============================================================

void procesarTrama() {

  Serial.println();
  Serial.println(
    "Procesando trama HL7...");

  mostrarLCD(
    "PROCESANDO HL7",
    "Extrayendo datos...",
    "Paciente / ID / Fecha",
    "--------------------");

  delay(1000);

  String paciente =
    extraerCampo(
      "PID",
      5);

  String fecha =
    extraerCampo(
      "OBR",
      7);

  String ecgId =
    extraerCampo(
      "OBR",
      3);

  paciente.replace(
    "^",
    "_");

  Serial.printf(
    "Paciente : %s\n",
    paciente.c_str());

  Serial.printf(
    "ECG ID   : %s\n",
    ecgId.c_str());

  Serial.printf(
    "Fecha    : %s\n",
    fecha.c_str());

  mostrarLCD(
    "DATOS DEL ECG",
    "Paciente:",
    paciente,
    "--------------------");

  delay(2000);

  mostrarLCD(
    "DATOS DEL ECG",
    "ID: " + ecgId,
    "Fecha: " + fecha,
    "--------------------");

  delay(2000);

  Serial.println(
    "Buscando PDF...");

  mostrarLCD(
    "BUSCANDO PDF",
    "Analizando HL7...",
    "Buscando Base64...",
    "--------------------");

  delay(1000);

  char* pdfBase64 =
    extraerBase64PDF(
      hl7Buffer);

  if (
    pdfBase64 == nullptr) {

    Serial.println(
      "ERROR: PDF no encontrado");

    mostrarLCD(
      "ERROR",
      "PDF NO ENCONTRADO",
      "ECG no procesado",
      "--------------------");

    delay(3000);

    return;
  }

  int base64Length =
    strlen(pdfBase64);

  Serial.printf(
    "Base64 extraido: %d caracteres\n",
    base64Length);

  mostrarLCD(
    "PDF ENCONTRADO",
    "Base64:",
    String(base64Length) + " caracteres",
    "Preparando envio...");

  delay(2000);

  enviarAFirebase(
    pdfBase64,
    paciente,
    ecgId,
    fecha);
}

// ============================================================
// EXTRAER PDF BASE64
// ============================================================

char* extraerBase64PDF(
  char* hl7) {

  char* inicio =
    strstr(
      hl7,
      "JVBERi0");

  if (
    inicio == nullptr) {

    Serial.println(
      "Firma PDF no encontrada");

    return nullptr;
  }

  char* fin =
    inicio;

  while (
    *fin != '\0' && *fin != '|' && *fin != '\r' && *fin != '\n' && *fin != 0x1C) {

    fin++;
  }

  *fin =
    '\0';

  return inicio;
}

// ============================================================
// EXTRAER CAMPO HL7
// ============================================================

String extraerCampo(
  const char* segmento,
  int campoNum) {

  char patron[10];

  sprintf(
    patron,
    "%s|",
    segmento);

  char* inicio =
    strstr(
      hl7Buffer,
      patron);

  if (
    inicio == nullptr) {

    return "DESCONOCIDO";
  }

  inicio +=
    strlen(patron);

  int campoActual =
    1;

  char temp[256] = { 0 };

  int i = 0;

  while (
    *inicio && *inicio != '\r' && *inicio != '\n') {

    if (
      *inicio == '|') {

      campoActual++;

      inicio++;

      continue;
    }

    if (
      campoActual == campoNum) {

      temp[i++] =
        *inicio;

      if (
        i >= 255) {
        break;
      }
    }

    inicio++;
  }

  temp[i] =
    '\0';

  if (
    strlen(temp) == 0) {

    return "DESCONOCIDO";
  }

  return String(temp);
}

// ============================================================
// FIREBASE
// ============================================================

void enviarAFirebase(
  char* pdfBase64,
  String paciente,
  String ecgId,
  String fecha) {

  if (
    WiFi.status() != WL_CONNECTED) {

    Serial.println(
      "WiFi desconectado");

    mostrarLCD(
      "WIFI DESCONECTADO",
      "Reconectando...",
      "Espere...",
      "--------------------");

    conectarWifi();
  }

  Serial.println(
    "Enviando a Firestore...");

  mostrarLCD(
    "FIRESTORE",
    "Enviando ECG...",
    "Espere...",
    "--------------------");

  // ==========================================================
  // ID DOCUMENTO
  // ==========================================================

  String docId =
    paciente + "_" + ecgId;

  docId.replace(
    " ",
    "_");

  docId.replace(
    "^",
    "_");

  docId.replace(
    "/",
    "_");

  docId.replace(
    "\\",
    "_");

  // ==========================================================
  // URL
  // ==========================================================

  String url =
    String(FIREBASE_URL) + "?documentId=" + docId;

  // ==========================================================
  // JSON
  // ==========================================================

  String json =
    "{\"fields\":{";

  json +=
    "\"paciente\":{\"stringValue\":\"";

  json +=
    paciente;

  json +=
    "\"},";

  json +=
    "\"ecg_id\":{\"stringValue\":\"";

  json +=
    ecgId;

  json +=
    "\"},";

  json +=
    "\"fecha\":{\"stringValue\":\"";

  json +=
    fecha;

  json +=
    "\"},";

  json +=
    "\"pdf_base64\":{\"stringValue\":\"";

  json +=
    String(pdfBase64);

  json +=
    "\"}";

  json +=
    "}}";

  Serial.printf(
    "Tamano JSON: %d bytes\n",
    json.length());

  mostrarLCD(
    "FIRESTORE",
    "JSON preparado",
    String(json.length()) + " bytes",
    "Conectando...");

  delay(1500);

  // ==========================================================
  // HTTP
  // ==========================================================

  HTTPClient http;

  Serial.println(
    "Conectando con Firestore...");

  http.begin(
    url);

  http.addHeader(
    "Content-Type",
    "application/json");

  http.setTimeout(
    60000);

  Serial.println(
    "Enviando POST...");

  mostrarLCD(
    "FIRESTORE",
    "Enviando POST...",
    "ECG en proceso",
    "Espere...");

  int httpCode =
    http.POST(
      json);

  if (
    httpCode > 0) {

    Serial.printf(
      "HTTP Code: %d\n",
      httpCode);

    String response =
      http.getString();

    Serial.println(
      response);

    if (
      httpCode == 200 || httpCode == 201) {

      Serial.println(
        "ECG enviado correctamente!");

      Serial.println(
        "Documento: " + docId);

      mostrarLCD(
        "ECG ENVIADO",
        "CORRECTAMENTE",
        "ID: " + ecgId,
        "Firestore OK");

      delay(4000);

    } else {

      Serial.println(
        "ERROR: Firestore rechazo");

      mostrarLCD(
        "ERROR FIRESTORE",
        "HTTP: " + String(httpCode),
        "ECG NO ENVIADO",
        "Revisar Serial");

      delay(4000);
    }

  } else {

    Serial.printf(
      "ERROR HTTP: %s\n",
      http.errorToString(
            httpCode)
        .c_str());

    mostrarLCD(
      "ERROR HTTP",
      http.errorToString(
        httpCode),
      "ECG NO ENVIADO",
      "Revisar conexion");

    delay(4000);
  }

  http.end();
}

// ============================================================
// WIFI
// ============================================================

void conectarWifi() {

  Serial.printf(
    "Conectando WiFi: %s",
    WIFI_SSID);

  mostrarLCD(
    "CONECTANDO WIFI",
    WIFI_SSID,
    "Espere...",
    "--------------------");

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD);

  int intentos =
    0;

  while (
    WiFi.status() != WL_CONNECTED && intentos < 20) {

    delay(500);

    Serial.print(".");

    intentos++;

    mostrarLCD(
      "CONECTANDO WIFI",
      WIFI_SSID,
      "Intento: " + String(intentos) + "/20",
      "Espere...");
  }

  if (
    WiFi.status() == WL_CONNECTED) {

    Serial.println(
      "\nWiFi conectado");

    Serial.print(
      "IP: ");

    Serial.println(
      WiFi.localIP());

    Serial.printf(
      "Heap libre: %d\n",
      ESP.getFreeHeap());

    mostrarLCD(
      "WIFI CONECTADO",
      "IP:",
      WiFi.localIP().toString(),
      "Conexion OK");

    delay(3000);

  } else {

    Serial.println(
      "\nERROR WiFi");

    mostrarLCD(
      "ERROR WIFI",
      "No se pudo conectar",
      "Revisar red",
      "--------------------");

    delay(3000);
  }
}