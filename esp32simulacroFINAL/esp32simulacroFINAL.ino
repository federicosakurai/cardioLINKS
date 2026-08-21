#include <Arduino.h>
#include <WiFi.h>

// ============================================================
// CONFIGURACION WIFI
// Conectarse al AP del ESP32 receptor
// ============================================================

const char* WIFI_SSID = "CardioLink-Gateway";
const char* WIFI_PASS = "cardio1234";

// ============================================================
// IP / PUERTO DEL RECEPTOR
// ============================================================

const char* SERVIDOR_IP = "192.168.4.1";
const uint16_t SERVIDOR_PUERTO = 2575;

WiFiClient cliente;

// ============================================================
// MLLP
// ============================================================

#define MLLP_VT 0x0B
#define MLLP_FS 0x1C
#define MLLP_CR 0x0D

// ============================================================
// BUFFER PARA BASE64
// 200000 caracteres
// ============================================================

#define MAX_BASE64_LEN 10000

char base64Buffer[MAX_BASE64_LEN];

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(2000);

  // ==========================================================
  // PASO 1: RECIBIR BASE64 DEL PDF POR MONITOR SERIAL
  // ==========================================================

  Serial.println();
  Serial.println("=================================================");
  Serial.println(" Pegue el Base64 del PDF");
  Serial.println(" Debe comenzar con JVBERi");
  Serial.println(" Debe terminar normalmente en = o ==");
  Serial.println(" Sin comillas y en una sola linea");
  Serial.println(" Presione Enter al terminar");
  Serial.println("=================================================");
  Serial.println();

  size_t pos = 0;
  unsigned long ultimoDato = millis();
  bool recibiendo = false;

  while (true) {

    while (Serial.available()) {

      char c = Serial.read();

      // ------------------------------------------------------
      // ENTER = BASE64 COMPLETO
      // ------------------------------------------------------

      if (c == '\n' || c == '\r') {

        if (pos > 0) {
          goto base64Listo;
        }

        continue;
      }

      // ------------------------------------------------------
      // GUARDAR CARACTER
      // ------------------------------------------------------

      if (pos < MAX_BASE64_LEN - 1) {

        base64Buffer[pos++] = c;
        recibiendo = true;

      } else {

        Serial.println();
        Serial.println("ERROR: Base64 supera MAX_BASE64_LEN.");
        Serial.println("Se corto el contenido.");
        goto base64Listo;
      }

      ultimoDato = millis();
    }

    // --------------------------------------------------------
    // SI DEJAN DE LLEGAR DATOS DURANTE 2 SEGUNDOS
    // --------------------------------------------------------

    if (recibiendo && millis() - ultimoDato > 2000) {
      break;
    }

    delay(1);
  }

