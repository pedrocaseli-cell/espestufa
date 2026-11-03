#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHT.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// =====================================================================
// PREENCHA AQUI (os 4 valores abaixo)
// =====================================================================
const char* WIFI_SSID   = "Esp-32";
const char* WIFI_SENHA  = "123456789";
const char* URL_SITE    = "https://estufa-tcc.vercel.app";   // sem barra no final
const char* TOKEN_ESP32 = "TOKEN_ESP";

const unsigned long INTERVALO_ENVIO_MS  = 10000;  // manda leitura ao site a cada 10 s
const unsigned long INTERVALO_CONFIG_MS = 30000;  // pergunta os limites a cada 30 s

// --- CONFIGURAÇÃO DO DISPLAY LCD I2C ---
LiquidCrystal_I2C lcd(0x3F, 16, 2);

// --- DEFINIÇÃO DA LÓGICA DO RELÉ ---
#define RELE_LIGADO LOW
#define RELE_DESLIGADO HIGH

// --- CONFIGURAÇÃO DOS SENSORES ---
#define DHTPIN 23
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Pinos dos sensores analógicos (os mesmos do seu código original)
#define PIN_PH 15
float tensaoPh7 = 3.3 / 2;
float tensaoPh4 = 0.95 + tensaoPh7;
float m = (7.0 - 4.0) / (tensaoPh7 - tensaoPh4);
float b = 7.0 - (m * tensaoPh7);

#define PIN_SOLO_AO 2
const int VALOR_SECO = 1800;
const int VALOR_UMIDO = 0;

// --- PINOS DOS ATUADORES (RELÉS) ---
#define PIN_ATUADOR_1 25     // Loop: 10s ON / 5s OFF
#define PIN_ATUADOR_2 26     // Aquecimento   (temp < mínimo)
#define PIN_ATUADOR_3 27     // Umidificador  (umid. ar < mínimo)
#define PIN_ATUADOR_4 33     // Irrigação     (umid. solo < mínimo)
#define PIN_ATUADOR_5 18     // Ventilação    (temp > máximo)

// --- HISTERESE: evita o relé ficar ligando e desligando sem parar ---
const float HIST_TEMP = 1.0;   // °C
const float HIST_AR   = 3.0;   // %
const float HIST_SOLO = 5.0;   // %

// --- LIMITES (começam iguais ao seu código antigo; o site pode mudar) ---
struct Limites { float tMin, tMax, arMin, arMax, soloMin, soloMax, phMin, phMax; };
Limites limites = {25, 30, 80, 95, 50, 80, 5.5, 7.5};
uint64_t versaoAplicada = 0;

// --- DADOS COMPARTILHADOS ENTRE O CONTROLE E A TAREFA DE REDE ---
struct Dados {
  float temp, ar, solo, ph;
  bool aquecimento, ventilacao, umidificador, irrigacao;
  bool valido;
};
Dados dados = {0, 0, 0, 0, false, false, false, false, false};

SemaphoreHandle_t mutex;
Preferences prefs;
volatile bool wifiOk = false;
volatile bool siteOk = false;

// --- VARIÁVEIS DE TEMPORIZAÇÃO ---
unsigned long tempoAnteriorAtuador1 = 0;
bool estadoAtuador1 = false;
unsigned long tempoAnteriorTela = 0;
int telaAtual = 0;

// Estado dos atuadores (guardado entre uma volta e outra por causa da histerese)
bool stAquec = false, stVent = false, stUmid = false, stIrrig = false;

// =====================================================================
// MEMÓRIA: guarda os limites para continuar valendo se faltar energia/internet
// =====================================================================
void carregarLimites() {
  prefs.begin("estufa", true);
  limites.tMin    = prefs.getFloat("tMin", limites.tMin);
  limites.tMax    = prefs.getFloat("tMax", limites.tMax);
  limites.arMin   = prefs.getFloat("arMin", limites.arMin);
  limites.arMax   = prefs.getFloat("arMax", limites.arMax);
  limites.soloMin = prefs.getFloat("soloMin", limites.soloMin);
  limites.soloMax = prefs.getFloat("soloMax", limites.soloMax);
  limites.phMin   = prefs.getFloat("phMin", limites.phMin);
  limites.phMax   = prefs.getFloat("phMax", limites.phMax);
  versaoAplicada  = prefs.getULong64("versao", 0);
  prefs.end();
}

