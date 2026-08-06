/*
 * ================================================================
 * SISTEMA DE RIEGO FINCA — v7.6.0-stab
 * Correcciones de estabilidad aplicadas (agosto 2026):
 *
 * ESTAB-1: sprintf → snprintf con sizeof(buffer) en obtenerTimestamp()
 *           para descartar overflows de buffer de formato.
 *
 * ESTAB-2: Nuevo helper restanteMs() para calcular tiempo restante de
 *           riego de forma segura (evita subdesborde unsigned si el
 *           scheduler consulta el estado justo cuando duracion expira).
 *
 * ESTAB-3: Todos los millis()-variable crudos en rutas de temporización
 *           (valvulas, pozos, NTP, anti-spam, NVS, boyas) sustituidos
 *           por diffMillis() para correcta aritmética en el overflow de
 *           millis() (~49.7 días).
 *
 * ESTAB-4: Todas las expresiones duracionMs-(millis()-inicioRiego)
 *           reemplazadas por restanteMs() para evitar subdesborde
 *           unsigned en mensajes de estado/diagnóstico.
 *
 * ESTAB-5: reserve() añadido al builder de /ayuda y al mensaje de
 *           arranque del sistema para reducir fragmentación de heap.
 *
 * ESTAB-6: esp_task_wdt_reset() añadido en /ayuda, /diagnostico y /log
 *           dentro de procesarComando() para evitar disparo espurio del
 *           WDT de 6 minutos al construir mensajes extensos.
 *
 * ESTAB-7: Claves NVS que usaban ("prefijo_" + String(i)).c_str()
 *           sustituidas por char buf + snprintf() en gestionarPozos(),
 *           guardarYReiniciar(), iniciarRiegoAutomatico(), gestionarFinRiego()
 *           y /riego_stop. Elimina objetos String temporales cuyo puntero
 *           c_str() podría no sobrevivir en stacks con -O2.
 *
 * ESTAB-8: Restauración de zonas en setup() abierta en modo solo-lectura
 *           (true) en lugar de escritura (false).
 *
 * ESTAB-9: dirty flag nvsLogDirty en guardarLog() — solo escribe el log
 *           circular en NVS cuando registrarEvento() añadió al menos un
 *           evento desde la última persistencia. Antes: 288 escrituras/día
 *           fijas. Después: 0 escrituras si el sistema está en silencio
 *           (noche sin riegos, etc.).
 *
 * ESTAB-10: delta guard nvsUltimoRestante[z] en gestionarFinRiego() —
 *            el bloque periódico (cada 60 s) solo persiste rest_Z si el
 *            tiempo restante bajó más de 2 minutos desde la última
 *            escritura. Antes: 1 escritura/min por zona activa. Después:
 *            1 escritura cada 2 minutos como máximo por zona.
 *
 * ESTAB-11: dedup nvsUltimaZonaAbierta[z] en el mismo bloque periódico —
 *            zona_abierta_Z solo se reescribe si su valor cambió desde la
 *            última persistencia. Durante un riego el estado es siempre
 *            true, por lo que se escribe una sola vez al inicio y no
 *            vuelve a tocarse hasta que la zona cambia de estado.
 *
 * v7.6.0 MEJORAS TÉCNICAS (sin cambiar estética):
 *   - NVS atómica: guardarEstadoCritico() agrupa rest_Z, zona_abierta_Z,
 *     pozos, balsa interrumpido y modo lluvia en la mínima cantidad de
 *     transacciones begin/end posible.
 *   - schedulerRiego() usa time(nullptr)/60 (minuto absoluto epoch) para
 *     eliminar el bug de doble ejecución en cambio de año/día con tm_yday.
 *   - gestionarFinRiego() unificado: un solo bloque de cierre de válvula;
 *     el anti-spam aplica solo al mensaje Telegram, no a la lógica física.
 *   - Relay remoto: clientes WiFiClientSecure locales (no globales) para
 *     evitar estados TCP medio-cerrados entre ciclos.
 *   - Servidor HTTP: parsing de URI estricto desde la línea de request
 *     (evita falsos positivos si el body contiene palabras clave).
 *   - registrarEvento() construye la línea en stack char[] sin concatenar
 *     Strings intermedias, reduciendo fragmentación de heap.
 *   - yield() en bucles de mensajes largos (/log, /diagnostico, /ayuda)
 *     para evitar WDT espurio en builds con timeout corto.
 *   - Persistencia de tempAlertaUmbral y estadísticas acumuladas en NVS.
 *   - balsaInterrumpido* se guarda en NVS: recupera riegos pendientes
 *     incluso si el corte de luz ocurre mientras la balsa está vacía.
 *   - notificacionUptime() usa diffMillis() respecto a tiempoInicioSistema
 *     para robustez ante overflow de millis().
 *
 * ESTIMACIÓN ESCRITURAS NVS TRAS ESTAS CORRECCIONES (operación típica):
 *   - guardarLog()            : ≈ 20-40 esc./día  (solo eventos reales)
 *   - rest_Z periódico        : ≈ 15-20 esc./día  (delta 2 min)
 *   - zona_abierta_Z periódico: ≈  2- 4 esc./día  (1 por inicio riego)
 *   - inicios/fins/pozos/misc : ≈ 20-30 esc./día
 *   TOTAL estimado            : ≈ 60-100 esc./día
 *   Vida útil a 100 000 ciclos: > 2-4 años (vs. ~230 días anterior)
 *
 * SIN CAMBIOS FUNCIONALES: lógica de riego, boyas, pozos, Telegram,
 *   OTA, modo lluvia, log circular y reinicio automático son idénticos.
 * ================================================================
 */

SET_LOOP_TASK_STACK_SIZE(32 * 1024);

#include <WiFi.h>
#include <AsyncTelegram2.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <DHT.h>
#include <HTTPUpdate.h>      // OTA remota via HTTP desde Telegram
#include <HTTPClient.h>      // Relay remoto hacia servidor Replit

//--------------------------------------------------
// CONFIGURACIÓN ESP32-S3 - PINES SEGUROS
//--------------------------------------------------
#define DHTPIN 4        // GPIO4 para DHT22 (SEGURO en ESP32-S3)
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

//--------------------------------------------------
// WIFI
//--------------------------------------------------
const char* ssid = "MIWIFI_5G_ZWN5";         // ← Cambiar antes de compilar
const char* password = "3RjP5C74";    // ← Cambiar antes de compilar

// FIX v7.6: renombrar ip → ipFija para evitar sombra con variable local en fmtIp()
IPAddress ipFija(192,168,0,197);
IPAddress gateway(192,168,0,1);
IPAddress subnet(255,255,255,0);
IPAddress dns(8,8,8,8);

//--------------------------------------------------
// RELAY REMOTO — comunicación con servidor Replit
// El ESP32 empuja estado cada 5 s y recoge comandos
// cada 3 s. No requiere abrir puertos en el router.
//--------------------------------------------------
#define RELAY_HABILITADO 0      // ← 1 = activar relay remoto, 0 = solo LAN local
// Dominio de tu servidor Replit (sin https:// y sin barra final)
// En produccion usar el dominio desplegado, p.ej. "mi-finca.replit.app"
// En desarrollo usar el REPLIT_DEV_DOMAIN de tu Repl
const char* RELAY_HOST  = "TU-DOMINIO.replit.dev";  // ← CAMBIAR
const char* RELAY_PATH  = "/api/esp";
const char* RELAY_TOKEN = "riego2025";              // = HTTP_SECRET_TOKEN
const unsigned long RELAY_PUSH_INTERVALO = 5000UL;  // ms entre push de estado
const unsigned long RELAY_PULL_INTERVALO = 3000UL;  // ms entre pull de comandos

//--------------------------------------------------
// TELEGRAM
//--------------------------------------------------
#define BOT_TOKEN "8898525825:AAFp2IK7dfb5NOp-hXyHBV_giyhkv9iGf1E"  // ← Token de @BotFather
#define CHAT_ID 7806082386  // ← Tu chat_id de Telegram (obtener con @userinfobot)

WiFiClientSecure secureClient;
AsyncTelegram2 bot(secureClient);
WiFiServer servidorRiego(80);

//--------------------------------------------------
// PINES - 3 BOYAS (SEGUROS PARA ESP32-S3)
//--------------------------------------------------
const int pinAbrir[4] = {5, 6, 7, 8};
const int pinCerrar[4] = {9, 10, 11, 12};
const int pinVentilador = 14;       // GPIO14 seguro en ESP32-S3 (GPIO3 es strapping)
const int pinBoyaInferior = 13;   // GPIO13 - Boya inferior (CRÍTICA)
const int pinBoyaMedia = 15;      // GPIO15 - Boya media (50%)
const int pinBoyaSuperior = 16;   // GPIO16 - Boya superior (100%)
// LOW = sumergida (HAY AGUA) | HIGH = seca (SIN AGUA)

//--------------------------------------------------
// PINES - 2 VÁLVULAS POZO (INDEPENDIENTES)
//--------------------------------------------------
const int pinPozoAbrir[2] = {17, 18};    // GPIO17 y GPIO18 para abrir
const int pinPozoCerrar[2] = {19, 20};   // GPIO19 y GPIO20 para cerrar
// NO dependen de boyas, NO tienen programación

//--------------------------------------------------
// CÓDIGOS DE ERROR PARA abrirZona()
//--------------------------------------------------
#define ERR_OK              0
#define ERR_ZONA_INVALIDA   1
#define ERR_BALSA_CRITICA   2
#define ERR_VALVULA_OCUPADA 3
#define ERR_YA_ABIERTA      4

int ultimoErrorAbrir = ERR_OK;

//--------------------------------------------------
// SERVIDOR HTTP - TOKEN DE SEGURIDAD
// IMPORTANTE: cambia este token antes de usar en produccion
//--------------------------------------------------
#define HTTP_SECRET_TOKEN "riego2025"  // FIX: token de autenticacion HTTP local

//--------------------------------------------------
// WATCHDOG - 90 SEGUNDOS (configurable via /set)
//--------------------------------------------------
#define WDT_TIMEOUT_MS 90000  // 90 segundos por defecto

//--------------------------------------------------
// UMBRALES VENTILADOR — valores por defecto
// Sobreescribibles en tiempo real via /set
//--------------------------------------------------
#define TEMP_VENTILADOR_ON_C    30.0f  // °C — enciende ventilador
#define TEMP_VENTILADOR_OFF_C   28.0f  // °C — apaga  ventilador (histéresis)
#define HUM_VENTILADOR_ON_PCT   80.0f  // %  — enciende por humedad
#define HUM_VENTILADOR_OFF_PCT  75.0f  // %  — apaga   por humedad (histéresis)

//--------------------------------------------------
// TIEMPOS Y CONFIGURACIÓN - OPTIMIZADO PARA ESTABILIDAD
//--------------------------------------------------
unsigned long ultimoTelegram = 0;
unsigned long ultimoDHT = 0;
unsigned long ultimoNivel = 0;
unsigned long ultimoScheduler = 0;
unsigned long ultimoIntentoWifi = 0;
unsigned long ultimoIntentoNTP = 0;
unsigned long ultimoEnvioTelegram = 0;
unsigned long ultimoHealthCheck = 0;
unsigned long ultimoLimpiarTelegram = 0;
unsigned long ultimoGuardado = 0;
unsigned long ultimoResetWDT = 0;
unsigned long ultimoLoopExit = 0;
unsigned long ultimoReinicio = 0;
unsigned long maxTiempoLoop = 0;
unsigned long inicioLoop = 0;
unsigned long ultimoMensajeTelegram = 0;
// ESTAB-12: ULONG_MAX = "nunca enviado". Con {0,0,0,0} el filtro anti-spam disparaba
// en los primeros 30 s de boot porque diffMillis(millis(), 0) < 30000 era siempre true.
unsigned long ultimoMensajeFinRiego[4] = {ULONG_MAX, ULONG_MAX, ULONG_MAX, ULONG_MAX};
unsigned long ultimaActualizacionHora = 0;
unsigned long ultimaLecturaDHT = 0;
unsigned long tiempoInicioSistema = 0;
unsigned long ultimoTickHora = 0;
struct tm tiempoCache;

// v7.6: minuto absoluto basado en epoch para scheduler robusto ante cambio de año/día
static time_t ultimoMinutoAbsoluto = 0;

// TIEMPOS OPTIMIZADOS - PRIORIDAD ESTABILIDAD
const unsigned long intervaloTelegram = 2000;         // 2 segundos
const unsigned long intervaloDHT = 6000;             // 6 segundos
const unsigned long intervaloNivel = 3000;            // 3 segundos (evita rebotes)
const unsigned long intervaloScheduler = 5000;        // 5 segundos
const unsigned long intervaloReconexionWifi = 15000;  // 15 segundos
unsigned long intervaloNTP = 1800000;                 // 30 minutos (configurable via /set ntp)
const unsigned long intervaloHealthCheck = 15000;     // 15 segundos
const unsigned long intervaloLimpiarTelegram = 600000; // 10 minutos
const unsigned long intervaloGuardado = 120000;       // 2 minutos
const unsigned long intervaloMinimoReinicio = 60000;  // 1 minuto
const unsigned long INTERVALO_HORA_CACHE = 5000;      // 5 segundos

//--------------------------------------------------
// AJUSTE HORARIO MANUAL
//--------------------------------------------------
const int AJUSTE_HORARIO_HORAS = 0;

//--------------------------------------------------
// VARIABLES GLOBALES
//--------------------------------------------------
bool cerrandoTodo = false;
bool avisoFinCerrarTodoPendiente = false;
float temperatura = 25.0;
float humedad = 50.0;
bool nivelBajoBalsa = false;
bool nivelMedioBalsa = false;
bool nivelAltoBalsa = false;
bool ventiladorActivo = false;
bool ventiladorForzado = false;   // MEJORA: override manual del ventilador
bool sistemaEnEstadoCritico = false;
bool horaSincronizada = false;
unsigned long contadorRiegoZona[4]   = {0, 0, 0, 0};  // FIX: declaracion global anticipada
unsigned long minutosRiegadoZona[4]  = {0, 0, 0, 0};  // MEJORA: minutos totales regados/zona
bool programacionModificada = false;
bool wifiConectadoAnterior = false;
bool dhtDisponible = false;
bool primeraConexionWiFi = true;

// MEJORA: Modo lluvia (pausa riegos automaticos)
bool modoLluvia = false;

// ─────────────────────────────────────────────────────────────────────────
// ESTAB-9/10/11: Anti-desgaste NVS — dirty flags y guards de delta
//
// Objetivo: reducir escrituras NVS de ~430/día a ~60-100/día para alargar
// la vida útil de la flash (100 000 ciclos por sector, ESP32-S3).
// ─────────────────────────────────────────────────────────────────────────

// ESTAB-9: log circular — solo escribir cuando hay nuevos eventos
bool nvsLogDirty = false;

// ESTAB-10: rest_Z — solo escribir si el tiempo restante bajó > 2 min
// Inicializar a ULONG_MAX para que la primera escritura siempre ocurra.
unsigned long nvsUltimoRestante[4] = {ULONG_MAX, ULONG_MAX, ULONG_MAX, ULONG_MAX};

// ESTAB-11: zona_abierta_Z periódico — evitar reescribir el mismo valor
// -1 = nunca escrita en esta sesión de riego, 0 = false escrito, 1 = true escrito
int8_t nvsUltimaZonaAbierta[4] = {-1, -1, -1, -1};
unsigned long finModoLluvia   = 0;      // millis() en que expira el modo lluvia
unsigned long ultimoPushRelay = 0;      // último push de estado al relay
unsigned long ultimoPullRelay = 0;      // último pull de comandos del relay
// v7.6: eliminados _relayPushClient y _relayPullClient globales — se crean locales
// dentro de cada función para evitar conexiones TCP medio-cerradas entre ciclos.
bool estadoLluviaRestaurado  = false;   // true tras restaurar desde NVS (necesita NTP)
bool riegoActivoRestaurado   = false;   // true tras intentar recuperar riego en curso
static unsigned long ultimoGuardadoRiegoActivo = 0;

// Dirty flags para reducir escrituras NVS
bool riegoActivoDirty = false;
unsigned long riegoActivoUltimoRestante[4] = {ULONG_MAX, ULONG_MAX, ULONG_MAX, ULONG_MAX};
bool statsDirty = false;
unsigned long nvsStatsUltimoGuardado = 0;

// Riego interrumpido por balsa vacía — recuperación en RAM (sin NVS)
// v7.6: ahora también se persiste en NVS para sobrevivir cortes de luz
unsigned long balsaInterrumpidoMs[4]  = {0, 0, 0, 0}; // ms restantes por zona al cortar
long          balsaInterrumpidoEpoch  = 0;             // epoch cuando se cortó el riego

// MEJORA: Alerta temperatura alta (DHT22)
float   tempAlertaUmbral   = 38.0;     // °C — configurable via /temp_umbral
bool    alertaTempEnviada  = false;    // evita spam de alertas

// Umbrales de ventilador configurables en tiempo real via /set
float tempVentiladorOn  = TEMP_VENTILADOR_ON_C;
float tempVentiladorOff = TEMP_VENTILADOR_OFF_C;
float humVentiladorOn   = HUM_VENTILADOR_ON_PCT;
float humVentiladorOff  = HUM_VENTILADOR_OFF_PCT;

// WDT timeout configurable (segundos)
uint32_t wdtTimeoutSegActual = WDT_TIMEOUT_MS / 1000;

// Informe diario automático por Telegram
bool          informeDiarioActivo  = false;
int           informeDiarioHora    = 8;
int           informeDiarioMinuto  = 0;
unsigned long ultimoInformeDiario  = 0;  // evita dobles disparo en el mismo minuto

//--------------------------------------------------
// COLA DE MENSAJES TELEGRAM (no bloqueante)
// Permite enviar mensajes multi-parte (ej: /log largo)
// sin bloquear el loop con while+delay.
// Se encolan en procesarComando() y se drenan uno
// por iteración de loop() desde vaciarColaTelegram().
//--------------------------------------------------
#define COLA_TELEGRAM_MAX 10
static String  _colaTelegram[COLA_TELEGRAM_MAX];
static uint8_t _colaTelIn  = 0;
static uint8_t _colaTelOut = 0;

String estadoBalsa = "Desconocido";
Preferences prefs;

//--------------------------------------------------
// VARIABLES PARA POZOS
//--------------------------------------------------
bool pozoAbierto[2] = {false, false};
bool pozoEnMovimiento[2] = {false, false};
unsigned long pozoInicioMovimiento[2] = {0, 0};
const unsigned long pozoTiempoPulso = 1500;

//--------------------------------------------------
// CONTROL DE REINICIO AUTOMÁTICO
//--------------------------------------------------
bool reinicioAutomaticoActivado = true;
int  reinicioHoraProgramada = 3;
int  reinicioMinutoProgramado = 0;

//--------------------------------------------------
// MONITOREO DE MEMORIA
//--------------------------------------------------
size_t heapMinimoObservado = 0;
size_t maxAllocMinimoObservado = 0;
int contadorReconexionesWifi = 0;
int ciclosLoopLentos = 0;

//--------------------------------------------------
// PROGRAMACION - ESTRUCTURA ACTUALIZADA
//--------------------------------------------------
struct ProgramaRiego {
  bool habilitado;
  uint8_t hora;
  uint8_t minuto;
  bool dias[7];
  uint16_t duracionMin;
};

// Estructura para guardar en Preferences (SIN estado físico)
struct ZonaRiegoGuardado {
  ProgramaRiego programa[3];
  bool riegoAutomaticoActivo;
  unsigned long inicioRiego;
  unsigned long duracionMs;
};

struct ZonaRiego {
  bool abierta;  // Estado físico en tiempo real (NO se guarda en programación)
  ProgramaRiego programa[3];
  bool riegoAutomaticoActivo;
  unsigned long inicioRiego;
  unsigned long duracionMs;
};

ZonaRiego zona[4];

//--------------------------------------------------
// EVENTOS EN MEMORIA ESTÁTICA
//--------------------------------------------------
#define MAX_EVENTOS 40
#define MAX_LEN_EVENTO 120
char logEventos[MAX_EVENTOS][MAX_LEN_EVENTO];
uint8_t indiceEvento = 0;

//--------------------------------------------------
// ESTADOS VALVULAS
//--------------------------------------------------
enum EstadoValvula {
  VALVULA_IDLE,
  VALVULA_ABRIENDO,
  VALVULA_CERRANDO
};

EstadoValvula estadoValvula = VALVULA_IDLE;
int zonaEnMovimiento = -1;
unsigned long inicioMovimiento = 0;

// Zonas pendientes de reapertura tras microcorte (se abren en loop de una en una)
bool zonaReabrirPendiente[4] = {false, false, false, false};
const unsigned long tiempoPulso = 1500;
const unsigned long timeoutMovimiento = 8000;

//--------------------------------------------------
// ANTI-REBOTES BOYAS
//--------------------------------------------------
bool lecturaMecanicaBajoAnterior = false;
bool lecturaMecanicaMedioAnterior = false;
bool lecturaMecanicaAltoAnterior = false;
unsigned long tiempoCambioBajo = 0;
unsigned long tiempoCambioMedio = 0;
unsigned long tiempoCambioAlto = 0;
const unsigned long tiempoEstabilizacionBoyas = 8000;

//--------------------------------------------------
// NTP
//--------------------------------------------------
const char* ntpServer  = "pool.ntp.org";
const char* ntpServer2 = "time.google.com";   // FIX: servidor NTP de respaldo
const char* ntpServer3 = "es.pool.ntp.org";   // FIX: servidor NTP regional

//--------------------------------------------------
// DECLARACIONES ANTICIPADAS
//--------------------------------------------------
void procesarComando(String comando, int64_t chat_id);
void enviarTelegramConLog(String mensaje);
void enviarTelegramConLog(String mensaje, String comando);
void gestionarTelegram();
void monitorearMemoria();
void limpiarMemoria();
void actualizarMinimosMemoria();
void healthCheck();
void limpiarBufferTelegram();
void encolarTelegram(const String& txt);
void vaciarColaTelegram();
void gestionarServidorHttp();
void guardarModoLluvia();
void restaurarModoLluvia();
void guardarLog();
void cargarLog();
void realizarOTA(const String& url);
void verificarReinicioProgramado();
void registrarEvento(const String& texto);
void guardarProgramacion();
void cargarProgramacion();
void guardarYReiniciar();
void configurarProgramacionEjemplo();
void actualizarHoraCache();
void verificarEstadoBoyas();
String getEstadoBalsa();
String getErrorAbrir();
void forzarSincronizacionHora();
String mensajeProgramacion(int zonaNum);

void continuarCerrandoTodo();  // Forward declaration (usada en gestionarValvulas antes de definirse)
void verificarModoLluvia();    // Forward declaration
String proximosRiegos();       // Forward declaration
String mensajeInformeDiario(); // Forward declaration
bool procesarComandoConsulta(const String& texto, int64_t chat_id);
bool procesarComandoSistema(const String& texto, int64_t chat_id);
bool procesarComandoZonas(const String& texto, int64_t chat_id);
bool procesarComandoProgramacion(const String& texto, int64_t chat_id);
bool procesarComandoAuxiliar(const String& texto, int64_t chat_id);
// Relay remoto
int  buildEstadoJson(char* buf, int size);
void pushEstadoRemoto();
void pullComandos();
String _relayGetVal(const String& json, const String& key);
// Recuperación de riego tras reinicio
void guardarRiegoActivo();
void restaurarRiegoActivo();
void reanudarRiegoBalsaRecuperada();
bool extenderRiego(int z, int minutos); // Forward declaration
// v7.6: guardado atómico de estado crítico
void guardarEstadoCritico();