base64Listo:

  base64Buffer[pos] = '\0';

  // ==========================================================
  // PASO 2: DIAGNOSTICO DEL BASE64
  // ==========================================================

  Serial.println();
  Serial.println("=================================================");
  Serial.println(" DIAGNOSTICO BASE64");
  Serial.println("=================================================");

  Serial.printf(
    "Base64 recibido: %lu bytes\n",
    (unsigned long)pos
  );

  Serial.printf(
    "Multiplo de 4: %s\n",
    (pos % 4 == 0) ? "SI" : "NO"
  );

  bool empiezaJVBERi =
    (pos >= 6 &&
     strncmp(base64Buffer, "JVBERi", 6) == 0);

  Serial.printf(
    "Empieza con JVBERi: %s\n",
    empiezaJVBERi ? "SI" : "NO"
  );

  if (pos > 0) {

    char ultimo = base64Buffer[pos - 1];

    Serial.printf(
      "Ultimo caracter: %c\n",
      ultimo
    );

    bool terminaCorrectamente =
      (ultimo == '=' ||
       (ultimo >= 'A' && ultimo <= 'Z') ||
       (ultimo >= 'a' && ultimo <= 'z') ||
       (ultimo >= '0' && ultimo <= '9'));

    Serial.printf(
      "Final aparentemente valido: %s\n",
      terminaCorrectamente ? "SI" : "NO"
    );
  }

  Serial.println("=================================================");
  Serial.println();

  // ==========================================================
  // PASO 3: ARMAR OBX
  // ==========================================================
  //
  // IMPORTANTE:
  //
  // OBX-2 = ED
  //
  // OBX-5:
  //
  // ^Application^PDF^Base64^DATOS
  //
  // La estructura ED es:
  //
  // ED.1 = Source Application
  // ED.2 = Type of Data
  // ED.3 = Data Subtype
  // ED.4 = Encoding
  // ED.5 = Data
  //
  // ==========================================================

  String obx =
    "OBX|1|ED|PDF REPORT^ECG PDF Report^LN||"
    "^Application^PDF^Base64^" +
    String(base64Buffer) +
    "|||||F|||20260816120000";

  // ==========================================================
  // MOSTRAR OBX
  // ==========================================================

  Serial.println("=================================================");
  Serial.println(" SEGMENTO OBX ARMADO");
  Serial.println("=================================================");

  Serial.printf(
    "Longitud total del OBX: %d caracteres\n",
    obx.length()
  );

  Serial.println();

  Serial.println("--- Primeros 180 caracteres ---");

  Serial.println(
    obx.substring(
      0,
      min(180, (int)obx.length())
    )
  );

  Serial.println();

  Serial.println("--- Ultimos 180 caracteres ---");

  int inicioFinal = max(
    0,
    (int)obx.length() - 180
  );

  Serial.println(
    obx.substring(inicioFinal)
  );

  Serial.println();
  Serial.println("=================================================");
  Serial.println();

  // ==========================================================
  // PASO 4: ARMAR TRAMA HL7
  // ==========================================================

  String tramaHL7 =
    "MSH|^~\\&|ECG100PLUS|CARDIOLINE|CARDIOLINK|HOSPITAL|20260816120000||ORU^R01^ORU_R01|MSG00001|P|2.5\r"
    "PID|1||123456^^^HOSPITAL^MR||Perez^Juan^Carlos||19800101|M\r"
    "PV1|1|O\r"
    "OBR|1|ORD001|FIL001|93000^ECG 12 derivaciones^LN|||20260816120000\r" +
    obx +
    "\r";

  // ==========================================================
  // DIAGNOSTICO TRAMA
  // ==========================================================

  Serial.printf(
    "Trama HL7 completa: %d bytes\n",
    tramaHL7.length()
  );

  Serial.println();

  // ==========================================================
  // PASO 5: CONECTAR AL AP WIFI
  // ==========================================================

  Serial.println(
    "Conectando al AP del receptor..."
  );

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASS
  );

  unsigned long inicioWiFi = millis();

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");

    // --------------------------------------------------------
    // TIMEOUT WIFI
    // --------------------------------------------------------

    if (millis() - inicioWiFi > 20000) {

      Serial.println();
      Serial.println(
        "ERROR: Timeout conectando al WiFi."
      );

      return;
    }
  }

  Serial.println();

  Serial.println(
    "WiFi conectado."
  );

  Serial.print(
    "IP local: "
  );

  Serial.println(
    WiFi.localIP()
  );

  Serial.println();

  delay(1000);

  // ==========================================================
  // PASO 6: CONECTAR AL SERVIDOR TCP
  // ==========================================================

  Serial.println(
    "Conectando al servidor TCP MLLP..."
  );

  if (!cliente.connect(
        SERVIDOR_IP,
        SERVIDOR_PUERTO
      )) {

    Serial.println();
    Serial.println(
      "ERROR: No se pudo conectar al servidor."
    );

    return;
  }

  // ==========================================================
  // CONEXION EXITOSA
  // ==========================================================

  Serial.println(
    "Conectado al servidor."
  );

  Serial.println(
    "Enviando trama HL7 mediante MLLP..."
  );

  // ==========================================================
  // PASO 7: ENVOLVER EN MLLP
  // ==========================================================

  // Inicio MLLP
  cliente.write(
    (uint8_t)MLLP_VT
  );

  // Trama HL7
  cliente.print(
    tramaHL7
  );

  // Fin MLLP
  cliente.write(
    (uint8_t)MLLP_FS
  );

  cliente.write(
    (uint8_t)MLLP_CR
  );

  cliente.flush();

  // ==========================================================
  // PASO 8: ESPERAR ACK
  // ==========================================================

  Serial.println(
    "Trama enviada. Esperando ACK..."
  );

  unsigned long inicioEspera = millis();

  while (
    cliente.connected() &&
    !cliente.available() &&
    millis() - inicioEspera < 5000
  ) {

    delay(10);
  }

  // ==========================================================
  // ACK RECIBIDO
  // ==========================================================

  if (cliente.available()) {

    Serial.println();
    Serial.println(
      "================================================="
    );

    Serial.println(
      " ACK RECIBIDO"
    );

    Serial.println(
      "================================================="
    );

    while (cliente.available()) {

      Serial.write(
        cliente.read()
      );
    }

    Serial.println();
    Serial.println(
      "================================================="
    );

  } else {

    Serial.println();
    Serial.println(
      "NO se recibio ACK."
    );

    Serial.println(
      "Timeout de 5 segundos."
    );
  }

  // ==========================================================
  // CERRAR CONEXION
  // ==========================================================

  cliente.stop();

  Serial.println();
  Serial.println(
    "Conexion TCP cerrada."
  );

  Serial.println();
  Serial.println(
    "================================================="
  );

  Serial.println(
    " ENVIO FINALIZADO"
  );

  Serial.println(
    "================================================="
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // Envio unico de prueba.
}



