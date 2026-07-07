#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "freertos/semphr.h"

// ==========================================
// CONFIGURAÇÕES E CONSTANTES
// ==========================================
#define NUM_AMOSTRAS_ADC 10
#define DELAY_ENTRE_AMOSTRAS_MS 2
#define TEMPO_MORTO_MS 200     
#define TEMPO_WATCHDOG_SENSOR 5000 

// Calibração do Sensor PT1000
constexpr float OFFSET_SENSOR = 0.5515f;
constexpr float GANHO_SENSOR = 0.266f;
constexpr float TENSAO_MIN_SENSOR = 0.10f; // 100 mV
constexpr float TENSAO_MAX_SENSOR = 3.20f; // 3200 mV

// Parâmetros de Controlo Térmico
constexpr float HISTERESE_MIN = 1.0f;
constexpr float HISTERESE_PERCENT = 0.05f;

// Credenciais de Rede
const char* ssid = "ESP32_Peltier_IoT";
const char* password = ""; 
WebServer server(80);
Preferences memoriaFlash;

// Pinos de Hardware
const int PINO_SENSOR = A0; 
const int PINO_MODO = D2;
const int PINO_PELTIER = D1;

// ==========================================
// VARIÁVEIS GLOBAIS (Protegidas por Mutex)
// ==========================================
float temperaturaAtual = 20.0f; 
float temperaturaAlvo = 20.0f; 
bool sistemaLigado = false;   
bool emAlerta = false; 

SemaphoreHandle_t xMutex;
unsigned long ultimaLeituraMillis = 0;

enum EstadoPeltier {
    DESLIGADO,
    AQUECER,
    ARREFECER
};
EstadoPeltier estadoAtual = DESLIGADO;

void atualizarHardware();

// ==========================================
// LÓGICA DE CONTROLO (Máquina de Estados)
// ==========================================
void atualizarHardware() {
    // 1. Cópia Local Segura (Ocupa o Mutex < 100µs)
    xSemaphoreTake(xMutex, portMAX_DELAY);
    float tAtual = temperaturaAtual;
    float tAlvo = temperaturaAlvo;
    bool alerta = emAlerta;
    bool ligado = sistemaLigado;
    EstadoPeltier estAtual = estadoAtual;
    xSemaphoreGive(xMutex);

    // 2. Segurança Crítica: Paragem Imediata
    if (alerta || !ligado) {
        if (estAtual != DESLIGADO) {
            digitalWrite(PINO_PELTIER, LOW);
            xSemaphoreTake(xMutex, portMAX_DELAY);
            estadoAtual = DESLIGADO;
            xSemaphoreGive(xMutex);
        }
        return;
    }

    // 3. Avaliação da Histerese 
    float histerese = max(HISTERESE_MIN, tAlvo * HISTERESE_PERCENT);
    float erro = tAtual - tAlvo;
    EstadoPeltier novoEstado = estAtual;

    if (erro > histerese) {
        novoEstado = ARREFECER;
    } else if (erro < -histerese) {
        novoEstado = AQUECER;
    } else if (abs(erro) < (histerese * 0.5f)) {
        novoEstado = DESLIGADO; 
    }

    // 4. Execução de Transição de Hardware
    if (novoEstado != estAtual) {
        digitalWrite(PINO_PELTIER, LOW); 
        
        // Dead-time APENAS na inversão de marcha térmica
        if (estAtual != DESLIGADO && novoEstado != DESLIGADO) {
            vTaskDelay(pdMS_TO_TICKS(TEMPO_MORTO_MS));
        }

        if (novoEstado == ARREFECER) {
            digitalWrite(PINO_MODO, LOW); 
            digitalWrite(PINO_PELTIER, HIGH);
        } else if (novoEstado == AQUECER) {
            digitalWrite(PINO_MODO, HIGH); 
            digitalWrite(PINO_PELTIER, HIGH);
        }
        
        xSemaphoreTake(xMutex, portMAX_DELAY);
        estadoAtual = novoEstado; 
        xSemaphoreGive(xMutex);
    }
}

