# FrioSeguro — monitor de câmara fria de restaurante

Trabalho N2 de IoT (FAM · Sistemas de Informação · prof. Leonam V. Hemann · 2026-02).

**O problema:** a RDC 216 da ANVISA exige que alimentos refrigerados fiquem até 5 °C. Uma câmara fria que esquenta de madrugada (porta mal fechada, compressor parado) faz o restaurante perder o estoque, e sem histórico não há como provar à fiscalização que a temperatura ficou na faixa.

**O que o sistema faz:** um ESP32 mede a temperatura e a porta da câmara, liga e desliga o compressor com histerese e dispara sirene e sinaleiro quando a câmara passa do limite por tempo demais. **Tudo isso funciona sem internet.** Quando há rede, ele manda telemetria, alertas e o histórico pela nuvem, e um painel web mostra tudo ao vivo e permite silenciar o alarme ou mudar os limites, sempre com confirmação do dispositivo.

O caderno de projeto completo, com o porquê de cada decisão, está em [`docs/`](docs/).

## Arquitetura

```
 DHT22 · HC-SR04 · botão                     relé (compressor) · buzzer · LED RGB
            \                                        /
             ESP32 (Wokwi) — decide na borda, estado seguro, buffer offline
                    |
                    |  MQTT com TLS 8883 (certificado do broker conferido)
                    v
             EMQX Cloud (broker) <---------- WSS 8084 ---------- Painel (navegador)
                    |                                                |
                    |  regra: tópico .../historico                   |  HTTPS + PIN
                    v  HTTPS + token                                 v
             Cloudflare Worker  <-----------------------------------+
                    |--> D1  (leituras, eventos; resumo por hora em SQL)
                    '--> KV  (últimos limites, contador de PIN errado)
```

| Tópico (`sis1a/gabrieltelles/camara01/...`) | Publica | Assina |
|---|---|---|
| `telemetria` (a cada 10 s) | ESP32 | painel |
| `estado` (retido; `offline` pela *last will*) | ESP32 | painel |
| `alerta` (com cooldown) | ESP32 | painel |
| `historico` (lote assinado a cada 60 s) | ESP32 | regra do EMQX → Worker |
| `comando` (assinado com HMAC) | painel | ESP32 |
| `ack` | ESP32 | painel |

## Estrutura do repositório

```
docs/                     caderno de projeto
firmware/
  sketch/sketch.ino       código do ESP32 (Arduino)
  diagram.json            montagem no Wokwi
  libraries.txt           bibliotecas do Wokwi
  certs.h                 certificados raiz do broker (públicos)
  secrets.example.h       modelo das credenciais -> copiar para secrets.h
worker/
  src/index.js            Cloudflare Worker (API)
  public/                 painel (HTML, CSS, JS)
  schema.sql              tabelas do D1
  wrangler.toml           configuração do Worker (D1, KV, cron)
  ca_workers_dev.pem      certificados raiz do workers.dev (para o conector do EMQX)
```

## Como montar

### 1. Broker (EMQX Cloud)
1. Crie um deployment e anote o **Address** (portas 8883 e 8084).
2. Em **Ports Management**, desligue as portas sem TLS (1883 e 8083).
3. Em **Access Control → Authentication**, crie os usuários `esp32-camara01` e `painel`.
4. Em **Network Management**, ative o **NAT Gateway** (sem ele o broker não alcança a internet).
5. Em **Data Integration**, crie um conector **HTTP Server**:
   - URL `https://<seu-worker>.workers.dev/api/leituras`, TLS ligado com verificação, SNI do Worker e CA Cert `worker/ca_workers_dev.pem`;
   - cabeçalhos `content-type: application/json` e `authorization: Bearer <DEVICE_TOKEN>`.
6. Crie a regra `SELECT payload FROM "sis1a/gabrieltelles/camara01/historico"` com uma ação `POST` nesse conector e corpo `${payload}`.

### 2. Nuvem (Cloudflare Worker)
Requer Node 20 ou mais (o projeto usa o Wrangler 3).

