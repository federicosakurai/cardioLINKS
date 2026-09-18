#include <Arduino.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>

// ============================================================
// ETHERNET - W5500 (reemplaza al WiFi AP local)
// ============================================================

#define ETH_MOSI 23
#define ETH_MISO 19
#define ETH_SCLK 18
#define ETH_CS   13
#define ETH_RST  14

byte macAddress[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// IP fija del gateway (antes era la IP del AP: 192.168.4.1)
IPAddress ipGateway(192, 168, 4, 1);
IPAddress dnsGateway(192, 168, 4, 1);
IPAddress gatewayRed(192, 168, 4, 1);
IPAddress subnetRed(255, 255, 255, 0);

const uint16_t PUERTO_TCP = 2575;

EthernetServer servidorTCP(PUERTO_TCP);

bool ethernetConectado = false;

// ============================================================
// MONITOREO CONTINUO W5500 + RJ45
// ============================================================

bool w5500Detectado = false;
bool cableEthernetConectado = false;

bool estadoW5500Anterior = false;
bool estadoCableAnterior = false;
bool primeraLecturaEthernet = true;

unsigned long ultimaVerificacionEthernet = 0;
const unsigned long INTERVALO_VERIFICACION_ETH = 500;

// ============================================================
// MLLP
// ============================================================

#define MLLP_VT 0x0B
#define MLLP_FS 0x1C
#define MLLP_CR 0x0D

// ============================================================
// LCD
// ============================================================

#define LCD_SDA 21
#define LCD_SCL 22
#define LCD_ADDRESS 0x27

hd44780_I2Cexp lcd(LCD_ADDRESS);

// ============================================================
// MODEM A7670SA
// ============================================================

#define TINY_GSM_MODEM_A7670
#define TINY_GSM_RX_BUFFER 1024

#define MODEM_BAUDRATE     115200
#define MODEM_DTR_PIN      25
#define MODEM_TX_PIN       26
#define MODEM_RX_PIN       27

#define BOARD_PWRKEY_PIN   4
#define BOARD_POWERON_PIN  12

#define MODEM_RING_PIN     33
#define MODEM_RESET_PIN    5
#define MODEM_RESET_LEVEL  HIGH

#define SerialAT Serial1

#define SIM_PIN ""

const char* NETWORK_APN = "internet.ctimovil.com.ar";
const char* GPRS_USER   = "";
const char* GPRS_PASS   = "";

#include <TinyGsmClient.h>

#ifdef DUMP_AT_COMMANDS

#include <StreamDebugger.h>

StreamDebugger debugger(SerialAT, Serial);
TinyGsm modem(debugger);

#else

TinyGsm modem(SerialAT);

#endif

TinyGsmClient gsmClient(modem);

// ============================================================
// MUTEX MODEM
// ============================================================

SemaphoreHandle_t modemMutex;

// ============================================================
// FIREBASE
// ============================================================

const char* FIREBASE_HOST =
  "firestore.googleapis.com";

const char* FIREBASE_PATH_BASE =
  "/v1/projects/cardiolink-f186f/databases/(default)/documents/ecgs";

// ============================================================
// PSRAM
// ============================================================

#define BUFFER_SIZE (1280UL * 1024UL)

#define MAX_ECG_PENDIENTES 2

#define ECG_SLOT_SIZE (1200UL * 1024UL)

char* hl7Buffer = nullptr;

// ============================================================
// ECG
// ============================================================

struct ECGPendiente {

  bool ocupado;
  bool enviado;

  char paciente[128];
  char ecgId[64];
  char fecha[64];

  // Datos HL7 adicionales para Firebase
  char paciente_id[64];
  char fecha_nacimiento[32];
  char sexo[16];
  char sector[64];
  char cama[64];
  char frecuencia_cardiaca[32];
  char derivacion[64];
  char ritmo_automatico[128];
  char equipo[64];
  char estado_resultado[32];

  char* leadData;   // antes: pdfBase64. Ahora guarda los valores numericos del ECG lead (OBX tipo NA)
};

ECGPendiente colaECG[MAX_ECG_PENDIENTES];

int totalPendientes = 0;

// ============================================================
// LCD QUEUE
// ============================================================

struct MensajeLCD {

  char l1[21];
  char l2[21];
  char l3[21];
  char l4[21];

  int numLineas;
};

QueueHandle_t colaLCD;

TaskHandle_t taskLCDHandle;

// ============================================================
// RED 4G
// ============================================================

bool hayInternet = false;
bool estadoRedAnterior = false;

unsigned long ultimoIntentoRed = 0;

const unsigned long INTERVALO_RECONEXION_RED = 5000;

unsigned long inicioSinRed = 0;
bool contandoSinRed = false;

const unsigned long TIMEOUT_RESET_MODEM = 180000;

unsigned long ultimoReencendido = 0;

const unsigned long COOLDOWN_REENCENDIDO = 30000;


// ============================================================
// ESTADO
// ============================================================

bool huboAlMenosUnECG = false;

// ============================================================
// DELIMITADORES HL7
// ============================================================

struct DelimitadoresHL7 {

  char campo;
  char componente;
  char repeticion;
  char escape;
  char subcomponente;
};

DelimitadoresHL7 delims = {
  '|',
  '^',
  '~',
  '\\',
  '&'
};

// ============================================================
// PROTOTIPOS
// ============================================================

void inicializarEthernet();

void gestionarEthernet();

void inicializarModem();

bool conectarRed(bool mostrarEnLCD);

bool verificarConexion();

bool modemVivo();

void gestionarRed();

bool inicializarPSRAM();

bool inicializarColaECG();

bool recibirHL7PorTCP();

void procesarTrama();

bool leerDelimitadores(const char* hl7);

String extraerComponente(
  const String& campo,
  int numComponente
);

String extraerCampoDeSegmento(
  const char* segmento,
  int numCampo
);

String extraerCampoHL7(
  const char* nombreSegmento,
  int numCampo,
  int numComponente
);

String extraerCampoOBXPorIdentificador(
  const char* identificador,
  int numCampo,
  int numComponente
);

// ============================================================
// EXTRACCION DE DATOS DE ECG LEAD (NA)
// ============================================================

bool copiarComponenteDirecto(
  const char* segmento,
  int numCampo,
  int numComponente,
  char* destino,
  size_t capacidad,
  size_t& longitud
);

bool extraerLeadECGAlSlot(
  char* hl7,
  char* destino,
  size_t capacidad
);

// ============================================================
// ECG
// ============================================================

void liberarSlotECG(int index);

void procesarColaPendientes();

void mostrarEstadoListo();

void tareaLCD(void* parametro);

int buscarSlotLibre();

// ============================================================
// JSON / FIREBASE
// ============================================================

size_t calcularJSONSize(
  ECGPendiente& ecg
);

size_t copiarJSONEscapado(
  char* destino,
  size_t capacidad,
  const char* origen
);

bool construirJSONEnPSRAM(
  ECGPendiente& ecg,
  char* json,
  size_t capacidad,
  size_t& longitud
);

bool enviarECGAFirebase(
  int index
);

// ============================================================
// LCD
// ============================================================

void limpiarLineaLCD(int linea) {

  lcd.setCursor(0, linea);
  lcd.print("                    ");
}

// ============================================================

void escribirLineaLCD(
  int linea,
  const char* texto
) {

  limpiarLineaLCD(linea);

  lcd.setCursor(0, linea);

  lcd.print(texto);
}

// ============================================================
// MOSTRAR LCD
// ============================================================

void mostrarLCD(String linea1) {

  MensajeLCD msg;

  strncpy(
    msg.l1,
    linea1.c_str(),
    sizeof(msg.l1) - 1
  );

  msg.l1[sizeof(msg.l1) - 1] = '\0';

  msg.l2[0] = '\0';
  msg.l3[0] = '\0';
  msg.l4[0] = '\0';

  msg.numLineas = 1;

  xQueueSend(
    colaLCD,
    &msg,
    0
  );
}

// ============================================================

void mostrarLCD(
  String linea1,
  String linea2
) {

  MensajeLCD msg;

  strncpy(
    msg.l1,
    linea1.c_str(),
    sizeof(msg.l1) - 1
  );

  msg.l1[sizeof(msg.l1) - 1] = '\0';

  strncpy(
    msg.l2,
    linea2.c_str(),
    sizeof(msg.l2) - 1
  );

  msg.l2[sizeof(msg.l2) - 1] = '\0';

  msg.l3[0] = '\0';
  msg.l4[0] = '\0';

  msg.numLineas = 2;

  xQueueSend(
    colaLCD,
    &msg,
    0
  );
}

// ============================================================

void mostrarLCD(
  String linea1,
  String linea2,
  String linea3
) {

  MensajeLCD msg;

  strncpy(
    msg.l1,
    linea1.c_str(),
    sizeof(msg.l1) - 1
  );

  msg.l1[sizeof(msg.l1) - 1] = '\0';

  strncpy(
    msg.l2,
    linea2.c_str(),
    sizeof(msg.l2) - 1
  );

  msg.l2[sizeof(msg.l2) - 1] = '\0';

  strncpy(
    msg.l3,
    linea3.c_str(),
    sizeof(msg.l3) - 1
  );

  msg.l3[sizeof(msg.l3) - 1] = '\0';

  msg.l4[0] = '\0';

  msg.numLineas = 3;

  xQueueSend(
    colaLCD,
    &msg,
    0
  );
}

// ============================================================

void mostrarLCD(
  String linea1,
  String linea2,
  String linea3,
  String linea4
) {

  MensajeLCD msg;

  strncpy(
    msg.l1,
    linea1.c_str(),
    sizeof(msg.l1) - 1
  );

  msg.l1[sizeof(msg.l1) - 1] = '\0';

  strncpy(
    msg.l2,
    linea2.c_str(),
    sizeof(msg.l2) - 1
  );

  msg.l2[sizeof(msg.l2) - 1] = '\0';

  strncpy(
    msg.l3,
    linea3.c_str(),
    sizeof(msg.l3) - 1
  );

  msg.l3[sizeof(msg.l3) - 1] = '\0';

  strncpy(
    msg.l4,
    linea4.c_str(),
    sizeof(msg.l4) - 1
  );

  msg.l4[sizeof(msg.l4) - 1] = '\0';

  msg.numLineas = 4;

  xQueueSend(
    colaLCD,
    &msg,
    0
  );
}

// ============================================================
// ESTADO LISTO
// ============================================================

void mostrarEstadoListo() {

  String textoEspera =
    huboAlMenosUnECG
      ? "Esperando otro ECG"
      : "Esperando ECG";

  // ----------------------------------------------------------
  // PRIORIDAD 1: W5500 NO DETECTADO
  // ----------------------------------------------------------

  if (!w5500Detectado) {

    mostrarLCD(
      "cardioLink",
      "W5500 NO DETECTADO",
      hayInternet
        ? "4G: CONECTADO"
        : "4G: SIN CONEXION",
      "Revisar Ethernet"
    );

    return;
  }

  // ----------------------------------------------------------
  // PRIORIDAD 2: NO HAY LINK FISICO RJ45
  // ----------------------------------------------------------

  if (!cableEthernetConectado) {

    mostrarLCD(
      "cardioLink",
      "CABLE DESCONECTADO",
      hayInternet
        ? "4G: CONECTADO"
        : "4G: SIN CONEXION",
      "CONECTAR EL CABLE"
    );

    return;
  }

  // ----------------------------------------------------------
  // TODO OK PARA RECIBIR ECG
  // ----------------------------------------------------------

  mostrarLCD(
    "CABLE CONECTADO",
    "W5500: OK",
    hayInternet
      ? "4G: CONECTADO"
      : "4G: SIN CONEXION",
    textoEspera
  );
}

// ============================================================
// TAREA LCD
// ============================================================

void tareaLCD(void* parametro) {

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

      if (msg.numLineas >= 2) {

        escribirLineaLCD(
          1,
          msg.l2
        );
      }

      if (msg.numLineas >= 3) {

        escribirLineaLCD(
          2,
          msg.l3
        );
      }

      if (msg.numLineas >= 4) {

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
    "       cardioLink - MULTI ECG"
  );

  Serial.println(
    "       4G + Ethernet W5500"
  );

  Serial.println(
    " TTGO T-A7670SA ESP32-WROVER"
  );

  Serial.println(
    " Recepcion HL7 v2.5/2.6"
  );

  Serial.println(
    " MLLP/TCP"
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

    hd44780::fatalError(status);
  }

  lcd.backlight();

  Serial.println(
    "LCD I2C iniciado"
  );

  // ==========================================================
  // MUTEX
  // ==========================================================

  modemMutex =
    xSemaphoreCreateMutex();

  if (modemMutex == nullptr) {

    Serial.println(
      "ERROR: No se pudo crear mutex"
    );

    while (true) {
      delay(1000);
    }
  }

  // ==========================================================
  // COLA LCD
  // ==========================================================

  colaLCD =
    xQueueCreate(
      10,
      sizeof(MensajeLCD)
    );

  if (colaLCD == nullptr) {

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
    "cardioLink",
    "Iniciando sistema...",
    "TTGO T-A7670SA"
  );

  delay(2000);

  // ==========================================================
  // ETHERNET W5500
  // ==========================================================

  inicializarEthernet();

  // ==========================================================
  // PSRAM
  // ==========================================================

  if (!inicializarPSRAM()) {

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
  // SLOTS
  // ==========================================================

  if (!inicializarColaECG()) {

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
  // INFORMACION PSRAM
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
    "Memoria disponible",
    "2 ECG x 1.2 MB",
    "Sistema listo"
  );

  delay(2500);

  // ==========================================================
  // MODEM
  // ==========================================================

  inicializarModem();

  conectarRed(true);

  Serial.println();

  Serial.println(
    "Sistema listo."
  );

  Serial.printf(
    "IP gateway (Ethernet): %s\n",
    Ethernet.localIP().toString().c_str()
  );

  Serial.printf(
    "Puerto MLLP: %d\n",
    PUERTO_TCP
  );

  Serial.println();

  mostrarEstadoListo();

  delay(2500);
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // Mantener la pila Ethernet
  Ethernet.maintain();

  // Verificar permanentemente:
  // - que el W5500 siga detectado
  // - que el cable RJ45 tenga link
  // - reconexion automatica del servidor TCP
  gestionarEthernet();

  // Gestionar conexion 4G
  gestionarRed();

  // Si hay 4G, intentar enviar ECG pendientes
  if (hayInternet) {

    procesarColaPendientes();
  }

  // Solo recibir HL7 cuando:
  // - W5500 detectado
  // - cable RJ45 conectado
  if (
    w5500Detectado &&
    cableEthernetConectado
  ) {

    if (recibirHL7PorTCP()) {

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
    }
  }

  delay(200);
}

// ============================================================
// ETHERNET W5500
// ============================================================

void inicializarEthernet() {

  Serial.println();

  Serial.println(
    "Inicializando Ethernet W5500..."
  );

  mostrarLCD(
    "cardioLink",
    "Iniciando Ethernet...",
    "Espere..."
  );

  // ----------------------------------------------------------
  // RESET FISICO DEL MODULO W5500
  // ----------------------------------------------------------

  pinMode(
    ETH_RST,
    OUTPUT
  );

  digitalWrite(
    ETH_RST,
    LOW
  );

  delay(100);

  digitalWrite(
    ETH_RST,
    HIGH
  );

  delay(200);

  // ----------------------------------------------------------
  // SPI CON PINES PERSONALIZADOS
  // ----------------------------------------------------------

  SPI.begin(
    ETH_SCLK,
    ETH_MISO,
    ETH_MOSI,
    ETH_CS
  );

  Ethernet.init(
    ETH_CS
  );

  // ----------------------------------------------------------
  // IP FIJA
  // ----------------------------------------------------------

  Ethernet.begin(
    macAddress,
    ipGateway,
    dnsGateway,
    gatewayRed,
    subnetRed
  );

  delay(1000);

  // ----------------------------------------------------------
  // DETECTAR W5500
  // ----------------------------------------------------------

  w5500Detectado =
    (
      Ethernet.hardwareStatus() !=
      EthernetNoHardware
    );

  ethernetConectado =
    w5500Detectado;

  estadoW5500Anterior =
    w5500Detectado;

  if (!w5500Detectado) {

    Serial.println(
      "ERROR: No se detecto el modulo W5500"
    );

    cableEthernetConectado =
      false;

    estadoCableAnterior =
      false;

    mostrarLCD(
      "cardioLink",
      "W5500 NO DETECTADO",
      "Revisar cableado",
      "Ethernet ERROR"
    );

  } else {

    Serial.println(
      "Modulo W5500 detectado OK"
    );

    // --------------------------------------------------------
    // DETECTAR CABLE RJ45
    // --------------------------------------------------------

    cableEthernetConectado =
      (
        Ethernet.linkStatus() ==
        LinkON
      );

    estadoCableAnterior =
      cableEthernetConectado;

    if (cableEthernetConectado) {

      Serial.println(
        "CABLE ETHERNET CONECTADO"
      );

      mostrarLCD(
        "cardioLink",
        "CABLE CONECTADO",
        "W5500: OK",
        "Iniciando servidor"
      );

    } else {

      Serial.println(
        "CABLE ETHERNET DESCONECTADO"
      );

      mostrarLCD(
        "cardioLink",
        "CABLE DESCONECTADO",
        "W5500: OK",
        "CONECTAR EL CABLE"
      );
    }
  }

  // El servidor queda inicializado aunque inicialmente no haya
  // cable. Cuando el link vuelva, gestionarEthernet() vuelve a
  // ejecutar begin() para dejarlo listo para recibir HL7.
  servidorTCP.begin();

  Serial.println(
    "Servidor TCP Ethernet iniciado"
  );

  Serial.print(
    "IP del gateway: "
  );

  Serial.println(
    Ethernet.localIP()
  );

  Serial.printf(
    "Servidor TCP escuchando puerto %d\n",
    PUERTO_TCP
  );

  primeraLecturaEthernet =
    false;

  delay(1500);
}

// ============================================================
// GESTION CONTINUA W5500 + CABLE RJ45
// ============================================================

void gestionarEthernet() {

  unsigned long ahora =
    millis();

  if (
    ahora - ultimaVerificacionEthernet <
    INTERVALO_VERIFICACION_ETH
  ) {

    return;
  }

  ultimaVerificacionEthernet =
    ahora;

  // ----------------------------------------------------------
  // 1. VERIFICAR QUE EL W5500 SIGA RESPONDIENDO
  // ----------------------------------------------------------

  bool w5500Actual =
    (
      Ethernet.hardwareStatus() !=
      EthernetNoHardware
    );

  // ----------------------------------------------------------
  // CAMBIO DE ESTADO DEL W5500
  // ----------------------------------------------------------

  if (
    primeraLecturaEthernet ||
    w5500Actual != estadoW5500Anterior
  ) {

    estadoW5500Anterior =
      w5500Actual;

    w5500Detectado =
      w5500Actual;

    ethernetConectado =
      w5500Actual;

    Serial.println();

    Serial.println(
      "================================="
    );

    if (w5500Actual) {

      Serial.println(
        "W5500 DETECTADO"
      );

      Serial.println(
        "Comunicacion SPI OK"
      );

      // Si el modulo reaparecio, volver a dejar configurada
      // la interfaz y el servidor TCP.
      Ethernet.begin(
        macAddress,
        ipGateway,
        dnsGateway,
        gatewayRed,
        subnetRed
      );

      delay(50);

      servidorTCP.begin();

    } else {

      Serial.println(
        "W5500 NO DETECTADO"
      );

      Serial.println(
        "Revisar alimentacion / SPI"
      );

      cableEthernetConectado =
        false;

      estadoCableAnterior =
        false;
    }

    Serial.println(
      "================================="
    );

    mostrarEstadoListo();
  }

  w5500Detectado =
    w5500Actual;

  ethernetConectado =
    w5500Actual;

  // ----------------------------------------------------------
  // SI NO HAY W5500, NO CONSULTAR LINK
  // ----------------------------------------------------------

  if (!w5500Actual) {

    cableEthernetConectado =
      false;

    primeraLecturaEthernet =
      false;

    return;
  }

  // ----------------------------------------------------------
  // 2. VERIFICAR LINK FISICO DEL RJ45
  // ----------------------------------------------------------

  EthernetLinkStatus link =
    Ethernet.linkStatus();

  bool cableActual =
    (
      link == LinkON
    );

  // ----------------------------------------------------------
  // 3. DETECTAR DESCONEXION / RECONEXION
  // ----------------------------------------------------------

  if (
    primeraLecturaEthernet ||
    cableActual != estadoCableAnterior
  ) {

    bool estabaConectado =
      estadoCableAnterior;

    estadoCableAnterior =
      cableActual;

    cableEthernetConectado =
      cableActual;

    Serial.println();

    Serial.println(
      "================================="
    );

    if (cableActual) {

      Serial.println(
        "CABLE ETHERNET CONECTADO"
      );

      Serial.println(
        "LINK RJ45: ON"
      );

      // IMPORTANTE:
      // Al reconectar el cable se vuelve a iniciar el servidor
      // para garantizar que quede escuchando nuevas tramas HL7.
      servidorTCP.begin();

      Serial.printf(
        "Servidor TCP listo en puerto %d\n",
        PUERTO_TCP
      );

      Serial.print(
        "IP Ethernet: "
      );

      Serial.println(
        Ethernet.localIP()
      );

      // Mostrar inmediatamente la reconexion.
      mostrarLCD(
        "cardioLink",
        "CABLE CONECTADO",
        "W5500: OK",
        hayInternet
          ? "4G: CONECTADO"
          : "4G: SIN CONEXION"
      );

      // Dar tiempo para que el usuario vea "CABLE CONECTADO".
      // Luego mostrar el estado normal de espera.
      delay(1200);

      mostrarEstadoListo();

    } else {

      Serial.println(
        "CABLE ETHERNET DESCONECTADO"
      );

      Serial.println(
        "LINK RJ45: OFF"
      );

      // En pantalla queda claramente indicado que hay que
      // volver a conectar el cable, junto al estado del 4G.
      mostrarLCD(
        "cardioLink",
        "CABLE DESCONECTADO",
        hayInternet
          ? "4G: CONECTADO"
          : "4G: SIN CONEXION",
        "CONECTAR EL CABLE"
      );
    }

    Serial.println(
      "================================="
    );
  }

  cableEthernetConectado =
    cableActual;

  primeraLecturaEthernet =
    false;
}

// ============================================================
// MODEM
// ============================================================

void inicializarModem() {

  Serial.println();

  Serial.println(
    "Inicializando modem 4G..."
  );

  mostrarLCD(
    "cardioLink",
    "Iniciando modem 4G...",
    "Espere..."
  );

  SerialAT.begin(
    MODEM_BAUDRATE,
    SERIAL_8N1,
    MODEM_RX_PIN,
    MODEM_TX_PIN
  );

  pinMode(
    BOARD_POWERON_PIN,
    OUTPUT
  );

  digitalWrite(
    BOARD_POWERON_PIN,
    HIGH
  );

  delay(1000);

  pinMode(
    MODEM_RESET_PIN,
    OUTPUT
  );

  digitalWrite(
    MODEM_RESET_PIN,
    !MODEM_RESET_LEVEL
  );

  delay(100);

  digitalWrite(
    MODEM_RESET_PIN,
    MODEM_RESET_LEVEL
  );

  delay(100);

  digitalWrite(
    MODEM_RESET_PIN,
    !MODEM_RESET_LEVEL
  );

  pinMode(
    BOARD_PWRKEY_PIN,
    OUTPUT
  );

  digitalWrite(
    BOARD_PWRKEY_PIN,
    LOW
  );

  delay(100);

  digitalWrite(
    BOARD_PWRKEY_PIN,
    HIGH
  );

  delay(1000);

  digitalWrite(
    BOARD_PWRKEY_PIN,
    LOW
  );

  Serial.println(
    "Esperando respuesta del modem..."
  );

  delay(8000);

  while (SerialAT.available()) {
    SerialAT.read();
  }

  int retry = 0;

  while (!modem.testAT(1000)) {

    Serial.print(".");

    if (retry++ > 10) {

      Serial.println();

      Serial.println(
        "Reintentando PWRKEY..."
      );

      digitalWrite(
        BOARD_PWRKEY_PIN,
        LOW
      );

      delay(100);

      digitalWrite(
        BOARD_PWRKEY_PIN,
        HIGH
      );

      delay(1000);

      digitalWrite(
        BOARD_PWRKEY_PIN,
        LOW
      );

      retry = 0;

      delay(5000);
    }
  }

  Serial.println();

  Serial.println(
    "Modem respondiendo OK"
  );

  Serial.println(
    "Comprobando SIM..."
  );

  unsigned long inicioSim =
    millis();

  SimStatus sim =
    SIM_ERROR;

  while (
    millis() - inicioSim < 15000
  ) {

    sim =
      modem.getSimStatus();

    if (sim == SIM_READY) {

      Serial.println(
        "SIM card lista"
      );

      break;
    }

    if (sim == SIM_LOCKED) {

      Serial.println(
        "SIM bloqueada"
      );

      modem.simUnlock(
        SIM_PIN
      );
    }

    delay(1000);
  }

  if (sim != SIM_READY) {

    Serial.println(
      "ADVERTENCIA: SIM no confirmo READY."
    );
  }

  modem.setNetworkMode(
    MODEM_NETWORK_AUTO
  );

  Serial.printf(
    "Configurando APN: %s\n",
    NETWORK_APN
  );

  modem.sendAT(
    GF("+CGDCONT=1,\"IP\",\""),
    NETWORK_APN,
    "\""
  );

  if (modem.waitResponse() != 1) {

    Serial.println(
      "ADVERTENCIA: fallo configurando APN"
    );
  }
}

// ============================================================
// CONECTAR 4G (CORREGIDO: con mutex + gprsDisconnect previo)
// ============================================================

bool conectarRed(bool mostrarEnLCD) {

  Serial.println();

  Serial.println(
    "Registrando en red celular..."
  );

  if (mostrarEnLCD) {

    mostrarLCD(
      "CONECTANDO 4G",
      "Buscando senal...",
      "Espere..."
    );
  }

  xSemaphoreTake(
    modemMutex,
    portMAX_DELAY
  );

  unsigned long inicio =
    millis();

  RegStatus status =
    REG_NO_RESULT;

  while (
    millis() - inicio < 60000
  ) {

    status =
      modem.getRegistrationStatus();

    if (
      status == REG_OK_HOME ||
      status == REG_OK_ROAMING
    ) {

      break;
    }

    if (status == REG_DENIED) {

      Serial.println(
        "Registro RECHAZADO."
      );

      break;
    }

    int16_t sq =
      modem.getSignalQuality();

    Serial.printf(
      "Buscando red... Senal: %d\n",
      sq
    );

    delay(1500);
  }

  bool registrado =
    (
      status == REG_OK_HOME ||
      status == REG_OK_ROAMING
    );

  if (!registrado) {

    hayInternet = false;

    Serial.println(
      "SIN REGISTRO DE RED"
    );

    xSemaphoreGive(
      modemMutex
    );

    return false;
  }

  Serial.println(
    "Registrado en red."
  );

  // ------------------------------------------------------------
  // FIX: limpiar cualquier contexto PDP previo antes de reconectar
  // ------------------------------------------------------------

  modem.gprsDisconnect();

  delay(300);

  bool gprsOK =
    modem.gprsConnect(
      NETWORK_APN,
      GPRS_USER,
      GPRS_PASS
    );

  if (!gprsOK) {

    hayInternet = false;

    Serial.println(
      "ERROR: no se pudo activar datos"
    );

    xSemaphoreGive(
      modemMutex
    );

    return false;
  }

  hayInternet = true;

  ultimoIntentoRed =
    millis();

  String ip =
    modem.getLocalIP();

  xSemaphoreGive(
    modemMutex
  );

  Serial.println(
    "Datos 4G activos"
  );

  Serial.print(
    "IP: "
  );

  Serial.println(ip);

  if (mostrarEnLCD) {

    mostrarLCD(
      "4G CONECTADO",
      "IP:",
      ip,
      "Conexion OK"
    );

    delay(2500);
  }

  return true;
}

// ============================================================
// VERIFICAR RED (CORREGIDO: con mutex)
// ============================================================

bool verificarConexion() {

  xSemaphoreTake(
    modemMutex,
    portMAX_DELAY
  );

  hayInternet =
    modem.isNetworkConnected() &&
    modem.isGprsConnected();

  xSemaphoreGive(
    modemMutex
  );

  return hayInternet;
}

// ============================================================
// MODEM VIVO
//
// Verifica si el modem sigue respondiendo a comandos AT.
// Si el modem se apago fisicamente (brownout, watchdog interno,
// perdida de alimentacion por picos de corriente al buscar red
// en zona sin senal), NINGUN comando AT (gprsConnect, etc.)
// va a funcionar, porque no hay nada del otro lado escuchando.
//
// Esto se detecta en campo por las luces rojas de la LilyGO
// que se apagan y no vuelven a prender solas.
// ============================================================

bool modemVivo() {

  xSemaphoreTake(
    modemMutex,
    portMAX_DELAY
  );

  bool vivo =
    modem.testAT(2000);

  xSemaphoreGive(
    modemMutex
  );

  return vivo;
}

// ============================================================
// GESTIONAR RED (CORREGIDO: gprsDisconnect antes de reintentar
// + watchdog de reencendido fisico del modem)
// ============================================================

void gestionarRed() {

  bool estadoActual =
    verificarConexion();

  if (
    estadoActual !=
    estadoRedAnterior
  ) {

    estadoRedAnterior =
      estadoActual;

    hayInternet =
      estadoActual;

    if (estadoActual) {

      contandoSinRed =
        false;

      Serial.println();

      Serial.println(
        "================================="
      );

      Serial.println(
        "RED 4G RECONECTADA"
      );

      xSemaphoreTake(
        modemMutex,
        portMAX_DELAY
      );

      String ipActual =
        modem.getLocalIP();

      xSemaphoreGive(
        modemMutex
      );

      Serial.print(
        "IP: "
      );

      Serial.println(
        ipActual
      );

      Serial.println(
        "================================="
      );

      mostrarLCD(
        "4G RECONECTADO",
        "Conexion restaurada",
        "IP:",
        ipActual
      );

      delay(2000);

      procesarColaPendientes();

      mostrarEstadoListo();

    } else {

      contandoSinRed =
        true;

      inicioSinRed =
        millis();

      Serial.println();

      Serial.println(
        "================================="
      );

      Serial.println(
        "RED 4G PERDIDA"
      );

      Serial.println(
        "MODO OFFLINE"
      );

      Serial.println(
        "ECG guardado en PSRAM"
      );

      Serial.println(
        "================================="
      );

      // Mostrar estado combinado Ethernet + 4G.
      // Si el cable esta desconectado, tiene prioridad
      // "CONECTAR EL CABLE".
      mostrarEstadoListo();
    }
  }

  hayInternet =
    estadoActual;

  if (!estadoActual) {

    // ----------------------------------------------------------
    // PASO 1: VERIFICAR SI EL MODEM SIGUE VIVO (responde AT)
    //
    // Si NO responde, no tiene sentido reintentar gprsConnect.
    // Hay que repetir la secuencia completa de encendido fisico
    // (BOARD_POWERON_PIN + PWRKEY), igual que en el setup().
    // ----------------------------------------------------------

    if (!modemVivo()) {

      unsigned long ahoraCooldown =
        millis();

      // ----------------------------------------------------
      // COOLDOWN: no reintentar el reencendido fisico antes
      // de que pase COOLDOWN_REENCENDIDO desde el ultimo intento.
      //
      // Sin esto, si el modem tarda en bootear despues de
      // inicializarModem(), el loop (cada ~200ms) lo va a
      // volver a llamar antes de que termine de arrancar,
      // entrando en un ciclo de reinicios sin darle tiempo
      // real de encender.
      // ----------------------------------------------------

      if (
        ahoraCooldown - ultimoReencendido <
        COOLDOWN_REENCENDIDO
      ) {

        return;
      }

      ultimoReencendido =
        ahoraCooldown;

      Serial.println();

      Serial.println(
        "========================================"
      );

      Serial.println(
        "MODEM NO RESPONDE (posible apagado fisico)"
      );

      Serial.println(
        "REENCENDIENDO MODEM COMPLETO..."
      );

      Serial.println(
        "========================================"
      );

      mostrarLCD(
        "MODEM APAGADO",
        "Reencendiendo...",
        "Espere...",
        ""
      );

      inicializarModem();

      conectarRed(false);

      ultimoIntentoRed =
        millis();

      inicioSinRed =
        millis();

      return;
    }

    // ----------------------------------------------------------
    // PASO 2: WATCHDOG POR TIEMPO
    //
    // El modem responde AT pero lleva demasiado tiempo sin
    // recuperar la red (contexto GPRS zombie que ni el
    // gprsDisconnect logra destrabar). Forzar reinicio completo.
    // ----------------------------------------------------------

    if (
      contandoSinRed &&
      (millis() - inicioSinRed >= TIMEOUT_RESET_MODEM)
    ) {

      Serial.println();

      Serial.println(
        "========================================"
      );

      Serial.println(
        "SIN RED PROLONGADO"
      );

      Serial.println(
        "REINICIANDO MODEM COMPLETO..."
      );

      Serial.println(
        "========================================"
      );

      mostrarLCD(
        "SIN SENAL 4G",
        "Reiniciando modem",
        "Espere...",
        ""
      );

      inicializarModem();

      conectarRed(false);

      inicioSinRed =
        millis();

      ultimoIntentoRed =
        millis();

      return;
    }

    // ----------------------------------------------------------
    // PASO 3: REINTENTO NORMAL (modem vivo, solo sin registro)
    // ----------------------------------------------------------

    unsigned long ahora =
      millis();

    if (
      ahora - ultimoIntentoRed >=
      INTERVALO_RECONEXION_RED
    ) {

      ultimoIntentoRed =
        ahora;

      Serial.println(
        "Intentando reconectar 4G..."
      );

      xSemaphoreTake(
        modemMutex,
        portMAX_DELAY
      );

      modem.gprsDisconnect();

      xSemaphoreGive(
        modemMutex
      );

      delay(500);

      conectarRed(false);
    }
  }
}

// ============================================================
// PSRAM
// ============================================================

bool inicializarPSRAM() {

  Serial.println();

  Serial.println(
    "Comprobando PSRAM..."
  );

  if (!psramFound()) {

    Serial.println(
      "ERROR: PSRAM no detectada"
    );

    return false;
  }

  size_t total =
    ESP.getPsramSize();

  size_t libre =
    ESP.getFreePsram();

  Serial.println();

  Serial.println(
    "===== INFORMACION PSRAM ====="
  );

  Serial.printf(
    "PSRAM total : %.2f MB\n",
    total / 1024.0 / 1024.0
  );

  Serial.printf(
    "PSRAM libre : %.2f MB\n",
    libre / 1024.0 / 1024.0
  );

  Serial.println(
    "============================="
  );

  Serial.println(
    "Reservando buffer HL7..."
  );

  hl7Buffer =
    (char*)ps_malloc(
      BUFFER_SIZE
    );

  if (hl7Buffer == nullptr) {

    Serial.println(
      "ERROR: No se pudo reservar buffer HL7"
    );

    return false;
  }

  hl7Buffer[0] = '\0';

  Serial.printf(
    "Buffer HL7 reservado: %lu KB\n",
    (unsigned long)(
      BUFFER_SIZE / 1024
    )
  );

  Serial.printf(
    "PSRAM libre despues: %lu KB\n",
    (unsigned long)(
      ESP.getFreePsram() / 1024
    )
  );

  return true;
}

// ============================================================
// SLOTS
// ============================================================

bool inicializarColaECG() {

  Serial.println();

  Serial.println(
    "Inicializando slots ECG..."
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

    colaECG[i].paciente_id[0] = '\0';
    colaECG[i].fecha_nacimiento[0] = '\0';
    colaECG[i].sexo[0] = '\0';
    colaECG[i].sector[0] = '\0';
    colaECG[i].cama[0] = '\0';
    colaECG[i].frecuencia_cardiaca[0] = '\0';
    colaECG[i].derivacion[0] = '\0';
    colaECG[i].ritmo_automatico[0] = '\0';
    colaECG[i].equipo[0] = '\0';
    colaECG[i].estado_resultado[0] = '\0';

    Serial.printf(
      "Reservando slot %d...\n",
      i + 1
    );

    colaECG[i].leadData =
      (char*)ps_malloc(
        ECG_SLOT_SIZE
      );

    if (
      colaECG[i].leadData ==
      nullptr
    ) {

      Serial.printf(
        "ERROR: No se pudo reservar slot %d\n",
        i + 1
      );

      return false;
    }

    colaECG[i].leadData[0] =
      '\0';

    Serial.printf(
      "Slot %d OK - %lu KB\n",
      i + 1,
      (unsigned long)(
        ECG_SLOT_SIZE / 1024
      )
    );
  }

  totalPendientes =
    0;

  return true;
}

// ============================================================
// BUSCAR SLOT LIBRE
// ============================================================

int buscarSlotLibre() {

  for (
    int i = 0;
    i < MAX_ECG_PENDIENTES;
    i++
  ) {

    if (!colaECG[i].ocupado) {

      return i;
    }
  }

  return -1;
}

// ============================================================
// RECIBIR HL7 MLLP
// ============================================================

bool recibirHL7PorTCP() {

  EthernetClient cliente =
    servidorTCP.available();

  if (!cliente) {

    return false;
  }

  Serial.println();

  Serial.println(
    "Cliente TCP conectado"
  );

  memset(
    hl7Buffer,
    0,
    BUFFER_SIZE
  );

  size_t pos =
    0;

  unsigned long ultimoDato =
    millis();

  bool vtRecibido =
    false;

  bool fsRecibido =
    false;

  bool bloqueCompleto =
    false;

  while (
    cliente.connected() ||
    cliente.available()
  ) {

    while (
      cliente.available()
    ) {

      char c =
        cliente.read();

      // ======================================================
      // VT
      // ======================================================

      if (!vtRecibido) {

        if (
          (uint8_t)c ==
          MLLP_VT
        ) {

          vtRecibido =
            true;
        }

        ultimoDato =
          millis();

        continue;
      }

      // ======================================================
      // FS
      // ======================================================

      if (
        (uint8_t)c ==
        MLLP_FS
      ) {

        fsRecibido =
          true;

        ultimoDato =
          millis();

        continue;
      }

      // ======================================================
      // CR DESPUES DE FS
      // ======================================================

      if (fsRecibido) {

        if (
          (uint8_t)c ==
          MLLP_CR
        ) {

          bloqueCompleto =
            true;

          break;
        }

        fsRecibido =
          false;
      }

      // ======================================================
      // GUARDAR CONTENIDO HL7
      // ======================================================

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

        cliente.stop();

        return false;
      }

      ultimoDato =
        millis();
    }

    if (bloqueCompleto) {
      break;
    }

    if (
      millis() -
      ultimoDato >
      2000
    ) {

      break;
    }

    delay(1);
  }

  if (pos == 0) {

    Serial.println(
      "Cliente desconectado sin datos"
    );

    cliente.stop();

    return false;
  }

  hl7Buffer[pos] =
    '\0';

  Serial.println();

  Serial.println(
    "===== TRAMA HL7 RECIBIDA ====="
  );

  Serial.printf(
    "VT detectado: %s\n",
    vtRecibido
      ? "SI"
      : "NO"
  );

  Serial.printf(
    "FS+CR detectado: %s\n",
    bloqueCompleto
      ? "SI"
      : "NO"
  );

  Serial.printf(
    "Tamano: %lu bytes\n",
    (unsigned long)pos
  );

  Serial.printf(
    "Tamano: %.2f KB\n",
    pos / 1024.0
  );

  Serial.println(
    "=============================="
  );

  // NOTA: se elimino el envio de ACK MLLP. No hace falta
  // responder al equipo emisor tras recibir la trama.

  cliente.stop();

  return true;
}

// ============================================================
// DELIMITADORES
// ============================================================

bool leerDelimitadores(
  const char* hl7
) {

  if (hl7 == nullptr)
    return false;

  if (
    strncmp(
      hl7,
      "MSH",
      3
    ) != 0
  )
    return false;

  if (
    strlen(hl7) < 8
  )
    return false;

  delims.campo =
    hl7[3];

  delims.componente =
    hl7[4];

  delims.repeticion =
    hl7[5];

  delims.escape =
    hl7[6];

  delims.subcomponente =
    hl7[7];

  Serial.println(
    "Delimitadores HL7 detectados:"
  );

  Serial.printf(
    "  Campo:        '%c'\n",
    delims.campo
  );

  Serial.printf(
    "  Componente:   '%c'\n",
    delims.componente
  );

  Serial.printf(
    "  Repeticion:   '%c'\n",
    delims.repeticion
  );

  Serial.printf(
    "  Escape:       '%c'\n",
    delims.escape
  );

  Serial.printf(
    "  Subcomponente:'%c'\n",
    delims.subcomponente
  );

  return true;
}

// ============================================================
// EXTRAER COMPONENTE
// ============================================================

String extraerComponente(
  const String& campo,
  int numComponente
) {

  int actual =
    1;

  int inicio =
    0;

  for (
    int i = 0;
    i <= (int)campo.length();
    i++
  ) {

    if (
      i == (int)campo.length() ||
      campo[i] ==
      delims.componente
    ) {

      if (
        actual ==
        numComponente
      ) {

        return campo.substring(
          inicio,
          i
        );
      }

      actual++;

      inicio =
        i + 1;
    }
  }

  return "";
}

// ============================================================
// EXTRAER CAMPO SEGMENTO
//
// ESTA FUNCION SIGUE SIENDO UTIL PARA CAMPOS PEQUENOS.
// NO SE UTILIZA PARA EXTRAER LOS VALORES DEL LEAD (OBX-5).
// ============================================================

String extraerCampoDeSegmento(
  const char* segmento,
  int numCampo
) {

  int actual =
    0;

  int len =
    strlen(segmento);

  int inicio =
    0;

  for (
    int i = 0;
    i <= len;
    i++
  ) {

    if (
      i == len ||
      segmento[i] ==
      delims.campo
    ) {

      if (
        actual ==
        numCampo
      ) {

        char buf[512] = {
          0
        };

        int copyLen =
          i - inicio;

        if (
          copyLen >
          511
        )
          copyLen =
            511;

        if (
          copyLen < 0
        )
          copyLen =
            0;

        strncpy(
          buf,
          segmento + inicio,
          copyLen
        );

        buf[copyLen] =
          '\0';

        return String(buf);
      }

      actual++;

      inicio =
        i + 1;
    }
  }

  return "";
}

// ============================================================
// EXTRAER CAMPO HL7
// ============================================================

String extraerCampoHL7(
  const char* nombreSegmento,
  int numCampo,
  int numComponente
) {

  char patron[16];

  snprintf(
    patron,
    sizeof(patron),
    "%s%c",
    nombreSegmento,
    delims.campo
  );

  char* inicio =
    strstr(
      hl7Buffer,
      nombreSegmento
    );

  while (
    inicio != nullptr
  ) {

    bool valido =
      (
        inicio ==
        hl7Buffer
      ) ||
      (
        *(inicio - 1) ==
        '\r'
      );

    if (
      valido &&
      strncmp(
        inicio,
        patron,
        strlen(patron)
      ) == 0
    ) {

      break;
    }

    inicio =
      strstr(
        inicio + 1,
        nombreSegmento
      );
  }

  if (
    inicio ==
    nullptr
  ) {

    return "DESCONOCIDO";
  }

  char* fin =
    strchr(
      inicio,
      '\r'
    );

  size_t largo =
    fin != nullptr
      ? (size_t)(
          fin - inicio
        )
      : strlen(inicio);

  char segTemp[1024] = {
    0
  };

  size_t copyLen =
    largo > 1023
      ? 1023
      : largo;

  strncpy(
    segTemp,
    inicio,
    copyLen
  );

  segTemp[copyLen] =
    '\0';

  String campo =
    extraerCampoDeSegmento(
      segTemp,
      numCampo
    );

  if (
    campo.length() == 0
  ) {

    return "DESCONOCIDO";
  }

  if (
    numComponente <= 1
  ) {

    return campo;
  }

  String comp =
    extraerComponente(
      campo,
      numComponente
    );

  if (
    comp.length() == 0
  ) {

    return "DESCONOCIDO";
  }

  return comp;
}

// ============================================================
// EXTRAER CAMPO DE UN OBX SEGUN SU IDENTIFICADOR OBX-3
// ============================================================

String extraerCampoOBXPorIdentificador(
  const char* identificador,
  int numCampo,
  int numComponente
) {

  if (identificador == nullptr) {
    return "DESCONOCIDO";
  }

  char* segmento = hl7Buffer;

  while (
    segmento != nullptr &&
    *segmento != '\0'
  ) {

    char* fin = strchr(segmento, '\r');

    size_t largo =
      fin != nullptr
        ? (size_t)(fin - segmento)
        : strlen(segmento);

    if (
      largo >= 4 &&
      segmento[0] == 'O' &&
      segmento[1] == 'B' &&
      segmento[2] == 'X' &&
      segmento[3] == delims.campo
    ) {

      char segTemp[1024] = {0};

      size_t copyLen =
        largo > 1023
          ? 1023
          : largo;

      memcpy(
        segTemp,
        segmento,
        copyLen
      );

      segTemp[copyLen] = '\0';

      String campoId =
        extraerCampoDeSegmento(
          segTemp,
          3
        );

      String codigo =
        extraerComponente(
          campoId,
          1
        );

      if (
        codigo.length() == 0
      ) {
        codigo = campoId;
      }

      if (
        codigo == identificador
      ) {

        String campo =
          extraerCampoDeSegmento(
            segTemp,
            numCampo
          );

        if (
          campo.length() == 0
        ) {
          return "DESCONOCIDO";
        }

        if (
          numComponente <= 1
        ) {
          return campo;
        }

        String componente =
          extraerComponente(
            campo,
            numComponente
          );

        if (
          componente.length() == 0
        ) {
          return "DESCONOCIDO";
        }

        return componente;
      }
    }

    if (
      fin == nullptr
    ) {
      break;
    }

    segmento =
      fin + 1;
  }

  return "DESCONOCIDO";
}

// ============================================================
// COPIAR COMPONENTE DIRECTAMENTE DESDE HL7
//
// Se conserva por si se necesita extraer un componente puntual
// de un campo grande sin pasar por String. No se usa actualmente
// para el lead ECG (que copia el campo OBX-5 completo), pero
// queda disponible para otros usos futuros.
// ============================================================

bool copiarComponenteDirecto(
  const char* segmento,
  int numCampo,
  int numComponente,
  char* destino,
  size_t capacidad,
  size_t& longitud
) {

  longitud = 0;

  if (
    segmento == nullptr ||
    destino == nullptr ||
    capacidad == 0
  ) {

    return false;
  }

  const char separadorCampo =
    delims.campo;

  const char separadorComponente =
    delims.componente;

  int campoActual = 0;

  const char* inicioCampo =
    segmento;

  const char* p =
    segmento;

  // ==========================================================
  // BUSCAR EL CAMPO SOLICITADO
  // ==========================================================

  while (true) {

    if (
      *p == separadorCampo ||
      *p == '\0'
    ) {

      if (
        campoActual ==
        numCampo
      ) {

        break;
      }

      if (*p == '\0') {

        return false;
      }

      campoActual++;

      inicioCampo =
        p + 1;
    }

    p++;

    if (*p == '\0') {

      if (
        campoActual ==
        numCampo
      ) {

        break;
      }

      return false;
    }
  }

  const char* finCampo =
    p;

  // ==========================================================
  // BUSCAR COMPONENTE
  // ==========================================================

  int componenteActual =
    1;

  const char* inicioComponente =
    inicioCampo;

  const char* q =
    inicioCampo;

  while (true) {

    if (
      q == finCampo ||
      *q == separadorComponente
    ) {

      if (
        componenteActual ==
        numComponente
      ) {

        const char* finComponente =
          q;

        size_t largo =
          (size_t)(
            finComponente -
            inicioComponente
          );

        // ----------------------------------------------------
        // COMPROBAR CAPACIDAD
        // ----------------------------------------------------

        if (
          largo >= capacidad
        ) {

          Serial.printf(
            "ERROR: componente demasiado grande: %lu bytes\n",
            (unsigned long)largo
          );

          return false;
        }

        // ----------------------------------------------------
        // COPIA DIRECTA A PSRAM
        // ----------------------------------------------------

        memcpy(
          destino,
          inicioComponente,
          largo
        );

        destino[largo] =
          '\0';

        longitud =
          largo;

        return true;
      }

      if (
        q == finCampo
      ) {

        break;
      }

      componenteActual++;

      inicioComponente =
        q + 1;
    }

    q++;
  }

  return false;
}

// ============================================================
// EXTRAER DATOS DE ECG LEAD (NA) DIRECTAMENTE AL SLOT
//
// Busca segmentos OBX tipo "NA" (Numeric Array) que contienen
// los valores de onda del ECG (ej: Lead II), separados por '^'.
//
// A diferencia del PDF Base64 que se manejaba antes:
// - No se valida cabecera de PDF.
// - No se valida Base64.
// - Se copia el campo OBX-5 completo (los valores numericos
//   separados por '^') tal cual viene, directo a PSRAM.
// - Si aparece mas de un OBX tipo NA, se van concatenando
//   separados por ';' con el formato:
//       LEAD_ID=valor1^valor2^valor3...;LEAD_ID2=...
// ============================================================

bool extraerLeadECGAlSlot(
  char* hl7,
  char* destino,
  size_t capacidad
) {

  if (
    hl7 == nullptr ||
    destino == nullptr
  ) {

    return false;
  }

  destino[0] =
    '\0';

  size_t totalEscrito =
    0;

  char* segmento =
    hl7;

  int numeroOBX =
    0;

  bool encontradoAlMenosUno =
    false;

  while (
    segmento != nullptr &&
    *segmento != '\0'
  ) {

    char* siguiente =
      strchr(
        segmento,
        '\r'
      );

    if (
      siguiente != nullptr
    ) {

      *siguiente =
        '\0';
    }

    // ========================================================
    // VERIFICAR OBX
    // ========================================================

    char patronOBX[8];

    snprintf(
      patronOBX,
      sizeof(patronOBX),
      "OBX%c",
      delims.campo
    );

    if (
      strncmp(
        segmento,
        patronOBX,
        strlen(patronOBX)
      ) == 0
    ) {

      numeroOBX++;

      Serial.printf(
        "Analizando OBX #%d...\n",
        numeroOBX
      );

      // ======================================================
      // OBX-2 (tipo de valor)
      // ======================================================

      String tipoValor =
        extraerCampoDeSegmento(
          segmento,
          2
        );

      Serial.printf(
        "  OBX-2: %s\n",
        tipoValor.c_str()
      );

      if (
        tipoValor != "NA"
      ) {

        Serial.println(
          "  No es NA. Se ignora."
        );

        if (
          siguiente != nullptr
        ) {

          *siguiente =
            '\r';

          segmento =
            siguiente + 1;

          continue;
        }

        break;
      }

      // ======================================================
      // OBX-3 (identificador del lead, ej: MDC_ECG_LEAD_II)
      // ======================================================

      String idLead =
        extraerCampoDeSegmento(
          segmento,
          3
        );

      Serial.printf(
        "  OBX-3 (Lead): %s\n",
        idLead.c_str()
      );

      // ======================================================
      // OBX-5 = campo 5, valores numericos separados por '^'
      //
      // Se copia el campo COMPLETO (todos los componentes),
      // directo a PSRAM, sin pasar por String.
      // ======================================================

      char* p =
        segmento;

      int campoActual =
        0;

      char* inicioCampo =
        segmento;

      bool encontroCampo5 =
        false;

      while (
        *p != '\0'
      ) {

        if (
          *p == delims.campo
        ) {

          campoActual++;

          if (
            campoActual == 5
          ) {

            inicioCampo =
              p + 1;

            encontroCampo5 =
              true;

            break;
          }
        }

        p++;
      }

      if (
        !encontroCampo5
      ) {

        Serial.println(
          "  ERROR: No se encontro OBX-5"
        );

        if (
          siguiente != nullptr
        ) {

          *siguiente =
            '\r';

          segmento =
            siguiente + 1;

          continue;
        }

        break;
      }

      char* finCampo =
        strchr(
          inicioCampo,
          delims.campo
        );

      size_t largoValores =
        finCampo != nullptr
          ? (size_t)(finCampo - inicioCampo)
          : strlen(inicioCampo);

      if (
        largoValores == 0
      ) {

        Serial.println(
          "  OBX-5 vacio. Se ignora."
        );

        if (
          siguiente != nullptr
        ) {

          *siguiente =
            '\r';

          segmento =
            siguiente + 1;

          continue;
        }

        break;
      }

      // ======================================================
      // ARMAR ETIQUETA + VALORES EN EL DESTINO
      //
      // Formato guardado:
      //   LEAD_ID=valor1^valor2^valor3...;LEAD_ID2=...
      // ======================================================

      size_t espacioNecesario =
        idLead.length() +
        1 + // '='
        largoValores +
        1;  // posible ';' separador

      if (
        totalEscrito +
        espacioNecesario >=
        capacidad
      ) {

        Serial.println(
          "  ERROR: No hay espacio en el slot para este lead"
        );

        if (
          siguiente != nullptr
        ) {

          *siguiente =
            '\r';

          segmento =
            siguiente + 1;

          continue;
        }

        break;
      }

      if (
        totalEscrito > 0
      ) {

        destino[totalEscrito++] =
          ';';
      }

      memcpy(
        destino + totalEscrito,
        idLead.c_str(),
        idLead.length()
      );

      totalEscrito +=
        idLead.length();

      destino[totalEscrito++] =
        '=';

      memcpy(
        destino + totalEscrito,
        inicioCampo,
        largoValores
      );

      totalEscrito +=
        largoValores;

      destino[totalEscrito] =
        '\0';

      encontradoAlMenosUno =
        true;

      Serial.printf(
        "  Lead '%s' guardado: %lu bytes de valores\n",
        idLead.c_str(),
        (unsigned long)largoValores
      );
    }

    if (
      siguiente == nullptr
    )
      break;

    *siguiente =
      '\r';

    segmento =
      siguiente + 1;
  }

  if (!encontradoAlMenosUno) {

    Serial.println(
      "ERROR: No se encontro ningun OBX tipo NA (lead ECG)"
    );

    return false;
  }

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "DATOS ECG (NA) EXTRAIDOS CORRECTAMENTE"
  );

  Serial.printf(
    "Total bytes guardados: %lu\n",
    (unsigned long)totalEscrito
  );

  Serial.printf(
    "PSRAM libre despues: %lu KB\n",
    (unsigned long)(
      ESP.getFreePsram() / 1024
    )
  );

  Serial.println(
    "========================================"
  );

  return true;
}

