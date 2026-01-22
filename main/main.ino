// LIBRERÍAS ESTÁNDAR Y DE TERCEROS

#include <cstdlib>
#include <string.h>
#include <time.h>
#include <WiFi.h>
#include <mqtt_client.h>
#include <az_core.h>
#include <az_iot.h>
#include <azure_ca.h>
#include "AzIoTSasToken.h"
#include "SerialLogger.h"
#include "iot_configs.h"


// CONFIGURACIÓN DE AZURE Y MQTT

#define AZURE_SDK_CLIENT_USER_AGENT "c%2F" AZ_SDK_VERSION_STRING "(ard;esp32)"
#define sizeofarray(a) (sizeof(a) / sizeof(a[0]))
#define NTP_SERVERS "pool.ntp.org", "time.nist.gov"
#define MQTT_QOS1 1
#define DO_NOT_RETAIN_MSG 0
#define SAS_TOKEN_DURATION_IN_MINUTES 60
#define UNIX_TIME_NOV_13_2017 1510592825
#define PST_TIME_ZONE -8
#define PST_TIME_ZONE_DAYLIGHT_SAVINGS_DIFF 1
#define GMT_OFFSET_SECS (PST_TIME_ZONE * 3600)
#define GMT_OFFSET_SECS_DST ((PST_TIME_ZONE + PST_TIME_ZONE_DAYLIGHT_SAVINGS_DIFF) * 3600)


// CONFIGURACIÓN FreeRTOS - PRIORIDADES DE TAREAS

#define PRIORIDAD_REGISTRADOR_SERIAL  1   // BAJA - Solo para debug
#define PRIORIDAD_MONITOR_WIFI        2   // MEDIA - Monitoreo periódico
#define PRIORIDAD_ENVIADOR_MQTT       3   // ALTA - Envío normal de datos
#define PRIORIDAD_ADQUISICION         3   // ALTA - Lectura de sensores
#define PRIORIDAD_MANEJADOR_ALARMAS   4   // CRÍTICA - Eventos prioritarios


// CONFIGURACIÓN FreeRTOS - TAMAÑOS DE STACK

#define TAMANIO_STACK_PEQUENIO  2048  // 8KB  - Tareas simples
#define TAMANIO_STACK_MEDIANO   4096  // 16KB - Tareas con procesamiento
#define TAMANIO_STACK_GRANDE    8192  // 32KB - Tareas con MQTT/WiFi


// CONFIGURACIÓN FreeRTOS - TAMAÑOS DE COLAS

#define TAMANIO_COLA_TELEMETRIA  8   // Máximo 8 mensajes pendientes
#define TAMANIO_COLA_ALARMAS     4   // Máximo 4 alarmas pendientes
#define TAMANIO_BUFFER_DATOS_ENTRANTES 128


// CONFIGURACIÓN FreeRTOS - BITS DE EVENTOS

#define BIT_WIFI_CONECTADO     (1 << 0)  // Bit 0: WiFi conectado
#define BIT_MQTT_CONECTADO     (1 << 1)  // Bit 1: MQTT conectado
#define BIT_HORA_SINCRONIZADA  (1 << 2)  // Bit 2: Hora sincronizada
#define BIT_ALARMA_ACTIVA      (1 << 3)  // Bit 3: Alarma activa


// VARIABLES GLOBALES DE AZURE IoT

static const char* ssid = IOT_CONFIG_WIFI_SSID;
static const char* password = IOT_CONFIG_WIFI_PASSWORD;
static const char* host = IOT_CONFIG_IOTHUB_FQDN;
static const char* mqtt_broker_uri = "mqtts://" IOT_CONFIG_IOTHUB_FQDN;
static const char* device_id = IOT_CONFIG_DEVICE_ID;
static const int mqtt_port = AZ_IOT_DEFAULT_MQTT_CONNECT_PORT;

static esp_mqtt_client_handle_t clienteMQTT;
static az_iot_hub_client clienteAzure;
static char idClienteMQTT[128];
static char nombreUsuarioMQTT[128];
static char contrasenaMQTT[200];
static uint8_t bufferFirmaSAS[256];
static char topicoTelemetria[128];
static uint32_t contadorEnviosTelemetria = 0;
static char datosEntrantes[TAMANIO_BUFFER_DATOS_ENTRANTES];

#ifndef IOT_CONFIG_USE_X509_CERT
static AzIoTSasToken tokenSAS(
    &clienteAzure,
    AZ_SPAN_FROM_STR(IOT_CONFIG_DEVICE_KEY),
    AZ_SPAN_FROM_BUFFER(bufferFirmaSAS),
    AZ_SPAN_FROM_BUFFER(contrasenaMQTT));
#endif


// ESTRUCTURA DE DATOS DEL MEDIDOR ELÉCTRICO

struct MedidorElectrico {
  String nombreArea;        // Identificador del área
  int id;                   // ID único del medidor (1-4)
  // Variables eléctricas trifásicas
  float voltajeL1N;         // Voltaje fase 1 a neutro (V)
  float voltajeL2N;         // Voltaje fase 2 a neutro (V)
  float voltajeL3N;         // Voltaje fase 3 a neutro (V)
  float corrienteL1;        // Corriente fase 1 (A)
  float corrienteL2;        // Corriente fase 2 (A)
  float corrienteL3;        // Corriente fase 3 (A)
  float factorPotenciaL1;   // Factor de potencia fase 1 (0-1)
  float factorPotenciaL2;   // Factor de potencia fase 2 (0-1)
  float factorPotenciaL3;   // Factor de potencia fase 3 (0-1)
  
