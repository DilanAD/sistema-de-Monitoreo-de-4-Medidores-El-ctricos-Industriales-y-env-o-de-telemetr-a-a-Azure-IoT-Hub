================================================================================
Sistema de Monitoreo de 4 Medidores Eléctricos Industriales y envío de telemetría a Azure IoT Hub
================================================================================

DESCRIPCIÓN DEL PROYECTO
================================================================================

Proyecto de firmware para ESP32 que simula 4 medidores de potencia eléctrica 
y envía telemetría a Azure IoT Hub. El sistema recopila datos como voltajes, 
corrientes, potencia y factor de potencia de cada medidor.

NOTA IMPORTANTE: Debido a problemas con la instalación del ESP-IDF v4.3, se 
optó por utilizar el IDE de Arduino para el desarrollo. Para la conexión con 
Azure IoT Hub se utiliza la librería "Azure SDK for C" de Microsoft.


PRERREQUISITOS
================================================================================

Software Necesario:
-------------------
- Arduino IDE (última versión)
  Descargar de: https://www.arduino.cc/en/software

- Cuenta activa de Microsoft Azure
  Registrarse en: https://azure.microsoft.com/

- PowerShell o Azure CLI instalado


INSTALACIÓN Y CONFIGURACIÓN
================================================================================

1. CONFIGURAR ARDUINO IDE
================================================================================

1.1 Instalar Soporte para ESP32
--------------------------------

Paso 1: Abre Arduino IDE

Paso 4: Ve a "Herramientas" → "Placa" → "Gestor de tarjetas"

Paso 5: Busca "esp32" de Espressif Systems

Paso 6: Instala "esp32 by Espressif Systems" (última versión estable)


1.2 Instalar Librería Azure SDK
--------------------------------

Paso 1: Ve a "Herramientas" → "Administrar Bibliotecas"

Paso 2: Busca "Azure SDK For C"

Paso 3: Instala la librería "Azure SDK For C" de Microsoft Corporation

Paso 4: También instala las dependencias que solicite automáticamente


2. CONFIGURAR AZURE IoT HUB
================================================================================

2.1 Crear Grupo de Recursos
----------------------------

Paso 1: Inicia sesión en Azure Portal
   https://portal.azure.com/

Paso 2: Busca "Grupos de recursos" en la barra de búsqueda

Paso 3: Clic en "+ Crear"

Paso 4: Completa los datos:
   - Nombre del grupo: rg-name-iot
   - Región: Selecciona la más cercana (ejemplo: East US)

Paso 5: Clic en "Revisar y crear" → "Crear"


2.2 Crear IoT Hub
-----------------

Paso 1: En el grupo de recursos creado, clic en "+ Crear"

Paso 2: Busca "IoT Hub" y selecciónalo

Paso 3: Configura los parámetros:
   - Nombre: iot-hub-name
   - Región: La misma del grupo de recursos
   - Plan de tarifa: F1 - Gratis (para pruebas)

Paso 4: Clic en "Revisar y crear" → "Crear"

Paso 5: Espera a que se complete el despliegue (2-5 minutos)


2.3 Crear Dispositivo IoT
--------------------------

Paso 1: Ve al IoT Hub recién creado

Paso 2: En el menú lateral, selecciona "Dispositivos" 
        (bajo "Administración de dispositivos")

Paso 3: Clic en "+ Agregar dispositivo"

Paso 4: Configura:
   - Id. de dispositivo: ESP32-01 
   - Tipo de autenticación: Clave simétrica
   - Deja las demás opciones por defecto

Paso 5: Clic en "Guardar"


2.4 Obtener Credenciales del Dispositivo
-----------------------------------------

Paso 1: Clic en el dispositivo recién creado (ESP32-01)

Paso 2: COPIA Y GUARDA los siguientes datos:
   - Cadena de conexión principal (Primary Connection String)
   
   O bien, anota por separado:
   - Nombre del IoT Hub: iot-hub-enerbit.azure-devices.net
   - Id del dispositivo: ESP32-01
   - Clave primaria: [tu clave generada]


3. CONFIGURAR EL CÓDIGO
================================================================================

3.1 Editar Archivo de Configuración
------------------------------------

Paso 1: Abre el proyecto en Arduino IDE

Paso 2: Localiza el archivo "iot_configs.h"

Paso 3: Edita las siguientes constantes:

// ========================================
// CONFIGURACIÓN Wi-Fi
// ========================================
#define WIFI_SSID "TuRedWiFi"              // Nombre de tu red WiFi
#define WIFI_PASSWORD "TuPasswordWiFi"     // Contraseña de tu WiFi

// ========================================
// CONFIGURACIÓN AZURE IoT HUB
// ========================================
#define IOT_CONFIG_IOTHUB_FQDN "iot-hub-name.azure-devices.net"
#define IOT_CONFIG_DEVICE_ID "ESP32-01"
#define IOT_CONFIG_DEVICE_KEY "tu_clave_primaria_aqui"



4. COMPILAR Y CARGAR EL PROGRAMA
================================================================================

4.1 Seleccionar la Placa
-------------------------