// ============================================================
// LIBERAR SLOT
// ============================================================

void liberarSlotECG(
  int index
) {

  if (
    index < 0 ||
    index >=
    MAX_ECG_PENDIENTES
  )
    return;

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

  colaECG[index].paciente_id[0] = '\0';
  colaECG[index].fecha_nacimiento[0] = '\0';
  colaECG[index].sexo[0] = '\0';
  colaECG[index].sector[0] = '\0';
  colaECG[index].cama[0] = '\0';
  colaECG[index].frecuencia_cardiaca[0] = '\0';
  colaECG[index].derivacion[0] = '\0';
  colaECG[index].ritmo_automatico[0] = '\0';
  colaECG[index].equipo[0] = '\0';
  colaECG[index].estado_resultado[0] = '\0';

  if (
    colaECG[index].leadData
  ) {

    colaECG[index].leadData[0] =
      '\0';
  }

  if (
    totalPendientes > 0
  )
    totalPendientes--;

  Serial.printf(
    "Slot %d liberado\n",
    index + 1
  );

  Serial.printf(
    "Pendientes: %d/%d\n",
    totalPendientes,
    MAX_ECG_PENDIENTES
  );
}

// ============================================================
// PROCESAR TRAMA
// ============================================================

void procesarTrama() {

  Serial.println();

  Serial.println(
    "Procesando trama HL7 v2.5/2.6..."
  );

  if (
    !leerDelimitadores(
      hl7Buffer
    )
  ) {

    Serial.println(
      "ERROR: MSH invalido"
    );

    return;
  }

  // ==========================================================
  // PACIENTE
  // ==========================================================

  String apellido =
    extraerCampoHL7(
      "PID",
      5,
      1
    );

  String nombre =
    extraerCampoHL7(
      "PID",
      5,
      2
    );

  String paciente =
    apellido +
    "_" +
    nombre;

  paciente.replace(
    "^",
    "_"
  );

  paciente.replace(
    " ",
    "_"
  );

  // ==========================================================
  // OBR
  // ==========================================================

  String fecha =
    extraerCampoHL7(
      "OBR",
      7,
      1
    );

  String ecgId =
    extraerCampoHL7(
      "OBR",
      3,
      1
    );

  // ==========================================================
  // DATOS HL7 ADICIONALES PARA FIREBASE
  // ==========================================================

  String pacienteId =
    extraerCampoHL7(
      "PID",
      3,
      1
    );

  String fechaNacimiento =
    extraerCampoHL7(
      "PID",
      7,
      1
    );

  String sexo =
    extraerCampoHL7(
      "PID",
      8,
      1
    );

  String sector =
    extraerCampoHL7(
      "PV1",
      3,
      1
    );

  String cama =
    extraerCampoHL7(
      "PV1",
      3,
      2
    );

  String frecuenciaCardiaca =
    extraerCampoOBXPorIdentificador(
      "8867-4",
      5,
      1
    );

  String derivacion =
    extraerCampoOBXPorIdentificador(
      "MINDRAY_LEAD_CONF",
      5,
      1
    );

  String ritmoAutomatico =
    extraerCampoOBXPorIdentificador(
      "MINDRAY_RHYTHM_STAT",
      5,
      1
    );

  String equipo =
    extraerCampoOBXPorIdentificador(
      "8867-4",
      16,
      1
    );

  String estadoResultado =
    extraerCampoOBXPorIdentificador(
      "8867-4",
      11,
      1
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
  // BUSCAR SLOT
  // ==========================================================

  int slot =
    buscarSlotLibre();

  if (
    slot < 0
  ) {

    Serial.println();

    Serial.println(
      "========================================"
    );

    Serial.println(
      "NO HAY SLOTS ECG DISPONIBLES"
    );

    Serial.println(
      "========================================"
    );

    mostrarLCD(
      "MEMORIA LLENA",
      "2 ECG PENDIENTES",
      "No hay slots",
      "libres"
    );

    delay(3000);

    return;
  }

  Serial.printf(
    "Slot disponible: %d\n",
    slot + 1
  );

  // ==========================================================
  // EXTRAER DATOS DE ECG LEAD DIRECTAMENTE AL SLOT
  // ==========================================================

  Serial.println();

  Serial.println(
    "Buscando datos de ECG Lead dentro de OBX (NA)..."
  );

  bool leadOK =
    extraerLeadECGAlSlot(
      hl7Buffer,
      colaECG[slot].leadData,
      ECG_SLOT_SIZE
    );

  if (!leadOK) {

    Serial.println(
      "ERROR: Datos de ECG Lead no encontrados"
    );

    colaECG[slot].leadData[0] =
      '\0';

    mostrarLCD(
      "ERROR",
      "LEAD NO ENCONTRADO",
      "Revisar OBX NA",
      "ECG no guardado"
    );

    delay(3000);

    return;
  }

  size_t leadDataLength =
    strlen(
      colaECG[slot].leadData
    );

  // ==========================================================
  // DIAGNOSTICO DATOS ECG LEAD
  // ==========================================================

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "DIAGNOSTICO DATOS ECG LEAD"
  );

  Serial.printf(
    "Tamano: %lu bytes\n",
    (unsigned long)
      leadDataLength
  );

  Serial.printf(
    "PSRAM libre: %lu KB\n",
    (unsigned long)(
      ESP.getFreePsram() /
      1024
    )
  );

  Serial.println(
    "========================================"
  );

  // ==========================================================
  // COMPLETAR DATOS DEL SLOT
  // ==========================================================

  strncpy(
    colaECG[slot].paciente,
    paciente.c_str(),
    sizeof(
      colaECG[slot].paciente
    ) - 1
  );

  colaECG[slot].paciente[
    sizeof(
      colaECG[slot].paciente
    ) - 1
  ] = '\0';

  strncpy(
    colaECG[slot].ecgId,
    ecgId.c_str(),
    sizeof(
      colaECG[slot].ecgId
    ) - 1
  );

  colaECG[slot].ecgId[
    sizeof(
      colaECG[slot].ecgId
    ) - 1
  ] = '\0';

  strncpy(
    colaECG[slot].fecha,
    fecha.c_str(),
    sizeof(
      colaECG[slot].fecha
    ) - 1
  );

  colaECG[slot].fecha[
    sizeof(
      colaECG[slot].fecha
    ) - 1
  ] = '\0';

  strncpy(colaECG[slot].paciente_id, pacienteId.c_str(), sizeof(colaECG[slot].paciente_id) - 1);
  colaECG[slot].paciente_id[sizeof(colaECG[slot].paciente_id) - 1] = '\0';

  strncpy(colaECG[slot].fecha_nacimiento, fechaNacimiento.c_str(), sizeof(colaECG[slot].fecha_nacimiento) - 1);
  colaECG[slot].fecha_nacimiento[sizeof(colaECG[slot].fecha_nacimiento) - 1] = '\0';

  strncpy(colaECG[slot].sexo, sexo.c_str(), sizeof(colaECG[slot].sexo) - 1);
  colaECG[slot].sexo[sizeof(colaECG[slot].sexo) - 1] = '\0';

  strncpy(colaECG[slot].sector, sector.c_str(), sizeof(colaECG[slot].sector) - 1);
  colaECG[slot].sector[sizeof(colaECG[slot].sector) - 1] = '\0';

  strncpy(colaECG[slot].cama, cama.c_str(), sizeof(colaECG[slot].cama) - 1);
  colaECG[slot].cama[sizeof(colaECG[slot].cama) - 1] = '\0';

  strncpy(colaECG[slot].frecuencia_cardiaca, frecuenciaCardiaca.c_str(), sizeof(colaECG[slot].frecuencia_cardiaca) - 1);
  colaECG[slot].frecuencia_cardiaca[sizeof(colaECG[slot].frecuencia_cardiaca) - 1] = '\0';

  strncpy(colaECG[slot].derivacion, derivacion.c_str(), sizeof(colaECG[slot].derivacion) - 1);
  colaECG[slot].derivacion[sizeof(colaECG[slot].derivacion) - 1] = '\0';

  strncpy(colaECG[slot].ritmo_automatico, ritmoAutomatico.c_str(), sizeof(colaECG[slot].ritmo_automatico) - 1);
  colaECG[slot].ritmo_automatico[sizeof(colaECG[slot].ritmo_automatico) - 1] = '\0';

  strncpy(colaECG[slot].equipo, equipo.c_str(), sizeof(colaECG[slot].equipo) - 1);
  colaECG[slot].equipo[sizeof(colaECG[slot].equipo) - 1] = '\0';

  strncpy(colaECG[slot].estado_resultado, estadoResultado.c_str(), sizeof(colaECG[slot].estado_resultado) - 1);
  colaECG[slot].estado_resultado[sizeof(colaECG[slot].estado_resultado) - 1] = '\0';

  colaECG[slot].ocupado =
    true;

  colaECG[slot].enviado =
    false;

  totalPendientes++;

  huboAlMenosUnECG =
    true;

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
    "PSRAM OK",
    "Slot " +
      String(slot + 1),
    hayInternet
      ? "Enviando..."
      : "SIN CONEXION"
  );

  delay(2000);

  // ==========================================================
  // ENVIAR FIREBASE
  // ==========================================================

  gestionarRed();

  if (
    hayInternet
  ) {

    enviarECGAFirebase(
      slot
    );

    mostrarEstadoListo();

  } else {

    Serial.println(
      "Sin conexion 4G."
    );

    Serial.println(
      "ECG queda almacenado en PSRAM."
    );

    mostrarLCD(
      "SIN CONEXION",
      "ECG guardado OK",
      "Se enviara cuando",
      "vuelva 4G"
    );

    delay(2500);
  }
}