  // Variables de energía y potencia
  float energiaAcumulada;   // Energía acumulada (kWh)
  float potenciaActiva;     // Potencia activa total (kW)
  float frecuencia;         // Frecuencia de red (Hz)
  
  // Estado de alarmas
  bool alarmaActivada;      // Flag de sobrecorriente
};

// ============================================================================
// ESTRUCTURA PARA TRANSPORTAR DATOS ENTRE TAREAS
// ============================================================================
struct DatosTelemetria {
  MedidorElectrico medidor;   // Copia completa del medidor
  uint32_t marcaTiempo;       // Timestamp en milisegundos
  bool esAlarma;              // Flag para identificar si es alarma
};

// ============================================================================
// CONFIGURACIÓN DE LOS 4 MEDIDORES
// ============================================================================
MedidorElectrico medidorFundicion;
MedidorElectrico medidorLaminacion;
MedidorElectrico medidorCorte;
MedidorElectrico medidorEnsamble;

MedidorElectrico* medidores[] = {
  &medidorFundicion,
  &medidorLaminacion,
  &medidorCorte,
  &medidorEnsamble
};
const int NUM_MEDIDORES = 4;

struct ConfiguracionArea {
  String nombre;
  float voltajeNominal;
  float corrienteMaxima;
  float factorPotenciaBase;
  float umbralAlarma;
  float cargaBase;
  float energiaInicial;
};

ConfiguracionArea configuraciones[] = {
  {"Fundicion", 380.0, 1000.0, 0.82, 900.0, 85.0, 12345.67},
  {"Laminacion", 380.0, 800.0, 0.88, 720.0, 75.0, 8920.45},
  {"Corte", 380.0, 600.0, 0.90, 540.0, 65.0, 5432.10},
  {"Ensamble", 380.0, 400.0, 0.92, 360.0, 55.0, 3210.88}
};

float tiempoSimulacion = 0;

// ============================================================================
// HANDLES DE FreeRTOS
// ============================================================================
// COLAS (Queues) - Para comunicación entre tareas
QueueHandle_t colaTelemetria;     // Cola para telemetría normal
QueueHandle_t colaAlarmas;        // Cola prioritaria para alarmas

// MUTEXES (Semáforos binarios) - Para proteger recursos compartidos
SemaphoreHandle_t mutexMQTT;      // Protege el cliente MQTT
SemaphoreHandle_t mutexSerial;    // Protege la salida Serial
SemaphoreHandle_t mutexMedidores; // Protege array de medidores

// EVENT GROUPS - Para sincronización de estados
EventGroupHandle_t grupoEventosWiFi; // Bits de estado WiFi/MQTT

// TASK HANDLES - Referencias a las tareas (para suspender/reanudar)
TaskHandle_t manejadorTareaAdquisicion;
TaskHandle_t manejadorTareaMQTT;
TaskHandle_t manejadorTareaAlarma;
TaskHandle_t manejadorTareaWiFi;
TaskHandle_t manejadorTareaSerial;


// FUNCIONES AUXILIARES DE SIMULACIÓN


/**
 * @brief Agrega ruido aleatorio a un valor para hacerlo realista
 * @param valor Valor base a modificar
 * @param porcentaje Porcentaje de variación (ej: 2.0 = ±2%)
 * @return float Valor con ruido aplicado
 */
float agregarRuido(float valor, float porcentaje) {
  float ruido = random(-100, 100) / 100.0 * porcentaje;
  return valor * (1.0 + ruido / 100.0);
}

/**
 * @brief Genera variación sinusoidal para simular cargas cíclicas
 * @param amplitud Amplitud de la variación
 * @return float Valor sinusoidal actual
 */
float variacionSinusoidal(float amplitud) {
  return amplitud * sin(2 * PI * tiempoSimulacion / 60.0);
}

/**
 * @brief Simula eventos aleatorios de sobrecorriente (2% probabilidad)
 * @return true Si debe ocurrir un evento de sobrecorriente
 */
bool debeOcurrirSobrecorriente() {
  return random(100) < 2;
}

/**
 * @brief Actualiza los valores de un medidor con datos simulados
 * @param medidor Puntero al medidor a actualizar
 * @param config Configuración específica del área
 * 
 */
