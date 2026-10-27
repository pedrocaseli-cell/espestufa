#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHT.h"

// --- CONFIGURAÇÃO DO DISPLAY LCD I2C ---
// Altere 0x27 para o endereço encontrado no scanner (ex: 0x3F)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- DEFINIÇÃO DA LÓGICA DO RELÉ ---
#define RELE_LIGADO LOW      
#define RELE_DESLIGADO HIGH   

// --- CONFIGURAÇÃO DOS SENSORES ---
#define DHTPIN 23
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

#define PIN_PH 15            // GPIO 15 (ADC1)
float tensaoPh7 = 3.3/2; 
float tensaoPh4 = 0.95 + tensaoPh7; 
float m = (7.0 - 4.0) / (tensaoPh7 - tensaoPh4);
float b = 7.0 - (m * tensaoPh7);

#define PIN_SOLO_AO 2       // GPIO 2 (ADC1)
const int VALOR_SECO = 1800;
const int VALOR_UMIDO = 0;

// --- PINOS DOS ATUADORES (RELÉS) ---
#define PIN_ATUADOR_1 25     // Loop: 10s ON / 5s OFF
#define PIN_ATUADOR_2 26     // Temp < 25 °C
#define PIN_ATUADOR_3 27     // Umid Ar < 80%
#define PIN_ATUADOR_4 33     // Umid Solo < 50%
#define PIN_ATUADOR_5 18     // Temp > 30 °C

// --- VARIÁVEIS DE TEMPORIZAÇÃO ---
unsigned long tempoAnteriorAtuador1 = 0;
bool estadoAtuador1 = false;

unsigned long tempoAnteriorTela = 0;
int telaAtual = 0;

void setup() {
  Serial.begin(9600);
  analogReadResolution(12);

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
  // 3. REGRAS DE AUTOMAÇÃO DOS ATUADORES
  // ==========================================
  bool stR2 = (!isnan(temperatura) && temperatura < 25.0);
  bool stR3 = (!isnan(umidadeAr) && umidadeAr < 80.0);
  bool stR4 = (umidadeSoloPercent < 50);
  bool stR5 = (!isnan(temperatura) && temperatura > 30.0);

  digitalWrite(PIN_ATUADOR_2, stR2 ? RELE_LIGADO : RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_3, stR3 ? RELE_LIGADO : RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_4, stR4 ? RELE_LIGADO : RELE_DESLIGADO);
  digitalWrite(PIN_ATUADOR_5, stR5 ? RELE_LIGADO : RELE_DESLIGADO);

  // ==========================================
  // 4. ATUALIZAÇÃO DO DISPLAY LCD (A CADA 2,5s)
  // ==========================================
  if (tempoAtual - tempoAnteriorTela >= 2500) {
    tempoAnteriorTela = tempoAtual;
    telaAtual = (telaAtual + 1) % 3;
    lcd.clear();
  }

  switch (telaAtual) {
    case 0: // TELA 1: Sensores de Ar
      lcd.setCursor(0, 0);
      lcd.print("Temp: ");
      if (isnan(temperatura)) {
        lcd.print("Erro    ");
      } else {
        lcd.print(temperatura, 1);
        lcd.print(" C   ");
      }

      lcd.setCursor(0, 1);
      lcd.print("Umid Ar: ");
      if (isnan(umidadeAr)) {
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
      lcd.print("R1 "); lcd.print(estadoAtuador1 ? "ON" : "OFF");
      lcd.print(" R2 "); lcd.print(stR2 ? "ON" : "OFF");
      lcd.print(" R3 "); lcd.print(stR3 ? "ON" : "OFF");

      lcd.setCursor(0, 1);
      lcd.print("R4 "); lcd.print(stR4 ? "ON" : "OFF");
      lcd.print(" R5 "); lcd.print(stR5 ? "ON" : "OFF");
      break;
  }

  // ==========================================
  // 5. MONITOR SERIAL
  // ==========================================
  Serial.print("T: "); Serial.print(temperatura, 1);
  Serial.print("C | U: "); Serial.print(umidadeAr, 1);
  Serial.print("% | pH: "); Serial.print(valorPh, 2);
  Serial.print(" | : "); Serial.print(analogRead(PIN_PH));  
  Serial.print(" | Solo: "); Serial.print(umidadeSoloPercent);
  Serial.print("% | Solo: "); Serial.print(analogRead(PIN_SOLO_AO));  
  Serial.print(" | R1 "); Serial.print(estadoAtuador1 ? "ON" : "OFF");
  Serial.print(" R2 "); Serial.print(stR2 ? "ON" : "OFF");
  Serial.print(" R3 "); Serial.print(stR3 ? "ON" : "OFF");
  Serial.print(" R4 "); Serial.print(stR4 ? "ON" : "OFF");
  Serial.print(" R5 "); Serial.println(stR5 ? "ON" : "OFF");

  delay(200);
}