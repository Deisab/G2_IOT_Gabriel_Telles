// FrioSeguro — monitor de câmara fria de restaurante (ESP32 / Wokwi)
// Trabalho N2 de IoT. Decisão na borda: compressor e alarme funcionam sem internet.
//
// Duas tarefas:
//   tarefaControle (núcleo 1, prioridade alta): sensores, regra com histerese, atuadores.
//   loop() (rede): Wi-Fi, MQTT/TLS, histórico no Worker, comandos assinados.
// Se a rede travar, o controle continua rodando.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <Preferences.h>
#include <time.h>
#include "mbedtls/md.h"

#include "secrets.h"   // credenciais (fora do Git) — veja secrets.example.h
#include "certs.h"     // certificados raiz para conferir o servidor

// ---------------------------------------------------------------- identificação
#define DISP_ID     "camara01"
#define TOPIC_BASE  "sis1a/gabrieltelles/" DISP_ID
#define T_TELEMETRIA TOPIC_BASE "/telemetria"
#define T_ESTADO     TOPIC_BASE "/estado"
#define T_COMANDO    TOPIC_BASE "/comando"
#define T_ACK        TOPIC_BASE "/ack"
#define T_ALERTA     TOPIC_BASE "/alerta"

// ---------------------------------------------------------------- pinos
#define PIN_DHT     15
#define PIN_TRIG     5
#define PIN_ECHO    18
#define PIN_RELE    26
#define PIN_BUZZER  25
#define PIN_LED_R   27
#define PIN_LED_G   14
#define PIN_LED_B   13
#define PIN_BOTAO    4

// ---------------------------------------------------------------- tempos
// MODO_DEMO 1 encurta os tempos para dar para mostrar ao vivo na apresentação.
#define MODO_DEMO 1

#if MODO_DEMO
  #define ALARME_TEMP_S_PADRAO   30
  #define ALARME_PORTA_S_PADRAO  20
  #define COMP_MIN_MS        10000UL
  #define COOLDOWN_MS        60000UL
#else
  #define ALARME_TEMP_S_PADRAO  300
  #define ALARME_PORTA_S_PADRAO 120
  #define COMP_MIN_MS        60000UL
  #define COOLDOWN_MS       300000UL
#endif

#define LEITURA_MS           2000UL    // DHT22 precisa de >= 2 s entre leituras
#define HISTORICO_MS        60000UL    // 1 leitura por minuto no D1
#define TELEMETRIA_MS       10000UL
#define ENVIO_MS            15000UL    // tenta esvaziar o buffer a cada 15 s
#define SILENCIO_MS        600000UL    // botão/comando silencia o buzzer por 10 min
#define FALHAS_P_ESTADO_SEGURO 3
#define PORTA_FECHADA_CM     10.0f
#define CMD_VALIDADE_MAX_S   60        // exp no máximo 60 s no futuro

// ---------------------------------------------------------------- configuração (NVS)
struct Config {
  float liga_c    = 4.0f;   // compressor liga acima
  float desliga_c = 2.0f;   // compressor desliga abaixo
  float alarme_c  = 5.0f;   // alarme se acima por alarme_s
  float normal_c  = 4.5f;   // alarme de temperatura normaliza abaixo
  uint32_t alarme_s = ALARME_TEMP_S_PADRAO;
  uint32_t porta_s  = ALARME_PORTA_S_PADRAO;
};

// ---------------------------------------------------------------- estado compartilhado
struct Estado {
  float temp_c = NAN, umid_pct = NAN, dist_cm = NAN;
  bool sensor_ok = false;
  bool porta_aberta = false;
  bool compressor = true;          // estado seguro até a primeira leitura
  bool alarme_temp = false, alarme_porta = false, falha_sensor = false;
  uint32_t silenciado_ate = 0;     // millis(); 0 = não silenciado
  uint32_t versao = 0;             // muda quando algo relevante muda (publica /estado)
};

struct Leitura { uint32_t seq, ms; float t, h; bool porta, comp, alarme, ok; };
struct Evento  { uint32_t seq, ms; const char* tipo; bool ativo; float t; };