void actualizarDatosMedidor(MedidorElectrico* medidor, ConfiguracionArea config) {
  // Calcular carga actual con variación sinusoidal
  float variacionCarga = variacionSinusoidal(15.0);
  float cargaActual = config.cargaBase + variacionCarga;
  
  // Simular picos esporádicos de demanda
  if (debeOcurrirSobrecorriente()) {
    cargaActual = 95.0;
  }
  
  // Generar voltajes estables (±2% variación)
  medidor->voltajeL1N = agregarRuido(config.voltajeNominal, 2.0);
  medidor->voltajeL2N = agregarRuido(config.voltajeNominal, 2.0);
  medidor->voltajeL3N = agregarRuido(config.voltajeNominal, 2.0);
  
  // Generar corrientes proporcionales a la carga
  float corrienteBase = config.corrienteMaxima * (cargaActual / 100.0);
  medidor->corrienteL1 = agregarRuido(corrienteBase * 1.02, 3.0);
  medidor->corrienteL2 = agregarRuido(corrienteBase * 0.98, 3.0);
  medidor->corrienteL3 = agregarRuido(corrienteBase * 1.00, 3.0);
  
  // Calcular factor de potencia (mejora con menor carga)
  float ajusteFP = (100.0 - cargaActual) * 0.001;
  medidor->factorPotenciaL1 = constrain(config.factorPotenciaBase + ajusteFP + random(-5, 5) / 1000.0, 0.7, 0.99);
  medidor->factorPotenciaL2 = constrain(config.factorPotenciaBase + ajusteFP + random(-5, 5) / 1000.0, 0.7, 0.99);
  medidor->factorPotenciaL3 = constrain(config.factorPotenciaBase + ajusteFP + random(-5, 5) / 1000.0, 0.7, 0.99);
  
  // Calcular potencia activa total (suma de las 3 fases)
  float potenciaL1 = (medidor->voltajeL1N * medidor->corrienteL1 * medidor->factorPotenciaL1) / 1000.0;
  float potenciaL2 = (medidor->voltajeL2N * medidor->corrienteL2 * medidor->factorPotenciaL2) / 1000.0;
  float potenciaL3 = (medidor->voltajeL3N * medidor->corrienteL3 * medidor->factorPotenciaL3) / 1000.0;
  medidor->potenciaActiva = potenciaL1 + potenciaL2 + potenciaL3;
  
  // Generar frecuencia estable (50Hz ±0.1Hz)
  medidor->frecuencia = 50.0 + random(-10, 10) / 100.0;
  
  // Acumular energía (Potencia × Tiempo en horas)
  medidor->energiaAcumulada += medidor->potenciaActiva * (5.0 / 3600.0);
  
  // Verificar si hay sobrecorriente
  float corrienteMaxima = max(medidor->corrienteL1, max(medidor->corrienteL2, medidor->corrienteL3));
  medidor->alarmaActivada = (corrienteMaxima > config.umbralAlarma);
}


// FUNCIONES DE AZURE IoT HUB - CONECTIVIDAD


/**
 * @brief Conecta el ESP32 a la red WiFi configurada
 * NOTA: Esta función puede tardar varios segundos
 */
static void conectarAWiFi() {
  if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
    Logger.Info("Conectando a WiFi SSID: " + String(ssid));
    xSemaphoreGive(mutexSerial);
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  vTaskDelay(pdMS_TO_TICKS(100)); // NO BLOQUEANTE 
  WiFi.begin(ssid, password);
  
  // Esperar conexión con timeout de 30 segundos
  int timeout = 60; // 60 iteraciones × 500ms = 30s
  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    vTaskDelay(pdMS_TO_TICKS(500)); // NO BLOQUEANTE 
    Serial.print(".");
    timeout--;
  }
  Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {
    if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
      Logger.Info("WiFi conectado, IP: " + WiFi.localIP().toString());
      xSemaphoreGive(mutexSerial);
    }
    // Activar bit de WiFi conectado
    xEventGroupSetBits(grupoEventosWiFi, BIT_WIFI_CONECTADO);
  } else {
    if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
      Logger.Error("Timeout conectando a WiFi");
      xSemaphoreGive(mutexSerial);
    }
  }
}

/**
 * @brief Sincroniza la hora del sistema usando NTP
 */
static void inicializarHora() {
  if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
    Logger.Info("Sincronizando hora con NTP...");
    xSemaphoreGive(mutexSerial);
  }

  configTime(GMT_OFFSET_SECS, GMT_OFFSET_SECS_DST, NTP_SERVERS);
  time_t now = time(NULL);
  int timeout = 20; // 20 segundos máximo
  
  while (now < UNIX_TIME_NOV_13_2017 && timeout > 0) {
    vTaskDelay(pdMS_TO_TICKS(500)); // NO BLOQUEANTE 
    Serial.print(".");
    now = time(nullptr);
    timeout--;
  }
  Serial.println("");
  
  if (now >= UNIX_TIME_NOV_13_2017) {
    if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
      Logger.Info("Hora sincronizada correctamente");
      xSemaphoreGive(mutexSerial);
    }
    xEventGroupSetBits(grupoEventosWiFi, BIT_HORA_SINCRONIZADA);
  }
}


// MANEJADORES DE EVENTOS MQTT