//--------------------------------------------------
// DECLARACIONES PARA POZOS
//--------------------------------------------------
void abrirPozo(int num);
void cerrarPozo(int num);
void gestionarPozos();
String getEstadoPozos();
void togglePozo(int num);

//--------------------------------------------------
// FUNCIONES AUXILIARES
//--------------------------------------------------
unsigned long diffMillis(unsigned long ahora, unsigned long anterior) {
  if (ahora >= anterior) return ahora - anterior;
  return (ULONG_MAX - anterior) + ahora + 1;
}

// ESTAB-2: helper seguro para tiempo restante de riego — evita subdesborde unsigned
// si el scheduler llama a mensajeEstado()/mensajeDiagnostico() justo cuando duracionMs se agota.
inline unsigned long restanteMs(unsigned long duracionMs, unsigned long inicioRiego) {
  unsigned long transcurrido = diffMillis(millis(), inicioRiego);
  return (transcurrido < duracionMs) ? (duracionMs - transcurrido) : 0UL;
}

// FIX: helper para obtener IP sin String temporal que deja puntero colgante
// Uso: char ipBuf[24]; fmtIp(ipBuf, sizeof(ipBuf));
void fmtIp(char* buf, size_t len) {
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ipLocal = WiFi.localIP();  // v7.6: renombrada variable local para evitar sombra
    snprintf(buf, len, "%d.%d.%d.%d", ipLocal[0], ipLocal[1], ipLocal[2], ipLocal[3]);
  } else {
    strncpy(buf, "OFFLINE", len);
    buf[len-1] = '\0';
  }
}

//--------------------------------------------------
// FUNCIONES WRAPPER PARA PREFERENCES (legacy — preferir guardarEstadoCritico)
//--------------------------------------------------
void guardarPreferenceBool(const char* key, bool value) {
    prefs.begin("riego", false);
    prefs.putBool(key, value);
    prefs.end();
}

bool leerPreferenceBool(const char* key, bool defaultValue) {
    prefs.begin("riego", true);  // FIX: solo-lectura
    bool value = prefs.getBool(key, defaultValue);
    prefs.end();
    return value;
}

void guardarPreferenceULong(const char* key, unsigned long value) {
    prefs.begin("riego", false);
    prefs.putULong(key, value);
    prefs.end();
}

unsigned long leerPreferenceULong(const char* key, unsigned long defaultValue) {
    prefs.begin("riego", true);  // FIX: solo-lectura
    unsigned long value = prefs.getULong(key, defaultValue);
    prefs.end();
    return value;
}

void guardarPreferenceInt(const char* key, int value) {
    prefs.begin("riego", false);
    prefs.putInt(key, value);
    prefs.end();
}

int leerPreferenceInt(const char* key, int defaultValue) {
    prefs.begin("riego", true);  // FIX: solo-lectura
    int value = prefs.getInt(key, defaultValue);
    prefs.end();
    return value;
}

void iniciarWatchdog() {
  // FIX: en core Arduino ESP32 2.x/3.x el TWDT ya está inicializado
  // y el loopTask ya está registrado. NO tocarlo en runtime — reconfigurar
  // el WDT en caliente corrompe el estado interno del core y provoca
  // panic inmediato (task_wdt triggered en loopTask). Solo informar.
  Serial.print("WDT core OK - Timeout config: ");
  Serial.print(wdtTimeoutSegActual);
  Serial.println(" s (gestionado por core Arduino, NO reconfigurable en vuelo)");
}

//--------------------------------------------------
// v7.6: GUARDADO ATÓMICO DE ESTADO CRÍTICO
// Agrupa en una sola transacción NVS todo lo que debe sobrevivir a un
// corte de luz: restantes de riego, estados de válvula, pozos, balsa
// interrumpida y modo lluvia. Reduce wear y elimina estados inconsistentes.
//--------------------------------------------------
void guardarEstadoCritico() {
    prefs.begin("riego", false);
    // Zonas: restantes y estado abierta
    for (int z = 0; z < 4; z++) {
        char _kRest[16], _kZA[24], _kRZ[8];
        snprintf(_kRest, sizeof(_kRest), "rest_%d", z);
        snprintf(_kZA, sizeof(_kZA), "zona_abierta_%d", z);
        snprintf(_kRZ, sizeof(_kRZ), "rz_r%d", z);
        if (zona[z].riegoAutomaticoActivo) {
            unsigned long rest = restanteMs(zona[z].duracionMs, zona[z].inicioRiego);
            prefs.putULong(_kRest, rest);
            prefs.putBool(_kZA, true);
            prefs.putLong(_kRZ, (long)rest);
        } else {
            prefs.remove(_kRest);
            prefs.putBool(_kZA, zona[z].abierta);
            prefs.putLong(_kRZ, 0L);
        }
    }
    // Pozos
    for (int i = 0; i < 2; i++) {
        char _kPozo[20];
        snprintf(_kPozo, sizeof(_kPozo), "pozo_abierto_%d", i);
        prefs.putBool(_kPozo, pozoAbierto[i]);
    }
    // Balsa interrumpido (v7.6: persistir para recuperar tras corte de luz)
    prefs.putLong("balsa_ie", balsaInterrumpidoEpoch);
    for (int z = 0; z < 4; z++) {
        char _kBI[16];
        snprintf(_kBI, sizeof(_kBI), "balsa_im%d", z);
        prefs.putULong(_kBI, balsaInterrumpidoMs[z]);
    }
    prefs.end();
    // Modo lluvia en namespace separado
    guardarModoLluvia();
}

//--------------------------------------------------
// HORA - CON TIMEOUT Y REINTENTOS
//--------------------------------------------------
// No bloqueante: intento único. Si NTP aún no respondió, devuelve la caché.
// gestionarNTP() reintentará en background en el siguiente ciclo.
bool obtenerHora(struct tm &tiempo) {
    if (getLocalTime(&tiempo, 0) && tiempo.tm_year > 100) {
        horaSincronizada = true;
        return true;
    }
    if (horaSincronizada) {   // fallback: caché válida del último tick
        tiempo = tiempoCache;
        return true;
    }
    horaSincronizada = false;
    return false;
}

void actualizarHoraCache() {
    struct tm ahora;

    if (getLocalTime(&ahora, 0)) {
        if (ahora.tm_year > 100) {
            tiempoCache = ahora;
            horaSincronizada = true;
            ultimaActualizacionHora = millis();
            return;
        }
    }

    horaSincronizada = false;
}

String obtenerTimestamp() {
    struct tm tiempoLocal;

    if (!getLocalTime(&tiempoLocal, 0)) {
        if (!horaSincronizada) {
            return "⏳ SIN HORA";
        }
        int horaAjustada = tiempoCache.tm_hour + AJUSTE_HORARIO_HORAS;
        if (horaAjustada < 0) horaAjustada += 24;
        if (horaAjustada >= 24) horaAjustada -= 24;
        char buffer[20];
        // ESTAB-1: snprintf evita overflow de buffer
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", horaAjustada, tiempoCache.tm_min, tiempoCache.tm_sec);
        return String(buffer);
    }

    if (tiempoLocal.tm_year < 100) {
        horaSincronizada = false;
        return "⏳ SIN HORA";
    }

    tiempoCache = tiempoLocal;
    horaSincronizada = true;
    ultimaActualizacionHora = millis();

    int horaAjustada = tiempoLocal.tm_hour + AJUSTE_HORARIO_HORAS;
    if (horaAjustada < 0) horaAjustada += 24;
    if (horaAjustada >= 24) horaAjustada -= 24;

    char buffer[20];
    // ESTAB-1: snprintf evita overflow de buffer
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", horaAjustada, tiempoLocal.tm_min, tiempoLocal.tm_sec);
    return String(buffer);
}

void forzarSincronizacionHora() {
    // No bloqueante: un único intento. Si el servidor NTP aún no respondió,
    // gestionarNTP() lo volverá a intentar en el siguiente intervalo.
    struct tm tiempo;
    Serial.println("🔄 Intentando sincronización de hora...");
    if (getLocalTime(&tiempo, 0) && tiempo.tm_year > 100) {
        tiempoCache = tiempo;
        horaSincronizada = true;
        ultimaActualizacionHora = millis();
        Serial.println("✅ Hora sincronizada: " + obtenerTimestamp());
        registrarEvento("✅ Hora sincronizada");
    } else {
        Serial.println("⏳ NTP aún no disponible — se reintentará en background");
        // No tocar horaSincronizada si ya estaba sincronizada (la caché sigue válida)
    }
}

// v7.6: registrarEvento() construye la línea en stack char[] sin concatenar
// Strings intermedias. Reduce fragmentación de heap y es más rápido.
void registrarEvento(const String& texto) {
  String timestamp = obtenerTimestamp();
  char buffer[MAX_LEN_EVENTO];
  int n = snprintf(buffer, sizeof(buffer), "[%s] %s", timestamp.c_str(), texto.c_str());
  if (n < 0 || n >= (int)sizeof(buffer)) {
    // Truncar si excede
    strncpy(buffer + MAX_LEN_EVENTO - 4, "...", 4);
  }
  strncpy(logEventos[indiceEvento], buffer, MAX_LEN_EVENTO - 1);
  logEventos[indiceEvento][MAX_LEN_EVENTO - 1] = '\0';
  indiceEvento = (indiceEvento + 1) % MAX_EVENTOS;
  nvsLogDirty = true;  // ESTAB-9: marcar log como sucio para persistencia diferida
  Serial.println(buffer);
}

// v7.6: obtenerLog ahora recibe String por referencia para evitar copias
void obtenerLog(String& dest) {
  dest.reserve(4000);
  dest = "";
  for (int i = 0; i < MAX_EVENTOS; i++) {
    yield();  // v7.6: ceder CPU en bucle largo para evitar WDT
    int idx = (indiceEvento + i) % MAX_EVENTOS;
    if (strlen(logEventos[idx]) > 0) {
      dest += logEventos[idx];
      dest += "\n";
      if (dest.length() > 4000) {
        dest += "\n... [Log truncado]";
        break;
      }
    }
  }
}

//--------------------------------------------------
// WIFI
//--------------------------------------------------
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  // v7.6: usar ipFija en lugar de ip (renombrado para evitar sombra)
  WiFi.config(ipFija, gateway, subnet, dns);
  WiFi.begin(ssid, password);
  unsigned long inicio = millis();
  Serial.print("Conectando WiFi");
  // FIX: no bloquear más de 5s. WiFi.begin() es asíncrono;
  // gestionarWiFi() en loop() se encarga de reconectar si falla.
  while (WiFi.status() != WL_CONNECTED && diffMillis(millis(), inicio) < 5000UL) {
    delay(100);
    yield();  // cede CPU para WDT interno del core
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    char ipBuf[24]; fmtIp(ipBuf, sizeof(ipBuf));
    registrarEvento("WiFi conectado. IP: " + String(ipBuf));
    contadorReconexionesWifi = 0;
    primeraConexionWiFi = true;
  } else {
    registrarEvento("WiFi pendiente — se reconectará en loop()");
    primeraConexionWiFi = false;
  }
  wifiConectadoAnterior = (WiFi.status() == WL_CONNECTED);
}


void gestionarWiFi() {
    bool conectado = (WiFi.status() == WL_CONNECTED);

    if (conectado == wifiConectadoAnterior) {
        if (conectado) return;
    }

    if (!conectado) {
        if (diffMillis(millis(), ultimoIntentoWifi) > intervaloReconexionWifi) {
            contadorReconexionesWifi++;

            if (contadorReconexionesWifi == 1) {
                registrarEvento("⚠️ WiFi desconectado - Intentando reconectar...");
            } else {
                registrarEvento("Reconectando WiFi... (Intento " + String(contadorReconexionesWifi) + ")");
            }

            // FIX #4: alerta Telegram al 5º intento fallido (cuando WiFi vuelva, se enviará)
            if (contadorReconexionesWifi == 5) {
                registrarEvento("⚠️ 5 intentos WiFi fallidos - señal debil?");
                // Se encola en log; se enviará cuando se recupere la conexión
            }

            if (contadorReconexionesWifi > 10) {
                registrarEvento("⚠️ Reiniciando WiFi por reconexiones fallidas");
                WiFi.mode(WIFI_OFF);
                delay(200);
                WiFi.mode(WIFI_STA);
                // v7.6: ipFija
                WiFi.config(ipFija, gateway, subnet, dns);
                WiFi.begin(ssid, password);
                contadorReconexionesWifi = 0;
                ultimoIntentoWifi = millis();
            } else {
                WiFi.disconnect(true);
                delay(100);    // FIX #4: pequeña pausa para que el driver libere el estado
                // v7.6: ipFija
                WiFi.config(ipFija, gateway, subnet, dns);
                WiFi.begin(ssid, password);
                ultimoIntentoWifi = millis();
            }
        }
    } else {
        if (!wifiConectadoAnterior && contadorReconexionesWifi > 0) {
            registrarEvento("✅ WiFi recuperado");
            contadorReconexionesWifi = 0;
        }
        if (contadorReconexionesWifi == 0 && primeraConexionWiFi) {
            primeraConexionWiFi = false;
        }
    }

    wifiConectadoAnterior = conectado;
}

//--------------------------------------------------
// NTP
//--------------------------------------------------
void iniciarHora() {
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", ntpServer, ntpServer2, ntpServer3);
  horaSincronizada = false;
}

void gestionarNTP() {
  // ESTAB-3: usar diffMillis() garantiza corrección en el overflow de millis() (~49.7 días)
  unsigned long _ahoraNTP = millis();
  if (diffMillis(_ahoraNTP, ultimoIntentoNTP) >= intervaloNTP) {
    ultimoIntentoNTP = _ahoraNTP;
    // FIX #3: no re-registrar servidores NTP si ya estamos sincronizados
    // Llamar a configTzTime() repetidamente sin sntp_stop() puede generar timers huerfanos
    // Si ya hay hora, solo forzar lectura del cache; si no la hay, reinicializar
    if (!horaSincronizada) {
      registrarEvento("Reintentando sincronizacion NTP...");
      iniciarHora();         // Re-registra servidores y arranca de nuevo
    } else {
      actualizarHoraCache(); // Solo refrescar cache local, sin tocar SNTP
    }
  }
}

//--------------------------------------------------
// VALVULAS PRINCIPALES (ZONAS 1-4)
//--------------------------------------------------
void detenerMovimientoValvula() {
  if (zonaEnMovimiento < 0) return;
  digitalWrite(pinAbrir[zonaEnMovimiento], LOW);
  digitalWrite(pinCerrar[zonaEnMovimiento], LOW);
  zonaEnMovimiento = -1;
  estadoValvula = VALVULA_IDLE;
}

bool valvulaLibre() {
  return estadoValvula == VALVULA_IDLE;
}

bool abrirZona(int z) {
    if (z < 0 || z > 3) {
        ultimoErrorAbrir = ERR_ZONA_INVALIDA;
        return false;
    }
    if (!nivelBajoBalsa) {
        ultimoErrorAbrir = ERR_BALSA_CRITICA;
        return false;
    }
    if (zona[z].abierta) {
        ultimoErrorAbrir = ERR_YA_ABIERTA;
        return true;
    }
    if (!valvulaLibre()) {
        ultimoErrorAbrir = ERR_VALVULA_OCUPADA;
        return false;
    }

    digitalWrite(pinCerrar[z], LOW);
    digitalWrite(pinAbrir[z], HIGH);
    zonaEnMovimiento = z;
    estadoValvula = VALVULA_ABRIENDO;
    inicioMovimiento = millis();
    registrarEvento("Abriendo zona " + String(z + 1));
    ultimoErrorAbrir = ERR_OK;
    return true;
}

String getErrorAbrir() {
    switch(ultimoErrorAbrir) {
        case ERR_OK: return "OK";
        case ERR_ZONA_INVALIDA: return "Zona inválida (1-4)";
        case ERR_BALSA_CRITICA: return "Balsa crítica - Sin agua";
        case ERR_VALVULA_OCUPADA: return "Válvula ocupada - Esperando";
        case ERR_YA_ABIERTA: return "Ya estaba abierta";
        default: return "Error desconocido";
    }
}

bool cerrarZona(int z) {
  if (z < 0 || z > 3) return false;
  if (!zona[z].abierta) return true;
  if (!valvulaLibre()) return false;

  digitalWrite(pinAbrir[z], LOW);
  digitalWrite(pinCerrar[z], HIGH);
  zonaEnMovimiento = z;
  estadoValvula = VALVULA_CERRANDO;
  inicioMovimiento = millis();
  registrarEvento("Cerrando zona " + String(z + 1));
  return true;
}

void gestionarValvulas() {
  if (estadoValvula != VALVULA_IDLE) {
    // ESTAB-3: diffMillis() para seguridad en overflow
    if (diffMillis(millis(), inicioMovimiento) > timeoutMovimiento) {
      String msgTimeout = "⚠️ TIMEOUT: valvula Zona " + String(zonaEnMovimiento + 1) + " no respondio\nRevisa mecanismo fisicamente";
      registrarEvento(msgTimeout);
      if (WiFi.status() == WL_CONNECTED) enviarTelegramConLog("🔴 " + msgTimeout);  // MEJORA: alerta por Telegram
      detenerMovimientoValvula();
      if (cerrandoTodo) continuarCerrandoTodo();
      return;
    }
  }

  if (estadoValvula == VALVULA_IDLE && cerrandoTodo) {
    continuarCerrandoTodo();
    return;
  }

  if (estadoValvula == VALVULA_IDLE) return;
  // ESTAB-3
  if (diffMillis(millis(), inicioMovimiento) < tiempoPulso) return;

  int z = zonaEnMovimiento;
  if (estadoValvula == VALVULA_ABRIENDO) {
    zona[z].abierta = true;
    marcarProgramacionModificada();
    registrarEvento("Zona " + String(z + 1) + " abierta");
    // Guardar estado en NVS — permite restaurar zona manual tras reinicio
    char _kZAo[24]; snprintf(_kZAo, sizeof(_kZAo), "zona_abierta_%d", z);
    guardarPreferenceBool(_kZAo, true);
  }
  if (estadoValvula == VALVULA_CERRANDO) {
    zona[z].abierta = false;
    marcarProgramacionModificada();
    registrarEvento("Zona " + String(z + 1) + " cerrada");
    // Guardar estado cerrado en NVS — sin esto todas aparecían abiertas tras reinicio
    char _kZAc[24]; snprintf(_kZAc, sizeof(_kZAc), "zona_abierta_%d", z);
    guardarPreferenceBool(_kZAc, false);
    nvsUltimaZonaAbierta[z] = 0;  // reset ESTAB-11 para próxima apertura
  }

  detenerMovimientoValvula();
  if (cerrandoTodo) continuarCerrandoTodo();
}

void continuarCerrandoTodo() {
  if (!cerrandoTodo || !valvulaLibre()) return;

  for (int i = 0; i < 4; i++) {
    if (zona[i].abierta) {
      digitalWrite(pinAbrir[i], LOW);
      digitalWrite(pinCerrar[i], HIGH);
      zonaEnMovimiento = i;
      estadoValvula = VALVULA_CERRANDO;
      inicioMovimiento = millis();
      registrarEvento("Cerrando zona " + String(i + 1));
      return;
    }
  }

  cerrandoTodo = false;
  registrarEvento("Cerrartodo completado");
  if (avisoFinCerrarTodoPendiente && WiFi.status() == WL_CONNECTED) {
    enviarTelegramConLog("CERRARTODO FINALIZADO\n\nTodas las zonas cerradas.");
    avisoFinCerrarTodoPendiente = false;
  }
}

void cerrarTodasLasZonas() {
  for (int i = 0; i < 4; i++) {
    zona[i].riegoAutomaticoActivo = false;
  }

  bool hayZonasAbiertas = false;
  for (int i = 0; i < 4; i++) {
    if (zona[i].abierta) {
      hayZonasAbiertas = true;
      break;
    }
  }

  if (!hayZonasAbiertas) return;

  cerrandoTodo = true;
  avisoFinCerrarTodoPendiente = true;
  registrarEvento("Iniciando cerrartodo");
  continuarCerrandoTodo();
}

//--------------------------------------------------
// FUNCIONES PARA POZOS (CON GUARDADO DE ESTADO)
//--------------------------------------------------
void abrirPozo(int num) {
    if (num < 0 || num > 1) {
        registrarEvento("❌ Pozo inválido: " + String(num + 1));
        return;
    }

    if (pozoAbierto[num]) {
        enviarTelegramConLog("⚠️ POZO " + String(num + 1) + " YA ESTÁ ABIERTO");
        return;
    }

    if (pozoEnMovimiento[num]) {
        enviarTelegramConLog("⚠️ POZO " + String(num + 1) + " está en movimiento, espera...");
        return;
    }

    digitalWrite(pinPozoCerrar[num], LOW);
    digitalWrite(pinPozoAbrir[num], HIGH);
    pozoEnMovimiento[num] = true;
    pozoInicioMovimiento[num] = millis();

    // FIX: NO guardar estado aqui - se guarda en gestionarPozos() cuando el pulso completa
    // Guardar antes del movimiento deja estado incorrecto si hay reinicio durante el pulso (1500ms)

    registrarEvento("🔓 Abriendo POZO " + String(num + 1));
    enviarTelegramConLog("🔓 Abriendo POZO " + String(num + 1));
}

void cerrarPozo(int num) {
    if (num < 0 || num > 1) {
        registrarEvento("❌ Pozo inválido: " + String(num + 1));
        return;
    }

    if (!pozoAbierto[num]) {
        enviarTelegramConLog("⚠️ POZO " + String(num + 1) + " YA ESTÁ CERRADO");
        return;
    }

    if (pozoEnMovimiento[num]) {
        enviarTelegramConLog("⚠️ POZO " + String(num + 1) + " está en movimiento, espera...");
        return;
    }

    digitalWrite(pinPozoAbrir[num], LOW);
    digitalWrite(pinPozoCerrar[num], HIGH);
    pozoEnMovimiento[num] = true;
    pozoInicioMovimiento[num] = millis();

    // FIX: NO guardar estado aqui - se guarda en gestionarPozos() cuando el pulso completa

    registrarEvento("🔒 Cerrando POZO " + String(num + 1));
    enviarTelegramConLog("🔒 Cerrando POZO " + String(num + 1));
}

void togglePozo(int num) {
    if (num < 0 || num > 1) {
        registrarEvento("❌ Pozo inválido: " + String(num + 1));
        return;
    }

    if (pozoAbierto[num]) {
        cerrarPozo(num);
    } else {
        abrirPozo(num);
    }
}

void gestionarPozos() {
    for (int i = 0; i < 2; i++) {
        if (!pozoEnMovimiento[i]) continue;

        // ESTAB-3
        if (diffMillis(millis(), pozoInicioMovimiento[i]) > 5000) {
            digitalWrite(pinPozoAbrir[i], LOW);
            digitalWrite(pinPozoCerrar[i], LOW);
            pozoEnMovimiento[i] = false;

            // ESTAB-7: char buf estático evita String temporal con c_str() de vida corta
            char _keyPozo[20];
            snprintf(_keyPozo, sizeof(_keyPozo), "pozo_abierto_%d", i);
            guardarPreferenceBool(_keyPozo, pozoAbierto[i]);

            registrarEvento("⚠️ TIMEOUT POZO " + String(i + 1));
            enviarTelegramConLog("⚠️ TIMEOUT POZO " + String(i + 1));
            continue;
        }

        // ESTAB-3
        if (diffMillis(millis(), pozoInicioMovimiento[i]) >= pozoTiempoPulso) {
            digitalWrite(pinPozoAbrir[i], LOW);
            digitalWrite(pinPozoCerrar[i], LOW);
            pozoEnMovimiento[i] = false;
            pozoAbierto[i] = !pozoAbierto[i];

            // ESTAB-7
            char _keyPozo2[20];
            snprintf(_keyPozo2, sizeof(_keyPozo2), "pozo_abierto_%d", i);
            guardarPreferenceBool(_keyPozo2, pozoAbierto[i]);

            String estado = pozoAbierto[i] ? "ABIERTO ✅" : "CERRADO 🔒";
            registrarEvento("✅ POZO " + String(i + 1) + " " + estado);
            enviarTelegramConLog("✅ POZO " + String(i + 1) + " " + estado);
        }
    }
}

