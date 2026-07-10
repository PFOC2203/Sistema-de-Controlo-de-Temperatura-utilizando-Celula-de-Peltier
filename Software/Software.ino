#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ==========================================
// CONFIGURAÇÕES DE REDE
// ==========================================
const char* ssid = "ESP32_Peltier";
const char* password = "senha_secreta";
WebServer server(80);

// ==========================================
// PINOS (A TUA CONFIGURAÇÃO)
// ==========================================
const int PINO_SENSOR = A0;   // D0 -> PT1000
const int PINO_MODO = D2;     // D1 -> Relé
const int PINO_PELTIER = D1;  // D2 -> MOSFET

// ==========================================
// CONFIGURAÇÃO DA SUPERVISÃO DE SEGURANÇA
// (Relatório 5.1 e Fluxograma 3 - Watchdog)
// ==========================================
const unsigned long WATCHDOG_TIMEOUT_MS = 5000;   // 5s sem leitura válida -> corta energia
const unsigned long WATCHDOG_INTERVALO_MS = 500;  // verificação a cada 500ms (ciclo do loop IoT)

// Limites de tensão "in natura" do ADC que indicam falha no circuito do
// sensor (cabo partido ou curto-circuito), conforme secção 5.1 do relatório
const float TENSAO_MIN_VALIDA = 0.03;  // próximo de 0 mV
const float TENSAO_MAX_VALIDA = 3.27;  // próximo de 3300 mV

// ==========================================
// VARIÁVEIS GLOBAIS E LIMITES
// ==========================================
float temperaturaAtual = 0.0;
float temperaturaAlvo = 20.0;
String estadoSistema = "A iniciar...";
String causaAlerta = "";        // Motivo do alerta atual (mostrado no site)
bool sistemaLigado = false;
bool emAlerta = false;          // Flag para o aviso no site

int categoriaAtual = 0;         // 0 = sem energia | 1 = arrefecer | 2 = aquecer
volatile unsigned long ultimaLeituraValidaMs = 0;  // usada pelo watchdog

SemaphoreHandle_t mutexTemperatura = NULL;  // protege "temperaturaAtual" (Relatório 5.1)
Preferences preferencias;                    // NVS - persistência do setpoint (Fluxograma 3)

void atualizarHardware();
void verificarWatchdogSeguranca();
float obterTemperaturaAtual();
void definirTemperaturaAtual(float novoValor);