#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void manejadorEventosMQTT(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  (void)handler_args;
  (void)base;
  (void)event_id;
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
#else
static esp_err_t manejadorEventosMQTT(esp_mqtt_event_handle_t event) {
#endif

  switch (event->event_id) {
    case MQTT_EVENT_ERROR:
      Logger.Info("MQTT ERROR");
      xEventGroupClearBits(grupoEventosWiFi, BIT_MQTT_CONECTADO);
      break;
      
    case MQTT_EVENT_CONNECTED:
      Logger.Info("MQTT CONECTADO");
      xEventGroupSetBits(grupoEventosWiFi, BIT_MQTT_CONECTADO);
      
      // Suscribirse a mensajes cloud-to-device
      esp_mqtt_client_subscribe(clienteMQTT, AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC, 1);
      esp_mqtt_client_subscribe(clienteMQTT, "$iothub/methods/POST/#", 1);
      break;
      
    case MQTT_EVENT_DISCONNECTED:
      Logger.Info("MQTT DESCONECTADO");
      xEventGroupClearBits(grupoEventosWiFi, BIT_MQTT_CONECTADO);
      break;
      
    case MQTT_EVENT_DATA:
      {
        char topico[TAMANIO_BUFFER_DATOS_ENTRANTES];
        char cargaUtil[TAMANIO_BUFFER_DATOS_ENTRANTES];
        
        int longitudTopico = min(event->topic_len, TAMANIO_BUFFER_DATOS_ENTRANTES - 1);
        strncpy(topico, event->topic, longitudTopico);
        topico[longitudTopico] = '\0';
        
        int longitudCargaUtil = min(event->data_len, TAMANIO_BUFFER_DATOS_ENTRANTES - 1);
        strncpy(cargaUtil, event->data, longitudCargaUtil);
        cargaUtil[longitudCargaUtil] = '\0';
        
        strncpy(datosEntrantes, topico, TAMANIO_BUFFER_DATOS_ENTRANTES);
        
        // Manejar métodos directos si es necesario
        if (strstr(topico, "$iothub/methods/POST/") != NULL) {
          char nombreMetodo[64];
          sscanf(topico, "$iothub/methods/POST/%[^/]", nombreMetodo);
          // directMethodCallback(nombreMetodo, cargaUtil, longitudCargaUtil);
        }
      }
      break;
      
    default:
      break;
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#else
  return ESP_OK;
#endif
}

/**
 * @brief Inicializa el cliente de Azure IoT Hub
 */
static void inicializarClienteIoTHub() {
  az_iot_hub_client_options opciones = az_iot_hub_client_options_default();
  opciones.user_agent = AZ_SPAN_FROM_STR(AZURE_SDK_CLIENT_USER_AGENT);

  if (az_result_failed(az_iot_hub_client_init(
          &clienteAzure,
          az_span_create((uint8_t*)host, strlen(host)),
          az_span_create((uint8_t*)device_id, strlen(device_id)),
          &opciones))) {
    Logger.Error("Error inicializando Azure IoT Hub client");
    return;
  }

  size_t longitudIdCliente;
  az_iot_hub_client_get_client_id(&clienteAzure, idClienteMQTT, sizeof(idClienteMQTT) - 1, &longitudIdCliente);
  az_iot_hub_client_get_user_name(&clienteAzure, nombreUsuarioMQTT, sizeofarray(nombreUsuarioMQTT), NULL);

  Logger.Info("Cliente IoT Hub inicializado");
}

/**
 * @brief Inicializa el cliente MQTT
 * @return int 0 si exitoso, 1 si falla
 */
static int inicializarClienteMQTT() {
#ifndef IOT_CONFIG_USE_X509_CERT
  if (tokenSAS.Generate(SAS_TOKEN_DURATION_IN_MINUTES) != 0) {
    Logger.Error("Error generando SAS token");
    return 1;
  }
#endif

  esp_mqtt_client_config_t configuracionMQTT;
  memset(&configuracionMQTT, 0, sizeof(configuracionMQTT));

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  configuracionMQTT.broker.address.uri = mqtt_broker_uri;
  configuracionMQTT.broker.address.port = mqtt_port;
  configuracionMQTT.credentials.client_id = idClienteMQTT;
  configuracionMQTT.credentials.username = nombreUsuarioMQTT;
  #ifdef IOT_CONFIG_USE_X509_CERT
    configuracionMQTT.credentials.authentication.certificate = IOT_CONFIG_DEVICE_CERT;
    configuracionMQTT.credentials.authentication.certificate_len = (size_t)sizeof(IOT_CONFIG_DEVICE_CERT);
    configuracionMQTT.credentials.authentication.key = IOT_CONFIG_DEVICE_CERT_PRIVATE_KEY;
    configuracionMQTT.credentials.authentication.key_len = (size_t)sizeof(IOT_CONFIG_DEVICE_CERT_PRIVATE_KEY);
  #else
    configuracionMQTT.credentials.authentication.password = (const char*)az_span_ptr(tokenSAS.Get());
  #endif
  configuracionMQTT.session.keepalive = 30;
  configuracionMQTT.session.disable_clean_session = 0;
  configuracionMQTT.network.disable_auto_reconnect = false;
  configuracionMQTT.broker.verification.certificate = (const char*)ca_pem;
  configuracionMQTT.broker.verification.certificate_len = (size_t)ca_pem_len;
#else
  configuracionMQTT.uri = mqtt_broker_uri;
  configuracionMQTT.port = mqtt_port;
  configuracionMQTT.client_id = idClienteMQTT;
  configuracionMQTT.username = nombreUsuarioMQTT;
  #ifdef IOT_CONFIG_USE_X509_CERT
    configuracionMQTT.client_cert_pem = IOT_CONFIG_DEVICE_CERT;
    configuracionMQTT.client_key_pem = IOT_CONFIG_DEVICE_CERT_PRIVATE_KEY;
  #else
    configuracionMQTT.password = (const char*)az_span_ptr(tokenSAS.Get());
  #endif
  configuracionMQTT.keepalive = 30;
  configuracionMQTT.disable_clean_session = 0;
  configuracionMQTT.disable_auto_reconnect = false;
  configuracionMQTT.event_handle = manejadorEventosMQTT;
  configuracionMQTT.user_context = NULL;
  configuracionMQTT.cert_pem = (const char*)ca_pem;
#endif

  clienteMQTT = esp_mqtt_client_init(&configuracionMQTT);
  if (clienteMQTT == NULL) {
    Logger.Error("Error creando cliente MQTT");
    return 1;
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_mqtt_client_register_event(clienteMQTT, MQTT_EVENT_ANY, manejadorEventosMQTT, NULL);
#endif

  esp_err_t resultadoInicio = esp_mqtt_client_start(clienteMQTT);
  if (resultadoInicio != ESP_OK) {
    Logger.Error("Error iniciando cliente MQTT");
    return 1;
  }

  Logger.Info("Cliente MQTT iniciado");
  return 0;
}

// ============================================================================
// FUNCIONES DE ENVÍO DE DATOS A AZURE
// ============================================================================

/**
 * @brief Genera el payload JSON de telemetría
 * @param medidor Puntero al medidor cuyos datos se enviarán
 * @return String JSON formateado con los datos del medidor
 */
String generarCargaUtilTelemetria(MedidorElectrico* medidor) {
  String cargaUtil = "{ \"messageType\": \"telemetry\""
                   ", \"msgCount\": " + String(contadorEnviosTelemetria++) +
                   ", \"deviceId\": \"" + medidor->nombreArea + "\""
                   ", \"timestamp\": " + String(millis()) +
                   ", \"voltageL1N\": " + String(medidor->voltajeL1N, 2) +
                   ", \"voltageL2N\": " + String(medidor->voltajeL2N, 2) +
                   ", \"voltageL3N\": " + String(medidor->voltajeL3N, 2) +
                   ", \"currentL1\": " + String(medidor->corrienteL1, 2) +
                   ", \"currentL2\": " + String(medidor->corrienteL2, 2) +
                   ", \"currentL3\": " + String(medidor->corrienteL3, 2) +
                   ", \"powerFactorL1\": " + String(medidor->factorPotenciaL1, 3) +
                   ", \"powerFactorL2\": " + String(medidor->factorPotenciaL2, 3) +
                   ", \"powerFactorL3\": " + String(medidor->factorPotenciaL3, 3) +
                   ", \"powerTotal\": " + String(medidor->potenciaActiva, 2) +
                   ", \"energyAccumulated\": " + String(medidor->energiaAcumulada, 2) +
                   ", \"frequency\": " + String(medidor->frecuencia, 2) +
                   " }";
  return cargaUtil;
}

/**
 * @brief Genera el payload JSON de un evento de alarma
 * @param medidor Puntero al medidor que generó la alarma
 * @return String JSON formateado con datos del evento
 */
String generarCargaUtilAlarma(MedidorElectrico* medidor) {
  float corrienteMax = max(medidor->corrienteL1, max(medidor->corrienteL2, medidor->corrienteL3));
  
  String faseAlarma = "L1";
  if (medidor->corrienteL2 == corrienteMax) faseAlarma = "L2";
  else if (medidor->corrienteL3 == corrienteMax) faseAlarma = "L3";

  String cargaUtil = "{ \"messageType\": \"event\""
                   ", \"eventType\": \"overcurrent\""
                   ", \"deviceId\": \"" + medidor->nombreArea + "\""
                   ", \"timestamp\": " + String(millis()) +
                   ", \"severity\": \"high\""
                   ", \"description\": \"Sobrecorriente en fase " + faseAlarma + "\""
                   ", \"phase\": \"" + faseAlarma + "\""
                   ", \"currentValue\": " + String(corrienteMax, 2) +
                   ", \"threshold\": " + String(configuraciones[medidor->id - 1].umbralAlarma, 2) +
                   " }";
  return cargaUtil;
}

/**
 * @brief Envía un mensaje MQTT a Azure IoT Hub
 * @param cargaUtil String JSON a enviar
 * @return bool true si el envío fue exitoso
 * 
 * IMPORTANTE: Esta función DEBE ser llamada con mutexMQTT tomado
 */
bool enviarMensajeMQTT(String cargaUtil) {
  // Obtener el topic de telemetría
  if (az_result_failed(az_iot_hub_client_telemetry_get_publish_topic(
          &clienteAzure, NULL, topicoTelemetria, sizeof(topicoTelemetria), NULL))) {
    return false;
  }

  // Publicar el mensaje
  int resultado = esp_mqtt_client_publish(
      clienteMQTT,
      topicoTelemetria,
      cargaUtil.c_str(),
      cargaUtil.length(),
      MQTT_QOS1,
      DO_NOT_RETAIN_MSG);
  
  return (resultado != 0);
}


// TAREA 1: ADQUISICIÓN DE DATOS (CORE 1 - PRIORIDAD ALTA)

/**
 * @brief Tarea encargada de leer los 4 medidores cada 5 segundos
 */
void tareaAdquisicionDatos(void *parametros) {
  const TickType_t frecuencia = pdMS_TO_TICKS(5000); // 5 segundos
  TickType_t tiempoUltimaEjecucion = xTaskGetTickCount();
  
  Logger.Info("✓ Tarea Adquisición iniciada");
  
  for(;;) {
    // Proteger acceso a los medidores con mutex
    if (xSemaphoreTake(mutexMedidores, pdMS_TO_TICKS(500)) == pdTRUE) {
      
      // Procesar cada medidor
      for (int i = 0; i < NUM_MEDIDORES; i++) {
        // Actualizar datos del medidor
        actualizarDatosMedidor(medidores[i], configuraciones[i]);
        
        // Preparar estructura de datos para enviar
        DatosTelemetria datos;
        datos.medidor = *medidores[i];  // Copia completa
        datos.marcaTiempo = millis();
        datos.esAlarma = medidores[i]->alarmaActivada;
        
        // Enviar a cola de telemetría (NO BLOQUEANTE)
        // Si la cola está llena, el dato se descarta
        if (xQueueSend(colaTelemetria, &datos, 0) != pdPASS) {
          if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
            Serial.println("⚠️ Cola telemetría llena - dato descartado");
            xSemaphoreGive(mutexSerial);
          }
        }
        
        // Si hay alarma, enviar a cola prioritaria
        if (datos.esAlarma) {
          if (xQueueSend(colaAlarmas, &datos, 0) != pdPASS) {
            if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
              Serial.println("⚠️ Cola alarmas llena - alarma descartada");
              xSemaphoreGive(mutexSerial);
            }
          }
          // Activar bit de alarma en event group
          xEventGroupSetBits(grupoEventosWiFi, BIT_ALARMA_ACTIVA);
        }
      }
      
      // Liberar mutex de medidores
      xSemaphoreGive(mutexMedidores);
      
      // Incrementar tiempo de simulación
      tiempoSimulacion += 5.0;
    }
    
    // Esperar hasta el próximo ciclo (TIMING PRECISO)
    // vTaskDelayUntil garantiza intervalos exactos de 5 segundos
    vTaskDelayUntil(&tiempoUltimaEjecucion, frecuencia);
  }
}