String getEstadoPozos() {
    String msg = "";
    for (int i = 0; i < 2; i++) {
        msg += "  POZO " + String(i + 1) + ": ";
        msg += pozoAbierto[i] ? "🔓 ABIERTO" : "🔒 CERRADO";
        if (pozoEnMovimiento[i]) msg += " [⏳ MOVIENDO...]";
        msg += "\n";
    }
    return msg;
}

//--------------------------------------------------
// DHT22 - LECTURA DE TEMPERATURA Y HUMEDAD
//--------------------------------------------------
void leerDHT() {
    // v7.6: usar diffMillis para consistencia con el resto del código
    unsigned long ahora = millis();
    if (diffMillis(ahora, ultimaLecturaDHT) < intervaloDHT) return;
    ultimaLecturaDHT = ahora;

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (isnan(h) || isnan(t)) {
        if (dhtDisponible) {
            dhtDisponible = false;
            registrarEvento("⚠️ DHT22: Sensor desconectado");
        }
        return;
    }

    if (!dhtDisponible) {
        dhtDisponible = true;
        registrarEvento("✅ DHT22: Sensor recuperado");
    }

    humedad = h;
    temperatura = t;

    // MEJORA #14: alerta si la temperatura supera el umbral configurado
    if (temperatura >= tempAlertaUmbral && !alertaTempEnviada) {
        alertaTempEnviada = true;
        registrarEvento("🌡️ ALERTA: Temperatura " + String(temperatura, 1) + "°C (umbral " + String(tempAlertaUmbral, 0) + "°C)");
        if (WiFi.status() == WL_CONNECTED) {
            enviarTelegramConLog("🌡️⚠️ TEMPERATURA ALTA\nActual: " + String(temperatura, 1) + " °C\nHumedad: " + String(humedad, 1) + " %\nUmbral: " + String(tempAlertaUmbral, 0) + " °C");
        }
    }
    if (temperatura < tempAlertaUmbral - 2.0) {
        alertaTempEnviada = false;  // reset cuando baja 2°C por debajo del umbral (histéresis)
    }
}

void gestionarVentilador() {
    // MEJORA: si esta en modo forzado manual, no aplicar logica automatica
    if (ventiladorForzado) return;

    if (!dhtDisponible) {
        if (ventiladorActivo) {
            digitalWrite(pinVentilador, LOW);
            ventiladorActivo = false;
        }
        return;
    }

    if (temperatura > tempVentiladorOn || humedad > humVentiladorOn) {
        if (!ventiladorActivo) {
            digitalWrite(pinVentilador, HIGH);
            ventiladorActivo = true;
            registrarEvento("🔵 Ventilador ON - Temp: " + String(temperatura, 1) + "°C Hum: " + String(humedad, 1) + "%");
        }
    } else {
        if (ventiladorActivo && temperatura < tempVentiladorOff && humedad < humVentiladorOff) {
            digitalWrite(pinVentilador, LOW);
            ventiladorActivo = false;
            registrarEvento("⚪ Ventilador OFF - Temp: " + String(temperatura, 1) + "°C Hum: " + String(humedad, 1) + "%");
        }
    }
}

void iniciarVentilador() {
  pinMode(pinVentilador, OUTPUT);
  digitalWrite(pinVentilador, LOW);
  ventiladorActivo = false;
}

//--------------------------------------------------
// BALSA - VERSIÓN 3 BOYAS
//--------------------------------------------------
void gestionarNivelBalsa() {
    bool lecturaInferior = (digitalRead(pinBoyaInferior) == LOW);
    bool lecturaMedia = (digitalRead(pinBoyaMedia) == LOW);
    bool lecturaSuperior = (digitalRead(pinBoyaSuperior) == LOW);

    static String ultimoEstadoEnviado = "";
    static String ultimoEstadoRegistrado = "";

    if (lecturaInferior != lecturaMecanicaBajoAnterior) {
        lecturaMecanicaBajoAnterior = lecturaInferior;
        tiempoCambioBajo = millis();
    }
    if (lecturaMedia != lecturaMecanicaMedioAnterior) {
        lecturaMecanicaMedioAnterior = lecturaMedia;
        tiempoCambioMedio = millis();
    }
    if (lecturaSuperior != lecturaMecanicaAltoAnterior) {
        lecturaMecanicaAltoAnterior = lecturaSuperior;
        tiempoCambioAlto = millis();
    }

    if (diffMillis(millis(), tiempoCambioBajo) > tiempoEstabilizacionBoyas &&
        diffMillis(millis(), tiempoCambioMedio) > tiempoEstabilizacionBoyas &&
        diffMillis(millis(), tiempoCambioAlto) > tiempoEstabilizacionBoyas) {

        bool nuevaInferior = lecturaInferior;
        bool nuevaMedia = lecturaMedia;
        bool nuevaSuperior = lecturaSuperior;

        bool huboCambio = false;

        if (nuevaInferior != nivelBajoBalsa) {
            nivelBajoBalsa = nuevaInferior;
            huboCambio = true;
            if (!nivelBajoBalsa) {
                registrarEvento("🔴 BALSA VACÍA - Nivel CRÍTICO");
                if (WiFi.status() == WL_CONNECTED && ultimoEstadoEnviado != "critico") {
                    enviarTelegramConLog("🔴🔴🔴 BALSA VACÍA\nNivel CRÍTICO - Riegos DETENIDOS");
                    ultimoEstadoEnviado = "critico";
                }
            } else {
                registrarEvento("🟢 Balsa empezando a llenarse (nivel inferior)");
                if (WiFi.status() == WL_CONNECTED) {
                    enviarTelegramConLog("🟢 BALSA EMPEZANDO A LLENARSE\nNivel inferior superado");
                    ultimoEstadoEnviado = "";
                }
            }
        }

        if (nuevaMedia != nivelMedioBalsa) {
            nivelMedioBalsa = nuevaMedia;
            huboCambio = true;
            if (!nivelMedioBalsa) {
                registrarEvento("🟡 Balsa al 50% - Nivel MEDIO");
                if (WiFi.status() == WL_CONNECTED && ultimoEstadoEnviado != "medio") {
                    enviarTelegramConLog("🟡 BALSA AL 50%\nNivel MEDIO alcanzado");
                    ultimoEstadoEnviado = "medio";
                }
            } else {
                registrarEvento("🟢 Balsa supera nivel medio (>50%)");
                if (WiFi.status() == WL_CONNECTED && ultimoEstadoEnviado != "supera_medio") {
                    enviarTelegramConLog("🟢 BALSA SUPERA NIVEL MEDIO\n>50% de capacidad");
                    ultimoEstadoEnviado = "supera_medio";
                }
            }
        }

        if (nuevaSuperior != nivelAltoBalsa) {
            nivelAltoBalsa = nuevaSuperior;
            huboCambio = true;
            if (nivelAltoBalsa) {
                registrarEvento("🟢 BALSA LLENA (100%)");
                if (WiFi.status() == WL_CONNECTED && ultimoEstadoEnviado != "llena") {
                    enviarTelegramConLog("🟢🟢🟢 BALSA LLENA\n100% de capacidad");
                    ultimoEstadoEnviado = "llena";
                }
            } else {
                registrarEvento("🟡 Balsa empezando a vaciarse (nivel superior)");
                if (WiFi.status() == WL_CONNECTED && ultimoEstadoEnviado != "vaciando") {
                    enviarTelegramConLog("🟡 BALSA EMPEZANDO A VACIARSE\nNivel superior descendido");
                    ultimoEstadoEnviado = "vaciando";
                }
            }
        }

        if (huboCambio) {
            estadoBalsa = getEstadoBalsa();  // FIX: reusar getEstadoBalsa() en lugar de duplicar la logica if/else

            if (estadoBalsa != ultimoEstadoRegistrado) {
                ultimoEstadoRegistrado = estadoBalsa;
                registrarEvento("📊 Nivel: " + estadoBalsa);
            }
        }
    }
}

String getEstadoBalsa() {
    if (nivelAltoBalsa && nivelMedioBalsa && nivelBajoBalsa) {
        return "🟢 BALSA LLENA";
    } else if (!nivelAltoBalsa && nivelMedioBalsa && nivelBajoBalsa) {
        return "🟡 NIVEL ALTO (75-100%)";
    } else if (!nivelAltoBalsa && !nivelMedioBalsa && nivelBajoBalsa) {
        return "🟠 NIVEL MEDIO (25-50%)";
    } else if (!nivelAltoBalsa && !nivelMedioBalsa && !nivelBajoBalsa) {
        return "🔴 NIVEL CRÍTICO (0-25%)";
    } else {
        return "⚪ ESTADO INDEFINIDO";
    }
}

String diagnosticarBoyas() {
    String info = "📊 ESTADO BOYAS:\n═══════════════════\n\n";
    info += "🔴 INFERIOR (GPIO14):  " + String(digitalRead(pinBoyaInferior));
    info += (nivelBajoBalsa ? " → ✅ SUMERGIDA (HAY AGUA)\n" : " → ⚠️ SECA (SIN AGUA)\n");
    info += "🟡 MEDIA (GPIO15):   " + String(digitalRead(pinBoyaMedia));
    info += (nivelMedioBalsa ? " → ✅ SUMERGIDA (HAY AGUA)\n" : " → ⚠️ SECA (SIN AGUA)\n");
    info += "🟢 SUPERIOR (GPIO16): " + String(digitalRead(pinBoyaSuperior));
    info += (nivelAltoBalsa ? " → ✅ SUMERGIDA (HAY AGUA)\n" : " → ⚠️ SECA (SIN AGUA)\n");
    info += "\n📊 " + getEstadoBalsa();
    return info;
}

//--------------------------------------------------
// VERIFICAR ESTADO DE BOYAS (DETECCIÓN DE AVERÍAS)
//--------------------------------------------------
void verificarEstadoBoyas() {
    static unsigned long ultimoAviso = 0;

    bool inf = nivelBajoBalsa;
    bool med = nivelMedioBalsa;
    bool sup = nivelAltoBalsa;

    String problema = "";

    if (sup && !med && inf) {
        problema = "⚠️ BOYA MEDIA AVERIADA\nSuperior: SUMERGIDA | Media: SECA | Inferior: SUMERGIDA";
    } else if (sup && !med && !inf) {
        problema = "⚠️ BOYA MEDIA AVERIADA\nSuperior: SUMERGIDA | Media: SECA | Inferior: SECA";
    } else if (!sup && med && !inf) {
        problema = "⚠️ BOYA SUPERIOR O MEDIA AVERIADA\nSuperior: SECA | Media: SUMERGIDA | Inferior: SECA";
    } else if (!inf && med && sup) {
        problema = "⚠️ BOYA INFERIOR AVERIADA\nInferior: SECA | Media: SUMERGIDA | Superior: SUMERGIDA";
    }

    if (problema.length() > 0) {
        // ESTAB-3
        if (diffMillis(millis(), ultimoAviso) > 3600000UL) {
            ultimoAviso = millis();
            registrarEvento(problema);
            if (WiFi.status() == WL_CONNECTED) {
                enviarTelegramConLog("🔴 " + problema + "\n\nRevisar boyas físicamente");
            }
        }
    }
}

//--------------------------------------------------
// PROGRAMACION - CORREGIDO
//--------------------------------------------------
void marcarProgramacionModificada() {
  programacionModificada = true;
}

void guardarProgramacion() {
  if (!programacionModificada) return;

  for (int i = 0; i < 4; i++) {
    for (int p = 0; p < 3; p++) {
      if (zona[i].programa[p].duracionMin > 240) {
        zona[i].programa[p].duracionMin = 240;
      }
    }
  }

  // Guardar SOLO la programación en una estructura sin 'abierta'
  ZonaRiegoGuardado zonaParaGuardar[4];
  for (int i = 0; i < 4; i++) {
    for (int p = 0; p < 3; p++) {
      zonaParaGuardar[i].programa[p] = zona[i].programa[p];
    }
    zonaParaGuardar[i].riegoAutomaticoActivo = zona[i].riegoAutomaticoActivo;
    zonaParaGuardar[i].inicioRiego = zona[i].inicioRiego;
    zonaParaGuardar[i].duracionMs = zona[i].duracionMs;
  }

  prefs.begin("riego", false);
  prefs.putBytes("zonas", &zonaParaGuardar, sizeof(zonaParaGuardar));
  prefs.end();

  programacionModificada = false;
  registrarEvento("Programacion guardada OK");
}

void cargarProgramacion() {
  prefs.begin("riego", true);  // FIX: solo-lectura, solo leemos
  size_t tam = prefs.getBytesLength("zonas");
  if (tam == sizeof(ZonaRiegoGuardado) * 4) {  // FIX BUG: comparar tamaño de las 4 zonas, no de 1
    ZonaRiegoGuardado zonaCargada[4];
    prefs.getBytes("zonas", &zonaCargada, sizeof(zonaCargada));

    for (int i = 0; i < 4; i++) {
      for (int p = 0; p < 3; p++) {
        zona[i].programa[p] = zonaCargada[i].programa[p];
      }
      zona[i].riegoAutomaticoActivo = zonaCargada[i].riegoAutomaticoActivo;
      zona[i].inicioRiego = zonaCargada[i].inicioRiego;
      zona[i].duracionMs = zonaCargada[i].duracionMs;
    }
    registrarEvento("Programacion cargada");
    // Sanitizar datos corruptos por corte de luz durante escritura
    for (int i = 0; i < 4; i++) {
      for (int p = 0; p < 3; p++) {
        if (zona[i].programa[p].hora > 23)       zona[i].programa[p].hora = 0;
        if (zona[i].programa[p].minuto > 59)     zona[i].programa[p].minuto = 0;
        if (zona[i].programa[p].duracionMin > 240) zona[i].programa[p].duracionMin = 30;
      }
    }
  }
  prefs.end();
}

//--------------------------------------------------
// PERSISTENCIA DE ESTADO: MODO LLUVIA + LOG CIRCULAR
//--------------------------------------------------

// Guarda la expiración del modo lluvia como timestamp Unix
// (millis() no sobrevive reinicios; epoch sí)
void guardarModoLluvia() {
    prefs.begin("estado", false);
    if (modoLluvia && (long)(millis() - finModoLluvia) < 0) {
        long restanteSeg = max(0L, (long)(finModoLluvia - millis()) / 1000);
        prefs.putLong("lluvia_exp", (long)time(nullptr) + restanteSeg);
    } else {
        prefs.putLong("lluvia_exp", 0L);
    }
    prefs.end();
}