// ============================================================
// COLA PENDIENTES
// ============================================================

void procesarColaPendientes() {

  if (!hayInternet)
    return;

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
  )
    return;

  Serial.printf(
    "Hay %d ECG pendientes.\n",
    pendientes
  );

  for (
    int i = 0;
    i < MAX_ECG_PENDIENTES;
    i++
  ) {

    if (
      colaECG[i].ocupado &&
      !colaECG[i].enviado
    ) {

      if (
        !verificarConexion()
      ) {

        return;
      }

      if (
        !enviarECGAFirebase(i)
      ) {

        return;
      }

      delay(500);
    }
  }
}

// ============================================================
// JSON
// ============================================================

size_t calcularJSONSize(
  ECGPendiente& ecg
) {

  size_t leadLen =
    strlen(
      ecg.leadData
    );

  return
    leadLen +
    strlen(ecg.paciente) +
    strlen(ecg.ecgId) +
    strlen(ecg.fecha) +
    strlen(ecg.paciente_id) +
    strlen(ecg.fecha_nacimiento) +
    strlen(ecg.sexo) +
    strlen(ecg.sector) +
    strlen(ecg.cama) +
    strlen(ecg.frecuencia_cardiaca) +
    strlen(ecg.derivacion) +
    strlen(ecg.ritmo_automatico) +
    strlen(ecg.equipo) +
    strlen(ecg.estado_resultado) +
    1400;
}