void salvarLimites() {
  prefs.begin("estufa", false);
  prefs.putFloat("tMin", limites.tMin);
  prefs.putFloat("tMax", limites.tMax);
  prefs.putFloat("arMin", limites.arMin);
  prefs.putFloat("arMax", limites.arMax);
  prefs.putFloat("soloMin", limites.soloMin);
  prefs.putFloat("soloMax", limites.soloMax);
  prefs.putFloat("phMin", limites.phMin);
  prefs.putFloat("phMax", limites.phMax);
  prefs.putULong64("versao", versaoAplicada);
  prefs.end();
}

// =====================================================================
// REDE (roda em paralelo, então a internet lenta não trava os relés)
// =====================================================================
void enviarLeitura() {
  Dados d;
  xSemaphoreTake(mutex, portMAX_DELAY);
  d = dados;
  xSemaphoreGive(mutex);
  if (!d.valido) return;  // sensor com erro: não manda lixo, o site mostra "offline"

  JsonDocument doc;
  doc["temp"] = d.temp;
  doc["ar"] = d.ar;
  doc["solo"] = d.solo;
  doc["ph"] = d.ph;
  JsonObject at = doc["atuadores"].to<JsonObject>();
  at["aquecimento"] = d.aquecimento;
  at["ventilacao"] = d.ventilacao;
  at["umidificador"] = d.umidificador;
  at["irrigacao"] = d.irrigacao;
  String corpo;
  serializeJson(doc, corpo);

  WiFiClientSecure client;
  client.setInsecure();  // aceita o certificado do Vercel sem conferir (simples para o TCC)
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, String(URL_SITE) + "/api/leitura")) { siteOk = false; return; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-token", TOKEN_ESP32);
  int codigo = http.POST(corpo);
  siteOk = (codigo == 200);
  if (!siteOk) Serial.printf("Envio falhou, codigo %d\n", codigo);
  http.end();
}

bool lerPar(JsonVariantConst v, float &mn, float &mx, float absMin, float absMax) {
  if (!v.is<JsonArrayConst>()) return false;
  JsonArrayConst a = v.as<JsonArrayConst>();
  if (a.size() != 2) return false;
  float x = a[0], y = a[1];
  if (!(x < y) || x < absMin || y > absMax) return false;
  mn = x; mx = y;
  return true;
}

void buscarConfig() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, String(URL_SITE) + "/api/config")) { siteOk = false; return; }
  http.addHeader("x-token", TOKEN_ESP32);
  int codigo = http.GET();
  if (codigo != 200) {
    siteOk = false;
    Serial.printf("Busca de limites falhou, codigo %d\n", codigo);
    http.end();
    return;
  }
  siteOk = true;
  String resp = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return;
  if (doc["limites"].isNull()) return;  // o site ainda não aplicou nada

  uint64_t versao = doc["versao"].as<uint64_t>();
  if (versao == versaoAplicada) return;  // nada novo

  // Valida de novo aqui, com as mesmas faixas do servidor
  Limites n = limites;
  JsonVariantConst l = doc["limites"];
  if (!lerPar(l["temp"], n.tMin, n.tMax, 5, 40)) return;
  if (!lerPar(l["ar"], n.arMin, n.arMax, 20, 100)) return;
  if (!lerPar(l["solo"], n.soloMin, n.soloMax, 10, 100)) return;
  if (!lerPar(l["ph"], n.phMin, n.phMax, 4, 9)) return;

  xSemaphoreTake(mutex, portMAX_DELAY);
  limites = n;
  xSemaphoreGive(mutex);
  versaoAplicada = versao;
  salvarLimites();
  Serial.println("Novos limites recebidos do site e salvos.");
}