#define BUF_LEITURAS 360   // 6 h a 1 leitura/min
#define BUF_EVENTOS   32

Config cfg;
Estado est;
SemaphoreHandle_t mtx;

Leitura bufL[BUF_LEITURAS]; uint32_t seqL = 0, enviadoL = 0;
Evento  bufE[BUF_EVENTOS];  uint32_t seqE = 0, enviadoE = 0, publicadoE = 0;

Preferences prefs;
DHTesp dht;
WiFiClientSecure netMqtt, netHttp;
PubSubClient mqtt(netMqtt);

#define LOCK()   xSemaphoreTake(mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(mtx)

bool alarmeAtivo(const Estado& e) { return e.alarme_temp || e.alarme_porta || e.falha_sensor; }

// ================================================================ configuração
bool validarConfig(const Config& c) {
  return c.desliga_c >= -5 && c.desliga_c < c.liga_c && c.liga_c <= 8 &&
         c.normal_c < c.alarme_c && c.alarme_c <= 10 &&
         c.alarme_s >= 10 && c.alarme_s <= 3600 &&
         c.porta_s >= 10 && c.porta_s <= 1800;
}

// Aplica só os campos presentes; devolve false (sem mudar nada) se o resultado for inválido.
bool aplicarConfig(JsonObjectConst o) {
  Config c;
  LOCK(); c = cfg; UNLOCK();
  if (o["liga_c"].is<float>())    c.liga_c    = o["liga_c"];
  if (o["desliga_c"].is<float>()) c.desliga_c = o["desliga_c"];
  if (o["alarme_c"].is<float>())  c.alarme_c  = o["alarme_c"];
  if (o["normal_c"].is<float>())  c.normal_c  = o["normal_c"];
  if (o["alarme_s"].is<uint32_t>()) c.alarme_s = o["alarme_s"];
  if (o["porta_s"].is<uint32_t>())  c.porta_s  = o["porta_s"];
  if (!validarConfig(c)) return false;
  LOCK(); cfg = c; est.versao++; UNLOCK();
  prefs.putBytes("cfg", &c, sizeof(c));
  return true;
}

void carregarConfig() {
  Config c;
  if (prefs.getBytes("cfg", &c, sizeof(c)) == sizeof(c) && validarConfig(c)) cfg = c;
}

// ================================================================ buffers
void registrarEvento(const char* tipo, bool ativo, float t) {   // chamado com LOCK
  Evento& e = bufE[seqE % BUF_EVENTOS];
  e = { ++seqE, millis(), tipo, ativo, t };
}

void registrarLeitura() {                                       // chamado com LOCK
  Leitura& l = bufL[seqL % BUF_LEITURAS];
  l = { ++seqL, millis(), est.temp_c, est.umid_pct, est.porta_aberta,
        est.compressor, alarmeAtivo(est), est.sensor_ok };
}

// ================================================================ tarefa de controle
float lerDistanciaCm() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 30000);
  return us == 0 ? NAN : us * 0.0343f / 2.0f;
}