// Restaurar modo lluvia desde NVS — llamar SOLO cuando horaSincronizada==true
// ya que necesita tiempo epoch válido para calcular el restante correcto
void restaurarModoLluvia() {
    prefs.begin("estado", true);
    long expEpoch = prefs.getLong("lluvia_exp", 0L);
    prefs.end();
    if (expEpoch <= 0) return;
    long restanteSeg = expEpoch - (long)time(nullptr);
    if (restanteSeg > 60) {
        modoLluvia = true;
        finModoLluvia = millis() + (unsigned long)restanteSeg * 1000UL;
        String hText = String(restanteSeg / 3600) + "h " + String((restanteSeg % 3600) / 60) + "m";
        registrarEvento("🌧️ Modo lluvia restaurado — " + hText + " restantes");
        if (WiFi.status() == WL_CONNECTED)
            enviarTelegramConLog("🌧️ MODO LLUVIA restaurado tras reinicio\n⏳ " + hText + " restantes");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RECUPERACIÓN DE RIEGO ACTIVO TRAS REINICIO / CORTE DE LUZ
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Guarda en NVS (namespace "riego") el tiempo restante de cada zona activa.
 * Llamada desde loop() cada 30 s mientras haya alguna zona regando.
 * Solo escribe si NTP está sincronizado (necesitamos epoch real para el cálculo).
 */
void guardarRiegoActivo() {
    bool hayActivo = false;
    for (int z = 0; z < 4; z++) {
        if (zona[z].riegoAutomaticoActivo) { hayActivo = true; break; }
    }
    if (!hayActivo) {
        // Si no hay activos pero había datos guardados, limpiar una sola vez
        if (riegoActivoDirty) {
            riegoActivoDirty = false;
            prefs.begin("riego", false);
            prefs.putLong("rz_ep", 0L);
            for (int z = 0; z < 4; z++) {
                char key[8]; snprintf(key, sizeof(key), "rz_r%d", z);
                prefs.putLong(key, 0L);
            }
            prefs.end();
            for (int z = 0; z < 4; z++) riegoActivoUltimoRestante[z] = ULONG_MAX;
        }
        return;
    }

    long epoch = (long)time(nullptr);
    if (epoch < 1000000L) return;   // NTP aún no listo

    // Delta: solo escribir si el restante bajó > 60s o es la primera vez
    // v7.6: también escribir si el restante AUMENTÓ (extensión de riego)
    bool necesitaEscribir = false;
    unsigned long restanteActual[4];
    for (int z = 0; z < 4; z++) {
        restanteActual[z] = zona[z].riegoAutomaticoActivo
            ? restanteMs(zona[z].duracionMs, zona[z].inicioRiego) : 0UL;
        if (zona[z].riegoAutomaticoActivo) {
            if (riegoActivoUltimoRestante[z] == ULONG_MAX) {
                necesitaEscribir = true;
            } else {
                unsigned long diff = (restanteActual[z] > riegoActivoUltimoRestante[z])
                    ? (restanteActual[z] - riegoActivoUltimoRestante[z])
                    : (riegoActivoUltimoRestante[z] - restanteActual[z]);
                if (diff > 60000UL) necesitaEscribir = true;
            }
        }
    }
    if (!necesitaEscribir && riegoActivoDirty) return;

    riegoActivoDirty = true;
    prefs.begin("riego", false);
    prefs.putLong("rz_ep", epoch);
    for (int z = 0; z < 4; z++) {
        char key[8]; snprintf(key, sizeof(key), "rz_r%d", z);
        long rest = zona[z].riegoAutomaticoActivo ? (long)restanteActual[z] : 0L;
        prefs.putLong(key, rest);
        if (zona[z].riegoAutomaticoActivo) riegoActivoUltimoRestante[z] = restanteActual[z];
    }
    prefs.end();
}

/**
 * Al arrancar (una sola vez, tras sincronizar NTP): lee NVS y reanuda las
 * zonas que estaban regando cuando se fue la corriente o el ESP32 se reinició.
 * Descuenta el tiempo apagado usando el epoch guardado vs. el epoch actual.
 */
void restaurarRiegoActivo() {
    prefs.begin("riego", true);
    long epochGuardado = prefs.getLong("rz_ep", 0L);
    long rest[4];
    for (int z = 0; z < 4; z++) {
        char key[8]; snprintf(key, sizeof(key), "rz_r%d", z);
        rest[z] = prefs.getLong(key, 0L);
    }
    prefs.end();

    if (epochGuardado <= 0L) return;   // nunca se guardó nada

    long epoch = (long)time(nullptr);
    long transcurridoMs = (epoch - epochGuardado) * 1000L;

    // Datos inválidos: tiempo negativo o más de 4 horas apagado
    if (transcurridoMs < 0L || transcurridoMs > 14400000L) {
        prefs.begin("riego", false); prefs.putLong("rz_ep", 0L); prefs.end();
        return;
    }

    bool restaurado = false;
    for (int z = 0; z < 4; z++) {
        if (rest[z] <= 0L) continue;
        long restReal = rest[z] - transcurridoMs;
        if (restReal < 15000L) continue;   // menos de 15 s → no vale la pena

        // Usar iniciarRiegoAutomatico() para respetar toda la lógica de válvulas
        uint16_t minRound = (uint16_t)max(1L, restReal / 60000L + 1L);
        if (iniciarRiegoAutomatico(z, minRound)) {
            // Ajustar precisión exacta al ms
            zona[z].duracionMs  = (unsigned long)restReal;
            zona[z].inicioRiego = millis();

            String msg = "🔄 Zona " + String(z + 1) + " reanudada — "
                         + String(restReal / 60000L) + " min restantes";
            registrarEvento(msg);
            if (WiFi.status() == WL_CONNECTED)
                encolarTelegram("⚡ Reinicio detectado\n" + msg);
            restaurado = true;
        }
    }

    // Limpiar epoch: el próximo guardarRiegoActivo() lo renovará si sigue activo
    prefs.begin("riego", false); prefs.putLong("rz_ep", 0L); prefs.end();

    if (restaurado) {
        // Guardar inmediatamente con los nuevos inicioRiego por si hay otro corte rápido
        guardarRiegoActivo();
    }
}

/**
 * Cuando la balsa se recupera, reanuda los riegos que fueron cortados por nivel
 * crítico, descontando el tiempo que la balsa estuvo vacía.
 * Solo actúa si quedaban más de 15 s y han pasado menos de 4 horas.
 */
void reanudarRiegoBalsaRecuperada() {
    if (balsaInterrumpidoEpoch <= 0L) return;

    long epoch = (long)time(nullptr);
    long transcurridoMs = (epoch - balsaInterrumpidoEpoch) * 1000L;

    if (transcurridoMs < 0L || transcurridoMs > 14400000L) {
        // Más de 4 horas sin agua — no tiene sentido reanudar
        balsaInterrumpidoEpoch = 0;
        memset(balsaInterrumpidoMs, 0, sizeof(balsaInterrumpidoMs));
        return;
    }

    for (int z = 0; z < 4; z++) {
        if (balsaInterrumpidoMs[z] == 0) continue;
        long restReal = (long)balsaInterrumpidoMs[z] - transcurridoMs;
        if (restReal < 15000L) continue;  // menos de 15 s — descartar

        uint16_t minRound = (uint16_t)max(1L, restReal / 60000L + 1L);
        if (iniciarRiegoAutomatico(z, minRound)) {
            zona[z].duracionMs  = (unsigned long)restReal;
            zona[z].inicioRiego = millis();
            String msg = "💧 Balsa recuperada — Zona " + String(z + 1)
                         + " reanudada: " + String(restReal / 60000L) + " min";
            registrarEvento(msg);
            if (WiFi.status() == WL_CONNECTED)
                encolarTelegram("💧 Balsa recuperada\n🔄 Zona " + String(z + 1)
                                + " reanudada — " + String(restReal / 60000L) + " min restantes");
        }
    }

    balsaInterrumpidoEpoch = 0;
    memset(balsaInterrumpidoMs, 0, sizeof(balsaInterrumpidoMs));
}

// Guarda el log circular en NVS (namespace "estado")
// ESTAB-9: solo escribe si nvsLogDirty==true (registrarEvento() lo activa).
// Evita ~288 escrituras/día fijas cuando el sistema está en silencio.
void guardarLog() {
    if (!nvsLogDirty) return;   // ESTAB-9: sin nuevos eventos → no desgastar flash
    nvsLogDirty = false;
    prefs.begin("estado", false);
    prefs.putBytes("log_buf", logEventos, sizeof(logEventos));
    prefs.putUChar("log_idx", indiceEvento);
    prefs.end();
}

// Carga el log circular desde NVS al arrancar
// (permite ver eventos del ciclo anterior tras un reinicio inesperado)
void cargarLog() {
    prefs.begin("estado", true);
    size_t tam = prefs.getBytesLength("log_buf");
    if (tam == sizeof(logEventos)) {
        prefs.getBytes("log_buf", logEventos, sizeof(logEventos));
        indiceEvento = prefs.getUChar("log_idx", 0) % MAX_EVENTOS;  // clamp vs NVS corrupta
        registrarEvento("📋 Log restaurado del reinicio anterior");
    }
    prefs.end();
}

//--------------------------------------------------
// OTA REMOTA VIA TELEGRAM
// Permite actualizar el firmware sin conectar USB.
// Uso: /ota http://ip-publica/firmware.bin
// Para obtener el .bin: Arduino IDE → Programa → Exportar binario compilado
//--------------------------------------------------
void realizarOTA(const String& url) {
    if (!WiFi.isConnected()) {
        enviarTelegramConLog("❌ OTA: sin conexión WiFi");
        return;
    }
    registrarEvento("🔄 OTA iniciado: " + url.substring(0, 50));
    guardarProgramacion();
    guardarLog();
    guardarModoLluvia();

    encolarTelegram(
        "🔄 OTA — ACTUALIZACIÓN FIRMWARE\n"
        "════════════════════════════\n"
        "Descargando nuevo firmware...\n"
        "⏳ El sistema no responderá 1-2 min.\n"
        "✅ Se reiniciará automáticamente al terminar.\n"
        "❌ Si hay error te avisaré aquí."
    );
    vaciarColaTelegram();
    delay(500);

    // FIX: no tocar el WDT del core Arduino. httpUpdate hace yield() internamente.

    // HTTPS (ej: raw.githubusercontent.com) requiere WiFiClientSecure
    httpUpdate.rebootOnUpdate(true);
    t_httpUpdate_return ret;
    if (url.startsWith("https://")) {
        WiFiClientSecure otaClienteSeguro;
        otaClienteSeguro.setInsecure();   // sin verificar certificado — OK para OTA interna
        ret = httpUpdate.update(otaClienteSeguro, url);
    } else {
        WiFiClient otaCliente;
        ret = httpUpdate.update(otaCliente, url);
    }

    // Solo se llega aquí si OTA falló (en éxito el ESP32 reinicia solo)
    iniciarWatchdog();
    if (ret == HTTP_UPDATE_FAILED) {
        String err = "❌ OTA FALLIDO\nError " + String(httpUpdate.getLastError()) +
                     ": " + httpUpdate.getLastErrorString();
        registrarEvento(err);
        encolarTelegram(err);
    } else if (ret == HTTP_UPDATE_NO_UPDATES) {
        encolarTelegram("ℹ️ OTA: el servidor indica sin actualizaciones");
    }
}

// ⭐⭐⭐ FUNCIÓN GUARDAR Y REINICIAR - CORREGIDA ⭐⭐⭐
void guardarYReiniciar() {
    static unsigned long ultimoReinicioGuardado = 0;

    // ESTAB-3
    if (diffMillis(millis(), ultimoReinicioGuardado) < 60000UL) {
        registrarEvento("⚠️ Reinicio ignorado - muy cercano al anterior");
        return;
    }
    ultimoReinicioGuardado = millis();

    registrarEvento("🔄 Guardando estado y reiniciando...");
    guardarProgramacion();

    // v7.6: usar guardarEstadoCritico() para agrupar todo en una sola transacción
    guardarEstadoCritico();

    // Guardar configuración de reinicio
    prefs.begin("riego", false);
    prefs.putBool("reinicio_activo", reinicioAutomaticoActivado);
    prefs.putInt("reinicio_hora", reinicioHoraProgramada);
    prefs.putInt("reinicio_minuto", reinicioMinutoProgramado);
    prefs.end();

    guardarLog();          // persistir log antes de reiniciar
    // Modo lluvia ya se guarda dentro de guardarEstadoCritico()
    // Persistir estadísticas acumuladas si están pendientes
    if (statsDirty) {
        statsDirty = false;
        prefs.begin("riego_stats", false);
        for (int z = 0; z < 4; z++) {
            char _kCnt[16]; snprintf(_kCnt, sizeof(_kCnt), "cnt_%d", z);
            char _kMin[16]; snprintf(_kMin, sizeof(_kMin), "min_%d", z);
            prefs.putULong(_kCnt, contadorRiegoZona[z]);
            prefs.putULong(_kMin, minutosRiegadoZona[z]);
        }
        prefs.end();
    }
    delay(100);
    registrarEvento("✅ Estado guardado - Reiniciando...");
    ESP.restart();
}

bool iniciarRiegoAutomatico(int numZona, uint16_t minutos) {
  if (numZona < 0 || numZona > 3) return false;
  if (!nivelBajoBalsa) {
    if (WiFi.status() == WL_CONNECTED) {
      enviarTelegramConLog("⚠️ Riego Zona " + String(numZona + 1) + " CANCELADO\n💧 Balsa en nivel crítico");
    }
    return false;
  }
  if (zona[numZona].riegoAutomaticoActivo) {
    if (WiFi.status() == WL_CONNECTED) {
      enviarTelegramConLog("⚠️ Zona " + String(numZona + 1) + " ya está regando");
    }
    return false;
  }
  if (!abrirZona(numZona)) {
    if (WiFi.status() == WL_CONNECTED) {
      enviarTelegramConLog("❌ Error al abrir Zona " + String(numZona + 1) + "\n" + getErrorAbrir());
    }
    return false;
  }

  zona[numZona].riegoAutomaticoActivo = true;
  zona[numZona].inicioRiego = millis();
  zona[numZona].duracionMs = (unsigned long)minutos * 60000UL;
  contadorRiegoZona[numZona]++;           // MEJORA: contar riegos acumulados
  minutosRiegadoZona[numZona] += minutos; // MEJORA: acumular minutos totales
  statsDirty = true;  // Se persistirán de forma diferida (cada hora o al apagar)

  // ESTAB-7: char buf para clave NVS (evita String temporal con c_str())
  char _keyRest[16];
  snprintf(_keyRest, sizeof(_keyRest), "rest_%d", numZona);
  prefs.begin("riego", false);
  prefs.putULong(_keyRest, zona[numZona].duracionMs);
  prefs.end();
  nvsUltimoRestante[numZona] = zona[numZona].duracionMs;  // ESTAB-10: sincronizar tracking

  registrarEvento("Riego automatico zona " + String(numZona + 1) + " duracion: " + String(minutos) + " min");

  if (WiFi.status() == WL_CONNECTED) {
    struct tm tiempo;
    String horaFin = "??:??";
    if (obtenerHora(tiempo)) {
      int minutosFin = tiempo.tm_min + minutos;
      int horasFin = tiempo.tm_hour + (minutosFin / 60);
      minutosFin = minutosFin % 60;
      if (horasFin >= 24) horasFin -= 24;
      horaFin = String(horasFin) + ":" + (minutosFin < 10 ? "0" : "") + String(minutosFin);
    }

    String msg = "🌱 INICIO RIEGO Zona " + String(numZona + 1);
    msg += " (#" + String(contadorRiegoZona[numZona]) + ")\n";  // MEJORA: numero de riego
    msg += "⏱ Duración: " + String(minutos) + " minutos\n";
    msg += "🕐 Inicio: " + obtenerTimestamp() + "\n";
    msg += "⌛ Finaliza: " + horaFin;
    enviarTelegramConLog(msg);
  }

  return true;
}

//--------------------------------------------------
// GESTIONAR FIN RIEGO  —  v7.6 UNIFICADO
//--------------------------------------------------
// v7.6: un solo bloque de cierre de válvula. El anti-spam aplica SOLO al
// mensaje de Telegram, no a la lógica física. Esto evita que una válvula
// ocupada deje el riego marcado como activo indefinidamente.
//--------------------------------------------------
void gestionarFinRiego() {
    for (int z = 0; z < 4; z++) {
        if (!zona[z].riegoAutomaticoActivo) continue;

        // ¿Ya expiró el tiempo de riego?
        if (diffMillis(millis(), zona[z].inicioRiego) < zona[z].duracionMs) continue;

        // Intento único de cierre físico
        bool cerrado = cerrarZona(z);

        if (cerrado) {
            // Válvula encolada correctamente: desactivar riego
            zona[z].riegoAutomaticoActivo = false;
            nvsUltimoRestante[z]    = ULONG_MAX;  // ESTAB-10: reset delta tracking
            nvsUltimaZonaAbierta[z] = -1;         // ESTAB-11: reset dedup tracking

            prefs.begin("riego", false);
            char _kFR[16]; snprintf(_kFR, sizeof(_kFR), "rest_%d", z); // ESTAB-7
            prefs.remove(_kFR);
            { char _kRZ[8]; snprintf(_kRZ, sizeof(_kRZ), "rz_r%d", z); prefs.putLong(_kRZ, 0L); }
            prefs.end();

            // Anti-spam: solo para el mensaje de Telegram, no para el cierre
            bool spam = (ultimoMensajeFinRiego[z] != ULONG_MAX) &&
                        (diffMillis(millis(), ultimoMensajeFinRiego[z]) < 30000UL);

            if (!spam) {
                ultimoMensajeFinRiego[z] = millis();
                registrarEvento("Fin riego zona " + String(z + 1));
                if (WiFi.status() == WL_CONNECTED) {
                    enviarTelegramConLog("✅ Fin riego Zona " + String(z + 1));
                }
            } else {
                registrarEvento("Fin riego Zona " + String(z + 1) + " - Msg omitido (spam)");
            }
        } else {
            // Válvula ocupada: NO desactivar riego, se reintentará en el próximo ciclo
            registrarEvento("⚠️ Fin riego Zona " + String(z + 1) + " - Válvula ocupada, reintentando...");
        }
    }

    // Guardado periódico de restantes (cada 10 s)
    static unsigned long ultimoGuardadoRestante = 0;
    if (diffMillis(millis(), ultimoGuardadoRestante) > 10000UL) {
        ultimoGuardadoRestante = millis();
        // ESTAB-11: abrir NVS solo si hay algo que escribir
        bool necesitaAbrir = false;
        for (int z = 0; z < 4; z++) {
            if (!zona[z].riegoAutomaticoActivo) continue;
            unsigned long restanteActual = restanteMs(zona[z].duracionMs, zona[z].inicioRiego);
            if (restanteActual <= 30000) continue;
            necesitaAbrir = true;
            break;
        }
        if (necesitaAbrir) {
            prefs.begin("riego", false);
            for (int z = 0; z < 4; z++) {
                if (!zona[z].riegoAutomaticoActivo) continue;
                unsigned long restanteActual = restanteMs(zona[z].duracionMs, zona[z].inicioRiego);
                if (restanteActual > 30000) {
                    // ESTAB-7
                    char _kFRP[16]; snprintf(_kFRP, sizeof(_kFRP), "rest_%d", z);
                    char _kFRZ[24]; snprintf(_kFRZ, sizeof(_kFRZ), "zona_abierta_%d", z);
                    // Siempre actualizar: error máximo = intervalo de guardado (10s)
                    prefs.putULong(_kFRP, restanteActual);
                    nvsUltimoRestante[z] = restanteActual;
                    // ESTAB-11: zona_abierta_Z solo una vez por sesión de riego
                    if (nvsUltimaZonaAbierta[z] != 1) {
                        prefs.putBool(_kFRZ, true);
                        nvsUltimaZonaAbierta[z] = 1;
                    }
                }
            }
            prefs.end();
        }
    }
}

//--------------------------------------------------
// REAPERTURAS PENDIENTES (microcorte multi-zona)
//--------------------------------------------------
// Abre zonas marcadas como pendientes de una en una, esperando a que la
// válvula quede libre entre apertura y apertura (~1500 ms de pulso físico).
void gestionarReaperturas() {
    for (int z = 0; z < 4; z++) {
        if (!zonaReabrirPendiente[z]) continue;
        if (!valvulaLibre()) return;  // esperar; solo un intento por ciclo

        zona[z].abierta = false;
        zona[z].inicioRiego = millis();
        if (abrirZona(z)) {
            zona[z].riegoAutomaticoActivo = true;
            ultimoMensajeFinRiego[z] = ULONG_MAX;
            zonaReabrirPendiente[z] = false;
            // Guardar rest_Z inmediatamente con el tiempo restante correcto
            { char _kRest[16]; snprintf(_kRest, sizeof(_kRest), "rest_%d", z);
              prefs.begin("riego", false);
              prefs.putULong(_kRest, zona[z].duracionMs);
              prefs.end(); }
            Serial.println("Zona " + String(z+1) + " reabierta (microcorte pendiente)");
            if (WiFi.status() == WL_CONNECTED) {
                enviarTelegramConLog("🔄 RIEGO REANUDADO Zona " + String(z+1) +
                    " (" + String(zona[z].duracionMs/60000) + "m restantes)");
            }
            registrarEvento("Reanudado riego zona " + String(z+1) + " (microcorte)");
        }
        return;  // Una zona por ciclo; el siguiente ciclo intentará la siguiente
    }
}

//--------------------------------------------------
// SCHEDULER RIEGO  —  v7.6 ROBUSTO
//--------------------------------------------------
// v7.6: usa time(nullptr)/60 (minuto absoluto epoch) en lugar de tm_yday*1440+...
// Esto elimina el bug de doble ejecución en cambio de año/día y es inmune
// al overflow de millis().
//--------------------------------------------------
void schedulerRiego() {
    actualizarHoraCache();
    if (!horaSincronizada) return;
    // MEJORA #8: no arrancar riegos automaticos si el modo lluvia esta activo
    if (modoLluvia) return;

    time_t minutoActual = time(nullptr) / 60;
    if (minutoActual == ultimoMinutoAbsoluto) return;
    ultimoMinutoAbsoluto = minutoActual;

    // Usar cache de tiempo local para hora ajustada y día de la semana
    int horaAjustada = tiempoCache.tm_hour + AJUSTE_HORARIO_HORAS;
    if (horaAjustada < 0) horaAjustada += 24;
    if (horaAjustada >= 24) horaAjustada -= 24;

    // Informe diario automático
    if (informeDiarioActivo &&
        horaAjustada      == informeDiarioHora &&
        tiempoCache.tm_min == informeDiarioMinuto &&
        minutoActual       != (time_t)ultimoInformeDiario) {
        ultimoInformeDiario = (unsigned long)minutoActual;
        if (WiFi.status() == WL_CONNECTED) {
            enviarTelegramConLog(mensajeInformeDiario());
        }
    }

    int diaSemana = tiempoCache.tm_wday;
    if (diaSemana == 0) diaSemana = 7;

    for (int z = 0; z < 4; z++) {
        // No iniciar via scheduler si la zona está pendiente de reapertura por microcorte:
        // el scheduler usaría la duración completa del programa y sobreescribiría
        // el tiempo restante que se está restaurando.
        if (zonaReabrirPendiente[z]) continue;
        for (int p = 0; p < 3; p++) {
            ProgramaRiego &prog = zona[z].programa[p];
            if (!prog.habilitado) continue;
            if (!prog.dias[diaSemana - 1]) continue;
            if (prog.hora == horaAjustada && prog.minuto == tiempoCache.tm_min) {
                iniciarRiegoAutomatico(z, prog.duracionMin);
            }
        }
    }
}

String diasSemanaTexto(bool dias[]) {
  String txt = "";
  txt.reserve(8);
  if (dias[0]) txt += "L";
  if (dias[1]) txt += "M";
  if (dias[2]) txt += "X";
  if (dias[3]) txt += "J";
  if (dias[4]) txt += "V";
  if (dias[5]) txt += "S";
  if (dias[6]) txt += "D";
  return txt;
}

//--------------------------------------------------
// MENSAJES
//--------------------------------------------------
String mensajeEstado() {
  String msg;
  msg.reserve(1024);
  msg += "ESTADO DEL SISTEMA\n===================\n\n";
  msg += "VALVULAS:\n";

  for (int i = 0; i < 4; i++) {
    msg += "  Zona " + String(i + 1) + ": ";
    if (zona[i].abierta) {
      msg += "🔓 ABIERTA";
    } else {
      msg += "🔒 CERRADA";
    }
    if (zona[i].riegoAutomaticoActivo) {
      // ESTAB-4: restanteMs() evita subdesborde
      unsigned long _rmsg = restanteMs(zona[i].duracionMs, zona[i].inicioRiego);
      unsigned long restante = _rmsg / 60000;
      unsigned long segundos = (_rmsg % 60000) / 1000;
      msg += " [Riego " + String(restante) + "m " + String(segundos) + "s]";
    }
    msg += "\n";
  }

  msg += "\n🔧 POZOS (independientes):\n";
  msg += getEstadoPozos();

  msg += "\n🌡️ CLIMA (DHT22):\n";
  msg += "  Temp: " + String(temperatura, 1) + " °C\n";
  msg += "  Humedad: " + String(humedad, 1) + " %\n";
  msg += "  Ventilador: " + String(ventiladorActivo ? "🔵 ON" : "⚪ OFF");
  if (ventiladorForzado) msg += " [MANUAL]";  // MEJORA: indicar si es override manual
  if (!dhtDisponible) msg += " ⚠️ Sensor no disponible";
  msg += "\n";

  // MEJORA #8: mostrar modo lluvia en /estado
  if (modoLluvia) {
    unsigned long minRestante = (finModoLluvia > millis()) ? (finModoLluvia - millis()) / 60000 : 0;
    msg += "\n🌧️ MODO LLUVIA ACTIVO — riegos pausados " + String(minRestante) + " min más\n";
  }

  msg += "\n💧 BALSA:\n";
  msg += "  " + getEstadoBalsa() + "\n";
  msg += "  Inferior (GPIO13):  " + String(nivelBajoBalsa ? "✅ SUMERGIDA" : "⚠️ SECA") + "\n";
  msg += "  Media (GPIO15):    " + String(nivelMedioBalsa ? "✅ SUMERGIDA" : "⚠️ SECA") + "\n";
  msg += "  Superior (GPIO16): " + String(nivelAltoBalsa ? "✅ SUMERGIDA" : "⚠️ SECA") + "\n";

  msg += "\n📡 RED: " + WiFi.localIP().toString() + " (" + String(WiFi.RSSI()) + " dBm)\n";

  msg += "\n🕐 HORA:\n";
  msg += "  " + obtenerTimestamp() + "\n";
  msg += "  Sincronizada: " + String(horaSincronizada ? "✅ Si" : "❌ No") + "\n";

  msg += "\n💾 MEMORIA: " + String(ESP.getFreeHeap() / 1024) + " KB libre\n";
  msg += "⏱️ Uptime: " + String(millis() / 1000 / 60) + " minutos";
  return msg;
}

String mensajeProgramacion(int zonaNum) {
  String msg;
  msg.reserve(1024);

  if (zonaNum >= 0 && zonaNum < 4) {
    msg += "PROGRAMA ZONA " + String(zonaNum + 1) + "\n\n";
    for (int p = 0; p < 3; p++) {
      ProgramaRiego &prog = zona[zonaNum].programa[p];
      msg += "P" + String(p + 1) + ": ";
      if (!prog.habilitado) {
        msg += "INACTIVO\n";
        continue;
      }
      msg += String(prog.hora) + ":" + (prog.minuto < 10 ? "0" : "") + String(prog.minuto);
      msg += " Dias: " + diasSemanaTexto(prog.dias);
      msg += " Dur: " + String(prog.duracionMin) + " min\n";
    }
  } else {
    msg += "PROGRAMACION TODAS LAS ZONAS\n\n";
    for (int z = 0; z < 4; z++) {
      msg += "Zona " + String(z + 1) + ":\n";
      bool tieneProgramas = false;

      for (int p = 0; p < 3; p++) {
        if (zona[z].programa[p].habilitado) {
          tieneProgramas = true;
          ProgramaRiego &prog = zona[z].programa[p];
          msg += "  P" + String(p + 1) + ": " + String(prog.hora) + ":" + 
                 (prog.minuto < 10 ? "0" : "") + String(prog.minuto) + " " + 
                 diasSemanaTexto(prog.dias) + " (" + String(prog.duracionMin) + " min)\n";
        }
      }
      if (!tieneProgramas) msg += "  Sin programacion\n";
      msg += "\n";
    }
  }
  return msg;
}

String mensajeDiagnostico() {
  String diag;
  diag.reserve(2048);
  diag = "🔧 DIAGNÓSTICO DEL SISTEMA\n═══════════════════════\n\n";

  diag += "📊 WATCHDOG:\n";
  diag += "  Timeout: " + String(wdtTimeoutSegActual) + " segundos\n";
  diag += "  Último reset: " + String((millis() - ultimoResetWDT) / 1000) + "s\n";
  diag += "  Max loop: " + String(maxTiempoLoop) + " ms\n";
  diag += "  Ciclos lentos: " + String(ciclosLoopLentos) + "\n\n";

  diag += "📡 WIFI:\n";
  diag += "  Estado: " + String(WiFi.status() == WL_CONNECTED ? "✅ Conectado" : "❌ Desconectado") + "\n";
  diag += "  IP: " + WiFi.localIP().toString() + "\n";
  diag += "  RSSI: " + String(WiFi.RSSI()) + " dBm";
  if (WiFi.RSSI() < -80) diag += " (Muy débil ⚠️)";
  else if (WiFi.RSSI() < -70) diag += " (Débil ⚠️)";
  else if (WiFi.RSSI() > -50) diag += " (Excelente ✅)";
  else diag += " (Buena 👍)";
  diag += "\n  Reconexiones: " + String(contadorReconexionesWifi) + "\n\n";

  diag += "🕒 HORA:\n";
  diag += "  Sincronizada: " + String(horaSincronizada ? "✅ Si" : "❌ No") + "\n";
  diag += "  Actual: " + obtenerTimestamp() + "\n";
  diag += "  Última actualización: hace " + String((millis() - ultimaActualizacionHora) / 1000) + "s\n\n";

  diag += "🌡️ DHT22:\n";
  diag += "  Estado: " + String(dhtDisponible ? "✅ OK" : "❌ Sin señal") + "\n";
  diag += "  Temp: " + String(temperatura, 1) + " °C\n";
  diag += "  Humedad: " + String(humedad, 1) + " %\n";
  diag += "  Ventilador: " + String(ventiladorActivo ? "🔵 ON" : "⚪ OFF") + "\n\n";

  diag += "💧 BALSA (3 BOYAS):\n";
  diag += "  " + getEstadoBalsa() + "\n";
  diag += "  Inferior (GPIO13):  " + String(digitalRead(pinBoyaInferior));
  diag += (nivelBajoBalsa ? " → ✅ SUMERGIDA (HAY AGUA)\n" : " → ⚠️ SECA (SIN AGUA)\n");
  diag += "  Media (GPIO15):    " + String(digitalRead(pinBoyaMedia));
  diag += (nivelMedioBalsa ? " → ✅ SUMERGIDA (HAY AGUA)\n" : " → ⚠️ SECA (SIN AGUA)\n");
  diag += "  Superior (GPIO16):  " + String(digitalRead(pinBoyaSuperior));
  diag += (nivelAltoBalsa ? " → ✅ SUMERGIDA (HAY AGUA)\n" : " → ⚠️ SECA (SIN AGUA)\n");
  diag += "  Estabilizando: " + String(diffMillis(millis(), tiempoCambioBajo) < tiempoEstabilizacionBoyas ? "Sí" : "No") + "\n\n";

  diag += "🔌 VÁLVULAS PRINCIPALES (ESTADO REAL):\n";
  for (int i = 0; i < 4; i++) {
    yield();  // v7.6: ceder CPU en bucle largo
    diag += "  Z" + String(i+1) + ": ";
    if (zona[i].abierta) {
      diag += "🔓 ABIERTA";
    } else {
      diag += "🔒 CERRADA";
    }
    if (zona[i].riegoAutomaticoActivo) {
      // ESTAB-4
      unsigned long _rdiag = restanteMs(zona[i].duracionMs, zona[i].inicioRiego);
      unsigned long restante = _rdiag / 60000;
      unsigned long segundos = (_rdiag % 60000) / 1000;
      diag += " [⏱ " + String(restante) + "m " + String(segundos) + "s]";
    }
    diag += "\n";
  }
  diag += "  Movimiento: ";
  switch(estadoValvula) {
    case VALVULA_IDLE: diag += "IDLE 😴"; break;
    case VALVULA_ABRIENDO: diag += "ABRIENDO ⏫"; break;
    case VALVULA_CERRANDO: diag += "CERRANDO ⏬"; break;
  }
  if (zonaEnMovimiento >= 0 && zonaEnMovimiento < 4) {
    diag += " Z" + String(zonaEnMovimiento + 1);
  } else {
    diag += " (ninguna)";
  }
  diag += "\n  Cerrando todo: " + String(cerrandoTodo ? "🛑 Si" : "No") + "\n\n";

  diag += "🔧 POZOS:\n";
  diag += getEstadoPozos();
  diag += "\n";

  diag += "💾 MEMORIA:\n";
  diag += "  Libre: " + String(ESP.getFreeHeap()/1024) + " KB\n";
  diag += "  Total: " + String(ESP.getHeapSize()/1024) + " KB\n";
  diag += "  Uso: " + String(((ESP.getHeapSize() - ESP.getFreeHeap()) * 100) / ESP.getHeapSize()) + "%\n";
  diag += "  Mínimo observado: " + String(heapMinimoObservado/1024) + " KB\n";
  diag += "  Máx. bloque mínimo: " + String(maxAllocMinimoObservado/1024) + " KB\n\n";

  diag += "🔄 SISTEMA:\n";
  diag += "  Tiempo activo: " + String(millis() / 1000 / 60) + " minutos (" + String(millis() / 1000) + " seg)\n";
  diag += "  Último reinicio: hace " + String((millis() - ultimoReinicio) / 1000 / 60) + " min\n";
  diag += "  Estado crítico: " + String(sistemaEnEstadoCritico ? "⚠️ Si" : "✅ No") + "\n";
  diag += "  Reinicio automático: " + String(reinicioAutomaticoActivado ? "✅ ACTIVADO" : "❌ DESACTIVADO") + "\n";
  diag += "  Hora reinicio: " + String(reinicioHoraProgramada) + ":" + 
         String(reinicioMinutoProgramado < 10 ? "0" : "") + String(reinicioMinutoProgramado) + "\n\n";

  diag += "📊 RIEGOS (desde último reinicio):\n";
  for (int z = 0; z < 4; z++) {
    yield();  // v7.6: ceder CPU en bucle largo
    unsigned long h = minutosRiegadoZona[z] / 60, m = minutosRiegadoZona[z] % 60;
    diag += "  Z" + String(z+1) + ": " + String(contadorRiegoZona[z]) + " riego" + (contadorRiegoZona[z] == 1 ? "" : "s");
    if (minutosRiegadoZona[z] > 0)
      diag += " / " + (h > 0 ? String(h) + "h " : "") + String(m) + " min";
    diag += "\n";
  }
  diag += "\n";

  diag += "🌧️ MODO LLUVIA: ";
  if (modoLluvia) {
    unsigned long minR = (finModoLluvia > millis()) ? (finModoLluvia - millis()) / 60000 : 0;
    diag += "✅ ACTIVO — expira en " + String(minR) + " min\n\n";
  } else {
    diag += "❌ Inactivo\n\n";
  }

  diag += "🌡️ TEMPERATURA:\n";
  diag += "  Actual: " + String(temperatura, 1) + " °C\n";
  diag += "  Umbral alerta: " + String(tempAlertaUmbral, 0) + " °C\n";
  diag += "  Alerta enviada: " + String(alertaTempEnviada ? "Sí" : "No") + "\n\n";

  diag += "📝 PROGRAMACIÓN:\n";
  int progActivos = 0;
  for (int z = 0; z < 4; z++) {
    for (int p = 0; p < 3; p++) {
      if (zona[z].programa[p].habilitado) progActivos++;
    }
  }
  diag += "  Programas activos: " + String(progActivos) + "\n";
  diag += "  Pendiente guardar: " + String(programacionModificada ? "⚠️ Si" : "✅ No") + "\n";

  return diag;
}

//--------------------------------------------------
// TELEGRAM
//--------------------------------------------------
void enviarTelegramConLog(String mensaje) {
  if (WiFi.status() != WL_CONNECTED) {
    registrarEvento("⚠️ WiFi no disponible - mensaje no enviado");
    return;
  }

  if (mensaje.length() > 3900) {
    mensaje = mensaje.substring(0, 3900) + "\n\n...[Mensaje truncado]";
  }

  TBMessage msgOut;
  msgOut.chatId = CHAT_ID;
  msgOut.text = mensaje;
  bot.sendMessage(msgOut, mensaje);
  ultimoEnvioTelegram = millis();
}

// Nota: la sobrecarga de 2 argumentos se eliminó — usar siempre la de 1 argumento

void gestionarTelegram() {
    if (WiFi.status() != WL_CONNECTED) return;

    // ESTAB-3
    unsigned long _ahoraTG = millis();
    if (diffMillis(_ahoraTG, ultimoMensajeTelegram) < 1000UL) return;
    ultimoMensajeTelegram = _ahoraTG;

    // No bloqueante: un único sondeo por iteración de loop.
    // AsyncTelegram2 gestiona la conexión HTTPS internamente; no hay que esperar.
    TBMessage msg;
    if (bot.getNewMessage(msg)) {
        if (msg.chatId == CHAT_ID) {
            procesarComando(msg.text, msg.chatId);
        }
    }
}

//--------------------------------------------------
// INFORME DIARIO
//--------------------------------------------------
String mensajeInformeDiario() {
    String msg;
    msg.reserve(600);
    msg = "📋 INFORME DIARIO\n";
    msg += obtenerTimestamp() + "\n";
    msg += "═══════════════════════════\n\n";

    // Balsa
    int pct = 0;
    if      ( nivelAltoBalsa &&  nivelMedioBalsa &&  nivelBajoBalsa) pct = 100;
    else if (!nivelAltoBalsa &&  nivelMedioBalsa &&  nivelBajoBalsa) pct = 70;
    else if (!nivelAltoBalsa && !nivelMedioBalsa &&  nivelBajoBalsa) pct = 30;
    msg += "💧 Balsa: " + getEstadoBalsa() + " (~" + String(pct) + "%)\n";
    msg += String(sistemaEnEstadoCritico ? "⛔ Riegos BLOQUEADOS\n" : "✅ Riegos permitidos\n");

    // Temperatura
    if (dhtDisponible) {
        msg += "\n🌡️ Temp: " + String(temperatura, 1) + " °C | Hum: " + String(humedad, 1) + "%\n";
        msg += "🔵 Ventilador: " + String(ventiladorActivo ? "ON" : "OFF") + "\n";
    }

    // Lluvia
    if (modoLluvia) {
        unsigned long restH = (finModoLluvia > millis()) ? (finModoLluvia - millis()) / 3600000UL : 0;
        msg += "\n🌧️ Modo lluvia activo — " + String(restH) + " h restantes\n";
    }

    // Contadores acumulados
    msg += "\n📊 Riegos acumulados:\n";
    unsigned long totalRiegos = 0, totalMin = 0;
    for (int z = 0; z < 4; z++) {
        yield();  // v7.6
        if (contadorRiegoZona[z] == 0) continue;
        unsigned long h = minutosRiegadoZona[z] / 60, m = minutosRiegadoZona[z] % 60;
        msg += "  Z" + String(z+1) + ": " + String(contadorRiegoZona[z]) + " riego" +
               (contadorRiegoZona[z] == 1 ? "" : "s") + " / " +
               (h > 0 ? String(h) + "h " : "") + String(m) + " min\n";
        totalRiegos += contadorRiegoZona[z]; totalMin += minutosRiegadoZona[z];
    }
    if (totalRiegos == 0) {
        msg += "  (Sin riegos registrados)\n";
    } else {
        unsigned long th = totalMin / 60, tm = totalMin % 60;
        msg += "  TOTAL: " + String(totalRiegos) + " riegos / " +
               (th > 0 ? String(th) + "h " : "") + String(tm) + " min\n";
    }

    msg += "\n💡 Usa /proximo para ver los próximos riegos del día";
    return msg;
}

//--------------------------------------------------
// COMANDOS: CONSULTA (solo lectura)
//--------------------------------------------------
bool procesarComandoConsulta(const String& texto, int64_t chat_id) {
  if (texto == "/estado") {
    enviarTelegramConLog(mensajeEstado());
  }
  else if (texto == "/diagnostico") {
    enviarTelegramConLog(mensajeDiagnostico());
  }
  else if (texto == "/ayuda") {
    String ayuda;
    ayuda.reserve(2200);
    ayuda = "COMANDOS DISPONIBLES\n\n";
    ayuda += "📊 Estado:\n/estado /diagnostico /temperatura /ip /logcorto /boyas /memoria\n";
    ayuda += "  /balsa           → Estado detallado balsa\n";
    ayuda += "  /log             → Últimos eventos del sistema\n\n";
    ayuda += "🎮 Control manual:\n/on1..4 /off1..4 /cerrartodo\n";
    ayuda += "  /riego ZONA MIN          → Iniciar riego\n";
    ayuda += "  /riego_stop ZONA         → Parar riego zona\n";
    ayuda += "  /riego_extend ZONA MIN   → Extender riego activo\n";
    ayuda += "  /test ZONA               → Test válvula 10 seg\n\n";
    ayuda += "🌧️ Lluvia:\n";
    ayuda += "  /lluvia_on [horas]  → Pausar riegos auto (def. 24h)\n";
    ayuda += "  /lluvia_off         → Reanudar riegos auto\n\n";
    ayuda += "🌡️ Ventilador/Temp:\n";
    ayuda += "  /ventilador_on      → Forzar ON manual\n";
    ayuda += "  /ventilador_off     → Volver a automático\n";
    ayuda += "  /temp_umbral T      → Alerta si temp > T°C (actual: " + String(tempAlertaUmbral, 0) + "°C)\n\n";
    ayuda += "📊 Estadísticas:\n";
    ayuda += "  /contadores         → Riegos y minutos/zona (acumulados)\n";
    ayuda += "  /contadores_reset   → Poner contadores a cero\n";
    ayuda += "  /proximo            → Próximos riegos (24h)\n\n";
    ayuda += "📋 Informe diario:\n";
    ayuda += "  /informe_on         → Activar informe diario (def. 08:00)\n";
    ayuda += "  /informe_off        → Desactivar informe diario\n";
    ayuda += "  /informe_hora HH MM → Cambiar hora (ej: /informe_hora 8 0)\n\n";
    ayuda += "⚙️ Ajustes (/set):\n";
    ayuda += "  /set wdt S          → WDT timeout en segundos (30-300, actual: " + String(wdtTimeoutSegActual) + ")\n";
    ayuda += "  /set ntp M          → Sincronizar NTP cada M min (5-120, actual: " + String(intervaloNTP/60000) + ")\n";
    ayuda += "  /set temp_vent T    → Temp. activación ventilador (actual: " + String(tempVentiladorOn, 0) + "°C)\n";
    ayuda += "  /set hum_vent H     → Hum. activación ventilador (actual: " + String(humVentiladorOn, 0) + "%)\n\n";
    ayuda += "📅 Programacion:\n/programacion /programar Z P H M DIAS DUR\n/desactivar Z P /ejemplo /guardar\n\n";
    ayuda += "🔧 POZOS:\n";
    ayuda += "  /pozo1on /pozo1off /pozo2on /pozo2off\n";
    ayuda += "  /pozo1toggle /pozo2toggle /pozoestado\n\n";
    ayuda += "🔧 OTA:\n";
    ayuda += "  /ota URL → /ota http://servidor/firmware.bin\n\n";
    ayuda += "⚙️ Sistema:\n/reset_wdt /reiniciar /limpiar /forzarhora /hora\n";
    ayuda += "/reinicio_on /reinicio_off /reinicio_hora HH /reinicio_minuto MM /reinicio_estado\n\n";
    ayuda += "🌡️ DHT22 GPIO" + String(DHTPIN) + " | Umbral temp: " + String(tempAlertaUmbral, 0) + " °C\n";
    ayuda += "✅ Reinicio automático: " + String(reinicioAutomaticoActivado ? "ACTIVADO" : "DESACTIVADO") + "\n";
    ayuda += "   Hora: " + String(reinicioHoraProgramada) + ":" +
             String(reinicioMinutoProgramado < 10 ? "0" : "") + String(reinicioMinutoProgramado);
    ayuda += "\n\n💧 BALSA: 3 BOYAS (LOW=SUMERGIDA ✅ | HIGH=SECA ⚠️)";
    enviarTelegramConLog(ayuda);
  }
  else if (texto == "/temperatura") {
    String msg = "🌡️ DHT22\n\n";
    if (dhtDisponible) {
      msg += "✅ Sensor OK\n";
      msg += "Temp: " + String(temperatura, 1) + " °C\n";
      msg += "Humedad: " + String(humedad, 1) + " %\n";
      msg += "Ventilador: " + String(ventiladorActivo ? "🔵 ON" : "⚪ OFF") + "\n";
      msg += "Umbral ON: " + String(tempVentiladorOn, 0) + "°C / " + String(humVentiladorOn, 0) + "%\n";
      msg += "Umbral OFF: " + String(tempVentiladorOff, 0) + "°C / " + String(humVentiladorOff, 0) + "%";
    } else {
      msg += "❌ Sensor NO DISPONIBLE\n";
      msg += "Verificar conexión en GPIO" + String(DHTPIN);
    }
    enviarTelegramConLog(msg);
  }
  else if (texto == "/ip") {
    enviarTelegramConLog("📡 IP: " + WiFi.localIP().toString() + "\nRSSI: " + String(WiFi.RSSI()) + " dBm");
  }
  else if (texto == "/logcorto") {
    String logMsg;
    obtenerLog(logMsg);  // v7.6: por referencia
    int lineas = 0, ultimaPos = logMsg.length();
    for (int j = logMsg.length() - 1; j >= 0 && lineas < 10; j--) {
      if (logMsg[j] == '\n') { lineas++; if (lineas == 10) ultimaPos = j; }
    }
    String logCorto = logMsg.substring(ultimaPos);
    if (logCorto.length() == 0) logCorto = logMsg;
    enviarTelegramConLog("📝 ULTIMAS 10 LINEAS:\n" + logCorto);
  }
  else if (texto == "/log") {
    String logMsg;
    obtenerLog(logMsg);  // v7.6: por referencia
    if (logMsg.length() == 0) {
      encolarTelegram("📝 Log vacío");
    } else if (logMsg.length() <= 3800) {
      encolarTelegram("📝 LOG:\n" + logMsg);
    } else {
      int parte = 1, inicio = 0;
      while (inicio < (int)logMsg.length()) {
        yield();  // v7.6: ceder CPU en bucle largo
        int fin = min(inicio + 3800, (int)logMsg.length());
        encolarTelegram("📝 LOG (parte " + String(parte) + "):\n" + logMsg.substring(inicio, fin));
        inicio = fin; parte++;
      }
    }
  }
  else if (texto == "/boyas") {
    enviarTelegramConLog(diagnosticarBoyas());
  }
  else if (texto == "/reset_wdt") {
    enviarTelegramConLog("Watchdog reseteado");
  }
  else if (texto == "/limpiar") {
    limpiarBufferTelegram();
    enviarTelegramConLog("🧹 Buffer de Telegram limpiado");
  }
  else if (texto == "/memoria") {
    size_t h = ESP.getFreeHeap(), t = ESP.getHeapSize();
    String msg = "💾 MEMORIA:\nLibre: " + String(h/1024) + " KB\nTotal: " + String(t/1024) +
                 " KB\nUso: " + String(((t-h)*100)/t) + "%\nMinimo observado: " + String(heapMinimoObservado/1024) + " KB";
    enviarTelegramConLog(msg);
  }
  else if (texto == "/hora") {
    struct tm ahora;
    String msg = "🕐 DIAGNÓSTICO DE HORA\n\n";
    if (getLocalTime(&ahora, 0)) {
      msg += "✅ getLocalTime() OK\n";
      msg += "  Año: " + String(ahora.tm_year + 1900) + "\n";
      msg += "  Mes: " + String(ahora.tm_mon + 1) + "\n";
      msg += "  Día: " + String(ahora.tm_mday) + "\n";
      msg += "  Hora: " + String(ahora.tm_hour) + "\n";
      msg += "  Min: " + String(ahora.tm_min) + "\n";
      msg += "  Seg: " + String(ahora.tm_sec) + "\n";
      msg += "  Año válido: " + String(ahora.tm_year > 100 ? "✅ Si" : "❌ No") + "\n\n";
      int horaAj = ahora.tm_hour + AJUSTE_HORARIO_HORAS;
      if (horaAj < 0) horaAj += 24; if (horaAj >= 24) horaAj -= 24;
      msg += "🕐 Hora ajustada: " + String(horaAj) + ":" +
             String(ahora.tm_min < 10 ? "0" : "") + String(ahora.tm_min) + ":" +
             String(ahora.tm_sec < 10 ? "0" : "") + String(ahora.tm_sec);
    } else {
      msg += "❌ getLocalTime() FALLÓ\n";
      msg += "  Hora sincronizada: " + String(horaSincronizada ? "Si" : "No") + "\n";
      if (horaSincronizada) msg += "  Cache: " + obtenerTimestamp();
    }
    msg += "\n\n📡 WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Conectado" : "Desconectado");
    msg += "\n🔄 Última actualización: hace " + String((millis() - ultimaActualizacionHora) / 1000) + "s";
    enviarTelegramConLog(msg);
  }
  else if (texto == "/balsa") {
    int porcentaje = 0;
    if      ( nivelAltoBalsa &&  nivelMedioBalsa &&  nivelBajoBalsa) porcentaje = 100;
    else if (!nivelAltoBalsa &&  nivelMedioBalsa &&  nivelBajoBalsa) porcentaje = 70;
    else if (!nivelAltoBalsa && !nivelMedioBalsa &&  nivelBajoBalsa) porcentaje = 30;
    String msg = "💧 ESTADO BALSA\n════════════════\n\n";
    msg += "  " + getEstadoBalsa() + " (~" + String(porcentaje) + "%)\n\n";
    msg += "  Boya inferior: " + String(nivelBajoBalsa  ? "✅ SUMERGIDA" : "⚠️ SECA") + "\n";
    msg += "  Boya media:    " + String(nivelMedioBalsa ? "✅ SUMERGIDA" : "⚠️ SECA") + "\n";
    msg += "  Boya superior: " + String(nivelAltoBalsa  ? "✅ SUMERGIDA" : "⚠️ SECA") + "\n\n";
    msg += String(sistemaEnEstadoCritico ? "🔴 Riegos BLOQUEADOS (nivel crítico)" : "🟢 Riegos PERMITIDOS");
    enviarTelegramConLog(msg);
  }
  else { return false; }
  return true;
}

//--------------------------------------------------
// COMANDOS: SISTEMA
//--------------------------------------------------
bool procesarComandoSistema(const String& texto, int64_t chat_id) {
  if (texto == "/reiniciar") {
    unsigned long ahora = millis();
    if (ahora - ultimoReinicio > intervaloMinimoReinicio) {
      ultimoReinicio = ahora;
      enviarTelegramConLog("🔄 Reiniciando...");
      guardarYReiniciar();
    } else {
      enviarTelegramConLog("⏳ Espera " + String((intervaloMinimoReinicio - (ahora - ultimoReinicio)) / 1000) + "s");
    }
  }
  else if (texto == "/forzarhora") {
    forzarSincronizacionHora();
    if (horaSincronizada) {
      enviarTelegramConLog("✅ Hora forzada correctamente\n🕐 " + obtenerTimestamp());
    } else {
      enviarTelegramConLog("❌ No se pudo sincronizar la hora\nVerificar conexión a internet");
    }
  }
  else if (texto == "/reinicio_on") {
    reinicioAutomaticoActivado = true;
    guardarPreferenceBool("reinicio_activo", true);
    enviarTelegramConLog("✅ REINICIO AUTOMÁTICO ACTIVADO\nEl sistema se reiniciará a las " +
                         String(reinicioHoraProgramada) + ":" +
                         String(reinicioMinutoProgramado < 10 ? "0" : "") + String(reinicioMinutoProgramado));
  }
  else if (texto == "/reinicio_off") {
    reinicioAutomaticoActivado = false;
    guardarPreferenceBool("reinicio_activo", false);
    enviarTelegramConLog("❌ REINICIO AUTOMÁTICO DESACTIVADO");
  }
  else if (texto.startsWith("/reinicio_hora ")) {
    int nuevaHora = texto.substring(15).toInt();
    if (nuevaHora >= 0 && nuevaHora <= 23) {
      reinicioHoraProgramada = nuevaHora;
      guardarPreferenceInt("reinicio_hora", nuevaHora);
      enviarTelegramConLog("✅ Hora de reinicio → " + String(nuevaHora) + ":00");
    } else {
      enviarTelegramConLog("⚠️ Hora inválida. Usa 0-23\nEjemplo: /reinicio_hora 4");
    }
  }
  else if (texto.startsWith("/reinicio_minuto ")) {
    int nuevoMinuto = texto.substring(17).toInt();
    if (nuevoMinuto >= 0 && nuevoMinuto <= 59) {
      reinicioMinutoProgramado = nuevoMinuto;
      guardarPreferenceInt("reinicio_minuto", nuevoMinuto);
      enviarTelegramConLog("✅ Minuto de reinicio → :" + String(nuevoMinuto));
    } else {
      enviarTelegramConLog("⚠️ Minuto inválido. Usa 0-59\nEjemplo: /reinicio_minuto 30");
    }
  }
  else if (texto == "/reinicio_estado") {
    String msg = "📊 ESTADO REINICIO AUTOMÁTICO\n═══════════════════════\n\n";
    msg += "  Estado: " + String(reinicioAutomaticoActivado ? "✅ ACTIVADO" : "❌ DESACTIVADO") + "\n";
    msg += "  Hora: " + String(reinicioHoraProgramada) + ":" +
           String(reinicioMinutoProgramado < 10 ? "0" : "") + String(reinicioMinutoProgramado) + "\n";
    msg += "\n💡 Comandos:\n";
    msg += "  /reinicio_on       → Activar\n";
    msg += "  /reinicio_off      → Desactivar\n";
    msg += "  /reinicio_hora HH  → Cambiar hora (0-23)\n";
    msg += "  /reinicio_minuto MM → Cambiar minuto (0-59)";
    enviarTelegramConLog(msg);
  }
  // ──────────────────────────────────────────────
  // /set CLAVE VALOR — ajustar parámetros en vuelo
  // ──────────────────────────────────────────────
  else if (texto.startsWith("/set ")) {
    String params = texto.substring(5);
    params.trim();
    int sp = params.indexOf(' ');
    if (sp == -1) {
      enviarTelegramConLog(
        "⚙️ USO: /set CLAVE VALOR\n\n"
        "Claves disponibles:\n"
        "  wdt S         → WDT timeout (30-300 s, actual: " + String(wdtTimeoutSegActual) + ")\n"
        "  ntp M         → NTP cada M min (5-120, actual: " + String(intervaloNTP/60000) + ")\n"
        "  temp_vent T   → Temp activación ventilador (actual: " + String(tempVentiladorOn, 0) + "°C)\n"
        "  hum_vent H    → Hum activación ventilador (actual: " + String(humVentiladorOn, 0) + "%)");
      return true;
    }
    String clave = params.substring(0, sp);
    String val   = params.substring(sp + 1);
    clave.trim(); val.trim();

    if (clave == "wdt") {
      int seg = val.toInt();
      if (seg < 30 || seg > 300) {
        enviarTelegramConLog("❌ Rango válido: 30-300 segundos");
      } else {
        wdtTimeoutSegActual = (uint32_t)seg;
        prefs.begin("riego_cfg", false);
        prefs.putUInt("wdt_seg", (uint32_t)seg);
        prefs.end();
        registrarEvento("WDT timeout guardado → " + String(seg) + " s (aplica tras reinicio)");
        enviarTelegramConLog("✅ WDT timeout guardado → " + String(seg) + " s\n💡 Aplica tras reinicio (core Arduino gestiona el WDT internamente)");
      }
    }
    else if (clave == "ntp") {
      int min = val.toInt();
      if (min < 5 || min > 120) {
        enviarTelegramConLog("❌ Rango válido: 5-120 minutos");
      } else {
        intervaloNTP = (unsigned long)min * 60000UL;
        prefs.begin("riego_cfg", false);
        prefs.putUInt("ntp_min", (uint32_t)min);
        prefs.end();
        registrarEvento("NTP intervalo → " + String(min) + " min");
        enviarTelegramConLog("✅ NTP sync cada " + String(min) + " minutos");
      }
    }
    else if (clave == "temp_vent") {
      float t = val.toFloat();
      if (t < 15.0f || t > 60.0f) {
        enviarTelegramConLog("❌ Rango válido: 15-60 °C");
      } else {
        tempVentiladorOn  = t;
        tempVentiladorOff = t - 2.0f;
        prefs.begin("riego_cfg", false);
        prefs.putFloat("tv_on",  tempVentiladorOn);
        prefs.putFloat("tv_off", tempVentiladorOff);
        prefs.end();
        registrarEvento("Ventilador ON umbral → " + String(t, 0) + "°C");
        enviarTelegramConLog("✅ Ventilador ON → " + String(t, 0) + "°C\n"
                             "   Ventilador OFF → " + String(tempVentiladorOff, 0) + "°C (histéresis 2°C)");
      }
    }
    else if (clave == "hum_vent") {
      float h = val.toFloat();
      if (h < 30.0f || h > 99.0f) {
        enviarTelegramConLog("❌ Rango válido: 30-99 %");
      } else {
        humVentiladorOn  = h;
        humVentiladorOff = h - 5.0f;
        prefs.begin("riego_cfg", false);
        prefs.putFloat("hv_on",  humVentiladorOn);
        prefs.putFloat("hv_off", humVentiladorOff);
        prefs.end();
        registrarEvento("Ventilador ON hum umbral → " + String(h, 0) + "%");
        enviarTelegramConLog("✅ Ventilador ON hum → " + String(h, 0) + "%\n"
                             "   Ventilador OFF hum → " + String(humVentiladorOff, 0) + "% (histéresis 5%)");
      }
    }
    else {
      enviarTelegramConLog("❌ Clave desconocida: " + clave + "\nUsa /set sin argumentos para ver las claves disponibles");
    }
  }
  else if (texto.startsWith("/ota ")) {
    if (millis() < 45000) {
      enviarTelegramConLog("⏳ Sistema recién iniciado — espera " +
        String((45000 - millis()) / 1000) + " seg y reenvía el comando /ota");
      return true;
    }
    String url = texto.substring(5);
    url.trim();
    if (url.length() < 10 || (!url.startsWith("http://") && !url.startsWith("https://"))) {
      enviarTelegramConLog("❌ URL inválida\nFormato: /ota http://servidor/firmware.bin");
    } else {
      realizarOTA(url);
    }
  }
  else { return false; }
  return true;
}

//--------------------------------------------------
// COMANDOS: ZONAS
//--------------------------------------------------
bool procesarComandoZonas(const String& texto, int64_t chat_id) {
  if (texto.startsWith("/on")) {
    int z = texto.substring(3).toInt() - 1;
    if (z < 0 || z > 3) enviarTelegramConLog("❌ Zona inválida. Usa /on1..4");
    else if (abrirZona(z)) enviarTelegramConLog("✅ Zona " + String(z+1) + " abierta");
    else enviarTelegramConLog("❌ Error al abrir Zona " + String(z+1) + "\n" + getErrorAbrir());
  }
  else if (texto.startsWith("/off")) {
    int z = texto.substring(4).toInt() - 1;
    if (z < 0 || z > 3) enviarTelegramConLog("❌ Zona inválida. Usa /off1..4");
    else if (cerrarZona(z)) enviarTelegramConLog("✅ Zona " + String(z+1) + " cerrada");
    else enviarTelegramConLog("❌ Error al cerrar Zona " + String(z+1));
  }
  else if (texto == "/cerrartodo") {
    cerrarTodasLasZonas();
    enviarTelegramConLog("🛑 Cerrando todas las zonas...");
  }
  else if (texto.startsWith("/riego ")) {
    String params = texto.substring(7); params.trim();
    int espacio = params.indexOf(' ');
    if (espacio == -1) { enviarTelegramConLog("Formato: /riego ZONA MINUTOS (1-240)\nEjemplo: /riego 1 20"); return true; }
    int z = params.substring(0, espacio).toInt() - 1;
    int min = params.substring(espacio + 1).toInt();
    if (z < 0 || z > 3 || min < 1 || min > 240) { enviarTelegramConLog("Formato: /riego ZONA MINUTOS (1-240)\nEjemplo: /riego 1 20"); return true; }
    if (!nivelBajoBalsa) { enviarTelegramConLog("❌ Error: Balsa critica, no se puede regar"); return true; }
    if (zona[z].riegoAutomaticoActivo) { enviarTelegramConLog("❌ Error: Zona " + String(z+1) + " ya esta regando"); return true; }
    if (iniciarRiegoAutomatico(z, min)) enviarTelegramConLog("✅ Riego manual Zona " + String(z+1) + " iniciado por " + String(min) + " min");
    else enviarTelegramConLog("❌ Error: Valvula ocupada");
  }
  else if (texto.startsWith("/riego_stop ")) {
    int z = texto.substring(12).toInt() - 1;
    if (z < 0 || z > 3) {
      enviarTelegramConLog("❌ Zona invalida. Usa /riego_stop 1..4");
    } else if (!zona[z].riegoAutomaticoActivo) {
      enviarTelegramConLog("⚠️ Zona " + String(z+1) + " no tiene riego activo");
    } else {
      bool cerrado = cerrarZona(z);
      if (cerrado) {
        zona[z].riegoAutomaticoActivo = false;
        nvsUltimoRestante[z]    = ULONG_MAX;
        nvsUltimaZonaAbierta[z] = -1;
        prefs.begin("riego", false);
        char _kRS1[16]; snprintf(_kRS1, sizeof(_kRS1), "rest_%d", z);
        prefs.remove(_kRS1); prefs.end();
        enviarTelegramConLog("🛑 Riego Zona " + String(z+1) + " DETENIDO manualmente");
        registrarEvento("Riego Z" + String(z+1) + " detenido por comando");
      } else {
        zona[z].duracionMs = 1; zona[z].inicioRiego = 0;
        nvsUltimoRestante[z]    = ULONG_MAX;
        nvsUltimaZonaAbierta[z] = -1;
        prefs.begin("riego", false);
        char _kRS2[16]; snprintf(_kRS2, sizeof(_kRS2), "rest_%d", z);
        prefs.remove(_kRS2); prefs.end();
        enviarTelegramConLog("⏳ Zona " + String(z+1) + ": valvula ocupada, se cerrará al terminar el movimiento actual");
        registrarEvento("Riego Z" + String(z+1) + " marcado para cierre inmediato");
      }
    }
  }
  else if (texto.startsWith("/riego_extend ")) {
    String params = texto.substring(14); params.trim();
    int sp = params.indexOf(' ');
    if (sp == -1) { enviarTelegramConLog("Formato: /riego_extend ZONA MINUTOS\nEjemplo: /riego_extend 1 10"); return true; }
    int z   = params.substring(0, sp).toInt() - 1;
    int min = params.substring(sp + 1).toInt();
    if (z < 0 || z > 3 || min < 1 || min > 240) {
      enviarTelegramConLog("❌ Parámetros inválidos. Zona 1-4, minutos 1-240");
    } else if (!zona[z].riegoAutomaticoActivo) {
      enviarTelegramConLog("⚠️ Zona " + String(z+1) + " no tiene riego activo");
    } else {
      zona[z].duracionMs += (unsigned long)min * 60000UL;
      minutosRiegadoZona[z] += min;
      unsigned long restante = restanteMs(zona[z].duracionMs, zona[z].inicioRiego) / 60000;
      enviarTelegramConLog("⏱ Zona " + String(z+1) + " extendida " + String(min) + " min\nTiempo restante: " + String(restante) + " min");
    }
  }
  else if (texto.startsWith("/test ")) {
    int z = texto.substring(6).toInt() - 1;
    if (z < 0 || z > 3) enviarTelegramConLog("❌ Zona inválida. Usa /test 1..4");
    else if (!valvulaLibre()) enviarTelegramConLog("⚠️ Válvula ocupada, espera y vuelve a intentarlo");
    else if (!nivelBajoBalsa) enviarTelegramConLog("❌ Balsa crítica — test cancelado");
    else {
      enviarTelegramConLog("🔧 TEST Zona " + String(z+1) + " — abriendo 10 segundos...");
      iniciarRiegoAutomatico(z, 0);
      zona[z].duracionMs = 10000UL; zona[z].inicioRiego = millis();
    }
  }
  else { return false; }
  return true;
}

//--------------------------------------------------
// COMANDOS: PROGRAMACIÓN
//--------------------------------------------------
bool procesarComandoProgramacion(const String& texto, int64_t chat_id) {
  if (texto.startsWith("/programar ")) {
    int p1 = texto.indexOf(' ', 11), p2 = texto.indexOf(' ', p1+1);
    int p3 = texto.indexOf(' ', p2+1), p4 = texto.indexOf(' ', p3+1);
    int p5 = texto.indexOf(' ', p4+1);
    if (p1==-1||p2==-1||p3==-1||p4==-1||p5==-1) {
      enviarTelegramConLog("Formato: /programar Z P H M DIAS DUR\nEj: /programar 1 1 7 0 1111100 15");
      return true;
    }
    int zn = texto.substring(11,p1).toInt()-1, pn = texto.substring(p1+1,p2).toInt()-1;
    int h  = texto.substring(p2+1,p3).toInt(), m  = texto.substring(p3+1,p4).toInt();
    int dur = texto.substring(p5+1).toInt();
    String diasStr = texto.substring(p4+1,p5);
    if (zn<0||zn>3||pn<0||pn>2||h<0||h>23||m<0||m>59||diasStr.length()!=7||dur<1||dur>240) {
      enviarTelegramConLog("Parametros invalidos"); return true;
    }
    for (int i = 0; i < 7; i++) {
      if (diasStr.charAt(i) != '0' && diasStr.charAt(i) != '1') {
        enviarTelegramConLog("❌ Dias inválidos. Usa solo 0 y 1 (ej: 1111100)"); return true;
      }
    }
    zona[zn].programa[pn].habilitado = true;
    zona[zn].programa[pn].hora = h; zona[zn].programa[pn].minuto = m;
    for (int i=0;i<7;i++) zona[zn].programa[pn].dias[i] = (diasStr.charAt(i)=='1');
    zona[zn].programa[pn].duracionMin = dur;
    marcarProgramacionModificada(); guardarProgramacion();
    String horaStr = String(h), minStr = String(m);
    if (h < 10) horaStr = "0" + horaStr;
    if (m < 10) minStr  = "0" + minStr;
    enviarTelegramConLog("✅ Programa OK:\nZ" + String(zn+1) + " P" + String(pn+1) + " " + horaStr + ":" + minStr + " " + diasSemanaTexto(zona[zn].programa[pn].dias) + " " + String(dur)+"m");
  }
  else if (texto.startsWith("/desactivar ")) {
    int p1 = texto.indexOf(' ', 12);
    if (p1==-1) return true;
    int zn = texto.substring(12,p1).toInt()-1, pn = texto.substring(p1+1).toInt()-1;
    if (zn<0||zn>3||pn<0||pn>2) return true;
    zona[zn].programa[pn].habilitado = false;
    marcarProgramacionModificada(); guardarProgramacion();
    enviarTelegramConLog("✅ Programa desactivado Z" + String(zn+1) + " P" + String(pn+1));
  }
  else if (texto == "/programacion")   { enviarTelegramConLog(mensajeProgramacion(-1)); }
  else if (texto == "/programacion 1") { enviarTelegramConLog(mensajeProgramacion(0)); }
  else if (texto == "/programacion 2") { enviarTelegramConLog(mensajeProgramacion(1)); }
  else if (texto == "/programacion 3") { enviarTelegramConLog(mensajeProgramacion(2)); }
  else if (texto == "/programacion 4") { enviarTelegramConLog(mensajeProgramacion(3)); }
  else if (texto == "/ejemplo") {
    configurarProgramacionEjemplo();
    enviarTelegramConLog("✅ Programacion ejemplo configurada");
  }
  else if (texto == "/guardar") {
    guardarProgramacion();
    enviarTelegramConLog("✅ Programacion guardada");
  }
  else { return false; }
  return true;
}

//--------------------------------------------------
// COMANDOS: AUXILIARES
//--------------------------------------------------
bool procesarComandoAuxiliar(const String& texto, int64_t chat_id) {
  // ── POZOS ──
  if      (texto == "/pozo1on")     { abrirPozo(0); }
  else if (texto == "/pozo1off")    { cerrarPozo(0); }
  else if (texto == "/pozo2on")     { abrirPozo(1); }
  else if (texto == "/pozo2off")    { cerrarPozo(1); }
  else if (texto == "/pozo1toggle") { togglePozo(0); }
  else if (texto == "/pozo2toggle") { togglePozo(1); }
  else if (texto == "/pozoestado") {
    String msg = "📊 ESTADO POZOS\n═══════════════════\n\n";
    msg += getEstadoPozos();
    msg += "\n💡 Comandos:\n";
    msg += "  /pozo1on /pozo1off /pozo2on /pozo2off\n";
    msg += "  /pozo1toggle /pozo2toggle\n";
    msg += "\n✅ Los POZOS guardan su estado en reinicios";
    enviarTelegramConLog(msg);
  }
  // ── VENTILADOR ──
  else if (texto == "/ventilador_on") {
    ventiladorForzado = true; ventiladorActivo = true;
    digitalWrite(pinVentilador, HIGH);
    enviarTelegramConLog("🔵 Ventilador FORZADO ON\n💡 Usa /ventilador_off para volver a automático");
    registrarEvento("Ventilador forzado ON manualmente");
  }
  else if (texto == "/ventilador_off") {
    ventiladorForzado = false; ventiladorActivo = false;
    digitalWrite(pinVentilador, LOW);
    enviarTelegramConLog("⚪ Ventilador OFF - Vuelve a modo AUTOMÁTICO\n🌡️ Temp: " + String(temperatura, 1) + "°C / Hum: " + String(humedad, 1) + "%");
    registrarEvento("Ventilador manual OFF - modo automático restaurado");
  }
  // ── CONTADORES ──
  else if (texto == "/contadores") {
    String msg = "📊 CONTADORES DE RIEGO\n═══════════════════\n\n";
    msg += "Totales acumulados (persistentes):\n\n";
    unsigned long totalRiegos = 0, totalMinutos = 0;
    for (int z = 0; z < 4; z++) {
      msg += "  Zona " + String(z+1) + ": " + String(contadorRiegoZona[z]) + " riego" +
             (contadorRiegoZona[z] == 1 ? "" : "s");
      if (minutosRiegadoZona[z] > 0) {
        unsigned long h = minutosRiegadoZona[z] / 60, m = minutosRiegadoZona[z] % 60;
        msg += " (" + (h > 0 ? String(h) + "h " : "") + String(m) + " min)";
      }
      msg += "\n";
      totalRiegos  += contadorRiegoZona[z];
      totalMinutos += minutosRiegadoZona[z];
    }
    msg += "\n  TOTAL: " + String(totalRiegos) + " riegos";
    if (totalMinutos > 0) {
      unsigned long th = totalMinutos / 60, tm = totalMinutos % 60;
      msg += " / " + (th > 0 ? String(th) + "h " : "") + String(tm) + " min";
    }
    msg += "\n\n💡 Usa /contadores_reset para reiniciar a cero";
    enviarTelegramConLog(msg);
  }
  else if (texto == "/contadores_reset") {
    prefs.begin("riego_stats", false);
    for (int z = 0; z < 4; z++) {
      contadorRiegoZona[z] = 0; minutosRiegadoZona[z] = 0;
      char _kCnt[16]; snprintf(_kCnt, sizeof(_kCnt), "cnt_%d", z);
      char _kMin[16]; snprintf(_kMin, sizeof(_kMin), "min_%d", z);
      prefs.putULong(_kCnt, 0); prefs.putULong(_kMin, 0);
    }
    prefs.end();
    enviarTelegramConLog("✅ Contadores reiniciados a cero (NVS borrado)");
    statsDirty = false;
  }
  // ── LLUVIA ──
  else if (texto.startsWith("/lluvia_on")) {
    int horas = 24;
    String param = texto.substring(10); param.trim();
    if (param.length() > 0) horas = param.toInt();
    if (horas < 1 || horas > 168) horas = 24;
    modoLluvia = true;
    finModoLluvia = millis() + (unsigned long)horas * 3600000UL;
    guardarModoLluvia();
    int zonasParadas = 0;
    for (int z = 0; z < 4; z++) {
      if (zona[z].riegoAutomaticoActivo) { cerrarZona(z); zonasParadas++; }
    }
    registrarEvento("🌧️ Modo lluvia activado " + String(horas) + " h");
    String msg = "🌧️ MODO LLUVIA ACTIVADO\nRiegos automáticos pausados " + String(horas) + " hora" + (horas == 1 ? "" : "s");
    if (zonasParadas > 0) msg += "\n⛔ " + String(zonasParadas) + " riego(s) detenido(s)";
    enviarTelegramConLog(msg);
  }
  else if (texto == "/lluvia_off") {
    modoLluvia = false; finModoLluvia = 0; guardarModoLluvia();
    registrarEvento("☀️ Modo lluvia desactivado manualmente");
    enviarTelegramConLog("☀️ MODO LLUVIA DESACTIVADO\nRiegos automáticos reanudados");
  }
  // ── PRÓXIMOS RIEGOS ──
  else if (texto == "/proximo") {
    enviarTelegramConLog(proximosRiegos());
  }
  // ── UMBRAL TEMPERATURA ALERTA ──
  else if (texto.startsWith("/temp_umbral ")) {
    float umbral = texto.substring(13).toFloat();
    if (umbral < 20.0 || umbral > 60.0) {
      enviarTelegramConLog("❌ Umbral inválido. Usa 20-60 °C\nEjemplo: /temp_umbral 38");
    } else {
      tempAlertaUmbral = umbral;
      alertaTempEnviada = false;
      // v7.6: persistir umbral en NVS
      prefs.begin("riego_cfg", false);
      prefs.putFloat("temp_alerta", tempAlertaUmbral);
      prefs.end();
      enviarTelegramConLog("✅ Umbral de temperatura → " + String(tempAlertaUmbral, 0) + " °C\nActual: " + String(temperatura, 1) + " °C");
    }
  }
  // ── INFORME DIARIO ──
  else if (texto == "/informe_on") {
    informeDiarioActivo = true;
    prefs.begin("riego_cfg", false);
    prefs.putBool("inf_activo", true); prefs.end();
    registrarEvento("Informe diario activado");
    enviarTelegramConLog("📋 INFORME DIARIO ACTIVADO\n🕗 Hora: " + String(informeDiarioHora) + ":" +
                         String(informeDiarioMinuto < 10 ? "0" : "") + String(informeDiarioMinuto) +
                         "\n💡 Cambia la hora con /informe_hora HH MM");
  }
  else if (texto == "/informe_off") {
    informeDiarioActivo = false;
    prefs.begin("riego_cfg", false);
    prefs.putBool("inf_activo", false); prefs.end();
    registrarEvento("Informe diario desactivado");
    enviarTelegramConLog("📋 Informe diario DESACTIVADO");
  }
  else if (texto.startsWith("/informe_hora ")) {
    String params = texto.substring(14); params.trim();
    int sp = params.indexOf(' ');
    if (sp == -1) {
      enviarTelegramConLog("Formato: /informe_hora HH MM\nEjemplo: /informe_hora 8 0");
      return true;
    }
    int hh = params.substring(0, sp).toInt();
    int mm = params.substring(sp + 1).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
      enviarTelegramConLog("❌ Hora inválida. HH: 0-23, MM: 0-59");
    } else {
      informeDiarioHora   = hh;
      informeDiarioMinuto = mm;
      prefs.begin("riego_cfg", false);
      prefs.putInt("inf_hora", hh); prefs.putInt("inf_min", mm); prefs.end();
      registrarEvento("Informe diario hora → " + String(hh) + ":" + String(mm));
      enviarTelegramConLog("✅ Informe diario → " + String(hh) + ":" +
                           String(mm < 10 ? "0" : "") + String(mm) +
                           "\n  Estado: " + String(informeDiarioActivo ? "ACTIVADO ✅" : "DESACTIVADO ❌ (usa /informe_on)"));
    }
  }
  else { return false; }
  return true;
}