// TAREA 2: ENVÍO DE TELEMETRÍA (CORE 0 - PRIORIDAD ALTA)

/**
 * @brief Tarea encargada de enviar telemetría normal a Azure IoT Hub
 */
void tareaEnviadorMQTT(void *parametros) {
  DatosTelemetria datos;
  
  Logger.Info("✓ Tarea Enviador MQTT iniciada");
  
  for(;;) {
    // Esperar datos de la cola (BLOQUEANTE en cola, NO en CPU)
    // portMAX_DELAY = esperar indefinidamente
    if (xQueueReceive(colaTelemetria, &datos, portMAX_DELAY) == pdPASS) {
      
      // Verificar que MQTT esté conectado
      EventBits_t bits = xEventGroupGetBits(grupoEventosWiFi);
      if ((bits & BIT_MQTT_CONECTADO) == 0) {
        if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
          Serial.println("⚠️ MQTT desconectado - telemetría descartada");
          xSemaphoreGive(mutexSerial);
        }
        continue;
      }
      
      // Proteger cliente MQTT con mutex (timeout 1 segundo)
      if (xSemaphoreTake(mutexMQTT, pdMS_TO_TICKS(1000)) == pdTRUE) {
        
        // Generar payload y enviar
        String cargaUtil = generarCargaUtilTelemetria(&datos.medidor);
        bool enviado = enviarMensajeMQTT(cargaUtil);
        
        if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
          if (enviado) {
            Logger.Info("📤 Telemetría enviada: " + datos.medidor.nombreArea);
          } else {
            Logger.Error("❌ Error enviando telemetría");
          }
          xSemaphoreGive(mutexSerial);
        }
        
        // Liberar mutex MQTT
        xSemaphoreGive(mutexMQTT);
        
      } else {
        if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
          Serial.println("⏱️ Timeout esperando mutex MQTT");
          xSemaphoreGive(mutexSerial);
        }
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}