void tarefaControle(void*) {
  uint32_t ultLeitura = 0, ultHist = 0, ultTroca = millis();
  uint32_t quenteDesde = 0, portaDesde = 0;
  uint8_t falhas = 0;
  bool botaoAnt = HIGH; uint32_t botaoMs = 0;
  digitalWrite(PIN_RELE, HIGH);   // estado seguro: compressor ligado até a 1ª leitura

  for (;;) {
    uint32_t agora = millis();

    // --- botão silenciar (com debounce)
    bool b = digitalRead(PIN_BOTAO);
    if (b != botaoAnt && agora - botaoMs > 50) {
      botaoMs = agora; botaoAnt = b;
      if (b == LOW) {
        LOCK();
        if (alarmeAtivo(est)) { est.silenciado_ate = agora + SILENCIO_MS; est.versao++;
                                registrarEvento("silenciado_local", true, est.temp_c); }
        UNLOCK();
        Serial.println("[botao] alarme silenciado no local");
      }
    }

    // --- leitura e decisão
    if (agora - ultLeitura >= LEITURA_MS) {
      ultLeitura = agora;
      TempAndHumidity th = dht.getTempAndHumidity();
      bool leituraOk = dht.getStatus() == DHTesp::ERROR_NONE && !isnan(th.temperature);
      float dist = lerDistanciaCm();

      LOCK();
      Config c = cfg;
      Estado antes = est;

      falhas = leituraOk ? 0 : min<uint8_t>(falhas + 1, 255);
      if (leituraOk) { est.temp_c = th.temperature; est.umid_pct = th.humidity; }
      est.sensor_ok = falhas < FALHAS_P_ESTADO_SEGURO;
      if (!est.sensor_ok) { est.temp_c = NAN; est.umid_pct = NAN; }
      est.dist_cm = dist;
      est.porta_aberta = isnan(dist) || dist >= PORTA_FECHADA_CM;  // sem eco = pior caso

      // compressor: histerese + tempo mínimo entre trocas
      bool querComp = est.compressor;
      if (!est.sensor_ok)               querComp = true;            // estado seguro
      else if (est.temp_c > c.liga_c)    querComp = true;
      else if (est.temp_c < c.desliga_c) querComp = false;
      if (querComp != est.compressor && agora - ultTroca >= COMP_MIN_MS) {
        est.compressor = querComp; ultTroca = agora;
      }

      // alarme de temperatura: acima de alarme_c por alarme_s; normaliza abaixo de normal_c
      if (est.sensor_ok && est.temp_c > c.alarme_c) { if (!quenteDesde) quenteDesde = agora; }
      else quenteDesde = 0;
      if (quenteDesde && agora - quenteDesde >= c.alarme_s * 1000UL) est.alarme_temp = true;
      if (est.sensor_ok && est.temp_c < c.normal_c) est.alarme_temp = false;

      // alarme de porta: aberta por porta_s; normaliza ao fechar
      if (est.porta_aberta) { if (!portaDesde) portaDesde = agora; }
      else portaDesde = 0;
      est.alarme_porta = portaDesde && agora - portaDesde >= c.porta_s * 1000UL;

      est.falha_sensor = !est.sensor_ok;

      // eventos nas transições
      if (est.alarme_temp  != antes.alarme_temp)  registrarEvento("temp_alta",    est.alarme_temp,  est.temp_c);
      if (est.alarme_porta != antes.alarme_porta) registrarEvento("porta_aberta", est.alarme_porta, est.temp_c);
      if (est.falha_sensor != antes.falha_sensor) registrarEvento("falha_sensor", est.falha_sensor, est.temp_c);
      if (!alarmeAtivo(est)) est.silenciado_ate = 0;   // próximo alarme volta a tocar

      if (est.compressor != antes.compressor || est.porta_aberta != antes.porta_aberta ||
          alarmeAtivo(est) != alarmeAtivo(antes) || est.sensor_ok != antes.sensor_ok ||
          est.alarme_temp != antes.alarme_temp || est.alarme_porta != antes.alarme_porta)
        est.versao++;

      if (agora - ultHist >= HISTORICO_MS || ultHist == 0) { ultHist = agora; registrarLeitura(); }
      Estado e = est;
      UNLOCK();

      digitalWrite(PIN_RELE, e.compressor ? HIGH : LOW);
    }

    // --- sinaleiro e sirene (a cada 100 ms)
    LOCK(); Estado e = est; float normal = cfg.normal_c; UNLOCK();
    bool alarme = alarmeAtivo(e);
    bool pisca = (agora / 500) % 2;
    bool r = false, g = false;
    if (e.falha_sensor)  r = pisca;                                   // vermelho piscando
    else if (alarme)     r = true;                                    // vermelho
    else if (e.porta_aberta || e.temp_c > normal) { r = true; g = true; }  // amarelo
    else                 g = true;                                    // verde
    digitalWrite(PIN_LED_R, r); digitalWrite(PIN_LED_G, g); digitalWrite(PIN_LED_B, LOW);

    bool silenciado = e.silenciado_ate && (int32_t)(e.silenciado_ate - agora) > 0;
    static bool tocando = false;
    bool deveTocar = alarme && !silenciado && (agora / 1000) % 2 == 0;   // 1 s sim, 1 s não
    if (deveTocar != tocando) { tocando = deveTocar; if (tocando) tone(PIN_BUZZER, 2000); else noTone(PIN_BUZZER); }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ================================================================ utilidades de rede
bool horaValida() { return time(nullptr) > 1700000000; }
uint32_t agoraUnix() { return horaValida() ? (uint32_t)time(nullptr) : 0; }
// converte um millis() antigo para horário unix (precisa da hora já sincronizada)
uint32_t msParaUnix(uint32_t ms) { return agoraUnix() - (millis() - ms) / 1000; }

String hmacHex(const char* msg, size_t len) {
  uint8_t out[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)HMAC_KEY, strlen(HMAC_KEY));
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, len);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", out[i]);
  return String(hex);
}