```bash
cd worker
npm install
npx wrangler login
npx wrangler d1 create frioseguro          # copie o database_id para o wrangler.toml
npx wrangler kv namespace create KV        # copie o id para o wrangler.toml
npx wrangler d1 execute frioseguro --remote --file schema.sql
npx wrangler deploy
npx wrangler secret put DEVICE_TOKEN       # o mesmo valor do cabeçalho no EMQX
npx wrangler secret put HMAC_KEY           # o mesmo valor do secrets.h do ESP32
npx wrangler secret put PAINEL_PIN         # PIN do gerente no painel
```

Para gerar token e chave: `openssl rand -hex 32`.

### 3. Dispositivo (Wokwi)
1. Crie um projeto **ESP32** no [Wokwi](https://wokwi.com).
2. Cole `firmware/sketch/sketch.ino`, `firmware/diagram.json` e `firmware/libraries.txt` nas abas de mesmo nome.
3. Crie as abas `certs.h` (copie de `firmware/`) e `secrets.h` (a partir de `secrets.example.h`, com a senha do `esp32-camara01` e a `HMAC_KEY`).
4. Rode a simulação. Em ~1 min o Serial mostra `[historico] publicado: ...`.

> O projeto salvo no Wokwi fica visível para quem tiver o link, **incluindo o `secrets.h`**. Não compartilhe o link e troque as senhas depois da apresentação.

O código está em **modo demonstração** (`MODO_DEMO 1`), com tempos curtos (alarme de temperatura em 30 s, porta em 20 s, cooldown de 60 s). Com `MODO_DEMO 0`, valem os tempos reais (5 min, 2 min e 5 min).

## Como testar

Abra o painel (`https://<seu-worker>.workers.dev`) e entre com o PIN e a senha MQTT do usuário `painel`. Deixe o Wokwi e o painel **em janelas lado a lado**: aba em segundo plano pausa a simulação.

| O que fazer no Wokwi | O que deve acontecer |
|---|---|
| DHT22 em 4,5 °C, depois 1,5 °C | relé do compressor liga acima de 4 °C e desliga abaixo de 2 °C |
| DHT22 em 5,5 °C por 30 s | sirene, LED vermelho, banner de alarme e alerta no painel |
| Painel: **Silenciar** e depois **Cancelar silêncio** | sirene para e volta; o painel mostra "executado pelo dispositivo" |
| Ultrassônico acima de 10 cm por 20 s | alarme de porta aberta |
| Tirar o fio de dados do DHT22 e reiniciar | estado seguro: compressor ligado, LED vermelho piscando, alerta `falha_sensor` |
| Parar a simulação | painel mostra "Dispositivo: OFFLINE" (*last will*) |
| Publicar em `.../comando` sem assinatura (cliente MQTTX) | ESP32 rejeita e responde ack `ok:false` |

O histórico pode ser conferido no painel (gráfico e resumo por hora) ou direto no banco:

```bash
cd worker
npx wrangler d1 execute frioseguro --remote --command "SELECT * FROM leituras ORDER BY ts DESC LIMIT 5"
```

## Segurança (resumo)

- **MQTT só com TLS**, com o certificado do broker conferido por `setCACert()`. Não há `setInsecure()`, e as portas sem TLS ficam desligadas.
- **Comandos assinados com HMAC-SHA256**, com validade e `id` único (proteção contra replay). A chave fica só no ESP32 e no Worker; o painel pede a assinatura ao Worker, protegido por PIN com bloqueio após 5 erros.
- **Histórico assinado pelo ESP32** e entregue ao Worker com token: quem só tem a senha do broker não grava dados falsos.
- **Sem segredos no repositório:** `firmware/secrets.h` e `worker/.dev.vars` estão no `.gitignore`, e o Worker usa `wrangler secret`.

## Por que o ESP32 não chama o Worker direto por HTTPS?

Foi a primeira versão. No Wokwi, a verificação do certificado ECDSA da Cloudflare é tão lenta que a Cloudflare encerra o aperto de mão TLS antes de o ESP32 terminar (medimos: 10 s de pausa é aceito, 15 s derruba). Com o broker fazendo o repasse, o dispositivo fala só MQTT e mantém uma única conexão TLS, o que também é melhor para um produto real.
