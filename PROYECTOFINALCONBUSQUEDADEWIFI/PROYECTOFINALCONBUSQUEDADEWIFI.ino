
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
"https://firestore.googleapis.com/v1/projects/"
"cardiolink-f186f/databases/(default)/documents/ecgs";

// ============================================================
// CONFIGURACION PSRAM
// ============================================================

#define BUFFER_SIZE (1024UL * 1024UL)

#define MAX_ECG_PENDIENTES 2

#define ECG_SLOT_SIZE (1024UL * 1024UL)

#define PSRAM_NECESARIA \
(BUFFER_SIZE + (MAX_ECG_PENDIENTES * ECG_SLOT_SIZE))

char* hl7Buffer = nullptr;

// ============================================================
// ESTRUCTURA ECG
// ============================================================

struct ECGPendiente {

  bool ocupado;
  bool enviado;

  char paciente[128];
  char ecgId[64];
  char fecha[64];

  char* pdfBase64;
};

ECGPendiente colaECG[MAX_ECG_PENDIENTES];

int totalPendientes = 0;

// ============================================================
// LCD
// ============================================================

struct MensajeLCD {

  String l1;
  String l2;
  String l3;
  String l4;

  int numLineas;
};

QueueHandle_t colaLCD;
TaskHandle_t taskLCDHandle;

// ============================================================
// ESTADO WIFI
// ============================================================

bool hayInternet = false;

bool estadoWifiAnterior = false;

unsigned long ultimoIntentoWifi = 0;

const unsigned long INTERVALO_RECONEXION_WIFI = 5000;

// ============================================================
// PROTOTIPOS
// ============================================================

void conectarWifi(bool mostrarEnLCD);

bool verificarConexion();

void gestionarWifi();

bool inicializarPSRAM();

bool inicializarColaECG();

void recibirHL7Serial();

void procesarTrama();

String extraerCampo(
  const char* segmento,
  int campoNum
);

char* extraerBase64PDF(
  char* hl7
);

int guardarECGPendiente(
  String paciente,
  String ecgId,
  String fecha,
  const char* pdfBase64
);

void liberarSlotECG(
  int index
);

bool enviarECGAFirebase(
  int index
);

void procesarColaPendientes();

void mostrarEstadoListo();

void tareaLCD(
  void* parametro
);

// ============================================================
// LCD - FUNCIONES
// ============================================================

void limpiarLineaLCD(
  int linea
) {

  lcd.setCursor(0, linea);

  lcd.print(
    "                    "
  );
}

void escribirLineaLCD(
  int linea,
  String texto
) {

  limpiarLineaLCD(linea);

  lcd.setCursor(0, linea);

  if (texto.length() > 20) {

    texto =
      texto.substring(
        0,
        20
      );
  }

  lcd.print(texto);
}

// ============================================================
// MOSTRAR LCD
// ============================================================

void mostrarLCD(
  String linea1
) {

  MensajeLCD msg = {
    linea1,
    "",
    "",
    "",
    1
  };

  xQueueSend(
    colaLCD,
    &msg,
    0
  );
}

void mostrarLCD(
  String linea1,
  String linea2
) {

  MensajeLCD msg = {
    linea1,
    linea2,
    "",
    "",
    2
  };

  xQueueSend(
    colaLCD,
    &msg,
    0
  );
}

void mostrarLCD(
  String linea1,
  String linea2,
  String linea3
) {

  MensajeLCD msg = {
    linea1,
    linea2,
    linea3,
    "",
    3
  };

  xQueueSend(
    colaLCD,
    &msg,
    0
  );
}

void mostrarLCD(
  String linea1,
  String linea2,
  String linea3,
  String linea4
) {

  MensajeLCD msg = {
    linea1,
    linea2,
    linea3,
    linea4,
    4
  };

  xQueueSend(
    colaLCD,
    &msg,
    0
  );
}

// ============================================================
// LCD - ESTADO NORMAL
// ============================================================

void mostrarEstadoListo() {

  if (hayInternet) {

    mostrarLCD(
      "PULSONET",
      "SISTEMA LISTO",
      "WiFi: CONECTADO",
      "Esperando otro ECG..."
    );

  } else {

    mostrarLCD(
      "PULSONET",
      "SIN CONEXION",
      "Datos en PSRAM",
      "Esperando otro ECG..."
    );
  }
}

// ============================================================
// TAREA LCD - CORE 0
// ============================================================

