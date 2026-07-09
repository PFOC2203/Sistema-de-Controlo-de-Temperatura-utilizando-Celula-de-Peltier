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
constexpr float TENSAO_MIN_SENSOR = 0.10f; 
constexpr float TENSAO_MAX_SENSOR = 3.20f; 

constexpr float HISTERESE_MIN = 1.0f;
constexpr float HISTERESE_PERCENT = 0.05f;

const char* ssid = "ESP32_Peltier_IoT";
const char* password = "password123";

WebServer server(80);
Preferences memoriaFlash;

const int PINO_SENSOR = A0;
const int PINO_MODO = D2;
const int PINO_PELTIER = D1;

// ==========================================
// VARIÁVEIS GLOBAIS
// ==========================================
float temperaturaAtual = 20.0f;
float temperaturaAlvo = 20.0f;
bool sistemaLigado = false;
bool emAlerta = false;

SemaphoreHandle_t xMutex;
unsigned long ultimaLeituraMillis = 0;

enum EstadoPeltier { DESLIGADO, AQUECER, ARREFECER };
EstadoPeltier estadoAtual = DESLIGADO;

void atualizarHardware();

// ==========================================
// INTERFACE WEB (HTML & Endpoints)
// ==========================================
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Controlo Térmico - Pedro Cardoso</title>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Arial, sans-serif; text-align: center; margin-top: 30px; background-color: #e9ecef; color: #212529; }";
  html += "h1 { color: #343a40; font-size: 2em; margin-bottom: 20px; }";
  html += ".header-info { font-size: 1.1em; margin-bottom: 40px; line-height: 1.6; }";
  html += ".bold { font-weight: bold; }";
  html += ".card { background: white; padding: 25px 20px; border-radius: 8px; box-shadow: 0px 4px 10px rgba(0,0,0,0.08); box-sizing: border-box; }";
  html += ".container-topo { max-width: 320px; margin: 0 auto 20px auto; }";
  html += ".grelha { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; max-width: 660px; margin: 0 auto; }";
  html += "@media (max-width: 600px) { .grelha { grid-template-columns: 1fr; } }";
  html += "h2 { font-size: 1.3em; margin: 0 0 20px 0; }";
  html += ".valor { font-size: 2.6em; font-weight: bold; color: #007bff; }";
  html += "#estado { color: #333; font-size: 1.6em; margin-bottom: 15px; }";
  html += "input { padding: 8px; font-size: 1.1em; width: 100px; text-align: center; border: 1px solid #ced4da; border-radius: 4px; margin-bottom: 15px; }";
  html += "button { padding: 12px 20px; font-size: 1.1em; font-weight: bold; background-color: #007bff; color: white; border: none; border-radius: 5px; cursor: pointer; width: 100%; transition: 0.2s; }";
  html += "button:hover { opacity: 0.9; }";
  html += ".btn-export { background-color: #6c757d; font-size: 0.9em; margin-top: 10px; }";
  html += "#alerta { display: none; background-color: #dc3545; color: white; padding: 15px; margin: 10px auto 30px auto; width: 90%; max-width: 500px; font-size: 1.2em; font-weight: bold; border-radius: 8px; animation: piscar 1.5s infinite; }";
  html += "@keyframes piscar { 0% { opacity: 1; } 50% { opacity: 0.7; } 100% { opacity: 1; } }";
  html += "</style>";
  html += "<script>";
  html += "setInterval(function() {";
  html += "  fetch('/dados').then(response => response.json()).then(dados => {";
  html += "    document.getElementById('t_atual').innerText = dados.atual + ' °C';";
  html += "    document.getElementById('t_alvo').innerText = dados.alvo + ' °C';";
  html += "    document.getElementById('estado').innerText = dados.estado;";
  html += "    document.getElementById('alerta').style.display = dados.alerta ? 'block' : 'none';";
  html += "    var btn = document.getElementById('btnPower');";
  html += "    if(dados.ligado) { btn.innerText = 'DESLIGAR SISTEMA'; btn.style.backgroundColor = '#dc3545'; }";
  html += "    else { btn.innerText = 'LIGAR SISTEMA'; btn.style.backgroundColor = '#28a745'; }";
  html += "  });";
  html += "}, 1000);";
  html += "function enviarTemperatura() { var novaTemp = document.getElementById('inputTemp').value; if(novaTemp !== '') { fetch('/definir?valor=' + novaTemp); document.getElementById('inputTemp').value = ''; } }";
  html += "function toggleEnergia() { fetch('/toggle'); }";
  html += "function descarregarCSV() { window.location.href = '/csv'; }";
  html += "</script></head><body>";
  html += "<div id='alerta'>⚠️ ATENÇÃO: LIMITES EXCEDIDOS!<br>Sistema parado por segurança.</div>";
  html += "<h1>Sistema de Controlo Térmico</h1>";
  html += "<div class='header-info'><div class='bold'>Pedro Cardoso - 1210717</div><div>LECC - PESTA</div></div>";
  html += "<div class='card container-topo'><h2>Energia</h2><button id='btnPower' onclick='toggleEnergia()'>A carregar...</button></div>";
  html += "<div class='grelha'><div class='card'><h2>Temp. Atual</h2><div class='valor' id='t_atual'>-- °C</div></div>";
  html += "<div class='card'><h2>Temp. Alvo</h2><div class='valor' id='t_alvo'>-- °C</div></div>";
  html += "<div class='card'><h2>Ajustar Temp.</h2><input type='number' id='inputTemp' step='1'><br><button onclick='enviarTemperatura()'>Definir</button></div>";
  html += "<div class='card'><h2>Estado</h2><div class='valor' id='estado'>--</div><button class='btn-export' onclick='descarregarCSV()'>Exportar CSV</button></div>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleDados() {
  xSemaphoreTake(xMutex, portMAX_DELAY);
  const char* estados[] = {"Desligado", "Aquecendo", "Arrefecendo"};
  char jsonBuffer[256];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
           "{\"atual\":%.1f,\"alvo\":%.1f,\"ligado\":%s,\"alerta\":%s,\"estado\":\"%s\"}",
           temperaturaAtual, temperaturaAlvo, sistemaLigado ? "true" : "false", emAlerta ? "true" : "false", estados[estadoAtual]);
  xSemaphoreGive(xMutex);
  server.send(200, "application/json", jsonBuffer);
}