//--------------------------------------------------
// procesarComando — dispatcher
//--------------------------------------------------
void procesarComando(String texto, int64_t chat_id) {
  texto.trim();
  registrarEvento("Comando: " + texto);

  if (procesarComandoConsulta(texto, chat_id))     return;
  if (procesarComandoSistema(texto, chat_id))      return;
  if (procesarComandoZonas(texto, chat_id))        return;
  if (procesarComandoProgramacion(texto, chat_id)) return;
  if (procesarComandoAuxiliar(texto, chat_id))     return;
  enviarTelegramConLog("❌ Comando no reconocido. /ayuda");
}

//--------------------------------------------------
// MONITOREO Y HEALTH CHECK
//--------------------------------------------------
void actualizarMinimosMemoria() {
  size_t heapLibre = ESP.getFreeHeap();
  size_t maxAlloc = ESP.getMaxAllocHeap();
  if (heapMinimoObservado == 0 || heapLibre < heapMinimoObservado) {
    heapMinimoObservado = heapLibre;
  }
  if (maxAllocMinimoObservado == 0 || maxAlloc < maxAllocMinimoObservado) {
    maxAllocMinimoObservado = maxAlloc;
  }
}

void monitorearMemoria() {
  static unsigned long ultimoCheckMemoria = 0;
  unsigned long ahora = millis();
  actualizarMinimosMemoria();

  if (diffMillis(ahora, ultimoCheckMemoria) > 480000UL) {
    ultimoCheckMemoria = ahora;
    size_t heapLibre = ESP.getFreeHeap();
    size_t heapTotal = ESP.getHeapSize();
    int porcentajeUso = ((heapTotal - heapLibre) * 100) / heapTotal;

    if (heapLibre < 20000) {
      registrarEvento("⚠️ Memoria baja - reiniciando (" + String(heapLibre/1024) + " KB)");
      TBMessage msgOut;
      msgOut.chatId = CHAT_ID;
      msgOut.text = "⚠️ Memoria baja (" + String(heapLibre/1024) + " KB) - Reiniciando";
      bot.sendMessage(msgOut, msgOut.text);
      delay(200);
      guardarProgramacion();
      guardarLog();
      guardarModoLluvia();
      ESP.restart();
    }

    if (porcentajeUso > 70) {
      registrarEvento("⚠️ Memoria: " + String(porcentajeUso) + "% usado (" + String(heapLibre / 1024) + " KB)");
    }
  }
}