bool iguaisTempoConstante(const String& a, const char* b) {
  size_t n = strlen(b);
  if (a.length() != n) return false;
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

// sensor inválido vira null no JSON (NaN não é JSON válido); arredonda para 1 casa
template <typename T>
void setFloatOuNull(T v, float x) { if (isnan(x)) v.set(nullptr); else v.set(roundf(x * 10) / 10); }

// ================================================================ MQTT: publicações
void publicarEstado() {
  LOCK(); Estado e = est; Config c = cfg; UNLOCK();
  JsonDocument d;
  d["disp"] = DISP_ID; d["online"] = true; d["ts"] = agoraUnix();
  setFloatOuNull(d["temp_c"], e.temp_c);
  d["porta_aberta"] = e.porta_aberta; d["compressor"] = e.compressor;
  d["alarme"] = alarmeAtivo(e); d["sensor_ok"] = e.sensor_ok;
  d["silenciado"] = e.silenciado_ate && (int32_t)(e.silenciado_ate - millis()) > 0;
  JsonObject a = d["alarmes"].to<JsonObject>();
  a["temp_alta"] = e.alarme_temp; a["porta_aberta"] = e.alarme_porta; a["falha_sensor"] = e.falha_sensor;
  JsonObject k = d["config"].to<JsonObject>();
  k["liga_c"] = c.liga_c; k["desliga_c"] = c.desliga_c; k["alarme_c"] = c.alarme_c;
  k["normal_c"] = c.normal_c; k["alarme_s"] = c.alarme_s; k["porta_s"] = c.porta_s;
  String s; serializeJson(d, s);
  mqtt.publish(T_ESTADO, s.c_str(), true);   // retido: o painel recebe ao abrir
}

void publicarTelemetria() {
  LOCK(); Estado e = est; UNLOCK();
  JsonDocument d;
  d["disp"] = DISP_ID; d["ts"] = agoraUnix();
  setFloatOuNull(d["temp_c"], e.temp_c);
  setFloatOuNull(d["umid_pct"], e.umid_pct);
  setFloatOuNull(d["dist_cm"], e.dist_cm);
  d["porta_aberta"] = e.porta_aberta; d["compressor"] = e.compressor;
  d["alarme"] = alarmeAtivo(e); d["sensor_ok"] = e.sensor_ok;
  String s; serializeJson(d, s);
  mqtt.publish(T_TELEMETRIA, s.c_str());
}

const char* mensagemAlerta(const char* tipo, bool ativo) {
  if (!ativo) return "Situação normalizada";
  if (!strcmp(tipo, "temp_alta"))    return "Temperatura acima do limite por tempo demais";
  if (!strcmp(tipo, "porta_aberta")) return "Porta da câmara aberta por tempo demais";
  if (!strcmp(tipo, "falha_sensor")) return "Falha no sensor de temperatura: compressor forçado ligado";
  return tipo;
}

// Alertas com cooldown por tipo: ativação não se repete antes de COOLDOWN_MS;
// enquanto o alarme continua ativo, um lembrete sai a cada COOLDOWN_MS; "normalizado" sempre sai.
const char* TIPOS[] = { "temp_alta", "porta_aberta", "falha_sensor" };
uint32_t ultAlertaMs[3] = { 0, 0, 0 };

int idxTipo(const char* t) { for (int i = 0; i < 3; i++) if (!strcmp(t, TIPOS[i])) return i; return -1; }

void enviarAlerta(int i, bool ativo, float t, uint32_t ms) {
  JsonDocument d;
  d["disp"] = DISP_ID; d["tipo"] = TIPOS[i]; d["ativo"] = ativo;
  setFloatOuNull(d["temp_c"], t);
  d["ts"] = horaValida() ? msParaUnix(ms) : 0;
  d["msg"] = mensagemAlerta(TIPOS[i], ativo);
  String s; serializeJson(d, s);
  if (mqtt.publish(T_ALERTA, s.c_str())) {
    if (ativo) ultAlertaMs[i] = millis();
    Serial.printf("[alerta] %s ativo=%d\n", TIPOS[i], ativo);
  }
}

void processarAlertas() {
  // novos eventos de alarme desde a última passada
  for (;;) {
    LOCK();
    if (publicadoE >= seqE) { UNLOCK(); break; }
    if (seqE - publicadoE > BUF_EVENTOS) publicadoE = seqE - BUF_EVENTOS;  // perdeu os mais antigos
    Evento e = bufE[publicadoE % BUF_EVENTOS];
    publicadoE++;
    UNLOCK();
    int i = idxTipo(e.tipo);
    if (i < 0) continue;
    if (!e.ativo) enviarAlerta(i, false, e.t, e.ms);
    else if (!ultAlertaMs[i] || millis() - ultAlertaMs[i] >= COOLDOWN_MS) enviarAlerta(i, true, e.t, e.ms);
    // senão: segurado pelo cooldown; o lembrete abaixo envia quando ele acabar
  }
  // lembrete enquanto o alarme continua ativo depois do cooldown
  LOCK(); Estado e = est; UNLOCK();
  bool ativos[3] = { e.alarme_temp, e.alarme_porta, e.falha_sensor };
  for (int i = 0; i < 3; i++)
    if (ativos[i] && millis() - ultAlertaMs[i] >= COOLDOWN_MS) enviarAlerta(i, true, e.temp_c, millis());
}

// ================================================================ MQTT: comandos assinados
// Formato: {"p":"<JSON do comando como texto>","sig":"<HMAC-SHA256(p) em hex>"}
// p = {"id":"c-...","cmd":"silenciar"|"config","exp":<unix>,"args":{...}}
#define IDS_RECENTES 16
String idsRecentes[IDS_RECENTES]; uint8_t idxIds = 0;

void responderAck(const char* id, bool ok, const char* motivo) {
  JsonDocument d;
  if (id) d["id"] = id; else d["id"] = nullptr;
  d["ok"] = ok;
  if (motivo) d["motivo"] = motivo; else d["motivo"] = nullptr;
  d["ts"] = agoraUnix();
  String s; serializeJson(d, s);
  mqtt.publish(T_ACK, s.c_str());
}

void rejeitar(const char* id, const char* motivo) {
  Serial.printf("[comando] REJEITADO: %s\n", motivo);
  LOCK(); registrarEvento("comando_rejeitado", true, est.temp_c); UNLOCK();
  responderAck(id, false, motivo);
}

void aoReceber(char* topico, byte* payload, unsigned int len) {
  if (strcmp(topico, T_COMANDO) != 0) return;

  JsonDocument ext;
  if (deserializeJson(ext, payload, len)) { rejeitar(nullptr, "json_invalido"); return; }
  const char* p   = ext["p"];
  const char* sig = ext["sig"];
  if (!p || !sig) { rejeitar(nullptr, "sem_assinatura"); return; }

  JsonDocument cmd;
  if (deserializeJson(cmd, p)) { rejeitar(nullptr, "json_invalido"); return; }
  const char* id = cmd["id"];

  if (!iguaisTempoConstante(hmacHex(p, strlen(p)), sig)) { rejeitar(id, "assinatura_invalida"); return; }
  if (!id || strlen(id) < 8 || strlen(id) > 40)          { rejeitar(id, "id_invalido"); return; }
  if (!horaValida())                                      { rejeitar(id, "sem_hora"); return; }
  uint32_t exp = cmd["exp"] | 0, agora = agoraUnix();
  if (exp < agora || exp > agora + CMD_VALIDADE_MAX_S)    { rejeitar(id, "expirado"); return; }
  for (auto& r : idsRecentes) if (r == id)                { rejeitar(id, "repetido"); return; }
  idsRecentes[idxIds] = id; idxIds = (idxIds + 1) % IDS_RECENTES;

  const char* nome = cmd["cmd"] | "";
  if (!strcmp(nome, "silenciar")) {
    LOCK();
    if (alarmeAtivo(est)) est.silenciado_ate = millis() + SILENCIO_MS;
    est.versao++;
    registrarEvento("silenciado_remoto", true, est.temp_c);
    UNLOCK();
    responderAck(id, true, nullptr);
  } else if (!strcmp(nome, "config")) {
    if (!aplicarConfig(cmd["args"].as<JsonObjectConst>())) { rejeitar(id, "config_invalida"); return; }
    LOCK(); registrarEvento("config_alterada", true, est.temp_c); UNLOCK();
    responderAck(id, true, nullptr);
  } else {
    rejeitar(id, "comando_desconhecido"); return;
  }
  Serial.printf("[comando] executado: %s (%s)\n", nome, id);
}

// ================================================================ HTTPS: Worker
bool postarHistorico() {
  // monta um lote com até 30 leituras e todos os eventos ainda não enviados
  JsonDocument d;
  d["disp"] = DISP_ID;
  JsonArray L = d["leituras"].to<JsonArray>();
  JsonArray E = d["eventos"].to<JsonArray>();
  uint32_t ateL, ateE;

  LOCK();
  if (seqL - enviadoL > BUF_LEITURAS) enviadoL = seqL - BUF_LEITURAS;  // buffer deu a volta
  if (seqE - enviadoE > BUF_EVENTOS)  enviadoE = seqE - BUF_EVENTOS;
  ateL = min(seqL, enviadoL + 30);
  ateE = seqE;
  for (uint32_t s = enviadoL; s < ateL; s++) {
    const Leitura& l = bufL[s % BUF_LEITURAS];
    JsonObject o = L.add<JsonObject>();
    o["ts"] = msParaUnix(l.ms);
    setFloatOuNull(o["temp_c"], l.t); setFloatOuNull(o["umid_pct"], l.h);
    o["porta_aberta"] = l.porta ? 1 : 0; o["compressor"] = l.comp ? 1 : 0;
    o["alarme"] = l.alarme ? 1 : 0;     o["sensor_ok"] = l.ok ? 1 : 0;
  }
  for (uint32_t s = enviadoE; s < ateE; s++) {
    const Evento& e = bufE[s % BUF_EVENTOS];
    JsonObject o = E.add<JsonObject>();
    o["ts"] = msParaUnix(e.ms); o["tipo"] = e.tipo; o["ativo"] = e.ativo ? 1 : 0;
    setFloatOuNull(o["temp_c"], e.t);
  }
  UNLOCK();

  if (L.size() == 0 && E.size() == 0) return true;

  String corpo; serializeJson(d, corpo);
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(netHttp, WORKER_URL "/api/leituras")) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " DEVICE_TOKEN);
  int code = http.POST(corpo);
  http.end();

  if (code >= 200 && code < 300) {
    LOCK(); enviadoL = max(enviadoL, ateL); enviadoE = max(enviadoE, ateE); UNLOCK();
    Serial.printf("[historico] enviado: %u leituras, %u eventos\n", (unsigned)L.size(), (unsigned)E.size());
    return true;
  }
  Serial.printf("[historico] falhou (HTTP %d), fica no buffer\n", code);
  return false;
}