void handleCSV() {
    // Exemplo básico de exportação
    String csv = "Tempo(ms),TempAtual(C),Alvo(C)\n";
    csv += String(millis()) + "," + String(temperaturaAtual) + "," + String(temperaturaAlvo);
    server.send(200, "text/csv", csv);
}

// ==========================================
// LÓGICA DE CONTROLO (Manter igual)
// ==========================================
void atualizarHardware() {
    xSemaphoreTake(xMutex, portMAX_DELAY);
    float tAtual = temperaturaAtual; float tAlvo = temperaturaAlvo; bool alerta = emAlerta; bool ligado = sistemaLigado; EstadoPeltier estAtual = estadoAtual;
    xSemaphoreGive(xMutex);

    if (alerta || !ligado) {
        if (estAtual != DESLIGADO) {
            digitalWrite(PINO_PELTIER, LOW);
            xSemaphoreTake(xMutex, portMAX_DELAY); estadoAtual = DESLIGADO; xSemaphoreGive(xMutex);
        }
        return;
    }

    float histerese = max(HISTERESE_MIN, tAlvo * HISTERESE_PERCENT);
    float erro = tAtual - tAlvo;
    EstadoPeltier novoEstado = estAtual;

    if (erro > histerese) novoEstado = ARREFECER;
    else if (erro < -histerese) novoEstado = AQUECER;
    else if (abs(erro) < (histerese * 0.5f)) novoEstado = DESLIGADO;

    if (novoEstado != estAtual) {
        digitalWrite(PINO_PELTIER, LOW);
        if (estAtual != DESLIGADO && novoEstado != DESLIGADO) vTaskDelay(pdMS_TO_TICKS(TEMPO_MORTO_MS));
        if (novoEstado == ARREFECER) { digitalWrite(PINO_MODO, LOW); digitalWrite(PINO_PELTIER, HIGH); }
        else if (novoEstado == AQUECER) { digitalWrite(PINO_MODO, HIGH); digitalWrite(PINO_PELTIER, HIGH); }
        xSemaphoreTake(xMutex, portMAX_DELAY); estadoAtual = novoEstado; xSemaphoreGive(xMutex);
    }
}

// ... [Manter as restantes funções: taskSensorControlo, handleDefinir, handleToggle, setup e loop como estavam] ...
void handleDefinir() { if (server.hasArg("valor")) { float novoAlvo = server.arg("valor").toFloat(); if (novoAlvo >= -20.0f && novoAlvo <= 100.0f) { xSemaphoreTake(xMutex, portMAX_DELAY); temperaturaAlvo = novoAlvo; xSemaphoreGive(xMutex); memoriaFlash.putFloat("temp_alvo", novoAlvo); atualizarHardware(); } } server.send(200, "text/plain", "OK"); }
void handleToggle() { xSemaphoreTake(xMutex, portMAX_DELAY); bool alerta = emAlerta; xSemaphoreGive(xMutex); if (!alerta) { xSemaphoreTake(xMutex, portMAX_DELAY); sistemaLigado = !sistemaLigado; xSemaphoreGive(xMutex); atualizarHardware(); } server.send(200, "text/plain", "OK"); }

void setup() {
    Serial.begin(115200);
    pinMode(PINO_MODO, OUTPUT); pinMode(PINO_PELTIER, OUTPUT);
    digitalWrite(PINO_MODO, LOW); digitalWrite(PINO_PELTIER, LOW);
    memoriaFlash.begin("peltier", false);
    temperaturaAlvo = memoriaFlash.getFloat("temp_alvo", 20.0f);
    xMutex = xSemaphoreCreateMutex();
    WiFi.setSleep(false); WiFi.softAP(ssid, password);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/dados", HTTP_GET, handleDados);
    server.on("/definir", HTTP_GET, handleDefinir);
    server.on("/toggle", HTTP_GET, handleToggle);
    server.on("/csv", HTTP_GET, handleCSV);
    server.begin();
    xTaskCreate(taskSensorControlo, "SensorControlo", 3072, NULL, 1, NULL);
}

void loop() { server.handleClient(); vTaskDelay(pdMS_TO_TICKS(10)); }