// TAREA 3: MANEJO DE ALARMAS (CORE 0 - PRIORIDAD MUY ALTA)

/**
 * @brief Tarea encargada de enviar eventos de alarma con prioridad
 */
void tareaManejadorAlarmas(void *parametros) {
  DatosTelemetria datos;
  
  Logger.Info("✓ Tarea Manejador de Alarmas iniciada");
  
  for(;;) {
    // Esperar alarmas con timeout corto (100ms)
    // Esto permite verificar periódicamente si hay alarmas pendientes
    if (xQueueReceive(colaAlarmas, &datos, pdMS_TO_TICKS(100)) == pdPASS) {
      
      if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
        Serial.println("🚨 ALARMA DETECTADA - Envío prioritario: " + datos.medidor.nombreArea);
        xSemaphoreGive(mutexSerial);
      }
      
      // Verificar conexión MQTT
      EventBits_t bits = xEventGroupGetBits(grupoEventosWiFi);
      if ((bits & BIT_MQTT_CONECTADO) == 0) {
        if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
          Serial.println("⚠️ No se puede enviar alarma - MQTT desconectado");
          xSemaphoreGive(mutexSerial);
        }
        continue;
      }
      
      // Proteger cliente MQTT (timeout largo para alarmas)
      if (xSemaphoreTake(mutexMQTT, pdMS_TO_TICKS(2000)) == pdTRUE) {
        
        // Generar payload de alarma y enviar
        String cargaUtil = generarCargaUtilAlarma(&datos.medidor);
        bool enviado = enviarMensajeMQTT(cargaUtil);
        
        if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
          if (enviado) {
            Logger.Info("✅ ALARMA ENVIADA exitosamente");
          } else {
            Logger.Error("❌ Error enviando alarma");
          }
          xSemaphoreGive(mutexSerial);
        }
        
        xSemaphoreGive(mutexMQTT);
        
      } else {
        if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(50)) == pdTRUE) {
          Serial.println("⚠️ Timeout crítico - No se pudo enviar alarma");
          xSemaphoreGive(mutexSerial);
        }
      }
      
    } else {
      // No hay alarmas pendientes, limpiar bit
      xEventGroupClearBits(grupoEventosWiFi, BIT_ALARMA_ACTIVA);
    }
  }
}


// TAREA 4: MONITOR DE CONECTIVIDAD (CORE 0 - PRIORIDAD MEDIA)

/**
 * @brief Tarea encargada de monitorear WiFi y MQTT
 */