void limpiarMemoria() {
  static unsigned long ultimaLimpieza = 0;
  unsigned long ahora = millis();

  if (diffMillis(ahora, ultimaLimpieza) > 21600000UL) {
    ultimaLimpieza = ahora;
    size_t heapLibre = ESP.getFreeHeap();
    size_t heapTotal = ESP.getHeapSize();
    int porcentajeUso = ((heapTotal - heapLibre) * 100) / heapTotal;

    if (porcentajeUso > 40) {
      registrarEvento("Limpieza periodica memoria (" + String(heapLibre / 1024) + " KB)");
      if (porcentajeUso > 80) {
        guardarProgramacion();
        guardarLog();
        guardarModoLluvia();
        ESP.restart();
      }
    }
  }
}

void healthCheck() {
  unsigned long ahora = millis();

  if (diffMillis(ahora, ultimoHealthCheck) > intervaloHealthCheck) {
    ultimoHealthCheck = ahora;

    if (maxTiempoLoop > 15000) {
      ciclosLoopLentos++;
      registrarEvento("⚠️ Loop lento detectado: " + String(maxTiempoLoop) + " ms (" + String(ciclosLoopLentos) + " veces)");

      if (ciclosLoopLentos > 10) {
        registrarEvento("⚠️ Demasiados loops lentos - Reiniciando");
        guardarProgramacion();
        guardarLog();
        guardarModoLluvia();
        delay(200);
        ESP.restart();
      }
    } else {
      ciclosLoopLentos = 0;
    }

    maxTiempoLoop = 0;  // FIX #2: siempre resetear para que el proximo periodo empiece desde 0
  }
}