// ============================================================

size_t copiarJSONEscapado(
  char* destino,
  size_t capacidad,
  const char* origen
) {

  size_t pos =
    0;

  if (
    destino == nullptr ||
    origen == nullptr ||
    capacidad == 0
  )
    return 0;

  while (
    *origen != '\0'
  ) {

    char c =
      *origen++;

    if (
      c == '"' ||
      c == '\\'
    ) {

      if (
        pos + 2 >=
        capacidad
      )
        return 0;

      destino[pos++] =
        '\\';

      destino[pos++] =
        c;

    } else {

      if (
        pos + 1 >=
        capacidad
      )
        return 0;

      destino[pos++] =
        c;
    }
  }

  destino[pos] =
    '\0';

  return pos;
}

// ============================================================
// CONSTRUIR JSON
// ============================================================

bool construirJSONEnPSRAM(
  ECGPendiente& ecg,
  char* json,
  size_t capacidad,
  size_t& longitud
) {

  longitud =
    0;

  const char* inicio =
    "{\"fields\":{\"paciente\":{\"stringValue\":\"";

  const char* medio1 =
    "\"},\"ecg_id\":{\"stringValue\":\"";

  const char* medio2 =
    "\"},\"fecha\":{\"stringValue\":\"";

  const char* medio3 =
    "\"},\"paciente_id\":{\"stringValue\":\"";

  const char* medio4 =
    "\"},\"fecha_nacimiento\":{\"stringValue\":\"";

  const char* medio5 =
    "\"},\"sexo\":{\"stringValue\":\"";

  const char* medio6 =
    "\"},\"sector\":{\"stringValue\":\"";

  const char* medio7 =
    "\"},\"cama\":{\"stringValue\":\"";

  const char* medio8 =
    "\"},\"frecuencia_cardiaca\":{\"stringValue\":\"";

  const char* medio9 =
    "\"},\"derivacion\":{\"stringValue\":\"";

  const char* medio10 =
    "\"},\"ritmo_automatico\":{\"stringValue\":\"";

  const char* medio11 =
    "\"},\"equipo\":{\"stringValue\":\"";

  const char* medio12 =
    "\"},\"estado_resultado\":{\"stringValue\":\"";

  const char* medio13 =
    "\"},\"ecg_lead_data\":{\"stringValue\":\"";

  const char* finalJSON =
    "\"}}}";

  size_t n;

  // ==========================================================
  // INICIO
  // ==========================================================

  n =
    strlen(inicio);

  if (
    longitud + n >= capacidad
  )
    return false;

  memcpy(
    json + longitud,
    inicio,
    n
  );

  longitud += n;

  // ==========================================================
  // PACIENTE
  // ==========================================================

  n =
    copiarJSONEscapado(
      json + longitud,
      capacidad - longitud,
      ecg.paciente
    );

  if (
    n == 0 &&
    ecg.paciente[0] != '\0'
  )
    return false;

  longitud += n;

  // ==========================================================
  // ECG ID
  // ==========================================================

  n =
    strlen(medio1);

  if (
    longitud + n >= capacidad
  )
    return false;

  memcpy(
    json + longitud,
    medio1,
    n
  );

  longitud += n;

  n =
    copiarJSONEscapado(
      json + longitud,
      capacidad - longitud,
      ecg.ecgId
    );

  if (
    n == 0 &&
    ecg.ecgId[0] != '\0'
  )
    return false;

  longitud += n;

  // ==========================================================
  // FECHA
  // ==========================================================

  n =
    strlen(medio2);

  if (
    longitud + n >= capacidad
  )
    return false;

  memcpy(
    json + longitud,
    medio2,
    n
  );

  longitud += n;

  n =
    copiarJSONEscapado(
      json + longitud,
      capacidad - longitud,
      ecg.fecha
    );

  if (
    n == 0 &&
    ecg.fecha[0] != '\0'
  )
    return false;

  longitud += n;

  // ==========================================================
  // DATOS HL7 ADICIONALES
  // ==========================================================

  const char* separadoresAdicionales[] = {
    medio3, medio4, medio5, medio6, medio7,
    medio8, medio9, medio10, medio11, medio12
  };

  const char* valoresAdicionales[] = {
    ecg.paciente_id,
    ecg.fecha_nacimiento,
    ecg.sexo,
    ecg.sector,
    ecg.cama,
    ecg.frecuencia_cardiaca,
    ecg.derivacion,
    ecg.ritmo_automatico,
    ecg.equipo,
    ecg.estado_resultado
  };

  for (int i = 0; i < 10; i++) {

    n = strlen(separadoresAdicionales[i]);

    if (
      longitud + n >= capacidad
    )
      return false;

    memcpy(
      json + longitud,
      separadoresAdicionales[i],
      n
    );

    longitud += n;

    n =
      copiarJSONEscapado(
        json + longitud,
        capacidad - longitud,
        valoresAdicionales[i]
      );

    if (
      n == 0 &&
      valoresAdicionales[i][0] != '\0'
    )
      return false;

    longitud += n;
  }

  // ==========================================================
  // DATOS ECG LEAD
  // ==========================================================

  n =
    strlen(medio13);

  if (
    longitud + n >= capacidad
  )
    return false;

  memcpy(
    json + longitud,
    medio13,
    n
  );

  longitud += n;

  size_t leadLen =
    strlen(
      ecg.leadData
    );

  if (
    longitud +
    leadLen +
    strlen(finalJSON) +
    1 >=
    capacidad
  ) {

    return false;
  }

  // ==========================================================
  // COPIAR DATOS DE LEAD EXACTAMENTE (memcpy conserva todo)
  // ==========================================================

  memcpy(
    json + longitud,
    ecg.leadData,
    leadLen
  );

  longitud +=
    leadLen;

  // ==========================================================
  // FINAL JSON
  // ==========================================================

  n =
    strlen(finalJSON);

  memcpy(
    json + longitud,
    finalJSON,
    n
  );

  longitud += n;

  json[longitud] =
    '\0';

  return true;
}