void tareaMonitorWiFi(void *parametros) {
  const TickType_t frecuencia = pdMS_TO_TICKS(10000); // 10 segundos
  TickType_t tiempoUltimaEjecucion = xTaskGetTickCount();
  uint32_t ultimaRenovacionToken = 0;
  
  Logger.Info("✓ Tarea Monitor WiFi iniciada");
  
  for(;;) {
    // Verificar estado de WiFi
    if (WiFi.status() != WL_CONNECTED) {
      if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.println("📡 WiFi desconectado - Intentando reconectar...");
        xSemaphoreGive(mutexSerial);
      }
      
      // Limpiar bits de estado
      xEventGroupClearBits(grupoEventosWiFi, BIT_WIFI_CONECTADO | BIT_MQTT_CONECTADO);
      
      // Intentar reconexión (esta función ya usa vTaskDelay internamente)
      conectarAWiFi();
      
      // Si WiFi se reconectó, intentar reconectar MQTT
      if (WiFi.status() == WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(2000)); // Esperar 2 segundos
        
        if (xSemaphoreTake(mutexMQTT, pdMS_TO_TICKS(1000)) == pdTRUE) {
          esp_err_t resultado = esp_mqtt_client_reconnect(clienteMQTT);
          xSemaphoreGive(mutexMQTT);
          
          if (resultado == ESP_OK) {
            if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
              Logger.Info("✓ MQTT reconectado");
              xSemaphoreGive(mutexSerial);
            }
          }
        }
      }
    }
    
    // Verificar si es necesario renovar el SAS Token
    // Renovar cada 55 minutos (antes de que expire a los 60)
    if (millis() - ultimaRenovacionToken > 55UL * 60 * 1000) {
      if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.println("🔄 Renovando SAS Token...");
        xSemaphoreGive(mutexSerial);
      }
      
      #ifndef IOT_CONFIG_USE_X509_CERT
      if (tokenSAS.Generate(SAS_TOKEN_DURATION_IN_MINUTES) == 0) {
        ultimaRenovacionToken = millis();
        if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
          Logger.Info("✓ SAS Token renovado");
          xSemaphoreGive(mutexSerial);
        }
      }
      #endif
    }
    
    // Esperar hasta el próximo ciclo
    vTaskDelayUntil(&tiempoUltimaEjecucion, frecuencia);
  }
}


// TAREA 5: LOGGER SERIAL (CORE 1 - PRIORIDAD BAJA)

/**
 * @brief Tarea encargada de mostrar datos en el monitor seria
 */
void tareaRegistradorSerial(void *parametros) {
  const TickType_t frecuencia = pdMS_TO_TICKS(5000); // 5 segundos
  TickType_t tiempoUltimaEjecucion = xTaskGetTickCount();
  
  Logger.Info("✓ Tarea Registrador Serial iniciada");
  
  for(;;) {
    // Proteger Serial con mutex
    if (xSemaphoreTake(mutexSerial, pdMS_TO_TICKS(500)) == pdTRUE) {
      Serial.println("║          ESTADO SISTEMA - 4 MEDIDORES                        ║");
      
      // Mostrar estado de conectividad
      EventBits_t bits = xEventGroupGetBits(grupoEventosWiFi);
      Serial.print("📶 WiFi: ");
      Serial.print((bits & BIT_WIFI_CONECTADO) ? "✓ CONECTADO" : "✗ DESCONECTADO");
      Serial.print("  |  📡 MQTT: ");
      Serial.print((bits & BIT_MQTT_CONECTADO) ? "✓ CONECTADO" : "✗ DESCONECTADO");
      Serial.print("  |  🚨 Alarmas: ");
      Serial.println((bits & BIT_ALARMA_ACTIVA) ? "ACTIVAS" : "✓ NORMAL");
      
      // Proteger acceso a medidores
      if (xSemaphoreTake(mutexMedidores, pdMS_TO_TICKS(200)) == pdTRUE) {
        
        // Mostrar resumen de cada medidor
        for (int i = 0; i < NUM_MEDIDORES; i++) {
          Serial.println("\n────────────────────────────────────────────────────────────────");
          Serial.print("⚡ Medidor "); Serial.print(i+1); Serial.print(": ");
          Serial.print(medidores[i]->nombreArea);
          Serial.print("  |  Potencia: ");
          Serial.print(medidores[i]->potenciaActiva, 1);
          Serial.print(" kW  |  Estado: ");
          Serial.println(medidores[i]->alarmaActivada ? "⚠️ ALARMA" : "✓ Normal");
        }
        
        xSemaphoreGive(mutexMedidores);
      }
      
      Serial.println("────────────────────────────────────────────────────────────────");
      Serial.print("⏱️ Tiempo simulación: ");
      Serial.print(tiempoSimulacion, 0);
      Serial.print("s  |  ⏰ Uptime: ");
      Serial.print(millis() / 1000);
      Serial.println("s\n");
      
      xSemaphoreGive(mutexSerial);
    }
    
    // Esperar hasta el próximo ciclo
    vTaskDelayUntil(&tiempoUltimaEjecucion, frecuencia);
  }
}

// SETUP - INICIALIZACIÓN DEL SISTEMA