// Encola un mensaje para enviarlo en la próxima iteración disponible del loop.
// Silencia la parte que no cabe si la cola está llena.
void encolarTelegram(const String& txt) {
    uint8_t siguiente = (_colaTelIn + 1) % COLA_TELEGRAM_MAX;
    if (siguiente != _colaTelOut) {      // no llena
        _colaTelegram[_colaTelIn] = txt;
        _colaTelIn = siguiente;
    } else {
        registrarEvento("⚠️ Cola Telegram llena — mensaje descartado");
    }
}

// Llamar desde loop() — envía UNA entrada pendiente por iteración.
// Así el envío HTTPS no bloquea más de un ciclo.
void vaciarColaTelegram() {
    if (_colaTelIn == _colaTelOut) return;   // vacía
    if (WiFi.status() != WL_CONNECTED) return;
    enviarTelegramConLog(_colaTelegram[_colaTelOut]);
    _colaTelOut = (_colaTelOut + 1) % COLA_TELEGRAM_MAX;
}

//--------------------------------------------------
// SERVIDOR HTTP — v7.6 PARSING URI ESTRICTO
//--------------------------------------------------
String extraerUri(const String& peticion) {
    int firstSpace = peticion.indexOf(' ');
    if (firstSpace == -1) return "";
    int secondSpace = peticion.indexOf(' ', firstSpace + 1);
    if (secondSpace == -1) return "";
    return peticion.substring(firstSpace + 1, secondSpace);
}

void _procesarPeticionHttp(WiFiClient& c, const String& uri, const String& peticion);

void gestionarServidorHttp() {
    static String peticion;
    static unsigned long tInicio = 0;
    WiFiClient cliente = servidorRiego.available();
    if (!cliente) return;
    peticion = "";
    tInicio  = millis();
    while (cliente.connected()) {
        while (cliente.available() && peticion.length() < 512) {
            char ch = cliente.read();
            peticion += ch;
            if (peticion.indexOf("\r\n\r\n") != -1 || peticion.indexOf("\n\n") != -1) {
                String uri = extraerUri(peticion);
                _procesarPeticionHttp(cliente, uri, peticion);
                cliente.stop();
                peticion = "";
                return;
            }
        }
        if ((long)(millis() - tInicio) > 50) break;
        delay(1);
    }
    if (peticion.length() > 0) {
        String uri = extraerUri(peticion);
        _procesarPeticionHttp(cliente, uri, peticion);
    }
    cliente.stop();
    peticion = "";
}

void _procesarPeticionHttp(WiFiClient& c, const String& uri, const String& peticion) {
    bool tokenValido = (peticion.indexOf(HTTP_SECRET_TOKEN) != -1);
    if (!tokenValido) {
        c.print("HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\n\r\nAcceso denegado");
        return;
    }
    if (uri == "/cerrartodo") {
        registrarEvento("⚠️ EMERGENCIA: /cerrartodo recibido por HTTP");
        cerrarTodasLasZonas();
        c.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\n\r\nOK");
    } else if (uri == "/estadojson") {
        char buf[1280];
        buildEstadoJson(buf, sizeof(buf));
        c.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
        c.print(buf);
    } else if (uri == "/estado") {
        String est = "Z1:" + String(zona[0].abierta ? "A" : "C") +
                     " Z2:" + String(zona[1].abierta ? "A" : "C") +
                     " Z3:" + String(zona[2].abierta ? "A" : "C") +
                     " Z4:" + String(zona[3].abierta ? "A" : "C") +
                     " B:"  + String(nivelBajoBalsa ? "OK" : "CRIT") +
                     " T:"  + String(temperatura, 1) +
                     " H:"  + String(humedad, 1) +
                     " P1:" + String(pozoAbierto[0] ? "A" : "C") +
                     " P2:" + String(pozoAbierto[1] ? "A" : "C") +
                     " RA:" + String(reinicioAutomaticoActivado ? "1" : "0") +
                     " RH:" + String(reinicioHoraProgramada) +
                     " RM:" + String(reinicioMinutoProgramado);
        c.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
        c.print(est);
    } else if (uri == "/abrirzona") {
        int z = -1, idxZ = peticion.indexOf("zona=");
        if (idxZ != -1) z = peticion.substring(idxZ + 5).toInt() - 1;
        int idxM = peticion.indexOf("min="), min = 30;
        if (idxM != -1) min = peticion.substring(idxM + 4).toInt();
        if (z >= 0 && z < 4 && min >= 1 && min <= 240) {
            bool ok = iniciarRiegoAutomatico(z, (uint16_t)min);
            if (ok) {
                c.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\n\r\nOK");
            } else {
                c.print("HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\n\r\nERR:balsaCritica");
            }
        } else {
            c.print("HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nERR:parametrosInvalidos");
        }
    } else if (uri == "/cerrarzona") {
        int z = -1, idxZ = peticion.indexOf("zona=");
        if (idxZ != -1) z = peticion.substring(idxZ + 5).toInt() - 1;
        if (z >= 0 && z < 4) {
            zona[z].riegoAutomaticoActivo = false; cerrarZona(z);
            c.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\n\r\nOK");
        } else {
            c.print("HTTP/1.1 400 Bad Request\r\n\r\nParametros invalidos");
        }
    } else if (uri == "/pozo") {
        int num = -1, idxN = peticion.indexOf("num=");
        if (idxN != -1) num = peticion.substring(idxN + 4).toInt() - 1;
        bool esAbrir = peticion.indexOf("accion=abrir") != -1;
        bool esCerrar = peticion.indexOf("accion=cerrar") != -1;
        if (num >= 0 && num < 2 && (esAbrir || esCerrar)) {
            if (esAbrir) abrirPozo(num); else cerrarPozo(num);
            c.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\n\r\nOK");
        } else {
            c.print("HTTP/1.1 400 Bad Request\r\n\r\nParametros invalidos");
        }
    } else if (uri == "/lluvia") {
        bool esOn = peticion.indexOf("accion=on") != -1;
        bool esOff = peticion.indexOf("accion=off") != -1;
        if (!esOn && !esOff) {
            c.print("HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nERR:accion requerida (on|off)");
        } else {
            int idxH = peticion.indexOf("horas="), horas = 24;
            if (idxH != -1) horas = peticion.substring(idxH + 6).toInt();
            if (horas < 1) horas = 1; if (horas > 168) horas = 168;
            modoLluvia = esOn;
            if (esOn) {
                finModoLluvia = millis() + (unsigned long)horas * 3600000UL;
                for (int z = 0; z < 4; z++) if (zona[z].riegoAutomaticoActivo) cerrarZona(z);
            } else finModoLluvia = 0;
            guardarModoLluvia();
            registrarEvento(esOn ? ("🌧️ Modo lluvia ON (" + String(horas) + "h) via HTTP") : "☀️ Modo lluvia OFF via HTTP");
            c.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\n\r\nOK");
        }
    } else if (uri == "/programarzona") {
        int z = -1, p = -1, idxZ = peticion.indexOf("zona="), idxP = peticion.indexOf("prog=");
        if (idxZ != -1) z = peticion.substring(idxZ + 5).toInt() - 1;
        if (idxP != -1) p = peticion.substring(idxP + 5).toInt() - 1;
        if (z >= 0 && z < 4 && p >= 0 && p < 3) {
            int idxH = peticion.indexOf("hora="), idxM = peticion.indexOf("&min="), idxD = peticion.indexOf("dias="), idxDur = peticion.indexOf("dur="), idxAct = peticion.indexOf("activo=");
            if (idxH != -1) zona[z].programa[p].hora = (uint8_t)peticion.substring(idxH + 5).toInt();
            if (idxM != -1) zona[z].programa[p].minuto = (uint8_t)peticion.substring(idxM + 5).toInt();
            if (idxDur != -1) zona[z].programa[p].duracionMin = (uint16_t)peticion.substring(idxDur + 4).toInt();
            if (idxAct != -1) zona[z].programa[p].habilitado = peticion.substring(idxAct + 7).toInt() != 0;
            if (idxD != -1) { String ds = peticion.substring(idxD + 5, idxD + 12); for (int d = 0; d < 7 && d < (int)ds.length(); d++) zona[z].programa[p].dias[d] = (ds[d] == '1'); }
            programacionModificada = true;
            registrarEvento("📅 Prog Z" + String(z+1) + " P" + String(p+1) + " actualizado via HTTP");
            c.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\n\r\nOK");
        } else {
            c.print("HTTP/1.1 400 Bad Request\r\n\r\nParametros invalidos");
        }
    } else {
        c.print("HTTP/1.1 200 OK\r\n\r\nOK");
    }
}

void limpiarBufferTelegram() {
  unsigned long _ahoraLB = millis();
  if (diffMillis(_ahoraLB, ultimoLimpiarTelegram) > intervaloLimpiarTelegram) {
    ultimoLimpiarTelegram = _ahoraLB;
    TBMessage msg; int mensajesDescartados = 0;
    unsigned long inicioLimpieza = millis();
    while (mensajesDescartados < 20 && (diffMillis(millis(), inicioLimpieza) < 2000UL)) {
      yield(); if (bot.getNewMessage(msg)) mensajesDescartados++; else break;
    }
    if (mensajesDescartados > 0) registrarEvento("🧹 Buffer Telegram limpiado (" + String(mensajesDescartados) + " msgs)");
  }
}

// ── RELAY REMOTO v7.6 (clientes locales) ──
int buildEstadoJson(char* buf, int size) {
    int pos = 0;
    pos += snprintf(buf + pos, size - pos, "{");
    for (int z = 0; z < 4; z++) {
        unsigned long restaSeg = zona[z].riegoAutomaticoActivo ? (restanteMs(zona[z].duracionMs, zona[z].inicioRiego) / 1000UL) : 0UL;
        pos += snprintf(buf + pos, size - pos, "\"z%d\":{", z + 1);
        pos += snprintf(buf + pos, size - pos, "\"abierta\":%s,", zona[z].abierta ? "true" : "false");
        pos += snprintf(buf + pos, size - pos, "\"auto\":%s,", zona[z].riegoAutomaticoActivo ? "true" : "false");
        pos += snprintf(buf + pos, size - pos, "\"restaSeg\":%lu", restaSeg);
        for (int p = 0; p < 3; p++) {
            char diasStr[8]; for (int d = 0; d < 7; d++) diasStr[d] = zona[z].programa[p].dias[d] ? '1' : '0'; diasStr[7] = '\0';
            pos += snprintf(buf + pos, size - pos, ",\"p%d\":{", p + 1);
            pos += snprintf(buf + pos, size - pos, "\"act\":%s,", zona[z].programa[p].habilitado ? "true" : "false");
            pos += snprintf(buf + pos, size - pos, "\"h\":%d,\"m\":%d,\"dur\":%d,", zona[z].programa[p].hora, zona[z].programa[p].minuto, zona[z].programa[p].duracionMin);
            pos += snprintf(buf + pos, size - pos, "\"dias\":\"%s\"}", diasStr);
        }
        pos += snprintf(buf + pos, size - pos, "}%s", z < 3 ? "," : "");
    }
    pos += snprintf(buf + pos, size - pos, ",\"balsa\":{");
    pos += snprintf(buf + pos, size - pos, "\"bajo\":%s,\"medio\":%s,\"alto\":%s,", nivelBajoBalsa ? "true" : "false", nivelMedioBalsa ? "true" : "false", nivelAltoBalsa ? "true" : "false");
    pos += snprintf(buf + pos, size - pos, "\"estado\":\"%s\"}", estadoBalsa.c_str());
    char tBuf[8], hBuf[8]; dtostrf(temperatura, 4, 1, tBuf); dtostrf(humedad, 4, 1, hBuf);
    pos += snprintf(buf + pos, size - pos, ",\"temp\":%s,\"hum\":%s", tBuf, hBuf);
    pos += snprintf(buf + pos, size - pos, ",\"pozos\":[%s,%s]", pozoAbierto[0] ? "true" : "false", pozoAbierto[1] ? "true" : "false");
    long horasLluviaRest = 0;
    if (modoLluvia && (long)(finModoLluvia - millis()) > 0) { horasLluviaRest = (long)(finModoLluvia - millis()) / 3600000L; if (horasLluviaRest < 1) horasLluviaRest = 1; }
    pos += snprintf(buf + pos, size - pos, ",\"lluvia\":%s,\"horasLluvia\":%ld}", modoLluvia ? "true" : "false", horasLluviaRest);
    return pos;
}

void pushEstadoRemoto() {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure client; client.setInsecure();
    char buf[1280]; buildEstadoJson(buf, sizeof(buf));
    HTTPClient http; String urlPush = String("https://") + RELAY_HOST + RELAY_PATH + "/push";
    if (!http.begin(client, urlPush)) return;
    http.addHeader("Content-Type", "application/json"); http.addHeader("X-Token", RELAY_TOKEN); http.setTimeout(4000);
    http.POST(buf); http.end();
}

String _relayGetVal(const String& json, const String& key) {
    String sk = "\"" + key + "\":\""; int idx = json.indexOf(sk);
    if (idx != -1) { int s = idx + sk.length(); int e = json.indexOf('"', s); if (e != -1) return json.substring(s, e); }
    String sn = "\"" + key + "\":"; idx = json.indexOf(sn);
    if (idx != -1) { int s = idx + sn.length(); int e = json.indexOf(',', s); if (e == -1) e = json.indexOf('}', s); if (e != -1) return json.substring(s, e); }
    return "";
}

void pullComandos() {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http; String urlCmds = String("https://") + RELAY_HOST + RELAY_PATH + "/comandos?token=" + RELAY_TOKEN;
    if (!http.begin(client, urlCmds)) return;
    http.setTimeout(4000); int code = http.GET();
    if (code != 200) { http.end(); return; }
    String body = http.getString(); http.end();
    if (body.length() < 10 || body.indexOf("\"cmd\"") == -1) return;
    String cmdId = _relayGetVal(body, "id"), cmd = _relayGetVal(body, "cmd"), result = "OK";
    if (cmd == "abrirzona") { int z = _relayGetVal(body, "zona").toInt() - 1, min = _relayGetVal(body, "min").toInt(); if (min <= 0) min = 30; if (z >= 0 && z < 4 && min >= 1 && min <= 240) result = iniciarRiegoAutomatico(z, (uint16_t)min) ? "OK" : "ERR:balsaCritica"; else result = "ERR:parametros"; }
    else if (cmd == "cerrarzona") { int z = _relayGetVal(body, "zona").toInt() - 1; if (z >= 0 && z < 4) { zona[z].riegoAutomaticoActivo = false; cerrarZona(z); } else result = "ERR:parametros"; }
    else if (cmd == "cerrartodo") { registrarEvento("⚠️ EMERGENCIA: cerrartodo via relay remoto"); cerrarTodasLasZonas(); }
    else if (cmd == "pozo") { int num = _relayGetVal(body, "num").toInt() - 1; String accion = _relayGetVal(body, "accion"); if (num >= 0 && num < 2) { if (accion == "abrir") abrirPozo(num); else cerrarPozo(num); } else result = "ERR:parametros"; }
    else if (cmd == "lluvia") { String accion = _relayGetVal(body, "accion"); int horas = _relayGetVal(body, "horas").toInt(); if (horas < 1) horas = 24; if (accion == "on") { modoLluvia = true; finModoLluvia = millis() + (unsigned long)horas * 3600000UL; guardarModoLluvia(); for (int z = 0; z < 4; z++) if (zona[z].riegoAutomaticoActivo) cerrarZona(z); } else if (accion == "off") { modoLluvia = false; finModoLluvia = 0; guardarModoLluvia(); } else result = "ERR:accion"; }
    else if (cmd == "programarzona") { int z = _relayGetVal(body, "zona").toInt() - 1, p = _relayGetVal(body, "prog").toInt() - 1; if (z >= 0 && z < 4 && p >= 0 && p < 3) { String sh = _relayGetVal(body, "hora"), sm = _relayGetVal(body, "min"), sd = _relayGetVal(body, "dias"), sdur = _relayGetVal(body, "dur"), sact = _relayGetVal(body, "activo"); if (sh.length() > 0) zona[z].programa[p].hora = (uint8_t)sh.toInt(); if (sm.length() > 0) zona[z].programa[p].minuto = (uint8_t)sm.toInt(); if (sdur.length() > 0) zona[z].programa[p].duracionMin = (uint16_t)sdur.toInt(); if (sact.length() > 0) zona[z].programa[p].habilitado = sact.toInt() != 0; if (sd.length() >= 7) for (int d = 0; d < 7; d++) zona[z].programa[p].dias[d] = (sd[d] == '1'); programacionModificada = true; } else result = "ERR:parametros"; }
    else result = "ERR:cmdDesconocido";
    if (cmdId.length() > 0) { HTTPClient httpAck; WiFiClientSecure ackClient; ackClient.setInsecure(); String urlAck = String("https://") + RELAY_HOST + RELAY_PATH + "/ack?token=" + RELAY_TOKEN + "&id=" + cmdId + "&result=" + result; if (httpAck.begin(ackClient, urlAck)) { httpAck.setTimeout(3000); httpAck.GET(); httpAck.end(); } }
}

//--------------------------------------------------
// NUEVAS FUNCIONES: LLUVIA, PRÓXIMOS RIEGOS, EXTENDER
//--------------------------------------------------
void verificarModoLluvia() {
    if (!modoLluvia) return;
    if ((long)(millis() - finModoLluvia) >= 0) {
        modoLluvia = false; finModoLluvia = 0; guardarModoLluvia();
        registrarEvento("☀️ Modo lluvia expirado — riegos automáticos reanudados");
        if (WiFi.status() == WL_CONNECTED) enviarTelegramConLog("☀️ MODO LLUVIA TERMINADO\nRiegos automáticos reanudados automáticamente");
    }
}

String proximosRiegos() {
    if (!horaSincronizada) return "⏳ Sin hora sincronizada — no puedo calcular próximos riegos";
    const char* diasNombre[] = {"Dom","Lun","Mar","Mié","Jue","Vie","Sáb"};
    String resultado = "📅 PRÓXIMOS RIEGOS (24h)\n════════════════════\n\n";
    bool hayAlguno = false;
    struct tm ahora; if (!getLocalTime(&ahora)) { ahora = tiempoCache; }
    int minutoActualDia = ahora.tm_hour * 60 + ahora.tm_min;
    int diaActual = ahora.tm_wday; if (diaActual == 0) diaActual = 7;
    struct Evento { int minutosDesdeAhora; String texto; };
    Evento eventos[12]; int nEventos = 0;
    for (int z = 0; z < 4; z++) {
        for (int p = 0; p < 3; p++) {
            if (!zona[z].programa[p].habilitado) continue;
            ProgramaRiego &prog = zona[z].programa[p];
            for (int offset = 0; offset < 7 && nEventos < 12; offset++) {
                int diaBuscado = ((diaActual - 1 + offset) % 7);
                if (!prog.dias[diaBuscado]) continue;
                int minPrograma = prog.hora * 60 + prog.minuto;
                if (offset == 0 && minPrograma <= minutoActualDia) continue;
                int minutosDesde = offset * 1440 + minPrograma - minutoActualDia;
                if (minutosDesde > 1440) break;
                int wday = (ahora.tm_wday + offset) % 7;
                char buf[80];
                snprintf(buf, sizeof(buf), "  Z%d P%d: %s %02d:%02d (%d min)\n", z+1, p+1, diasNombre[wday], prog.hora, prog.minuto, prog.duracionMin);
                eventos[nEventos] = {minutosDesde, String(buf)}; nEventos++; break;
            }
        }
    }
    for (int i = 0; i < nEventos - 1; i++) for (int j = 0; j < nEventos - 1 - i; j++) if (eventos[j].minutosDesdeAhora > eventos[j+1].minutosDesdeAhora) { Evento tmp = eventos[j]; eventos[j] = eventos[j+1]; eventos[j+1] = tmp; }
    for (int i = 0; i < nEventos; i++) { resultado += eventos[i].texto; hayAlguno = true; }
    if (!hayAlguno) resultado += "  Sin riegos programados en las próximas 24h\n";
    if (modoLluvia) { unsigned long minRestante = (finModoLluvia > millis()) ? (finModoLluvia - millis()) / 60000 : 0; resultado += "\n🌧️ Modo lluvia activo — riegos pausados " + String(minRestante) + " min más"; }
    return resultado;
}

bool extenderRiego(int z, int minutos) {
    if (z < 0 || z > 3) return false;
    if (!zona[z].riegoAutomaticoActivo) return false;
    zona[z].duracionMs += (unsigned long)minutos * 60000UL;
    minutosRiegadoZona[z] += minutos; statsDirty = true; return true;
}