bool buscarConfigRemota() {
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(netHttp, WORKER_URL "/api/config?disp=" DISP_ID)) return false;
  http.addHeader("Authorization", "Bearer " DEVICE_TOKEN);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument d;
    if (!deserializeJson(d, http.getString())) ok = aplicarConfig(d.as<JsonObjectConst>());
  }
  http.end();
  Serial.printf("[config] remota %s (HTTP %d)\n", ok ? "aplicada" : "mantida da NVS", code);
  return code == 200 || code == 404;   // 404 = ainda não há config no KV
}

// ================================================================ conexões
void conectarMqtt() {
  static uint32_t proxTentativa = 0, espera = 2000;
  if (mqtt.connected() || millis() < proxTentativa) return;

  Serial.print("[mqtt] conectando com TLS... ");
  const char* will = "{\"disp\":\"" DISP_ID "\",\"online\":false}";
  if (mqtt.connect("frioseguro-" DISP_ID, MQTT_USER, MQTT_PASS, T_ESTADO, 1, true, will)) {
    Serial.println("ok");
    mqtt.subscribe(T_COMANDO, 1);
    publicarEstado();
    espera = 2000;
  } else {
    Serial.printf("falhou (estado %d), nova tentativa em %lus\n", mqtt.state(), espera / 1000);
    proxTentativa = millis() + espera;
    espera = min<uint32_t>(espera * 2, 60000);   // espera crescente
  }
}