void tarefaRede(void *param) {
  unsigned long ultimoEnvio = 0, ultimaConfig = 0, ultimaTentativaWifi = 0;
  bool primeiro = true;
  for (;;) {
    unsigned long agora = millis();
    if (WiFi.status() != WL_CONNECTED) {
      wifiOk = false;
      siteOk = false;
      if (agora - ultimaTentativaWifi >= 15000) {
        ultimaTentativaWifi = agora;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_SENHA);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    wifiOk = true;
    if (primeiro || agora - ultimaConfig >= INTERVALO_CONFIG_MS) {
      ultimaConfig = agora;
      buscarConfig();
    }
    if (primeiro || agora - ultimoEnvio >= INTERVALO_ENVIO_MS) {
      ultimoEnvio = agora;
      enviarLeitura();
    }
    primeiro = false;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// =====================================================================
void setup() {
  Serial.begin(9600);
  analogReadResolution(12);
  mutex = xSemaphoreCreateMutex();
  carregarLimites();

  // Inicialização do LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Estufa Automati.");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");

  dht.begin();

  // Configuração dos pinos dos relés
  pinMode(PIN_ATUADOR_1, OUTPUT);
  pinMode(PIN_ATUADOR_2, OUTPUT);
  pinMode(PIN_ATUADOR_3, OUTPUT);
  pinMode(PIN_ATUADOR_4, OUTPUT);
  pinMode(PIN_ATUADOR_5, OUTPUT);

  digitalWrite(PIN_ATUADOR_1, RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_2, RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_3, RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_4, RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_5, RELE_DESLIGADO);

  // Wi-Fi e tarefa de rede (o controle abaixo não espera por ela)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  xTaskCreatePinnedToCore(tarefaRede, "rede", 12288, NULL, 1, NULL, 0);

  delay(2000);
  lcd.clear();
}

void loop() {
  unsigned long tempoAtual = millis();

  // ==========================================
  // 1. CONTROLE TEMPORIZADO DO ATUADOR 1
  // ==========================================
  if (estadoAtuador1 == true) {
    if (tempoAtual - tempoAnteriorAtuador1 >= 10000) {
      estadoAtuador1 = false;
      digitalWrite(PIN_ATUADOR_1, RELE_DESLIGADO);
      tempoAnteriorAtuador1 = tempoAtual;
    }
  } else {
    if (tempoAtual - tempoAnteriorAtuador1 >= 5000) {
      estadoAtuador1 = true;
      digitalWrite(PIN_ATUADOR_1, RELE_LIGADO);
      tempoAnteriorAtuador1 = tempoAtual;
    }
  }

  // ==========================================
  // 2. LEITURA DOS SENSORES
  // ==========================================
  float umidadeAr = dht.readHumidity();
  float temperatura = dht.readTemperature();

  long somaADC_pH = 0;
  for (int i = 0; i < 20; i++) {
    somaADC_pH += analogRead(PIN_PH);
    delay(2);
  }
  float mediaADC_pH = somaADC_pH / 20.0;
  float tensaoLida = (mediaADC_pH * 3.3) / 4095.0;
  float valorPh = (m * tensaoLida) + b;
  if (valorPh < 0.0) valorPh = 0.0;
  if (valorPh > 14.0) valorPh = 14.0;

  long somaADC_Solo = 0;
  for (int i = 0; i < 20; i++) {
    somaADC_Solo += analogRead(PIN_SOLO_AO);
    delay(2);
  }
  int leituraSoloRaw = somaADC_Solo / 20;
  int umidadeSoloPercent = map(leituraSoloRaw, VALOR_SECO, VALOR_UMIDO, 0, 100);
  umidadeSoloPercent = constrain(umidadeSoloPercent, 0, 100);

  // ==========================================
  // 3. REGRAS DE AUTOMAÇÃO (limites vindos do site, com histerese)
  // ==========================================
  Limites L;
  xSemaphoreTake(mutex, portMAX_DELAY);
  L = limites;
  xSemaphoreGive(mutex);

  bool tempOk = !isnan(temperatura);
  bool arOk = !isnan(umidadeAr);

  if (tempOk) {
    if (temperatura < L.tMin) stAquec = true;
    else if (temperatura >= L.tMin + HIST_TEMP) stAquec = false;

    if (temperatura > L.tMax) stVent = true;
    else if (temperatura <= L.tMax - HIST_TEMP) stVent = false;
  } else {
    stAquec = false;  // sensor com erro: estado seguro (desliga)
    stVent = false;
  }
  if (stVent) stAquec = false;  // nunca aquecer e ventilar ao mesmo tempo

  if (arOk) {
    if (umidadeAr < L.arMin) stUmid = true;
    else if (umidadeAr >= fmin(L.arMin + HIST_AR, L.arMax)) stUmid = false;
  } else {
    stUmid = false;
  }

  if (umidadeSoloPercent < L.soloMin) stIrrig = true;
  else if (umidadeSoloPercent >= fmin(L.soloMin + HIST_SOLO, L.soloMax)) stIrrig = false;

  digitalWrite(PIN_ATUADOR_2, stAquec ? RELE_LIGADO : RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_3, stUmid ? RELE_LIGADO : RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_4, stIrrig ? RELE_LIGADO : RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_5, stVent ? RELE_LIGADO : RELE_DESLIGADO);

  // Entrega os dados para a tarefa de rede mandar ao site
  xSemaphoreTake(mutex, portMAX_DELAY);
  dados.temp = temperatura;
  dados.ar = umidadeAr;
  dados.solo = umidadeSoloPercent;
  dados.ph = valorPh;
  dados.aquecimento = stAquec;
  dados.ventilacao = stVent;
  dados.umidificador = stUmid;
  dados.irrigacao = stIrrig;
  dados.valido = tempOk && arOk;
  xSemaphoreGive(mutex);

  // ==========================================
  // 4. ATUALIZAÇÃO DO DISPLAY LCD (A CADA 2,5s)
  // ==========================================
  if (tempoAtual - tempoAnteriorTela >= 2500) {
    tempoAnteriorTela = tempoAtual;
    telaAtual = (telaAtual + 1) % 4;
    lcd.clear();
  }

  switch (telaAtual) {
    case 0: // TELA 1: Sensores de Ar
      lcd.setCursor(0, 0);
      lcd.print("Temp: ");
      if (!tempOk) {
        lcd.print("Erro    ");
      } else {
        lcd.print(temperatura, 1);
        lcd.print(" C   ");
      }

      lcd.setCursor(0, 1);
      lcd.print("Umid Ar: ");
      if (!arOk) {
        lcd.print("Erro  ");
      } else {
        lcd.print((int)umidadeAr);
        lcd.print("%   ");
      }
      break;

    case 1: // TELA 2: Sensores de Solo e pH
      lcd.setCursor(0, 0);
      lcd.print("pH: ");
      lcd.print(valorPh, 2);
      lcd.print("        ");

      lcd.setCursor(0, 1);
      lcd.print("Umid Solo: ");
      lcd.print(umidadeSoloPercent);
      lcd.print("%   ");
      break;

    case 2: // TELA 3: Status dos Atuadores
      lcd.setCursor(0, 0);
      lcd.print("R1 "); lcd.print(estadoAtuador1 ? "ON " : "OFF");
      lcd.print(" R2 "); lcd.print(stAquec ? "ON " : "OFF");
      lcd.print(" R3 "); lcd.print(stUmid ? "ON" : "OFF");

      lcd.setCursor(0, 1);
      lcd.print("R4 "); lcd.print(stIrrig ? "ON " : "OFF");
      lcd.print(" R5 "); lcd.print(stVent ? "ON " : "OFF");
      break;

    case 3: // TELA 4: Conexão
      lcd.setCursor(0, 0);
      lcd.print(wifiOk ? "WiFi: conectado " : "WiFi: sem sinal ");
      lcd.setCursor(0, 1);
      lcd.print(siteOk ? "Site: OK        " : "Site: sem resp. ");
      break;
  }

  // ==========================================
  // 5. MONITOR SERIAL
  // ==========================================
  Serial.print("T: "); Serial.print(temperatura, 1);
  Serial.print("C | U: "); Serial.print(umidadeAr, 1);
  Serial.print("% | pH: "); Serial.print(valorPh, 2);
  Serial.print(" | pH ADC: "); Serial.print((int)mediaADC_pH);
  Serial.print(" | Solo: "); Serial.print(umidadeSoloPercent);
  Serial.print("% | Solo ADC: "); Serial.print(leituraSoloRaw);
  Serial.print(" | R1 "); Serial.print(estadoAtuador1 ? "ON" : "OFF");
  Serial.print(" R2 "); Serial.print(stAquec ? "ON" : "OFF");
  Serial.print(" R3 "); Serial.print(stUmid ? "ON" : "OFF");
  Serial.print(" R4 "); Serial.print(stIrrig ? "ON" : "OFF");
  Serial.print(" R5 "); Serial.print(stVent ? "ON" : "OFF");
  Serial.print(" | WiFi "); Serial.print(wifiOk ? "OK" : "--");
  Serial.print(" Site "); Serial.println(siteOk ? "OK" : "--");

  delay(200);
}
