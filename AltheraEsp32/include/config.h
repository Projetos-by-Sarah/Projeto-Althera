#pragma once
#include <Arduino.h>

// ---------- Rede ----------
#define WIFI_SSID   "SEU_WIFI"
#define WIFI_PASS   "SUA_SENHA"
#define DEVICE_ID   "althera-01"   // muda em cada placa (também vira http://althera-01.local)
#define API_TOKEN   ""             // se não vazio, o POST exige o header X-Api-Key com esse valor

// ---------- Pinos ----------
#define PIN_I2C_SDA   8
#define PIN_I2C_SCL   9
#define PIN_CHG_4A    41   // CHG_4A  (paralelo R 20mΩ)
#define PIN_CHG_10A   40   // CHG_10A (paralelo R 5mΩ)

// ---------- Modo debug de bancada ----------
// WIFI_ENABLED 0 = não conecta no WiFi nem sobe o servidor (boot instantâneo, só serial)
#define WIFI_ENABLED  1

// LOG_MODE: 0 = desligado
//           1 = tabela legível no monitor serial
//           2 = gráfico ao vivo (extensão Teleplot do VS Code)
//           3 = CSV (para colar no Excel/Sheets ou usar no Serial Studio)
#define LOG_MODE         1
#define LOG_INTERVAL_MS  1000

// ---------- Amostragem ----------
#define SAMPLE_INTERVAL_MS  500
#define INA_RETRY_MS        5000