// ==========================================
// TASK: LEITURA DO SENSOR E SEGURANÇA
// (Relatório 5.1 - Lógica de Aquisição de Temperatura)
// ==========================================
void taskLeituraSensor(void* pvParameters) {
  for (;;) {
    long somaADC = 0;
    for (int i = 0; i < 30; i++) {
      somaADC += analogReadMilliVolts(PINO_SENSOR);
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    float mediaADC = ((somaADC) / 30.0) / 1000.0;  // Volts

    // Teste ao estado da PT1000: valores perto de 0V ou do máximo do ADC
    // indicam cabo partido ou curto-circuito no condicionamento de sinal
    bool falhaCircuito = (mediaADC < TENSAO_MIN_VALIDA || mediaADC > TENSAO_MAX_VALIDA);

    if (!falhaCircuito) {
      // Dados válidos: atualiza a variável partilhada protegida por mutex
      float leitura = (mediaADC - 0.5515) / 0.0266;
      definirTemperaturaAtual(leitura);
      ultimaLeituraValidaMs = millis();  // "alimenta" o watchdog de 5s
    }

    bool limitesExcedidos = (obterTemperaturaAtual() < -20.0 || obterTemperaturaAtual() > 100.0);

    // SISTEMA DE SEGURANÇA (Corta energia se fugir dos limites ou houver falha no circuito)
    if (falhaCircuito || limitesExcedidos) {
      if (!emAlerta) {
        causaAlerta = falhaCircuito
          ? "Falha no circuito do sensor (cabo partido ou curto-circuito)"
          : "Limites de temperatura excedidos (-20 a 100 C)";
      }
      emAlerta = true;
      if (sistemaLigado) {
        sistemaLigado = false;
        Serial.println("EMERGENCIA: " + causaAlerta + ". Sistema desligado!");
      }
    } else if (causaAlerta != "Watchdog: sensor sem leituras validas ha mais de 5s") {
      // Só limpa o alerta se não tiver sido o watchdog a levantá-lo
      // (esse só é limpo por um reinício de firmware, ver verificarWatchdogSeguranca)
      emAlerta = false;
      causaAlerta = "";
    }

    atualizarHardware();

    // Ciclo de leitura a cada ~1 segundo, conforme descrito na secção 5.1
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ==========================================
// FUNÇÕES AUXILIARES: ACESSO PROTEGIDO POR MUTEX
// (Relatório 5.1 - "alteramos o valor [...] com recurso ao mutex")
// ==========================================
float obterTemperaturaAtual() {
  float valor = temperaturaAtual;
  if (mutexTemperatura != NULL && xSemaphoreTake(mutexTemperatura, pdMS_TO_TICKS(50)) == pdTRUE) {
    valor = temperaturaAtual;
    xSemaphoreGive(mutexTemperatura);
  }
  return valor;
}

void definirTemperaturaAtual(float novoValor) {
  if (mutexTemperatura != NULL && xSemaphoreTake(mutexTemperatura, pdMS_TO_TICKS(50)) == pdTRUE) {
    temperaturaAtual = novoValor;
    xSemaphoreGive(mutexTemperatura);
  } else {
    temperaturaAtual = novoValor;  // último recurso, sem lock
  }
}

// ==========================================
// FUNÇÃO DE ATUALIZAÇÃO DO HARDWARE
// Máquina de Estados Finita de Controlo Físico (Relatório 5.3 / Fluxograma 2)
// ==========================================
void atualizarHardware() {
  // ---- "Sistema Ativo E Sem Alerta?" ----
  if (!sistemaLigado || emAlerta) {
    String estadoNovo = emAlerta ? "ERRO: LIMITES EXCEDIDOS " : "Desligado ";
    if (estadoNovo != estadoSistema || categoriaAtual != 0) {
      digitalWrite(PINO_PELTIER, LOW);  // Cortar potência da Peltier
      digitalWrite(PINO_MODO, LOW);
      categoriaAtual = 0;
    }
    estadoSistema = estadoNovo;
    return;  // Fim do ciclo
  }

  // ---- Calcular Erro (T.Atual - T.Alvo) + Aplicar Histerese Dinâmica (5%) ----
  float tAtual = obterTemperaturaAtual();
  float margem = max((float)abs(temperaturaAlvo * 0.05), (float)0.5);
  float erro = tAtual - temperaturaAlvo;

  // ---- Determinar Próximo Estado ----
  String estadoNovo;
  int categoriaNova;
  if (erro > margem) {
    estadoNovo = "A Arrefecer \xE2\x9D\x84";  // Calor em excesso
    categoriaNova = 1;
  } else if (erro < -margem) {
    estadoNovo = "A Aquecer \xF0\x9F\x94\xA5";  // Frio em excesso
    categoriaNova = 2;
  } else {
    estadoNovo = "Estabilizado \xE2\x9C\x85";  // Dentro do alvo -> Peltier sem energia
    categoriaNova = 0;
  }

  // ---- Estado Novo é Diferente do Atual? ----
  if (estadoNovo == estadoSistema) {
    return;  // Manter Hardware Intacto - Fim do Ciclo
  }

  // ---- Inversão Térmica? (Quente <-> Frio) ----
  bool inversaoTermica = (categoriaAtual == 1 && categoriaNova == 2) ||
                          (categoriaAtual == 2 && categoriaNova == 1);

  if (inversaoTermica) {
    digitalWrite(PINO_PELTIER, LOW);      // corta potência antes de comutar o relé
    vTaskDelay(pdMS_TO_TICKS(200));       // Tempo morto (200ms) para proteção dos MOSFETs
  }

  // ---- Comutar Ponte H (relé) e Ligar Potência ----
  if (categoriaNova == 1) {
    digitalWrite(PINO_MODO, LOW);         // Arrefecer
    digitalWrite(PINO_PELTIER, HIGH);
  } else if (categoriaNova == 2) {
    digitalWrite(PINO_MODO, HIGH);        // Aquecer
    digitalWrite(PINO_PELTIER, HIGH);
  } else {
    digitalWrite(PINO_PELTIER, LOW);      // Estabilizado -> sem energia
  }

  // ---- Guardar Novo Estado na FSM ----
  estadoSistema = estadoNovo;
  categoriaAtual = categoriaNova;
}

// ==========================================
// SUPERVISÃO DE SEGURANÇA (WATCHDOG)
// Relatório 5.1 / Fluxograma 3 - Interface IoT, Persistência e Supervisão
// Corre no loop() principal, independente da task do sensor, para detetar
// bloqueios do firmware (se o sensor não atualizar em >5s, corta energia)
// ==========================================
void verificarWatchdogSeguranca() {
  static unsigned long ultimaVerificacaoMs = 0;
  if (millis() - ultimaVerificacaoMs < WATCHDOG_INTERVALO_MS) return;
  ultimaVerificacaoMs = millis();

  if (millis() - ultimaLeituraValidaMs > WATCHDOG_TIMEOUT_MS) {
    if (!emAlerta) {
      causaAlerta = "Watchdog: sensor sem leituras validas ha mais de 5s";
    }
    emAlerta = true;
    if (sistemaLigado) {
      sistemaLigado = false;
      Serial.println("EMERGENCIA (Watchdog): " + causaAlerta);
    }
    atualizarHardware();
  }
}

// ==========================================
// HANDLERS WEB (SITE + API)
// (Relatório 5.2 - Interface de Monitorização IoT / Fluxograma 3)
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
  html += "    var divAlerta = document.getElementById('alerta');";
  html += "    if(dados.alerta) {";
  html += "      divAlerta.innerHTML = '⚠️ ATENÇÃO: ' + dados.causa + '<br>Sistema parado por segurança.';";
  html += "      divAlerta.style.display = 'block';";
  html += "    } else {";
  html += "      divAlerta.style.display = 'none';";
  html += "    }";
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
  html += "    if(novaTemp >= -20 && novaTemp <= 100) {";
  html += "      fetch('/definir?valor=' + novaTemp);";
  html += "      document.getElementById('inputTemp').value = '';";
  html += "    } else {";
  html += "      alert('Erro: O alvo tem de estar entre -20°C e 100°C!');";
  html += "    }";
  html += "  }";
  html += "}";
  html += "function toggleEnergia() { fetch('/toggle'); }";
  html += "</script></head><body>";
  html += "<div id='alerta'></div>";
  html += "<h1>Termóstato Peltier</h1>";
  html += "<div class='card'><h2>Energia</h2><button id='btnPower' onclick='toggleEnergia()'>A carregar...</button></div><br>";
  html += "<div class='card'><h2>Temp. Atual</h2><div class='valor' id='t_atual'>-- °C</div></div>";
  html += "<div class='card'><h2>Temp. Alvo</h2><div class='valor' id='t_alvo'>-- °C</div></div><br>";
  html += "<div class='card'><h2>Ajustar Temperatura</h2><input type='number' id='inputTemp' step='0.1' min='-20' max='100' placeholder='Ex: 22.5'><br><button onclick='enviarTemperatura()'>Definir</button></div>";
  html += "<div class='card'><h2>Estado do Módulo</h2><div class='valor' id='estado' style='color: #ff5722; font-size: 1.5em;'>--</div></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleDados() {
  float tAtualCopia = obterTemperaturaAtual();
  String estadoLigado = sistemaLigado ? "true" : "false";
  String flgAlerta = emAlerta ? "true" : "false";
  String json = "{\"atual\":" + String(tAtualCopia, 1) + ",\"alvo\":" + String(temperaturaAlvo, 1) +
                ",\"estado\":\"" + estadoSistema + "\",\"ligado\":" + estadoLigado +
                ",\"alerta\":" + flgAlerta + ",\"causa\":\"" + causaAlerta + "\"}";
  server.send(200, "application/json", json);
}

void handleDefinir() {
  if (server.hasArg("valor")) {
    float t = server.arg("valor").toFloat();
    if (t >= -20.0 && t <= 100.0) {
      temperaturaAlvo = t;
      preferencias.putFloat("alvo", temperaturaAlvo);  // Guardar em NVS (tolerância a falhas)
      Serial.println("Nova temperatura definida: " + String(temperaturaAlvo));
      atualizarHardware();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleToggle() {
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

  mutexTemperatura = xSemaphoreCreateMutex();
  if (mutexTemperatura == NULL) {
    Serial.println("Erro ao criar mutex da temperatura!");
  }

  // NVS - recupera o último setpoint guardado (tolerância a falhas / Fluxograma 3)
  preferencias.begin("peltier", false);
  temperaturaAlvo = preferencias.getFloat("alvo", 20.0);
  Serial.println("Temperatura alvo recuperada da NVS: " + String(temperaturaAlvo));

  ultimaLeituraValidaMs = millis();  // evita falso alerta do watchdog no arranque

  WiFi.softAP(ssid, password);

  server.on("/", handleRoot);
  server.on("/dados", handleDados);
  server.on("/definir", handleDefinir);
  server.on("/toggle", handleToggle);
  server.begin();

  xTaskCreatePinnedToCore(
    taskLeituraSensor,  // Task function
    "Leitura sensor",   // Task name
    4096,                // Stack size
    NULL,                // Task parameters
    1,                    // Priority
    NULL,                // Task handle
    1                     // Core ID
  );
}

void loop() {
  server.handleClient();
  verificarWatchdogSeguranca();  // Fluxograma 3 - verificação a cada 500ms
  vTaskDelay(pdMS_TO_TICKS(1));
}