// ================================================================ setup / loop
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_RELE, OUTPUT); pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_BOTAO, INPUT_PULLUP);
  digitalWrite(PIN_RELE, HIGH);   // estado seguro já no boot

  mtx = xSemaphoreCreateMutex();
  prefs.begin("frioseguro", false);
  carregarConfig();
  dht.setup(PIN_DHT, DHTesp::DHT22);

  Serial.println("\n=== FrioSeguro / " DISP_ID " ===");
  Serial.printf("limites: liga>%.1f desliga<%.1f alarme>%.1f por %lus, porta %lus\n",
                cfg.liga_c, cfg.desliga_c, cfg.alarme_c, cfg.alarme_s, cfg.porta_s);

  xTaskCreatePinnedToCore(tarefaControle, "controle", 6144, nullptr, 3, nullptr, 1);

  netMqtt.setCACert(ROOT_CA_BUNDLE);   // confere o certificado do broker (sem setInsecure)
  netHttp.setCACert(ROOT_CA_BUNDLE);   // confere o certificado do Worker
  netMqtt.setHandshakeTimeout(15);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(aoReceber);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
  static bool ntpIniciado = false, configBuscada = false, wifiAnt = false;
  static uint32_t ultTelemetria = 0, ultEnvio = 0, ultLog = 0, versaoPublicada = 0;

  bool wifi = WiFi.status() == WL_CONNECTED;
  if (wifi != wifiAnt) { wifiAnt = wifi; Serial.println(wifi ? "[wifi] conectado" : "[wifi] sem conexão — controle segue local"); }

  if (wifi) {
    if (!ntpIniciado) { configTime(0, 0, "pool.ntp.org", "time.google.com"); ntpIniciado = true; }

    conectarMqtt();
    mqtt.loop();

    if (mqtt.connected()) {
      LOCK(); uint32_t v = est.versao; UNLOCK();
      if (v != versaoPublicada) { versaoPublicada = v; publicarEstado(); }
      if (millis() - ultTelemetria >= TELEMETRIA_MS) { ultTelemetria = millis(); publicarTelemetria(); }
      processarAlertas();
    }

    if (horaValida()) {
      if (!configBuscada) configBuscada = buscarConfigRemota();
      if (millis() - ultEnvio >= ENVIO_MS) { ultEnvio = millis(); postarHistorico(); }
    }
  }

  if (millis() - ultLog >= 10000) {
    ultLog = millis();
    LOCK(); Estado e = est; uint32_t pend = seqL - enviadoL; UNLOCK();
    Serial.printf("T=%.1fC porta=%s comp=%s alarme=%s sensor=%s | buffer=%u | wifi=%d mqtt=%d\n",
                  e.temp_c, e.porta_aberta ? "ABERTA" : "fechada", e.compressor ? "ON" : "off",
                  alarmeAtivo(e) ? "SIM" : "nao", e.sensor_ok ? "ok" : "FALHA",
                  pend, wifi, mqtt.connected());
  }
  delay(20);
}
