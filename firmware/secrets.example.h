// Copie este arquivo para "secrets.h" e preencha.
// secrets.h está no .gitignore: NUNCA envie as credenciais para o GitHub.
#pragma once

// Wi-Fi (no Wokwi a rede é aberta)
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""

// HiveMQ Cloud: Overview do cluster -> URL (sem "mqtts://" e sem porta)
#define MQTT_HOST "xxxxxxxxxxxxxxxx.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
// Credencial criada em Access Management, só para o ESP32
#define MQTT_USER "esp32-camara01"
#define MQTT_PASS "troque-esta-senha"

// Cloudflare Worker (sem barra no final)
#define WORKER_URL "https://frioseguro.SEU-SUBDOMINIO.workers.dev"
// Token da rota de gravação (o mesmo valor do secret DEVICE_TOKEN do Worker)
#define DEVICE_TOKEN "gere-um-token-longo-e-aleatorio"

// Chave HMAC dos comandos (o mesmo valor do secret HMAC_KEY do Worker)
#define HMAC_KEY "gere-outra-chave-longa-e-aleatoria"
