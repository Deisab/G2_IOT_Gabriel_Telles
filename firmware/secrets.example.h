// Copie este arquivo para "secrets.h" e preencha.
// secrets.h está no .gitignore: NUNCA envie as credenciais para o GitHub.
#pragma once

// Wi-Fi (no Wokwi a rede é aberta)
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""

// EMQX Cloud: Overview do deployment -> Address (sem "mqtts://" e sem porta)
#define MQTT_HOST "zeebdd7a.ala.dedicated.aws.emqxcloud.com"
#define MQTT_PORT 8883
// Usuário criado em Access Control -> Authentication, só para o ESP32
#define MQTT_USER "esp32-camara01"
#define MQTT_PASS "troque-esta-senha"


// Chave HMAC dos comandos e do histórico (o mesmo valor do secret HMAC_KEY do Worker)
#define HMAC_KEY "gere-outra-chave-longa-e-aleatoria"