// ==========================================
// TASK: LEITURA DO SENSOR
// ==========================================
void taskSensorControlo(void *pvParameters) {
    for(;;) {
        uint32_t somaMv = 0;
        
        for(int i = 0; i < NUM_AMOSTRAS_ADC; i++) {
            somaMv += analogReadMilliVolts(PINO_SENSOR);
            vTaskDelay(pdMS_TO_TICKS(DELAY_ENTRE_AMOSTRAS_MS)); 
        }
        
        float mediaMv = somaMv / (float)NUM_AMOSTRAS_ADC;
        float tensaoVolts = mediaMv / 1000.0f;
        float tempCalculada = (tensaoVolts - OFFSET_SENSOR) / GANHO_SENSOR;

        xSemaphoreTake(xMutex, portMAX_DELAY);
        if (tensaoVolts < TENSAO_MIN_SENSOR || tensaoVolts > TENSAO_MAX_SENSOR) {
            emAlerta = true;
        } else {
            temperaturaAtual = tempCalculada;
            emAlerta = false;
        }
        ultimaLeituraMillis = millis(); 
        xSemaphoreGive(xMutex);

        atualizarHardware(); 
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}

// ==========================================
// INTERFACE WEB (Endpoints IoT)
// ==========================================
void handleDados() {
    xSemaphoreTake(xMutex, portMAX_DELAY);
    float tA = temperaturaAtual;
    float tAlvo = temperaturaAlvo;
    bool ligado = sistemaLigado;
    bool alerta = emAlerta;
    xSemaphoreGive(xMutex);

    char jsonBuffer[200];
    snprintf(jsonBuffer, sizeof(jsonBuffer), 
             "{\"atual\":%.1f,\"alvo\":%.1f,\"ligado\":%s,\"alerta\":%s}",
             tA, tAlvo, ligado ? "true" : "false", alerta ? "true" : "false");
    server.send(200, "application/json", jsonBuffer);
}

void handleDefinir() {
    if (server.hasArg("valor")) {
        float novoAlvo = server.arg("valor").toFloat();
        if (novoAlvo >= -20.0f && novoAlvo <= 100.0f) {
            xSemaphoreTake(xMutex, portMAX_DELAY);
            temperaturaAlvo = novoAlvo;
            xSemaphoreGive(xMutex);
            
            memoriaFlash.putFloat("temp_alvo", novoAlvo);
            atualizarHardware(); 
        }
    }
    server.send(200, "text/plain", "OK");
}

void handleToggle() {
    xSemaphoreTake(xMutex, portMAX_DELAY);
    bool alerta = emAlerta;
    xSemaphoreGive(xMutex);

    if (!alerta) {
        xSemaphoreTake(xMutex, portMAX_DELAY);
        sistemaLigado = !sistemaLigado;
        xSemaphoreGive(xMutex);
        atualizarHardware(); 
    }
    server.send(200, "text/plain", "OK");
}

// ==========================================
// SETUP & LOOP
// ==========================================
void setup() {
    Serial.begin(115200);
    
    pinMode(PINO_MODO, OUTPUT);
    pinMode(PINO_PELTIER, OUTPUT);
    digitalWrite(PINO_MODO, LOW);
    digitalWrite(PINO_PELTIER, LOW);

    memoriaFlash.begin("peltier", false);
    temperaturaAlvo = memoriaFlash.getFloat("temp_alvo", 20.0f); 

    xMutex = xSemaphoreCreateMutex();
    if(xMutex == NULL) { Serial.println("Erro Mutex"); while(1); }
    
    WiFi.setSleep(false); 
    WiFi.softAP(ssid, password);
    
    Serial.println("\n--- SISTEMA PESTA INICIADO ---");
    Serial.printf("SSID: %s\nPassword: %s\nIP: ", ssid, password);
    Serial.println(WiFi.softAPIP());

    server.on("/dados", HTTP_GET, handleDados);
    server.on("/definir", HTTP_GET, handleDefinir);
    server.on("/toggle", HTTP_GET, handleToggle);
    server.begin();

    BaseType_t taskStatus = xTaskCreate(taskSensorControlo, "SensorControlo", 3072, NULL, 1, NULL);
    if(taskStatus != pdPASS) { Serial.println("Erro na Task"); while(1); }
}

void loop() {
    server.handleClient();
    
    // Watchdog Cão de Guarda
    if (millis() - ultimaLeituraMillis > TEMPO_WATCHDOG_SENSOR) {
        xSemaphoreTake(xMutex, portMAX_DELAY);
        emAlerta = true; 
        EstadoPeltier estAtual = estadoAtual;
        xSemaphoreGive(xMutex);
        
        if (estAtual != DESLIGADO) {
            digitalWrite(PINO_PELTIER, LOW); 
            xSemaphoreTake(xMutex, portMAX_DELAY);
            estadoAtual = DESLIGADO; 
            xSemaphoreGive(xMutex);
        }
    }
    
    vTaskDelay(pdMS_TO_TICKS(500));
}