Paso 1: Ve a "Herramientas" → "Placa" → "ESP32 Arduino"

Paso 2: Selecciona tu modelo específico:
   - ESP32 Dev Module (genérico)
   - O el modelo exacto de tu placa


4.2 Configurar Puerto Serial
-----------------------------

Paso 1: Conecta el ESP32 a tu PC mediante USB

Paso 2: Ve a "Herramientas" → "Puerto"

Paso 3: Selecciona el puerto COM donde está conectado tu ESP32
   - Windows: COM3, COM4, etc.
   - Linux/Mac: /dev/ttyUSB0, /dev/cu.usbserial, etc.


4.3 Compilar y Cargar
---------------------

Paso 1: Clic en "✓ Verificar" para compilar el código

Paso 2: Si no hay errores, clic en "→ Subir" para cargar al ESP32

Paso 3: Espera a que termine la carga (puede tardar 1-2 minutos)


4.4 Monitorear la Salida Serial
--------------------------------

Paso 1: Abre el Monitor Serie: "Herramientas" → "Monitor Serie"

Paso 2: Configura la velocidad a 115200 baudios

Paso 3: Deberías ver mensajes como:

══════════════════════════════════════════════
   ESP32 + AZURE IOT HUB
══════════════════════════════════════════════

🔌 Conectando a WiFi 'TuRedWiFi'....
✅ WiFi Conectado! IP: 192.168.1.100

🔄 Conectando a Azure IoT Hub...
✅ CONECTADO A AZURE!

📤 Enviando telemetría...
✅ Mensaje enviado exitosamente


VERIFICAR CONEXIÓN CON AZURE
================================================================================

OPCIÓN 1: Dashboard de Azure Portal
================================================================================

Ver Métricas de Conexión
-------------------------

Paso 1: Ve a tu IoT Hub en Azure Portal

Paso 2: Selecciona "Información general"

Paso 3: Revisa los gráficos:
   - "Device to cloud messages": Debe mostrar mensajes entrantes
   - "Connected devices": Debe mostrar 1 dispositivo conectado


OPCIÓN 2: Azure CLI (Recomendado)
================================================================================

2.1 Instalar Azure CLI
----------------------

Si aún no lo tienes instalado:

Windows: 
   Descarga el instalador desde:
   https://learn.microsoft.com/cli/azure/install-azure-cli-windows

Linux:
   curl -sL https://aka.ms/InstallAzureCLIDeb | sudo bash

Mac:
   brew install azure-cli


2.2 Iniciar Sesión
------------------

Abre PowerShell (Windows) o Terminal (Linux/Mac) y ejecuta:

   az login

Se abrirá tu navegador para autenticarte. Completa el inicio de sesión.


2.3 Monitorear Mensajes en Tiempo Real
---------------------------------------

Una vez autenticado, ejecuta el siguiente comando:

az iot hub monitor-events \
  --hub-name iot-hub-name \
  --device-id ESP32-01 \
  --output json

REEMPLAZA:
- iot-hub-name → Nombre de tu IoT Hub
- ESP32-01 → ID de tu dispositivo


ESTRUCTURA DE LOS MENSAJES
================================================================================

Formato de Telemetría
----------------------

Cada medidor envía un mensaje JSON con la siguiente estructura:

{
  "medidor": "Fundicion",           // Área de proceso
  "voltajeL1": 380.5,                // Voltaje Fase L1 (V)
  "voltajeL2": 379.2,                // Voltaje Fase L2 (V)
  "voltajeL3": 381.0,                // Voltaje Fase L3 (V)
  "corrienteL1": 845.3,              // Corriente L1 (A)
  "corrienteL2": 852.1,              // Corriente L2 (A)
  "corrienteL3": 838.7,              // Corriente L3 (A)
  "energiaAcumulada": 12345.67,      // Energía total (kWh)
  "potenciaTotal": 987.45,           // Potencia total (kW)
  "factorPotenciaL1": 0.82,          // Factor de potencia L1
  "factorPotenciaL2": 0.81,          // Factor de potencia L2
  "factorPotenciaL3": 0.83,          // Factor de potencia L3
  "frecuencia": 50.1,                // Frecuencia de red (Hz)
  "timestamp": "2025-10-11T10:30:00Z"
}


Formato de Eventos (Alarmas)
-----------------------------

Cuando se detecta sobrecorriente:

{
  "tipo": "alarma",
  "descripcion": "Sobrecorriente detectada",
  "medidor": "Fundicion",
  "fase": "L1",
  "corriente": 1850.5,
  "umbral": 1500.0,
  "timestamp": "2025-10-11T10:31:15Z"
}


REFERENCIAS
================================================================================

- Documentación Azure IoT Hub:
  https://learn.microsoft.com/azure/iot-hub/

- Azure SDK for C Arduino:
  https://github.com/Azure/azure-sdk-for-c-arduino

- ESP32 Arduino Core:
  https://docs.espressif.com/projects/arduino-esp32/

- Azure CLI IoT Extension:
  https://learn.microsoft.com/cli/azure/iot


AUTOR:
Dillan Andrey Diaz Bocanegra 