//--------------------------------------------------
// REINICIO PROGRAMADO
//--------------------------------------------------
void notificacionUptime() {
    static unsigned long ultimoAvisoUptime = 0;
    const unsigned long SEMANA_MS = 7UL * 24 * 60 * 60 * 1000;
    unsigned long ahora = millis();
    if (ultimoAvisoUptime == 0) { ultimoAvisoUptime = ahora; return; }
    if (diffMillis(ahora, ultimoAvisoUptime) >= SEMANA_MS) {
        ultimoAvisoUptime = ahora;
        unsigned long tiempoActivo = diffMillis(ahora, tiempoInicioSistema);
        unsigned long diasActivo = tiempoActivo / 1000 / 60 / 60 / 24;
        unsigned long horasResto = (tiempoActivo / 1000 / 60 / 60) % 24;
        if (WiFi.status() == WL_CONNECTED) {
            String msg = "📊 INFORME SEMANAL\n─────────────────────\n⏱ Uptime: " + String(diasActivo) + "d " + String(horasResto) + "h\n💾 Heap libre: " + String(ESP.getFreeHeap()/1024) + " KB\n💧 Balsa: " + estadoBalsa + "\n📡 WiFi: " + String(WiFi.RSSI()) + " dBm\n🌡️ Temp: " + String(temperatura, 1) + " °C | Hum: " + String(humedad, 1) + " %\n";
            for (int z = 0; z < 4; z++) if (contadorRiegoZona[z] > 0) { unsigned long h = minutosRiegadoZona[z] / 60, m = minutosRiegadoZona[z] % 60; msg += "🌱 Z" + String(z+1) + ": " + String(contadorRiegoZona[z]) + " riegos" + (minutosRiegadoZona[z] > 0 ? " (" + String(h) + "h " + String(m) + " min)" : "") + "\n"; }
            enviarTelegramConLog(msg);
        }
        registrarEvento("Informe semanal enviado (" + String(diasActivo) + " dias activo)");
    }
}

void verificarReinicioProgramado() {
    if (!reinicioAutomaticoActivado) return;
    static bool reinicioProgramado = false; struct tm tiempo;
    if (obtenerHora(tiempo)) {
        if (tiempo.tm_hour == reinicioHoraProgramada && tiempo.tm_min == reinicioMinutoProgramado && !reinicioProgramado) {
            reinicioProgramado = true;
            registrarEvento("🔄 Reinicio programado a las " + String(reinicioHoraProgramada) + ":" + String(reinicioMinutoProgramado < 10 ? "0" : "") + String(reinicioMinutoProgramado));
            guardarYReiniciar();
        }
        if (tiempo.tm_hour != reinicioHoraProgramada || tiempo.tm_min != reinicioMinutoProgramado) reinicioProgramado = false;
    }
}

void supervisarSistema() {
    static unsigned long ultimoCheckBucle = 0;
    unsigned long ahora = millis();
    if (diffMillis(ahora, ultimoCheckBucle) > 30000UL) {
        ultimoCheckBucle = ahora;
        if (diffMillis(ahora, ultimoLoopExit) > 180000UL) {
            registrarEvento("⚠️ Sistema bloqueado - reiniciando"); guardarLog(); guardarModoLluvia(); delay(200); ESP.restart();
        }
    }
    ultimoLoopExit = ahora;
}

void configurarProgramacionEjemplo() {
    for (int z = 0; z < 4; z++) for (int p = 0; p < 3; p++) zona[z].programa[p].habilitado = false;
    zona[0].programa[0].habilitado = true; zona[0].programa[0].hora = 7; zona[0].programa[0].minuto = 0; for(int i=0;i<5;i++) zona[0].programa[0].dias[i] = true; zona[0].programa[0].dias[5] = false; zona[0].programa[0].dias[6] = false; zona[0].programa[0].duracionMin = 15;
    zona[0].programa[1].habilitado = true; zona[0].programa[1].hora = 19; zona[0].programa[1].minuto = 0; for(int i=0;i<5;i++) zona[0].programa[1].dias[i] = true; zona[0].programa[1].dias[5] = false; zona[0].programa[1].dias[6] = false; zona[0].programa[1].duracionMin = 10;
    zona[1].programa[0].habilitado = true; zona[1].programa[0].hora = 8; zona[1].programa[0].minuto = 30; for(int i=0;i<5;i++) zona[1].programa[0].dias[i] = false; zona[1].programa[0].dias[5] = true; zona[1].programa[0].dias[6] = true; zona[1].programa[0].duracionMin = 20;
    zona[2].programa[0].habilitado = true; zona[2].programa[0].hora = 6; zona[2].programa[0].minuto = 0; for(int i=0;i<7;i++) zona[2].programa[0].dias[i] = true; zona[2].programa[0].duracionMin = 5;
    marcarProgramacionModificada(); guardarProgramacion();
    registrarEvento("Programacion ejemplo configurada");
}

//--------------------------------------------------
// SETUP
//--------------------------------------------------
void setup() {
    Serial.begin(115200);
    cargarLog();
    Serial.println("\n\n==========================================");
    Serial.println("    SISTEMA DE RIEGO FINCA - v7.6.0-stab");
    Serial.println("==========================================\n");
    Serial.println("🔌 CHIP: " + String(ESP.getChipModel()));
    Serial.println("💻 CPU: " + String(ESP.getChipCores()) + " cores");
    Serial.println("💾 Flash: " + String(ESP.getFlashChipSize() / 1024 / 1024) + " MB");
    Serial.println("📊 Free Heap: " + String(ESP.getFreeHeap() / 1024) + " KB");
    Serial.println("==========================================\n");
    Serial.println("✅ ZONAS guardan estado en reinicio");
    Serial.println("✅ POZOS guardan estado en reinicio");
    Serial.println("✅ Optimizado - Sin bloqueos");
    Serial.println("==========================================\n");
    wifiConectadoAnterior = false; horaSincronizada = false; primeraConexionWiFi = true;
    tiempoInicioSistema = millis(); ultimoLoopExit = millis();
    Serial.println("🌡️ DHT22 ACTIVADO - GPIO4");
    Serial.println("🔵 Ventilador automático GPIO14");
    Serial.println("💧 3 BOYAS: Inferior (GPIO13), Media (GPIO15), Superior (GPIO16)");
    Serial.println("   LOW = SUMERGIDA = HAY AGUA ✅\n");
    Serial.println("🔧 2 POZOS: GPIO17/19 y GPIO18/20 (INDEPENDIENTES)");
    Serial.println("✅ POZOS guardan estado en reinicios\n");
    dht.begin();
    for (int i = 0; i < 10; i++) { delay(100); }
    Serial.println("🌡️ Leyendo DHT22 por primera vez...");
    float h = dht.readHumidity(); float t = dht.readTemperature();
    if (isnan(h) || isnan(t)) { Serial.println("❌ DHT22 no responde - Verificar conexión"); dhtDisponible = false; }
    else { Serial.println("✅ DHT22 OK - Temp: " + String(t, 1) + "°C | Hum: " + String(h, 1) + "%"); dhtDisponible = true; temperatura = t; humedad = h; }
    secureClient.setInsecure(); secureClient.setTimeout(10);
    prefs.begin("riego", true);
    reinicioAutomaticoActivado = prefs.getBool("reinicio_activo", true);
    reinicioHoraProgramada    = prefs.getInt("reinicio_hora",   3);
    reinicioMinutoProgramado  = prefs.getInt("reinicio_minuto", 0);
    prefs.end();
    Serial.println("🔁 Reinicio automático: " + String(reinicioAutomaticoActivado ? "ACTIVADO" : "DESACTIVADO"));
    Serial.println("   Hora: " + String(reinicioHoraProgramada) + ":" + String(reinicioMinutoProgramado < 10 ? "0" : "") + String(reinicioMinutoProgramado));
    prefs.begin("riego_stats", true);
    for (int i = 0; i < 4; i++) { char _kCnt[16]; snprintf(_kCnt, sizeof(_kCnt), "cnt_%d", i); char _kMin[16]; snprintf(_kMin, sizeof(_kMin), "min_%d", i); contadorRiegoZona[i] = prefs.getULong(_kCnt, 0); minutosRiegadoZona[i] = prefs.getULong(_kMin, 0); }
    prefs.end();
    prefs.begin("riego_cfg", true);
    tempVentiladorOn    = prefs.getFloat("tv_on",   TEMP_VENTILADOR_ON_C);
    tempVentiladorOff   = prefs.getFloat("tv_off",  TEMP_VENTILADOR_OFF_C);
    humVentiladorOn     = prefs.getFloat("hv_on",   HUM_VENTILADOR_ON_PCT);
    humVentiladorOff    = prefs.getFloat("hv_off",  HUM_VENTILADOR_OFF_PCT);
    wdtTimeoutSegActual = prefs.getUInt("wdt_seg",  WDT_TIMEOUT_MS / 1000);
    intervaloNTP        = (unsigned long)prefs.getUInt("ntp_min", 30) * 60000UL;
    informeDiarioActivo = prefs.getBool("inf_activo", false);
    informeDiarioHora   = prefs.getInt("inf_hora",    8);
    informeDiarioMinuto = prefs.getInt("inf_min",     0);
    tempAlertaUmbral    = prefs.getFloat("temp_alerta", 38.0f);
    prefs.end();
    Serial.println("✅ Configuración y estadísticas cargadas desde NVS");
    for (int i = 0; i < 4; i++) { pinMode(pinAbrir[i], OUTPUT); pinMode(pinCerrar[i], OUTPUT); digitalWrite(pinAbrir[i], LOW); digitalWrite(pinCerrar[i], LOW); Serial.println("  Zona " + String(i+1) + ": Pines inicializados"); }
    estadoValvula = VALVULA_IDLE; zonaEnMovimiento = -1;
    for (int i = 0; i < 2; i++) { pinMode(pinPozoAbrir[i], OUTPUT); pinMode(pinPozoCerrar[i], OUTPUT); digitalWrite(pinPozoAbrir[i], LOW); digitalWrite(pinPozoCerrar[i], LOW); pozoEnMovimiento[i] = false; Serial.println("  POZO " + String(i+1) + ": Pines inicializados (GPIO" + String(pinPozoAbrir[i]) + "/" + String(pinPozoCerrar[i]) + ")"); }
    for (int i = 0; i < 2; i++) { char _kPozoSetup[20]; snprintf(_kPozoSetup, sizeof(_kPozoSetup), "pozo_abierto_%d", i); pozoAbierto[i] = leerPreferenceBool(_kPozoSetup, false); if (pozoAbierto[i]) registrarEvento("🔄 POZO " + String(i+1) + " restaurado: ABIERTO"); else registrarEvento("🔄 POZO " + String(i+1) + " restaurado: CERRADO"); digitalWrite(pinPozoAbrir[i], LOW); digitalWrite(pinPozoCerrar[i], LOW); pozoEnMovimiento[i] = false; }
    bool hayMicrocorte = false; int zonasParaReabrir[4]; unsigned long tiemposRestantes[4]; int numZonasParaReabrir = 0;
    for (int i = 0; i < 4; i++) { unsigned long restante = leerPreferenceULong(("rest_" + String(i)).c_str(), 0); if (restante > 30000 && restante < 14400000) { zonasParaReabrir[numZonasParaReabrir] = i; tiemposRestantes[numZonasParaReabrir] = restante; numZonasParaReabrir++; hayMicrocorte = true; Serial.println("  Zona " + String(i+1) + ": Riego pendiente (" + String(restante/60000) + "m)"); } }
    prefs.begin("riego", false);
    for (int i = 0; i < 4; i++) { zona[i].abierta = false; zona[i].riegoAutomaticoActivo = false; char _kRest[16]; snprintf(_kRest, sizeof(_kRest), "rest_%d", i); char _kMan[24]; snprintf(_kMan, sizeof(_kMan), "zona_manual_%d", i); prefs.remove(_kRest); prefs.remove(_kMan); }
    balsaInterrumpidoEpoch = prefs.getLong("balsa_ie", 0);
    for (int z = 0; z < 4; z++) { char _kBI[16]; snprintf(_kBI, sizeof(_kBI), "balsa_im%d", z); balsaInterrumpidoMs[z] = prefs.getULong(_kBI, 0); }
    prefs.end();
    if (hayMicrocorte) { for (int i = 0; i < numZonasParaReabrir; i++) { int z = zonasParaReabrir[i]; zona[z].duracionMs = tiemposRestantes[i]; ultimoMensajeFinRiego[z] = ULONG_MAX; Serial.println("  Zona " + String(z+1) + ": Riego restaurado (" + String(tiemposRestantes[i]/60000) + "m)"); } }
    pinMode(pinBoyaInferior, INPUT_PULLUP); pinMode(pinBoyaMedia, INPUT_PULLUP); pinMode(pinBoyaSuperior, INPUT_PULLUP);
    nivelBajoBalsa = (digitalRead(pinBoyaInferior) == LOW); nivelMedioBalsa = (digitalRead(pinBoyaMedia) == LOW); nivelAltoBalsa = (digitalRead(pinBoyaSuperior) == LOW);
    estadoBalsa = getEstadoBalsa();
    Serial.println("💧 Estado inicial de la balsa:");
    Serial.println("  Inferior: " + String(nivelBajoBalsa ? "SUMERGIDA ✅" : "SECA ⚠️"));
    Serial.println("  Media:    " + String(nivelMedioBalsa ? "SUMERGIDA ✅" : "SECA ⚠️"));
    Serial.println("  Superior: " + String(nivelAltoBalsa ? "SUMERGIDA ✅" : "SECA ⚠️"));
    Serial.println("  Estado: " + estadoBalsa);
    lecturaMecanicaBajoAnterior = nivelBajoBalsa; lecturaMecanicaMedioAnterior = nivelMedioBalsa; lecturaMecanicaAltoAnterior = nivelAltoBalsa;
    tiempoCambioBajo = millis(); tiempoCambioMedio = millis(); tiempoCambioAlto = millis();
    iniciarVentilador(); iniciarWatchdog(); conectarWiFi(); iniciarHora();
    Serial.println("🕐 Comprobando hora NTP..."); actualizarHoraCache();
    if (horaSincronizada) Serial.println("✅ Hora disponible: " + obtenerTimestamp()); else Serial.println("⏳ NTP pendiente — sincronizará en background");
    Serial.println("🔵 Inicializando Telegram..."); bot.setTelegramToken(BOT_TOKEN); bot.setUpdateTime(2000); Serial.println("✅ Telegram configurado");
    servidorRiego.begin();
    ultimoIntentoWifi = millis(); ultimoIntentoNTP = millis(); ultimoReinicio = millis(); ultimoLoopExit = millis(); ultimoHealthCheck = millis(); ultimoLimpiarTelegram = millis(); ultimoMensajeTelegram = millis(); ultimaLecturaDHT = millis(); ultimoTickHora = millis(); ultimoGuardadoRiegoActivo = millis(); cerrandoTodo = false; sistemaEnEstadoCritico = !nivelBajoBalsa;
    cargarProgramacion();
    if (hayMicrocorte) { for (int i = 0; i < numZonasParaReabrir; i++) { int z = zonasParaReabrir[i]; zona[z].duracionMs = tiemposRestantes[i]; zona[z].riegoAutomaticoActivo = false; zona[z].inicioRiego = 0; } }
    prefs.begin("riego", true);
    for (int i = 0; i < 4; i++) { char _kZA[24]; snprintf(_kZA, sizeof(_kZA), "zona_abierta_%d", i); bool estabaAbierta = prefs.getBool(_kZA, false); if (!estabaAbierta) continue; bool tieneMicrocorte = false; for (int m = 0; m < numZonasParaReabrir; m++) if (zonasParaReabrir[m] == i) { tieneMicrocorte = true; break; } if (!tieneMicrocorte) { zona[i].abierta = true; registrarEvento("🔄 Zona " + String(i+1) + " manual restaurada: ABIERTA"); } }
    prefs.end();
    if (hayMicrocorte && nivelBajoBalsa) {
        registrarEvento("Reinicio detectado - Reanudando " + String(numZonasParaReabrir) + " riegos");
        for (int i = 0; i < numZonasParaReabrir; i++) {
            int z = zonasParaReabrir[i]; zona[z].abierta = false; digitalWrite(pinAbrir[z], LOW); digitalWrite(pinCerrar[z], LOW); delay(10); ultimoMensajeFinRiego[z] = ULONG_MAX;
            if (i == 0) { zona[z].inicioRiego = millis(); if (abrirZona(z)) { zona[z].riegoAutomaticoActivo = true; char _kRZ[16]; snprintf(_kRZ, sizeof(_kRZ), "rest_%d", z); prefs.begin("riego", false); prefs.putULong(_kRZ, zona[z].duracionMs); prefs.end(); Serial.println("  Zona " + String(z+1) + " reanudada (apertura en curso)"); if (WiFi.status() == WL_CONNECTED) enviarTelegramConLog("🔄 RIEGO REANUDADO Zona " + String(z+1) + " (" + String(zona[z].duracionMs/60000) + "m restantes)"); } else { zonaReabrirPendiente[z] = true; Serial.println("  Zona " + String(z+1) + " → pendiente (fallo apertura): " + getErrorAbrir()); } }
            else { zonaReabrirPendiente[z] = true; char _kRest[16]; snprintf(_kRest, sizeof(_kRest), "rest_%d", z); prefs.begin("riego", false); prefs.putULong(_kRest, zona[z].duracionMs); prefs.end(); Serial.println("  Zona " + String(z+1) + " pendiente (" + String(zona[z].duracionMs/60000) + "m), se abrirá en loop"); }
        }
    } else if (hayMicrocorte && !nivelBajoBalsa) { registrarEvento("Reinicio con balsa critica - Riegos cancelados"); prefs.begin("riego", false); for (int i = 0; i < 4; i++) prefs.remove(("rest_" + String(i)).c_str()); prefs.end(); }
    ultimoResetWDT = millis(); Serial.println("\n✅ Setup completado\n");
    if (WiFi.status() == WL_CONNECTED) {
        delay(1000); String msgInicio; msgInicio.reserve(2048);
        msgInicio += "═════════════════════════════\n   🔴🔵 SISTEMA REINICIADO 🔵🔴\n═════════════════════════════\n      ⚽ FC BARCELONA ⚽\n        MÉS QUE UN CLUB\n═════════════════════════════\n\n";
        char ipBuf[24]; fmtIp(ipBuf, sizeof(ipBuf)); msgInicio += "📡 IP: " + String(ipBuf) + "\n💧 Balsa: " + estadoBalsa + "\n🌡️ Temp: " + String(temperatura, 1) + "°C | Hum: " + String(humedad, 1) + "%\n🕐 Hora: " + obtenerTimestamp() + "\n📊 Uptime: 0 minutos\n\n🔌 Válvulas principales:\n";
        for (int i = 0; i < 4; i++) { msgInicio += "  Z" + String(i+1) + ": "; if (zona[i].abierta) { msgInicio += "🔓 ABIERTA"; if (zona[i].riegoAutomaticoActivo) { unsigned long restante = restanteMs(zona[i].duracionMs, zona[i].inicioRiego); msgInicio += " [⏱ " + String(restante/60000) + "m]"; } } else msgInicio += "🔒 CERRADA"; msgInicio += "\n"; }
        msgInicio += "\n🔧 POZOS:\n" + getEstadoPozos() + "\n🔁 REINICIO AUTOMÁTICO:\n  Estado: " + String(reinicioAutomaticoActivado ? "✅ ACTIVADO" : "❌ DESACTIVADO") + "\n  Hora: " + String(reinicioHoraProgramada) + ":" + String(reinicioMinutoProgramado < 10 ? "0" : "") + String(reinicioMinutoProgramado) + "\n";
        if (hayMicrocorte && nivelBajoBalsa) msgInicio += "\n⚡️ " + String(numZonasParaReabrir) + " riegos reanudados.\n";
        msgInicio += "\n💡 Usa /ayuda para ver comandos\n✅ DHT22 ACTIVADO - Ventilador automático\n✅ 2 POZOS independientes (sin boyas)\n✅ POZOS guardan estado en reinicios\n✅ ZONAS guardan estado en reinicios\n✅ Nuevos comandos: /pozo1toggle y /pozo2toggle\n\n👨‍💻👨 Creado por CIRIACO\n💧💧💧 RIEGO ALCUBILLAS\n🤖 ESP32-S3 - v7.6.0-stab";
        enviarTelegramConLog(msgInicio);
    }
}

//--------------------------------------------------
// LOOP PRINCIPAL
//--------------------------------------------------
void loop() {
    unsigned long duracionLoop;
    inicioLoop = millis();
    gestionarServidorHttp();
    unsigned long ahora = millis();
    if (diffMillis(ahora, ultimoTickHora) > 1000UL) {
        ultimoTickHora = ahora; struct tm ahoraTm;
        if (getLocalTime(&ahoraTm, 0)) { if (ahoraTm.tm_year > 100) { tiempoCache = ahoraTm; horaSincronizada = true; ultimaActualizacionHora = ahora; } }
    }
    gestionarNTP(); actualizarHoraCache();
    static unsigned long ultimoIntentoForzado = 0;
    if (!horaSincronizada && (diffMillis(ahora, ultimoIntentoForzado) > 600000UL)) { ultimoIntentoForzado = ahora; forzarSincronizacionHora(); }
    gestionarWiFi();
    if (diffMillis(ahora, ultimoTelegram) >= intervaloTelegram) { ultimoTelegram = ahora; gestionarTelegram(); }
    if (diffMillis(ahora, ultimoDHT) >= intervaloDHT) { ultimoDHT = ahora; leerDHT(); gestionarVentilador(); }
    if (diffMillis(ahora, ultimoNivel) >= intervaloNivel) { ultimoNivel = ahora; gestionarNivelBalsa(); verificarEstadoBoyas(); }
    if (diffMillis(ahora, ultimoScheduler) >= intervaloScheduler) { ultimoScheduler = ahora; schedulerRiego(); }
    gestionarValvulas(); gestionarReaperturas(); gestionarFinRiego(); gestionarPozos();
    monitorearMemoria(); limpiarMemoria(); supervisarSistema(); healthCheck(); verificarReinicioProgramado(); notificacionUptime(); verificarModoLluvia();
    if (diffMillis(ahora, ultimoLimpiarTelegram) >= intervaloLimpiarTelegram) limpiarBufferTelegram();
    vaciarColaTelegram();
    if (horaSincronizada && !estadoLluviaRestaurado) { estadoLluviaRestaurado = true; restaurarModoLluvia(); }
    if (horaSincronizada && !riegoActivoRestaurado) { riegoActivoRestaurado = true; restaurarRiegoActivo(); }
    if (diffMillis(ahora, ultimoGuardadoRiegoActivo) > 300000UL) { ultimoGuardadoRiegoActivo = ahora; guardarRiegoActivo(); }
#if RELAY_HABILITADO
    if (diffMillis(ahora, ultimoPushRelay) > RELAY_PUSH_INTERVALO) { ultimoPushRelay = ahora; pushEstadoRemoto(); }
    if (diffMillis(ahora, ultimoPullRelay) > RELAY_PULL_INTERVALO) { ultimoPullRelay = ahora; pullComandos(); }
#endif
    if (programacionModificada && diffMillis(ahora, ultimoGuardado) > intervaloGuardado) { guardarProgramacion(); ultimoGuardado = ahora; }
    static unsigned long ultimoGuardadoLog = 0;
    if (diffMillis(ahora, ultimoGuardadoLog) > 300000UL) { ultimoGuardadoLog = ahora; guardarLog(); }
    if (!nivelBajoBalsa && !sistemaEnEstadoCritico) {
        sistemaEnEstadoCritico = true; balsaInterrumpidoEpoch = (long)time(nullptr);
        for (int z = 0; z < 4; z++) balsaInterrumpidoMs[z] = zona[z].riegoAutomaticoActivo ? restanteMs(zona[z].duracionMs, zona[z].inicioRiego) : 0UL;
        guardarEstadoCritico(); cerrarTodasLasZonas(); registrarEvento("🔴 Balsa CRÍTICA - Riegos detenidos");
    }
    if (nivelBajoBalsa && sistemaEnEstadoCritico) { sistemaEnEstadoCritico = false; registrarEvento("🟢 Balsa recuperada - Riegos permitidos"); reanudarRiegoBalsaRecuperada(); }
    duracionLoop = millis() - inicioLoop; if (duracionLoop > maxTiempoLoop) maxTiempoLoop = duracionLoop;
    delay(1);
}