void tareaLCD(
  void* parametro
) {

  MensajeLCD msg;

  for (;;) {

    if (
      xQueueReceive(
        colaLCD,
        &msg,
        portMAX_DELAY
      ) == pdTRUE
    ) {

      lcd.clear();

      escribirLineaLCD(
        0,
        msg.l1
      );

      if (
        msg.numLineas >= 2
      ) {

        escribirLineaLCD(
          1,
          msg.l2
        );
      }

      if (
        msg.numLineas >= 3
      ) {

        escribirLineaLCD(
          2,
          msg.l3
        );
      }

      if (
        msg.numLineas >= 4
      ) {

        escribirLineaLCD(
          3,
          msg.l4
        );
      }
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

  Serial.println(
    "========================================"
  );

  Serial.println(
    "       PULSONET - MULTI ECG"
  );

  Serial.println(
    " TTGO T-A7670SA ESP32-WROVER"
  );

  Serial.println(
    "========================================"
  );

  // ==========================================================
  // LCD
  // ==========================================================

  Wire.begin(
    LCD_SDA,
    LCD_SCL
  );

  int status =
    lcd.begin(
      20,
      4
    );

  if (status) {

    Serial.println(
      "ERROR: Fallo al iniciar LCD"
    );

    hd44780::fatalError(
      status
    );
  }

  lcd.backlight();

  Serial.println(
    "LCD I2C iniciado"
  );

  // ==========================================================
  // COLA LCD
  // ==========================================================

  colaLCD =
    xQueueCreate(
      10,
      sizeof(MensajeLCD)
    );

  if (
    colaLCD == nullptr
  ) {

    Serial.println(
      "ERROR: No se pudo crear cola LCD"
    );

    while (true) {

      delay(1000);
    }
  }

  xTaskCreatePinnedToCore(
    tareaLCD,
    "TareaLCD",
    4096,
    NULL,
    1,
    &taskLCDHandle,
    0
  );

  mostrarLCD(
    "PULSONET",
    "Iniciando sistema...",
    "TTGO T-A7670SA"
  );

  delay(2000);

  // ==========================================================
  // PSRAM
  // ==========================================================

  if (
    !inicializarPSRAM()
  ) {

    Serial.println();

    Serial.println(
      "========================================"
    );

    Serial.println(
      "       ERROR CRITICO DE PSRAM"
    );

    Serial.println(
      "========================================"
    );

    mostrarLCD(
      "ERROR PSRAM",
      "Memoria insuficiente",
      "Sistema detenido"
    );

    while (true) {

      delay(1000);
    }
  }

  // ==========================================================
  // COLA ECG
  // ==========================================================

  if (
    !inicializarColaECG()
  ) {

    Serial.println(
      "ERROR: No se pudo inicializar"
    );

    mostrarLCD(
      "ERROR",
      "PSRAM INSUFICIENTE",
      "Slots ECG"
    );

    while (true) {

      delay(1000);
    }
  }

  // ==========================================================
  // RESUMEN PSRAM
  // ==========================================================

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "CONFIGURACION DE MEMORIA"
  );

  Serial.printf(
    "PSRAM total: %lu KB\n",
    (unsigned long)(
      ESP.getPsramSize() / 1024
    )
  );

  Serial.printf(
    "PSRAM libre: %lu KB\n",
    (unsigned long)(
      ESP.getFreePsram() / 1024
    )
  );

  Serial.printf(
    "Buffer HL7: %lu KB\n",
    (unsigned long)(
      BUFFER_SIZE / 1024
    )
  );

  Serial.printf(
    "Slots ECG: %d\n",
    MAX_ECG_PENDIENTES
  );

  Serial.printf(
    "Tamano cada slot: %lu KB\n",
    (unsigned long)(
      ECG_SLOT_SIZE / 1024
    )
  );

  Serial.printf(
    "PSRAM reservada: %lu KB\n",
    (unsigned long)(
      PSRAM_NECESARIA / 1024
    )
  );

  Serial.printf(
    "PSRAM libre final: %lu KB\n",
    (unsigned long)(
      ESP.getFreePsram() / 1024
    )
  );

  Serial.println(
    "========================================"
  );

  mostrarLCD(
    "PSRAM OK",
    "8 MB detectados",
    "2 ECG x 1 MB",
    "Sistema listo"
  );

  delay(2500);

  // ==========================================================
  // WIFI
  // ==========================================================

  conectarWifi(true);

  Serial.println();

  Serial.println(
    "Sistema listo."
  );

  mostrarEstadoListo();

  delay(2500);
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // COMPROBAR WIFI CONSTANTEMENTE
  // ----------------------------------------------------------

  gestionarWifi();

  // ----------------------------------------------------------
  // SI HAY INTERNET, PROCESAR PENDIENTES
  // ----------------------------------------------------------

  if (hayInternet) {

    procesarColaPendientes();
  }

  // ----------------------------------------------------------
  // RECIBIR NUEVO HL7
  // ----------------------------------------------------------

  recibirHL7Serial();

  // ----------------------------------------------------------
  // PROCESAR HL7
  // ----------------------------------------------------------

  procesarTrama();

  Serial.println();

  Serial.println(
    "================================="
  );

  Serial.println(
    "Listo para recibir otro ECG"
  );

  Serial.println(
    "================================="
  );

  Serial.println();

  mostrarEstadoListo();

  delay(1000);
}

// ============================================================
// GESTIONAR WIFI
// ============================================================

void gestionarWifi() {

  bool estadoActual =
    (
      WiFi.status() ==
      WL_CONNECTED
    );

  // ----------------------------------------------------------
  // CAMBIO DE ESTADO
  // ----------------------------------------------------------

  if (
    estadoActual !=
    estadoWifiAnterior
  ) {

    estadoWifiAnterior =
      estadoActual;

    hayInternet =
      estadoActual;

    // ========================================================
    // WIFI RECONECTADO
    // ========================================================

    if (estadoActual) {

      Serial.println();

      Serial.println(
        "================================="
      );

      Serial.println(
        "WIFI RECONECTADO"
      );

      Serial.print(
        "IP: "
      );

      Serial.println(
        WiFi.localIP()
      );

      Serial.println(
        "================================="
      );

      mostrarLCD(
        "WIFI CONECTADO",
        "Conexion restaurada",
        "IP:",
        WiFi.localIP().toString()
      );

      delay(2000);

      // ------------------------------------------------------
      // ENVIAR ECG PENDIENTES
      // ------------------------------------------------------

      procesarColaPendientes();

      // ------------------------------------------------------
      // VOLVER AL ESTADO NORMAL
      // ------------------------------------------------------

      mostrarEstadoListo();

    } else {

      // ======================================================
      // WIFI DESCONECTADO
      // ======================================================

      Serial.println();

      Serial.println(
        "================================="
      );

      Serial.println(
        "WIFI DESCONECTADO"
      );

      Serial.println(
        "MODO OFFLINE"
      );

      Serial.println(
        "Los ECG se guardaran en PSRAM"
      );

      Serial.println(
        "================================="
      );

      mostrarLCD(
        "SIN CONEXION",
        "Modo offline",
        "ECG en PSRAM",
        "Esperando WiFi..."
      );
    }
  }

  // ----------------------------------------------------------
  // ACTUALIZAR ESTADO
  // ----------------------------------------------------------

  hayInternet =
    estadoActual;

  // ----------------------------------------------------------
  // RECONEXION AUTOMATICA
  // ----------------------------------------------------------

  if (
    !estadoActual
  ) {

    unsigned long ahora =
      millis();

    if (
      ahora -
      ultimoIntentoWifi >=
      INTERVALO_RECONEXION_WIFI
    ) {

      ultimoIntentoWifi =
        ahora;

      Serial.println(
        "Intentando reconectar WiFi..."
      );

      WiFi.reconnect();
    }
  }
}

// ============================================================
// VERIFICAR WIFI
// ============================================================

bool verificarConexion() {

  hayInternet =
    (
      WiFi.status() ==
      WL_CONNECTED
    );

  return hayInternet;
}

// ============================================================
// INICIALIZAR PSRAM
// ============================================================

bool inicializarPSRAM() {

  Serial.println();

  Serial.println(
    "Comprobando PSRAM..."
  );

  mostrarLCD(
    "PULSONET",
    "Comprobando PSRAM..."
  );

  if (
    !psramFound()
  ) {

    Serial.println(
      "ERROR: PSRAM no detectada"
    );

    return false;
  }

  size_t psramTotal =
    ESP.getPsramSize();

  size_t psramLibre =
    ESP.getFreePsram();

  Serial.println();

  Serial.println(
    "===== INFORMACION PSRAM ====="
  );

  Serial.printf(
    "PSRAM total : %lu bytes\n",
    (unsigned long)
      psramTotal
  );

  Serial.printf(
    "PSRAM total : %.2f MB\n",
    psramTotal /
    1024.0 /
    1024.0
  );

  Serial.printf(
    "PSRAM libre : %lu bytes\n",
    (unsigned long)
      psramLibre
  );

  Serial.printf(
    "PSRAM libre : %.2f MB\n",
    psramLibre /
    1024.0 /
    1024.0
  );

  Serial.printf(
    "Necesaria   : %.2f MB\n",
    PSRAM_NECESARIA /
    1024.0 /
    1024.0
  );

  Serial.println(
    "============================="
  );

  if (
    psramTotal <
    PSRAM_NECESARIA
  ) {

    Serial.println(
      "ERROR: PSRAM total insuficiente"
    );

    Serial.printf(
      "Necesaria: %lu KB\n",
      (unsigned long)(
        PSRAM_NECESARIA /
        1024
      )
    );

    Serial.printf(
      "Disponible: %lu KB\n",
      (unsigned long)(
        psramTotal /
        1024
      )
    );

    return false;
  }

  Serial.println(
    "Reservando buffer HL7..."
  );

  hl7Buffer =
    (char*)ps_malloc(
      BUFFER_SIZE
    );

  if (
    hl7Buffer == nullptr
  ) {

    Serial.println(
      "ERROR: No se pudo reservar buffer HL7"
    );

    return false;
  }

  hl7Buffer[0] =
    '\0';

  Serial.printf(
    "Buffer HL7 reservado: %lu KB\n",
    (unsigned long)(
      BUFFER_SIZE /
      1024
    )
  );

  Serial.printf(
    "PSRAM libre despues del buffer: %lu KB\n",
    (unsigned long)(
      ESP.getFreePsram() /
      1024
    )
  );

  return true;
}

// ============================================================
// INICIALIZAR COLA ECG
// ============================================================

bool inicializarColaECG() {

  Serial.println();

  Serial.println(
    "Inicializando 2 slots ECG..."
  );

  for (
    int i = 0;
    i < MAX_ECG_PENDIENTES;
    i++
  ) {

    colaECG[i].ocupado =
      false;

    colaECG[i].enviado =
      false;

    colaECG[i].paciente[0] =
      '\0';

    colaECG[i].ecgId[0] =
      '\0';

    colaECG[i].fecha[0] =
      '\0';

    Serial.printf(
      "Reservando slot %d...\n",
      i + 1
    );

    colaECG[i].pdfBase64 =
      (char*)ps_malloc(
        ECG_SLOT_SIZE
      );

    if (
      colaECG[i].pdfBase64 ==
      nullptr
    ) {

      Serial.printf(
        "ERROR: No se pudo alocar PSRAM para slot %d\n",
        i + 1
      );

      Serial.printf(
        "PSRAM libre: %lu KB\n",
        (unsigned long)(
          ESP.getFreePsram() /
          1024
        )
      );

      for (
        int j = 0;
        j < i;
        j++
      ) {

        if (
          colaECG[j].pdfBase64
        ) {

          free(
            colaECG[j].pdfBase64
          );

          colaECG[j].pdfBase64 =
            nullptr;
        }
      }

      return false;
    }

    colaECG[i].pdfBase64[0] =
      '\0';

    Serial.printf(
      "Slot %d OK - %lu KB\n",
      i + 1,
      (unsigned long)(
        ECG_SLOT_SIZE /
        1024
      )
    );
  }

  totalPendientes =
    0;

  Serial.println(
    "2 slots ECG inicializados correctamente"
  );

  return true;
}

// ============================================================
// RECIBIR HL7
// ============================================================

void recibirHL7Serial() {

  Serial.println();

  Serial.println(
    "Esperando HL7 por Serial..."
  );

  Serial.println(
    "Pegar HL7 completo"
  );

  Serial.println(
    "Finaliza automaticamente"
  );

  if (hayInternet) {

    mostrarLCD(
      "ESPERANDO HL7",
      "WiFi: CONECTADO",
      "Ingrese ECG...",
      "--------------------"
    );

  } else {

    mostrarLCD(
      "ESPERANDO HL7",
      "SIN CONEXION",
      "Se guardara en PSRAM",
      "--------------------"
    );
  }

  memset(
    hl7Buffer,
    0,
    BUFFER_SIZE
  );

  size_t pos =
    0;

  // ==========================================================
  // ESPERAR DATOS SIN BLOQUEAR WIFI
  // ==========================================================

  unsigned long ultimaComprobacion =
    millis();

  while (
    !Serial.available()
  ) {

    if (
      millis() -
      ultimaComprobacion >=
      500
    ) {

      ultimaComprobacion =
        millis();

      gestionarWifi();

      if (hayInternet) {

        procesarColaPendientes();
      }
    }

    delay(10);
  }

  Serial.println(
    "Recibiendo datos..."
  );

  mostrarLCD(
    "RECIBIENDO HL7",
    "Datos en proceso...",
    "Espere...",
    "--------------------"
  );

  unsigned long ultimoDato =
    millis();

  ultimaComprobacion =
    millis();

  while (true) {

    // --------------------------------------------------------
    // LEER DATOS
    // --------------------------------------------------------

    while (
      Serial.available()
    ) {

      char c =
        Serial.read();

      if (
        pos <
        BUFFER_SIZE - 1
      ) {

        hl7Buffer[pos++] =
          c;

      } else {

        Serial.println(
          "ERROR: Buffer HL7 lleno"
        );

        Serial.printf(
          "Limite: %lu bytes\n",
          (unsigned long)
            BUFFER_SIZE
        );

        mostrarLCD(
          "ERROR",
          "BUFFER LLENO",
          "Limite: 1 MB",
          "ECG no procesado"
        );

        return;
      }

      ultimoDato =
        millis();
    }

    // --------------------------------------------------------
    // COMPROBAR WIFI MIENTRAS RECIBE
    // --------------------------------------------------------

    if (
      millis() -
      ultimaComprobacion >=
      500
    ) {

      ultimaComprobacion =
        millis();

      gestionarWifi();
    }

    // --------------------------------------------------------
    // FIN DE TRAMA
    // --------------------------------------------------------

    if (
      millis() -
      ultimoDato >
      3000
    ) {

      hl7Buffer[pos] =
        '\0';

      Serial.println();

      Serial.println(
        "===== TRAMA HL7 ====="
      );

      Serial.printf(
        "Tamano: %lu bytes\n",
        (unsigned long)pos
      );

      Serial.printf(
        "Tamano: %.2f KB\n",
        pos / 1024.0
      );

      Serial.printf(
        "Tamano: %.2f MB\n",
        pos / 1024.0 /
        1024.0
      );

      Serial.println(
        "====================="
      );

      mostrarLCD(
        "HL7 RECIBIDO",
        String(pos) +
          " bytes",
        String(
          pos / 1024.0,
          1
        ) + " KB",
        "Procesando..."
      );

      delay(1500);

      return;
    }

    delay(1);
  }
}

// ============================================================
// PROCESAR TRAMA
// ============================================================

void procesarTrama() {

  Serial.println();

  Serial.println(
    "Procesando trama HL7..."
  );

  mostrarLCD(
    "PROCESANDO HL7",
    "Extrayendo datos...",
    "Paciente / ID / Fecha",
    "--------------------"
  );

  delay(1000);

  String paciente =
    extraerCampo(
      "PID",
      5
    );

  String fecha =
    extraerCampo(
      "OBR",
      7
    );

  String ecgId =
    extraerCampo(
      "OBR",
      3
    );

  paciente.replace(
    "^",
    "_"
  );

  Serial.printf(
    "Paciente : %s\n",
    paciente.c_str()
  );

  Serial.printf(
    "ECG ID   : %s\n",
    ecgId.c_str()
  );

  Serial.printf(
    "Fecha    : %s\n",
    fecha.c_str()
  );

  // ==========================================================
  // PDF
  // ==========================================================

  Serial.println(
    "Buscando PDF..."
  );

  mostrarLCD(
    "BUSCANDO PDF",
    "Analizando HL7...",
    "Buscando Base64...",
    "--------------------"
  );

  delay(800);

  char* pdfBase64 =
    extraerBase64PDF(
      hl7Buffer
    );

  if (
    pdfBase64 ==
    nullptr
  ) {

    Serial.println(
      "ERROR: PDF no encontrado"
    );

    mostrarLCD(
      "ERROR",
      "PDF NO ENCONTRADO",
      "ECG no procesado",
      "--------------------"
    );

    delay(3000);

    return;
  }

  // ==========================================================
  // TAMANO PDF BASE64
  // ==========================================================

  size_t base64Length =
    strlen(
      pdfBase64
    );

  Serial.println();

  Serial.println(
    "===== PDF ====="
  );

  Serial.printf(
    "Base64: %lu bytes\n",
    (unsigned long)
      base64Length
  );

  Serial.printf(
    "Base64: %.2f KB\n",
    base64Length /
    1024.0
  );

  Serial.printf(
    "PDF aproximado: %.2f KB\n",
    base64Length *
    0.75 /
    1024.0
  );

  Serial.println(
    "==============="
  );

  // ==========================================================
  // COMPROBAR SLOT
  // ==========================================================

  if (
    base64Length >=
    ECG_SLOT_SIZE
  ) {

    Serial.println(
      "ERROR: ECG demasiado grande"
    );

    mostrarLCD(
      "ERROR",
      "ECG DEMASIADO GRANDE",
      "Max 1 MB Base64",
      "No guardado"
    );

    delay(3000);

    return;
  }

  // ==========================================================
  // GUARDAR
  // ==========================================================

  int slot =
    guardarECGPendiente(
      paciente,
      ecgId,
      fecha,
      pdfBase64
    );

  if (
    slot == -1
  ) {

    Serial.println();

    Serial.println(
      "================================="
    );

    Serial.println(
      "LOS 2 SLOTS ESTAN OCUPADOS"
    );

    Serial.println(
      "No se puede guardar otro ECG"
    );

    Serial.println(
      "================================="
    );

    mostrarLCD(
      "MEMORIA LLENA",
      "2 ECG PENDIENTES",
      "No hay slots libres",
      "Enviar pendientes"
    );

    delay(3000);

    return;
  }

  Serial.printf(
    "ECG guardado en slot %d/%d\n",
    slot + 1,
    MAX_ECG_PENDIENTES
  );

  Serial.printf(
    "Pendientes: %d/%d\n",
    totalPendientes,
    MAX_ECG_PENDIENTES
  );

  mostrarLCD(
    "ECG GUARDADO",
    "En memoria PSRAM",
    "Slot " +
      String(slot + 1) +
      "/" +
      String(MAX_ECG_PENDIENTES),
    hayInternet
      ? "Enviando..."
      : "SIN CONEXION"
  );

  delay(2000);

  // ==========================================================
  // COMPROBAR WIFI
  // ==========================================================

  gestionarWifi();

  if (hayInternet) {

    enviarECGAFirebase(
      slot
    );

    // Volver al estado normal
    mostrarEstadoListo();

  } else {

    Serial.println(
      "Sin conexion."
    );

    Serial.println(
      "ECG queda almacenado en PSRAM."
    );

    mostrarLCD(
      "SIN CONEXION",
      "ECG guardado OK",
      "Se enviara cuando",
      "vuelva WiFi"
    );

    delay(2500);
  }
}

// ============================================================
// GUARDAR ECG
// ============================================================

int guardarECGPendiente(
  String paciente,
  String ecgId,
  String fecha,
  const char* pdfBase64
) {

  for (
    int i = 0;
    i < MAX_ECG_PENDIENTES;
    i++
  ) {

    if (
      !colaECG[i].ocupado
    ) {

      strncpy(
        colaECG[i].paciente,
        paciente.c_str(),
        sizeof(
          colaECG[i].paciente
        ) - 1
      );

      colaECG[i].paciente[
        sizeof(
          colaECG[i].paciente
        ) - 1
      ] = '\0';

      strncpy(
        colaECG[i].ecgId,
        ecgId.c_str(),
        sizeof(
          colaECG[i].ecgId
        ) - 1
      );

      colaECG[i].ecgId[
        sizeof(
          colaECG[i].ecgId
        ) - 1
      ] = '\0';

      strncpy(
        colaECG[i].fecha,
        fecha.c_str(),
        sizeof(
          colaECG[i].fecha
        ) - 1
      );

      colaECG[i].fecha[
        sizeof(
          colaECG[i].fecha
        ) - 1
      ] = '\0';

      size_t longitud =
        strlen(
          pdfBase64
        );

      if (
        longitud >=
        ECG_SLOT_SIZE
      ) {

        return -1;
      }

      memcpy(
        colaECG[i].pdfBase64,
        pdfBase64,
        longitud + 1
      );

      colaECG[i].ocupado =
        true;

      colaECG[i].enviado =
        false;

      totalPendientes++;

      return i;
    }
  }

  return -1;
}

// ============================================================
// LIBERAR SLOT
// ============================================================

void liberarSlotECG(
  int index
) {

  if (
    index < 0 ||
    index >= MAX_ECG_PENDIENTES
  ) {

    return;
  }

  colaECG[index].ocupado =
    false;

  colaECG[index].enviado =
    false;

  colaECG[index].paciente[0] =
    '\0';

  colaECG[index].ecgId[0] =
    '\0';

  colaECG[index].fecha[0] =
    '\0';

  if (
    colaECG[index].pdfBase64
  ) {

    colaECG[index].pdfBase64[0] =
      '\0';
  }

  if (
    totalPendientes > 0
  ) {

    totalPendientes--;
  }

  Serial.printf(
    "Slot %d liberado\n",
    index + 1
  );

  Serial.printf(
    "Pendientes actuales: %d/%d\n",
    totalPendientes,
    MAX_ECG_PENDIENTES
  );
}

// ============================================================
// PROCESAR COLA
// ============================================================

void procesarColaPendientes() {

  if (
    !hayInternet
  ) {

    return;
  }

  int pendientes =
    0;

  for (
    int i = 0;
    i < MAX_ECG_PENDIENTES;
    i++
  ) {

    if (
      colaECG[i].ocupado &&
      !colaECG[i].enviado
    ) {

      pendientes++;
    }
  }

  if (
    pendientes == 0
  ) {

    return;
  }

  Serial.printf(
    "Hay %d ECG pendientes.\n",
    pendientes
  );

  mostrarLCD(
    "CONEXION DETECTADA",
    "Enviando pendientes",
    String(pendientes) +
      " ECG en cola",
    "Espere..."
  );

  delay(1500);

  for (
    int i = 0;
    i < MAX_ECG_PENDIENTES;
    i++
  ) {

    if (
      colaECG[i].ocupado &&
      !colaECG[i].enviado
    ) {

      // ------------------------------------------------------
      // Comprobar WiFi antes de cada ECG
      // ------------------------------------------------------

      gestionarWifi();

      if (
        !hayInternet
      ) {

        Serial.println(
          "Conexion perdida durante envio."
        );

        mostrarLCD(
          "SIN CONEXION",
          "Envio detenido",
          "ECG en PSRAM",
          "Esperando WiFi..."
        );

        return;
      }

      bool enviado =
        enviarECGAFirebase(
          i
        );

      if (
        !enviado
      ) {

        Serial.println(
          "ECG queda pendiente."
        );

        return;
      }

      delay(500);
    }
  }

  // ----------------------------------------------------------
  // TODOS LOS PENDIENTES ENVIADOS
  // ----------------------------------------------------------

  Serial.println(
    "Todos los ECG pendientes fueron procesados."
  );

  mostrarEstadoListo();
}

// ============================================================
// ENVIAR A FIREBASE
// ============================================================

bool enviarECGAFirebase(
  int index
) {

  if (
    index < 0 ||
    index >= MAX_ECG_PENDIENTES
  ) {

    return false;
  }

  ECGPendiente &ecg =
    colaECG[index];

  if (
    !ecg.ocupado
  ) {

    return false;
  }

  if (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    hayInternet =
      false;

    Serial.println(
      "WiFi desconectado."
    );

    mostrarLCD(
      "SIN CONEXION",
      "ECG queda en PSRAM",
      "Esperando WiFi...",
      "--------------------"
    );

    return false;
  }

  Serial.println();

  Serial.println(
    "===== ENVIANDO ECG ====="
  );

  Serial.printf(
    "Slot: %d\n",
    index + 1
  );

  Serial.printf(
    "Paciente: %s\n",
    ecg.paciente
  );

  Serial.printf(
    "ECG ID: %s\n",
    ecg.ecgId
  );

  Serial.printf(
    "Base64: %lu bytes\n",
    (unsigned long)
      strlen(
        ecg.pdfBase64
      )
  );

  Serial.println(
    "========================"
  );

  mostrarLCD(
    "FIRESTORE",
    "Enviando ECG...",
    "ID: " +
      String(ecg.ecgId),
    "Espere..."
  );

  // ==========================================================
  // DOCUMENT ID
  // ==========================================================

  String docId =
    String(ecg.paciente) +
    "_" +
    String(ecg.ecgId);

  docId.replace(
    " ",
    "_"
  );

  docId.replace(
    "^",
    "_"
  );

  docId.replace(
    "/",
    "_"
  );

  docId.replace(
    "\\",
    "_"
  );

  // ==========================================================
  // JSON
  // ==========================================================

  String json;

  json.reserve(
    strlen(
      ecg.pdfBase64
    ) +
    1000
  );

  json =
    "{\"fields\":{";

  json +=
    "\"paciente\":{\"stringValue\":\"";

  json +=
    String(ecg.paciente);

  json +=
    "\"},";

  json +=
    "\"ecg_id\":{\"stringValue\":\"";

  json +=
    String(ecg.ecgId);

  json +=
    "\"},";

  json +=
    "\"fecha\":{\"stringValue\":\"";

  json +=
    String(ecg.fecha);

  json +=
    "\"},";

  json +=
    "\"pdf_base64\":{\"stringValue\":\"";

  json +=
    String(ecg.pdfBase64);

  json +=
    "\"}";

  json +=
    "}}";

  Serial.printf(
    "Tamano JSON: %lu bytes\n",
    (unsigned long)
      json.length()
  );

  // ==========================================================
  // HTTP
  // ==========================================================

  HTTPClient http;

  String url =
    String(FIREBASE_URL) +
    "?documentId=" +
    docId;

  http.begin(url);

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  http.setTimeout(
    60000
  );

  Serial.println(
    "Enviando POST..."
  );

  int httpCode =
    http.POST(
      json
    );

  bool exito =
    false;

  if (
    httpCode > 0
  ) {

    Serial.printf(
      "HTTP Code: %d\n",
      httpCode
    );

    String response =
      http.getString();

    Serial.println(
      response
    );

    if (
      httpCode == 200 ||
      httpCode == 201
    ) {

      Serial.println(
        "ECG enviado correctamente!"
      );

      mostrarLCD(
        "ECG ENVIADO",
        "CORRECTAMENTE",
        "ID: " +
          String(ecg.ecgId),
        "Firestore OK"
      );

      exito =
        true;

      delay(2500);

      liberarSlotECG(
        index
      );

    } else {

      Serial.println(
        "ERROR: Firestore rechazo"
      );

      mostrarLCD(
        "ERROR FIRESTORE",
        "HTTP: " +
          String(httpCode),
        "Queda pendiente",
        "en PSRAM"
      );

      delay(2500);
    }

  } else {

    Serial.printf(
      "ERROR HTTP: %s\n",
      http.errorToString(
        httpCode
      ).c_str()
    );

    mostrarLCD(
      "ERROR HTTP",
      "Queda pendiente",
      "ECG en PSRAM",
      "Reintentando..."
    );

    delay(2500);
  }

  http.end();

  return exito;
}

// ============================================================
// EXTRAER PDF BASE64
// ============================================================

char* extraerBase64PDF(
  char* hl7
) {

  char* inicio =
    strstr(
      hl7,
      "JVBERi0"
    );

  if (
    inicio == nullptr
  ) {

    Serial.println(
      "Firma PDF no encontrada"
    );

    return nullptr;
  }

  char* fin =
    inicio;

  while (
    *fin != '\0' &&
    *fin != '|' &&
    *fin != '\r' &&
    *fin != '\n' &&
    *fin != 0x1C
  ) {

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
  int campoNum
) {

  char patron[10];

  sprintf(
    patron,
    "%s|",
    segmento
  );

  char* inicio =
    strstr(
      hl7Buffer,
      patron
    );

  if (
    inicio == nullptr
  ) {

    return "DESCONOCIDO";
  }

  inicio +=
    strlen(
      patron
    );

  int campoActual =
    1;

  char temp[256] =
    { 0 };

  int i =
    0;

  while (
    *inicio &&
    *inicio != '\r' &&
    *inicio != '\n'
  ) {

    if (
      *inicio == '|'
    ) {

      campoActual++;

      inicio++;

      continue;
    }

    if (
      campoActual ==
      campoNum
    ) {

      if (
        i < 255
      ) {

        temp[i++] =
          *inicio;
      }
    }

    inicio++;
  }

  temp[i] =
    '\0';

  if (
    strlen(temp) == 0
  ) {

    return "DESCONOCIDO";
  }

  return String(temp);
}

// ============================================================
// WIFI - CONEXION INICIAL
// ============================================================

void conectarWifi(
  bool mostrarEnLCD
) {

  Serial.printf(
    "Conectando WiFi: %s",
    WIFI_SSID
  );

  WiFi.mode(
    WIFI_STA
  );

  WiFi.setAutoReconnect(
    true
  );

  WiFi.persistent(
    false
  );

  if (
    mostrarEnLCD
  ) {

    mostrarLCD(
      "CONECTANDO WIFI",
      WIFI_SSID,
      "Espere...",
      "--------------------"
    );
  }

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  int intentos =
    0;

  while (
    WiFi.status() !=
      WL_CONNECTED &&
    intentos < 20
  ) {

    delay(500);

    Serial.print(
      "."
    );

    intentos++;

    if (
      mostrarEnLCD
    ) {

      mostrarLCD(
        "CONECTANDO WIFI",
        WIFI_SSID,
        "Intento: " +
          String(intentos) +
          "/20",
        "Espere..."
      );
    }
  }

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    hayInternet =
      true;

    estadoWifiAnterior =
      true;

    ultimoIntentoWifi =
      millis();

    Serial.println(
      "\nWiFi conectado"
    );

    Serial.print(
      "IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.printf(
      "Heap libre: %u bytes\n",
      ESP.getFreeHeap()
    );

    Serial.printf(
      "PSRAM libre: %lu KB\n",
      (unsigned long)(
        ESP.getFreePsram() /
        1024
      )
    );

    if (
      mostrarEnLCD
    ) {

      mostrarLCD(
        "WIFI CONECTADO",
        "IP:",
        WiFi.localIP().toString(),
        "Conexion OK"
      );

      delay(2500);
    }

  } else {

    hayInternet =
      false;

    estadoWifiAnterior =
      false;

    Serial.println(
      "\nSIN WIFI - Modo offline"
    );

    if (
      mostrarEnLCD
    ) {

      mostrarLCD(
        "SIN CONEXION",
        "Modo offline activo",
        "Datos se guardan",
        "en PSRAM"
      );

      delay(3000);
    }
  }
}

