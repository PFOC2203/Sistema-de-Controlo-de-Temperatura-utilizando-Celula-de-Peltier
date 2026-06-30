#include <WiFi.h>
#include <WebServer.h>

// ==========================================
// CONFIGURAÇÕES DE REDE
// ==========================================
const char* ssid = "ESP32_Peltier";
const char* password = "senha_secreta";

WebServer server(80);

// ==========================================
// PINOS (A TUA CONFIGURAÇÃO)
// ==========================================
const int PINO_SENSOR = A0;      // D0 -> PT1000
const int PINO_MODO = D2;        // D1 -> Relé
const int PINO_PELTIER = D1;     // D2 -> MOSFET PWM

// ==========================================
// VARIÁVEIS GLOBAIS E LIMITES
// ==========================================
float temperaturaAtual = 0.0; 
float temperaturaAlvo = 20.0; 
String estadoSistema = "A iniciar...";
bool sistemaLigado = false;   
bool emAlerta = false; // Flag para o aviso no site

// Potências de Controlo (0 a 255)

void atualizarHardware();

// ==========================================
// TASK: LEITURA DO SENSOR E SEGURANÇA
// ==========================================
void taskLeituraSensor(void *pvParameters) {
  for(;;) {
    long somaADC = 0;
    for(int i = 0; i < 10; i++) {
      somaADC += analogRead(PINO_SENSOR);
      vTaskDelay(pdMS_TO_TICKS(2)); 
    }
    float mediaADC = somaADC / 10.0;
    float voltagem = (mediaADC / 4095.0) * 3.3;
    
    temperaturaAtual = (voltagem - 0.5515) / 0.0266;

    // SISTEMA DE SEGURANÇA (Corta energia se fugir dos limites)
    if (temperaturaAtual < -20.0 || temperaturaAtual > 100.0) {
      emAlerta = true;
      if (sistemaLigado) {
        sistemaLigado = false;
        Serial.println("EMERGÊNCIA: Limites ultrapassados. Sistema desligado!");
      }
    } else {
      emAlerta = false;
    }

    atualizarHardware();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ==========================================
// FUNÇÃO DE ATUALIZAÇÃO DO HARDWARE
// ==========================================
void atualizarHardware() {
  if (!sistemaLigado) {
    digitalWrite(PINO_PELTIER, 0);  
    digitalWrite(PINO_MODO, LOW);  
    estadoSistema = emAlerta ? "ERRO: LIMITES EXCEDIDOS \xE2\x9A\xA0" : "Desligado \xE2\x9B\x94"; 
  } 
  else {
    float margem = max((float)abs(temperaturaAlvo * 0.05), (float)0.5); 

    if (temperaturaAtual > (temperaturaAlvo + margem)) {
      digitalWrite(PINO_MODO, LOW);             // Arrefecer
      digitalWrite(PINO_PELTIER, HIGH);    // Potência Máxima
      estadoSistema = "A Arrefecer \xE2\x9D\x84"; 
    } 
    else if (temperaturaAtual < (temperaturaAlvo - margem)) {
      digitalWrite(PINO_MODO, HIGH);            // Aquecer
      digitalWrite(PINO_PELTIER, HIGH);    // Potência Máxima
      estadoSistema = "A Aquecer \xF0\x9F\x94\xA5"; 
    } 
    else {
      // ESTABILIZADO: MANTÉM LIGADO (Potência Reduzida)
      // Mantém a direção atual do relé, mas reduz o PWM para segurar a temperatura
      digitalWrite(PINO_MODO, LOW);            // Aquecer
      digitalWrite(PINO_PELTIER, LOW);    // Potência Máxima
      estadoSistema = "Estabilizado (A manter) \xE2\x9C\x85"; 
    }
  }
}

// ==========================================
// HANDLERS WEB (SITE + API)
// ==========================================
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Controlo Peltier</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; margin-top: 20px; background-color: #e9ecef; }";
  html += "h1 { color: #343a40; }";
  html += ".card { background: white; padding: 20px; border-radius: 10px; box-shadow: 0px 4px 8px rgba(0,0,0,0.1); display: inline-block; margin: 10px; min-width: 250px; vertical-align: top; }";
  html += ".valor { font-size: 2.5em; font-weight: bold; color: #007bff; margin-top: 10px; }";
  html += "input { padding: 10px; font-size: 1.2em; width: 120px; text-align: center; border: 1px solid #ced4da; border-radius: 5px; margin-bottom: 10px; }";
  html += "button { padding: 10px 20px; font-size: 1.2em; background-color: #007bff; color: white; border: none; border-radius: 5px; cursor: pointer; width: 100%; transition: 0.3s; }";
  html += "button:hover { opacity: 0.8; }";
  html += "#btnPower { font-weight: bold; }";
  html += "#alerta { display: none; background-color: #dc3545; color: white; padding: 15px; margin: 10px auto; width: 90%; max-width: 500px; font-size: 1.2em; font-weight: bold; border-radius: 8px; box-shadow: 0 4px 8px rgba(220,53,69,0.4); animation: piscar 1.5s infinite; }";
  html += "@keyframes piscar { 0% { opacity: 1; } 50% { opacity: 0.7; } 100% { opacity: 1; } }";
  html += "</style>";
  html += "<script>";
  html += "setInterval(function() {";
  html += "  fetch('/dados').then(response => response.json()).then(dados => {";
  html += "    document.getElementById('t_atual').innerText = dados.atual + ' °C';";
  html += "    document.getElementById('t_alvo').innerText = dados.alvo + ' °C';";
  html += "    document.getElementById('estado').innerText = dados.estado;";
  
  // Lógica do Alerta Vermelho
  html += "    document.getElementById('alerta').style.display = dados.alerta ? 'block' : 'none';";
  
  html += "    var btn = document.getElementById('btnPower');";
  html += "    if(dados.ligado) {";
  html += "      btn.innerText = 'DESLIGAR SISTEMA'; btn.style.backgroundColor = '#dc3545';"; 
  html += "    } else {";
  html += "      btn.innerText = 'LIGAR SISTEMA'; btn.style.backgroundColor = '#28a745';"; 
  html += "    }";
  html += "  });";
  html += "}, 1000);"; 
  html += "function enviarTemperatura() {";
  html += "  var novaTemp = document.getElementById('inputTemp').value;";
  html += "  if(novaTemp !== '') {";
  html += "    if(novaTemp >= -20 && novaTemp <= 100) {"; // Validação no lado do Browser
  html += "      fetch('/definir?valor=' + novaTemp);";
  html += "      document.getElementById('inputTemp').value = '';"; 
  html += "    } else {";
  html += "      alert('Erro: O alvo tem de estar entre -20°C e 100°C!');";
  html += "    }";
  html += "  }";
  html += "}";
  html += "function toggleEnergia() { fetch('/toggle'); }";
  html += "</script></head><body>";
  
  // Banner de Alerta (Oculto por defeito)
  html += "<div id='alerta'>⚠️ ATENÇÃO: LIMITES EXCEDIDOS!<br>(-20°C a 100°C). Sistema parado por segurança.</div>";

  html += "<h1>Termóstato Peltier</h1>";
  html += "<div class='card'><h2>Energia</h2><button id='btnPower' onclick='toggleEnergia()'>A carregar...</button></div><br>";
  html += "<div class='card'><h2>Temp. Atual</h2><div class='valor' id='t_atual'>-- °C</div></div>";
  html += "<div class='card'><h2>Temp. Alvo</h2><div class='valor' id='t_alvo'>-- °C</div></div><br>";
  
  // Adicionados os atributos min e max no input do HTML
  html += "<div class='card'><h2>Ajustar Temperatura</h2><input type='number' id='inputTemp' step='0.1' min='-20' max='100' placeholder='Ex: 22.5'><br><button onclick='enviarTemperatura()'>Definir</button></div>";
  
  html += "<div class='card'><h2>Estado do Módulo</h2><div class='valor' id='estado' style='color: #ff5722; font-size: 1.5em;'>--</div></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleDados() {
  String estadoLigado = sistemaLigado ? "true" : "false";
  String flgAlerta = emAlerta ? "true" : "false";
  String json = "{\"atual\":" + String(temperaturaAtual, 1) + ",\"alvo\":" + String(temperaturaAlvo, 1) + ",\"estado\":\"" + estadoSistema + "\",\"ligado\":" + estadoLigado + ",\"alerta\":" + flgAlerta + "}";
  server.send(200, "application/json", json);
}

void handleDefinir() {
  if (server.hasArg("valor")) {
    float t = server.arg("valor").toFloat();
    // Validação no lado do Servidor ESP32
    if (t >= -20.0 && t <= 100.0) {
      temperaturaAlvo = t;
      Serial.println("Nova temperatura definida: " + String(temperaturaAlvo));
      atualizarHardware(); 
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleToggle() {
  // Impede ligar o sistema se estiver em estado de alerta crítico
  if (!emAlerta) {
    sistemaLigado = !sistemaLigado;
    Serial.println(sistemaLigado ? "Sistema LIGADO" : "Sistema DESLIGADO");
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
  digitalWrite(PINO_PELTIER, 0); 

  xTaskCreatePinnedToCore(taskLeituraSensor, "TaskSensor", 4096, NULL, 1, NULL, 0);

  WiFi.softAP(ssid, password);
  server.on("/", handleRoot);
  server.on("/dados", handleDados);
  server.on("/definir", handleDefinir);
  server.on("/toggle", handleToggle); 
  server.begin();
}

void loop() {
  server.handleClient(); 
  delay(1);              
}