// ============================================================
// FIREBASE
// ============================================================

bool enviarECGAFirebase(
  int index
) {

  if (
    index < 0 ||
    index >= MAX_ECG_PENDIENTES
  )
    return false;

  ECGPendiente& ecg =
    colaECG[index];

  if (
    !ecg.ocupado
  )
    return false;

  if (
    !verificarConexion()
  ) {

    hayInternet =
      false;

    return false;
  }

  Serial.println();

  Serial.println(
    "===== ENVIANDO ECG A FIREBASE ====="
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

  size_t leadDataLength =
    strlen(
      ecg.leadData
    );

  Serial.printf(
    "Datos ECG Lead: %lu bytes\n",
    (unsigned long)
      leadDataLength
  );

  // ==========================================================
  // JSON
  // ==========================================================

  size_t jsonSize =
    calcularJSONSize(
      ecg
    );

  char* json =
    (char*)ps_malloc(
      jsonSize
    );

  if (
    json == nullptr
  ) {

    Serial.println(
      "ERROR: No se pudo reservar JSON en PSRAM"
    );

    return false;
  }

  size_t jsonLength =
    0;

  if (
    !construirJSONEnPSRAM(
      ecg,
      json,
      jsonSize,
      jsonLength
    )
  ) {

    Serial.println(
      "ERROR construyendo JSON"
    );

    free(json);

    return false;
  }

  Serial.printf(
    "JSON: %lu bytes\n",
    (unsigned long)
      jsonLength
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

  String path =
    String(
      FIREBASE_PATH_BASE
    ) +
    "?documentId=" +
    docId;

  String url =
    "https://" +
    String(
      FIREBASE_HOST
    ) +
    path;

  // ==========================================================
  // HTTPS
  // ==========================================================

  bool exito =
    false;

  xSemaphoreTake(
    modemMutex,
    portMAX_DELAY
  );

  if (
    !modem.https_begin()
  ) {

    Serial.println(
      "ERROR: https_begin"
    );

    free(json);

    xSemaphoreGive(
      modemMutex
    );

    return false;
  }

  if (
    !modem.https_set_url(
      url.c_str()
    )
  ) {

    Serial.println(
      "ERROR: URL HTTPS"
    );

    modem.https_end();

    free(json);

    xSemaphoreGive(
      modemMutex
    );

    return false;
  }

  modem.https_set_content_type(
    "application/json"
  );

  Serial.println(
    "Enviando JSON por HTTPS..."
  );

  int httpCode =
    modem.https_post(
      json,
      jsonLength
    );

  free(json);

  Serial.printf(
    "HTTP Code: %d\n",
    httpCode
  );

  String response =
    modem.https_body();

  Serial.println(
    response
  );

  // ==========================================================
  // RESULTADO
  // ==========================================================

  if (
    httpCode == 200 ||
    httpCode == 201
  ) {

    Serial.println(
      "ECG enviado correctamente."
    );

    exito =
      true;

    liberarSlotECG(
      index
    );
  }

  else if (
    httpCode == 409
  ) {

    Serial.println(
      "TRAMA YA EXISTE EN FIRESTORE."
    );

    Serial.println(
      "Se considera enviada."
    );

    exito =
      true;

    liberarSlotECG(
      index
    );
  }

  else {

    Serial.println(
      "ERROR: Firestore no acepto ECG."
    );
  }

  modem.https_end();

  xSemaphoreGive(
    modemMutex
  );

  return exito;
}