void setup() {
  // Iniciar comunicación serial
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("║   SISTEMA DE MONITOREO INDUSTRIAL - 4 MEDIDORES PAC3200       ║");

  // ========== PASO 1: CREAR OBJETOS DE SINCRONIZACIÓN ==========
  Serial.println("🔧 Creando objetos FreeRTOS...");
  
  // Crear colas (queues)
  colaTelemetria = xQueueCreate(TAMANIO_COLA_TELEMETRIA, sizeof(DatosTelemetria));
  colaAlarmas = xQueueCreate(TAMANIO_COLA_ALARMAS, sizeof(DatosTelemetria));
  
  if (colaTelemetria == NULL || colaAlarmas == NULL) {
    Serial.println("❌ ERROR: No se pudieron crear las colas");
    while(1) { delay(1000); } // Detener sistema
  }
  Serial.println("  ✓ Colas creadas");
  
  // Crear mutexes
  mutexMQTT = xSemaphoreCreateMutex();
  mutexSerial = xSemaphoreCreateMutex();
  mutexMedidores = xSemaphoreCreateMutex();
  
  if (mutexMQTT == NULL || mutexSerial == NULL || mutexMedidores == NULL) {
    Serial.println("❌ ERROR: No se pudieron crear los mutexes");
    while(1) { delay(1000); }
  }
  Serial.println("  ✓ Mutexes creados");
  
  // Crear event group
  grupoEventosWiFi = xEventGroupCreate();
  if (grupoEventosWiFi == NULL) {
    Serial.println("❌ ERROR: No se pudo crear el event group");
    while(1) { delay(1000); }
  }
  Serial.println("  ✓ Event group creado");
  
  // ========== PASO 2: INICIALIZAR MEDIDORES ==========
  Serial.println("\n⚡ Inicializando medidores...");
  for (int i = 0; i < NUM_MEDIDORES; i++) {
    medidores[i]->nombreArea = configuraciones[i].nombre;
    medidores[i]->id = i + 1;
    medidores[i]->energiaAcumulada = configuraciones[i].energiaInicial;
    medidores[i]->alarmaActivada = false;
    
    Serial.print("  ✓ Medidor ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(configuraciones[i].nombre);
  }
  randomSeed(analogRead(0));
  
  // ========== PASO 3: ESTABLECER CONEXIÓN A AZURE ==========
  Serial.println("\n☁️ Estableciendo conexión a Azure IoT Hub...");
  conectarAWiFi();
  inicializarHora();
  inicializarClienteIoTHub();
  inicializarClienteMQTT();
  
  // Esperar a que MQTT se conecte
  Serial.println("⏳ Esperando conexión MQTT...");
  for (int i = 0; i < 20; i++) {
    EventBits_t bits = xEventGroupWaitBits(
        grupoEventosWiFi,
        BIT_MQTT_CONECTADO,
        pdFALSE,  // No limpiar bits
        pdTRUE,   // Esperar todos los bits
        pdMS_TO_TICKS(500));
    
    if (bits & BIT_MQTT_CONECTADO) {
      Serial.println("✅ MQTT conectado exitosamente");
      break;
    }
    Serial.print(".");
  }
  Serial.println("");
  
  // ========== PASO 4: CREAR TAREAS FreeRTOS ==========
  Serial.println("\n🚀 Creando tareas FreeRTOS...");
  
  // TAREA 1: Adquisición de datos (CORE 1 - Alta prioridad)
  xTaskCreatePinnedToCore(
      tareaAdquisicionDatos,        // Función de la tarea
      "Adquisicion",                // Nombre (para debug)
      TAMANIO_STACK_MEDIANO,        // Tamaño del stack
      NULL,                         // Parámetros
      PRIORIDAD_ADQUISICION,        // Prioridad
      &manejadorTareaAdquisicion,   // Handle
      1);                           // Core 1 (loop de Arduino)
  Serial.println("  ✓ Tarea Adquisición (Core 1)");
  
  // TAREA 2: Envío MQTT (CORE 0 - Alta prioridad)
  xTaskCreatePinnedToCore(
      tareaEnviadorMQTT,
      "Enviador_MQTT",
      TAMANIO_STACK_GRANDE,
      NULL,
      PRIORIDAD_ENVIADOR_MQTT,
      &manejadorTareaMQTT,
      0);                           // Core 0 (WiFi/Bluetooth)
  Serial.println("  ✓ Tarea Enviador MQTT (Core 0)");
  
  // TAREA 3: Manejador de alarmas (CORE 0 - Prioridad MUY alta)
  xTaskCreatePinnedToCore(
      tareaManejadorAlarmas,
      "Manejador_Alarmas",
      TAMANIO_STACK_GRANDE,
      NULL,
      PRIORIDAD_MANEJADOR_ALARMAS,
      &manejadorTareaAlarma,
      0);                           // Core 0
  Serial.println("  ✓ Tarea Manejador de Alarmas (Core 0)");
  
  // TAREA 4: Monitor WiFi (CORE 0 - Prioridad media)
  xTaskCreatePinnedToCore(
      tareaMonitorWiFi,
      "Monitor_WiFi",
      TAMANIO_STACK_GRANDE,
      NULL,
      PRIORIDAD_MONITOR_WIFI,
      &manejadorTareaWiFi,
      0);                           // Core 0
  Serial.println("  ✓ Tarea Monitor WiFi (Core 0)");
  
  // TAREA 5: Logger Serial (CORE 1 - Prioridad baja)
  xTaskCreatePinnedToCore(
      tareaRegistradorSerial,
      "Registrador_Serial",
      TAMANIO_STACK_PEQUENIO,
      NULL,
      PRIORIDAD_REGISTRADOR_SERIAL,
      &manejadorTareaSerial,
      1);                           // Core 1
  Serial.println("  ✓ Tarea Registrador Serial (Core 1)");
  
  Serial.println("║ ✅ SISTEMA INICIADO CORRECTAMENTE ║");
}

void loop() {  
  vTaskDelay(portMAX_DELAY); // Suspender loop indefinidamente
}