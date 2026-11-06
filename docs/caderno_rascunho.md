# Trabalho N2 — Caderno de Projeto (rascunho)

> Rascunho em Markdown para revisar. A versão final vai para `docs/` em .docx ou PDF.
> Itens marcados com **[PREENCHER]** dependem de você ou dos testes.

- **Integrante:** [PREENCHER] (trabalho solo: montagem, código, nuvem e painel, caderno e testes)
- **Desafio escolhido:** Opção B — FrioSeguro, monitor de câmara fria de restaurante
- **Repositório:** [PREENCHER link do GitHub]
- **Plataforma:** simulação no Wokwi (ESP32)

---

## Etapa 0 — Desafio

**Desafio (A) ou ideia própria (B):** B — ideia própria.

| Lugar | Sensor | Atuador | Restrição |
|---|---|---|---|
| Restaurante (cozinha / câmara fria) | DHT22 (temperatura) + ultrassônico (porta) | Relé (compressor) + buzzer + LED RGB | Tem que funcionar sem internet |

**Frase da ideia:** "Para restaurantes que perdem insumos e correm risco de autuação quando a câmara fria sai da faixa, o **FrioSeguro** sente a temperatura e a porta, decide com histerese quando ligar o compressor e quando alarmar, e age com relé, buzzer e LED, mesmo sem internet."

**Por que esse problema importa? Quem ganha?**
A RDC 216/2004 da ANVISA exige que alimentos refrigerados fiquem até 5 °C. Uma câmara que esquenta durante a madrugada (porta mal fechada, compressor travado) faz o restaurante perder o estoque inteiro e, se ninguém perceber, servir comida estragada. Ganham o dono (menos perda e uma prova para a vigilância sanitária), a equipe de cozinha (aviso na hora, sem precisar conferir o termômetro) e o cliente (segurança alimentar).

---

## Etapa 1 — Problema e requisitos

| Código | Requisito (o dispositivo deve...) | Como vamos comprovar | Por que é importante neste problema |
|---|---|---|---|
| RF1 | Ligar o compressor (relé) quando T > 4,0 °C e desligá-lo quando T < 2,0 °C, em até 2 s após a leitura. | No Wokwi, mover o controle de temperatura do DHT22 para 4,5 °C e para 1,5 °C e observar o relé e o Serial. | Mantém a câmara entre 2 °C e 4 °C, abaixo do limite de 5 °C, sem o compressor liga-desliga. |
| RF2 | Disparar o alarme local (buzzer + LED vermelho) quando T > 5,0 °C por mais de 5 min **ou** a porta ficar aberta por mais de 2 min, e desligá-lo quando T < 4,5 °C e a porta estiver fechada. | Forçar T = 6 °C e cronometrar; afastar o "obstáculo" do ultrassônico para simular a porta aberta. Na demonstração, os tempos são reduzidos (30 s e 20 s). | Avisa a equipe antes de o alimento estragar, sem disparar por uma abertura rápida da porta. |
| RF3 | Publicar telemetria por MQTT a cada 10 s, publicar alertas na hora em que ocorrem e gravar uma leitura no histórico (D1) a cada 60 s. | Painel atualizando, mensagens no broker e linhas novas no D1. | O histórico é a prova para a fiscalização de que a câmara nunca saiu da faixa. |
| RF4 | Executar comandos remotos (silenciar o alarme, cancelar o silêncio, ajustar limites) e responder com ack em até 3 s, rejeitando comandos sem assinatura válida, expirados ou repetidos. | Enviar um comando pelo painel e medir o tempo até o ack; publicar um comando falso no tópico e ver a rejeição. | O gerente consegue agir de longe, mas um estranho não consegue desligar o alarme. |

| Código | Requisito não funcional (como e quanto) |
|---|---|
| RNF1 | **Sem internet:** controle do compressor e alarme local continuam funcionando. As leituras ficam em buffer (até 360 leituras = 6 h) e são enviadas em lote quando a rede volta. |
| RNF2 | **Segurança:** toda comunicação usa TLS com verificação do certificado do servidor; o histórico só é gravado com a assinatura HMAC do ESP32 e o token do repasse; nenhum segredo fica no repositório ou no Serial. |
| RNF3 | **Tempo de resposta:** decisão local em menos de 1 s depois da leitura; o painel mostra a mudança em menos de 2 s (WebSocket). |

---

## Etapa 2 — Sentir e agir: componentes

O que o kit representa: o **relé** representa o contator do compressor da câmara fria, o **buzzer** representa a sirene da cozinha, o **LED RGB** representa o sinaleiro na porta da câmara, e o **ultrassônico** representa o sensor de porta (distância até a porta).

| Componente | Mede ou aciona o quê | GPIO | Faixa ou limite | Se falhar, o sistema... |
|---|---|---|---|---|
| DHT22 | Temperatura (°C) e umidade (%) dentro da câmara | 15 | −40 a 80 °C, ±0,5 °C; faixa de operação 2–4 °C | Depois de 3 leituras inválidas seguidas (NaN): mantém o **compressor ligado**, LED vermelho piscando e alerta `falha_sensor`. |
| HC-SR04 | Distância até a porta (cm): < 10 cm = fechada | TRIG 5, ECHO 18 | 2–400 cm | Sem eco (timeout): considera a **porta aberta** (pior caso) e conta o tempo para o alarme. |
| Relé | Liga e desliga o compressor | 26 | Tempo mínimo de 60 s entre trocas de estado (protege o compressor) | Estado seguro = ligado: frio demais é melhor do que perder o estoque. |
| Buzzer | Sirene de alarme | 25 | Bip intermitente (1 s ligado, 1 s desligado) | — (atuador de saída; o LED e o painel também avisam) |
| LED RGB | Verde = ok; amarelo = atenção (T > 4,5 °C ou porta aberta); vermelho = alarme | R 27, G 14, B 13 | PWM | — |
| Botão | Silenciar o alarme no local (silencia o buzzer por 10 min, mas o alarme continua registrado) | 4 (INPUT_PULLUP) | Debounce de 50 ms | — |

**Por que DHT22 e não DHT11:** o DHT11 mede de 0 a 50 °C com ±2 °C de erro. Numa faixa de 2 a 5 °C esse erro é maior que a própria margem. O DHT22 tem ±0,5 °C e mede abaixo de zero.

---

## Etapa 3 — Onde decidir: ESP32 ou nuvem?

**Regra de decisão em pseudocódigo:**

```
a cada 2 s:
  ler T e distância da porta
  se T inválida 3 vezes seguidas:
      compressor = LIGADO; alarme = LIGADO (falha_sensor); continuar

  // compressor com histerese e tempo mínimo
  se T > 4,0 e compressor desligado há >= 60 s: compressor = LIGADO
  se T < 2,0 e compressor ligado há >= 60 s:    compressor = DESLIGADO

  // alarme de temperatura com tempo mínimo
  se T > 5,0: contar tempo_quente   senão: zerar tempo_quente
  // alarme de porta
  se porta aberta: contar tempo_porta   senão: zerar tempo_porta

  se tempo_quente > 5 min ou tempo_porta > 2 min: alarme = LIGADO
  se T < 4,5 e porta fechada:                        alarme = DESLIGADO
```

Os limites (4,0 / 2,0 / 5,0 / 4,5 °C, 5 min e 2 min) ficam guardados no ESP32 (memória NVS) e podem ser ajustados pelo painel com um comando assinado.

**Onde a regra roda e por quê:** no **ESP32 (borda)**. Ele precisa reagir mesmo se a internet cair (restrição do projeto) e não pode depender da rede para ligar o compressor. A nuvem só guarda o histórico, mostra o painel e ajusta os limites. Como os limites são ajustados por alguém de fora, o painel manda um comando assinado; o ESP32 grava os novos limites na memória NVS, que sobrevive a reinícios, e o Worker guarda no KV um registro do último valor enviado.

**O que acontece se a internet cair? E quando ela voltar?**
- **Durante a queda:** compressor, alarme, buzzer e LED continuam funcionando normalmente. As leituras de 60 s vão para um buffer circular na RAM (até 360 leituras = 6 h), com o horário que o ESP32 obteve por NTP quando ainda estava online. No broker, a *last will* do MQTT marca o dispositivo como `offline`, e o painel mostra isso.
- **Quando volta:** o ESP32 reconecta (com espera crescente entre as tentativas), publica `online` no tópico de estado, publica o buffer em lotes no tópico `.../historico` (um lote a cada 2 s até esvaziar) e publica os alertas que ocorreram durante a queda.
- **Limitação conhecida:** se a energia cair junto, o buffer na RAM se perde. Num produto real, ele iria para a flash.

---

## Etapa 4 — Comunicar: protocolo, tópicos e mensagem

**Protocolos escolhidos e por quê:**
- **MQTT com TLS (8883)** entre o ESP32 e o broker (EMQX Cloud): telemetria e alertas em tempo real e comandos chegando ao ESP32 sem ele precisar perguntar.
- **MQTT sobre WebSocket seguro (WSS, 8084)** no painel: o navegador recebe as mudanças sozinho (é o Extra de tempo real).
- **HTTPS do broker para o Cloudflare Worker:** uma regra de *Data Integration* do EMQX repassa cada lote do tópico `.../historico` para a rota `POST /api/leituras`, que grava no D1. O painel também usa HTTPS para consultar o histórico e o resumo.
- **Por que o ESP32 não chama o Worker direto por HTTPS?** Testamos, e no Wokwi não funciona: o simulador demora para verificar o certificado ECDSA da Cloudflare, e a Cloudflare encerra o aperto de mão TLS quando o cliente passa de ~10–15 s parado (medimos: 10 s aceita, 15 s derruba com EOF). Com o broker fazendo o repasse, o dispositivo fala um protocolo só (MQTT), abre uma conexão TLS só e gasta menos memória e energia, o que também é melhor para um produto real.

**Árvore de tópicos** (prefixo `sis1a/gabrieltelles/camara01/`):

| Tópico | Quem publica | Quem assina | QoS / retain |
|---|---|---|---|
| `.../telemetria` | ESP32 (a cada 10 s) | Painel | QoS 0 |
| `.../estado` | ESP32 (ao mudar e ao conectar; `offline` pela last will) | Painel | QoS 0, retain (last will: QoS 1, retain) |
| `.../comando` | Painel | ESP32 (assina com QoS 1) | QoS 1 |
| `.../ack` | ESP32 | Painel | QoS 0 |
| `.../alerta` | ESP32 (com cooldown) | Painel | QoS 0 |
| `.../historico` | ESP32 (lote assinado a cada 60 s; a cada 2 s ao esvaziar o buffer depois de uma queda) | Regra do EMQX → Worker | QoS 0 |

A biblioteca PubSubClient do ESP32 só publica com QoS 0. Isso é compensado assim: o estado fica retido no broker; os alertas também entram no lote de histórico (que chega ao D1); e o painel espera o ack e, se ele não vier, mostra que não houve confirmação.

Com várias câmaras basta trocar `camara01` por `camara02`, e assim por diante. O painel assina `sis1a/<nome>/+/alerta` para ver todas.

**JSON de exemplo da telemetria:**

```json
{
  "disp": "camara01",
  "ts": 1762200000,
  "temp_c": 3.4,
  "umid_pct": 82.1,
  "dist_cm": 4.8,
  "porta_aberta": false,
  "compressor": true,
  "alarme": false,
  "sensor_ok": true
}
```

**Comando e ack:**

```json
{ "p": "{\"id\":\"c-1762200031-7f3a\",\"cmd\":\"silenciar\",\"exp\":1762200061}",
  "sig": "<HMAC-SHA256 de p, em hex>" }
{ "id": "c-1762200031-7f3a", "ok": true, "motivo": null, "ts": 1762200032 }
```

O comando vai como texto dentro de `p`, e a assinatura é calculada sobre esse texto exato. Assim o ESP32 e o painel não precisam formatar o JSON do mesmo jeito para a assinatura bater. Motivos de rejeição no ack: `assinatura_invalida`, `expirado`, `repetido`, `config_invalida`, `comando_desconhecido`.

**TLS: qual porta e como o ESP32 confere o servidor?**
Porta **8883**. O ESP32 usa `WiFiClientSecure` com `setCACert()` e os certificados raiz da autoridade que assina o broker (ISRG Root X1 e X2, do Let's Encrypt). A cadeia foi conferida com `openssl s_client` em 05/11/2026. Assim ele confere se está falando com o broker verdadeiro. **Não** é usado `setInsecure()`. O trecho EMQX → Worker também é HTTPS, verificado pelo próprio EMQX.

---

## Etapa 5 — Guardar: o histórico

**KV, D1 ou os dois? Por quê?** Os dois.
- **D1:** cada leitura é um registro, e é preciso responder perguntas sobre o passado (máxima por hora, tempo fora da faixa).
- **KV:** guarda o registro dos últimos limites enviados pelo painel e o contador de PIN errado, ambos lidos pela chave exata. Quem aplica os limites é o ESP32, que os grava na memória NVS ao receber o comando assinado. O último estado **não** vai para o KV: ele fica retido no broker MQTT (tópico `.../estado`), o que evita uma escrita no KV a cada mensagem (o plano gratuito tem limite de escritas por dia).

**Estrutura** (arquivo `worker/schema.sql`):

```sql
CREATE TABLE leituras (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  disp TEXT NOT NULL,
  ts INTEGER NOT NULL,            -- unix epoch (UTC)
  temp_c REAL,                    -- NULL quando o sensor falhou
  umid_pct REAL,
  porta_aberta INTEGER NOT NULL,  -- 0/1
  compressor INTEGER NOT NULL,    -- 0/1
  alarme INTEGER NOT NULL,        -- 0/1
  sensor_ok INTEGER NOT NULL,     -- 0/1
  UNIQUE (disp, ts)               -- reenvio do mesmo lote não duplica
);
CREATE INDEX idx_leituras_disp_ts ON leituras (disp, ts);

CREATE TABLE eventos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  disp TEXT NOT NULL,
  ts INTEGER NOT NULL,
  tipo TEXT NOT NULL,             -- temp_alta, porta_aberta, falha_sensor, silenciado_*, config_alterada, comando_*
  ativo INTEGER NOT NULL,         -- 1 = começou, 0 = normalizou
  temp_c REAL,
  origem TEXT NOT NULL DEFAULT 'esp32',  -- esp32 | painel
  UNIQUE (disp, ts, tipo, ativo, origem)
);
```

O `UNIQUE` existe porque um lote pode chegar duas vezes (por exemplo, o EMQX repete o repasse se o Worker demorar a responder). Com ele, a mesma leitura nunca entra duas vezes (`INSERT OR IGNORE`).

Chaves no KV:
- `config:camara01` → `{"liga_c":4.0,"desliga_c":2.0,"alarme_c":5.0,"normal_c":4.5,"alarme_s":30,"porta_s":20}`
- `falhas_pin:<ip>` → contador de PINs errados no painel (expira em 10 min; 5 erros bloqueiam)

**De quanto em quanto tempo grava?** Uma leitura a cada **60 s** no D1 (1.440 por dia). Os eventos são gravados na hora em que acontecem. A telemetria de 10 s vai só para o MQTT e o painel, não para o banco.

**Consulta de resumo** — "Nas últimas 24 h, qual foi a temperatura mínima, média e máxima de cada hora, e por quantos minutos a câmara ficou acima de 5 °C?":

```sql
SELECT strftime('%Y-%m-%d %H:00', ts, 'unixepoch', '-3 hours') AS hora,
       MIN(temp_c)            AS min_c,
       ROUND(AVG(temp_c), 1)  AS media_c,
       MAX(temp_c)            AS max_c,
       SUM(CASE WHEN temp_c > 5.0 THEN 1 ELSE 0 END) AS minutos_fora
FROM leituras
WHERE disp = 'camara01'
  AND sensor_ok = 1
  AND ts >= CAST(strftime('%s', 'now') AS INTEGER) - 86400
GROUP BY hora
ORDER BY hora;
```

Como há uma leitura por minuto, `minutos_fora` é a contagem de leituras acima de 5 °C.

**Por quanto tempo guardar? Há dado pessoal?**
**Não há dado pessoal**: só temperatura, porta e estado do equipamento. As leituras ficam guardadas por **90 dias**, tempo suficiente para cobrir intervalos entre fiscalizações e auditorias internas. Um *cron trigger* diário do Worker apaga as leituras mais antigas. Os eventos ficam por 1 ano, porque são poucos e servem de prova de que os incidentes foram tratados.

---

## Etapa 6 — Mostrar e comandar: o painel

**O que o painel mostra:**
- temperatura e umidade atuais;
- estado da porta, do compressor e do alarme;
- indicador online/offline do dispositivo;
- gráfico das últimas 24 h;
- tabela de resumo por hora (a consulta da Etapa 5);
- lista dos últimos alertas.

**Comando remoto: qual, por qual tópico ou rota:**
- `silenciar`: silencia o buzzer por 10 min, mas o alarme continua visível;
- `cancelar_silencio`: desfaz um silêncio dado sem querer (pelo painel ou pelo botão do local), e a sirene volta na hora se o alarme ainda estiver ativo. No painel, o botão só fica habilitado enquanto o dispositivo informa que está silenciado;
- `config`: ajusta os limites (também salvos no KV).

Todos são publicados em `.../comando`. Antes de publicar, o painel pede ao Worker (rota `POST /api/assinar`, que exige o PIN do gerente) a assinatura HMAC do comando. A chave HMAC fica só no Worker e no ESP32, nunca no navegador.

**Como o painel sabe que o comando foi executado? Quanto tempo espera?**
Cada comando tem um `id` único. O painel mostra **"enviado — aguardando confirmação"** e só troca para **"executado"** quando chega no tópico `.../ack` uma mensagem com o mesmo `id` e `ok: true`. Se o ack não chegar em **10 s**, mostra "dispositivo não respondeu". Se o estado retido for `offline`, o botão já avisa antes do envio que o dispositivo está offline.

**Alerta: quando dispara, por onde chega e qual o cooldown:**
- **Quando dispara:** em `temp_alta`, `porta_aberta` e `falha_sensor`, nas condições da Etapa 3.
- **Por onde chega:** o ESP32 publica em `.../alerta`. O painel mostra um banner vermelho e uma notificação do navegador, e o Worker grava o evento no D1.
- **Cooldown:** **5 min** por tipo de alerta (60 s no modo demonstração). O mesmo tipo não é republicado antes disso. Se o alarme continua ativo, sai um lembrete a cada cooldown, e a mensagem de que a situação normalizou é sempre enviada.

**Polling ou WebSocket? Por quê?**
**WebSocket** (MQTT sobre WSS) para o estado ao vivo: o painel muda sozinho em menos de 2 s, sem recarregar e sem ficar perguntando ao servidor a cada segundo. O **histórico, o resumo e a lista de eventos** vêm do D1 por HTTPS quando a página abre, a cada 60 s (o mesmo ritmo em que o ESP32 manda o lote) e ~65 s depois de cada alerta ou ack, para que eventos como "silenciado" apareçam sem recarregar a página.

---

## Etapa 7 — Proteger: segurança

| Buraco | Existe no nosso sistema? | Como fechamos (camada) |
|---|---|---|
| 1. MQTT sem TLS (porta 1883) | Sim: o EMQX Cloud vem com as portas 1883 (MQTT) e 8083 (WebSocket) abertas, sem TLS. | **Rede:** as portas 1883 e 8083 foram desligadas em *Ports Management*; só ficam a 8883 (TLS) para o ESP32 e a WSS 8084 para o painel, ambas com usuário e senha. |
| 2. `setInsecure()` no HTTPS | Existiria na conexão TLS do ESP32 com o broker. | **Dispositivo:** `setCACert()` com o certificado raiz; a conexão falha se o certificado não conferir. O ESP32 não abre nenhuma outra conexão TLS: o HTTPS até o Worker é feito pelo EMQX. |
| 3. Rota de gravação aceita qualquer um | Sim, a rota `POST /api/leituras` e o tópico `.../historico`. | **Nuvem:** o Worker exige duas provas: o cabeçalho `Authorization: Bearer <token>`, que só a regra do EMQX conhece, **e** a assinatura HMAC do lote, feita pelo ESP32. Assim, quem tiver só a senha do broker não injeta leituras falsas. Ele também valida o JSON (faixas e tipos) e recusa o resto com 401/400. |
| 4. Tópico de comando aberto | Sim, `.../comando`. | **Dispositivo + nuvem:** o broker exige credencial, e todo comando leva uma assinatura HMAC-SHA256, um `exp` (validade de 30 s) e um `id` único. O ESP32 rejeita (e responde ack com `ok:false`) comandos sem assinatura válida, expirados ou com `id` repetido. Mesmo quem tiver a senha do broker não consegue forjar um comando. A validade é comparada com o relógio do ESP32, que acerta a hora por NTP a cada 5 min e aceita até 2 min de diferença em relação ao Worker: no Wokwi a simulação às vezes roda mais devagar que o tempo real, e sem essa folga comandos legítimos eram recusados como `expirado`. Uma repetição dentro dessa folga continua bloqueada pela lista de `id`s recentes. |
| 5. Segredo no GitHub ou no Serial | Risco: senha do MQTT, token do repasse e chave HMAC. | **Código:** segredos ficam em `firmware/secrets.h` e `worker/.dev.vars`, ambos no `.gitignore`. O repositório traz só `secrets.example.h`, o Worker usa `wrangler secret`, o token do repasse fica guardado no conector do EMQX, e o Serial nunca imprime credenciais nem tokens. |

---

## Etapa 8 — Extra

**Extra escolhido e por que combina com o problema:** **Tempo real**, com o painel atualizado por WebSocket (MQTT sobre WSS). Numa câmara fria, a equipe precisa ver o alarme na hora, sem recarregar a página. O extra de energia (deep sleep) não combina, porque a câmara fica ligada na tomada e o controle do compressor exige o ESP32 sempre acordado.

**Evidência:** [PREENCHER] vídeo curto mostrando o slider de temperatura do Wokwi sendo mexido e o painel mudando sozinho, com o tempo medido entre a mudança e a atualização.

---

## Etapa 9 — Arquitetura final

| Camada | O que tem nela no nosso sistema |
|---|---|
| Dispositivo | ESP32 (Wokwi) com DHT22, HC-SR04, relé, buzzer, LED RGB e botão. Faz a decisão local com histerese, mantém o estado seguro, guarda o buffer offline, os limites na NVS e verifica o HMAC dos comandos. |
| Rede | Wi-Fi (Wokwi-GUEST); MQTT com TLS na 8883 entre o ESP32 e o EMQX Cloud; HTTPS do EMQX até o Cloudflare Worker (regra de Data Integration); WSS na 8084 entre o navegador e o broker; HTTPS entre o navegador e o Worker. |
| Nuvem | EMQX Cloud (broker); regra de Data Integration que repassa `.../historico` ao Worker; Cloudflare Worker com as rotas `/api/leituras` (gravação com token + HMAC), `/api/historico`, `/api/resumo`, `/api/eventos` e `/api/assinar`; D1 (`leituras`, `eventos`); KV (`config`, contador de PIN errado); cron diário de retenção. As rotas do painel exigem o PIN do gerente, com bloqueio depois de 5 erros. |
| Interface | Painel web servido pelo Worker com estado ao vivo, gráfico de 24 h, tabela de resumo, alertas, botão de silenciar e formulário de limites com confirmação por ack. |

**Desenho ou print do sistema:** [PREENCHER] print do diagrama no Wokwi e diagrama de arquitetura.

```
[DHT22]  [HC-SR04]  [Botão]
    \        |        /
     ---- ESP32 (decide) ---- [Relé/compressor] [Buzzer] [LED RGB]
              |  MQTT/TLS 8883 (telemetria, estado, alerta, histórico assinado; comando/ack)
              v
        EMQX Cloud  <------- WSS 8084 -------  Painel (navegador)
              |  regra: .../historico                 |  HTTPS + PIN
              v  HTTPS + token                        v
        Cloudflare Worker  <--------------------------+
              |--> D1 (leituras, eventos)
              |--> KV (limites, contador de PIN)
```

---

## Etapa 10 — Testes antes de apresentar

| Teste | Como fizemos | Resultado |
|---|---|---|
| Isolado: cada peça sozinha | Sketch de teste: ler o DHT22 e o HC-SR04 no Serial, ligar o relé, o buzzer e cada cor do LED; testar o Worker com `curl`; ver as mensagens do broker no cliente MQTTX. | [PREENCHER] |
| No limite: forçar o sensor | Slider do DHT22 em 4,1 / 1,9 / 5,1 °C e −10 / 60 °C; ultrassônico em 9 e 11 cm. | [PREENCHER] |
| Na falha: tirar o sensor ou o Wi-Fi | Desconectar o fio de dados do DHT22 no diagrama (estado seguro); trocar o SSID para um inexistente e ver se o controle continua, o buffer enche e o envio acontece quando volta. | [PREENCHER] |
| No reinício: tirar e ligar o USB | Reiniciar a simulação e conferir se os limites voltam da NVS, se o estado seguro vale até a primeira leitura e se o painel mostra offline → online. | [PREENCHER] |
| No ataque: comando falso | Publicar um `silenciar` sem assinatura, com assinatura errada, expirado e repetido (replay) pelo cliente MQTTX; chamar `/api/leituras` sem token, e com token mas com o lote sem assinatura ou com assinatura errada. | [PREENCHER] |
| Na duração: 10 min ligado | Deixar a simulação rodando 10 min: contar as leituras no D1 (~10), conferir que não há reconexões nem travamentos e acompanhar a memória livre no Serial. | [PREENCHER] |

---

## Etapa 11 — Apresentação e entrega

**Roteiro (4 min):**
1. **Problema** (30 s): a frase da ideia e a RDC 216 (≤ 5 °C).
2. **Arquitetura** (45 s): o diagrama da Etapa 9.
3. **Escolhas** (60 s): decisão na borda (sem internet), MQTT com TLS, D1 + KV, comando com HMAC + ack.
4. **Demonstração** (75 s): esquentar a câmara → compressor liga → alarme → alerta no painel → silenciar pelo painel → ack; depois, um comando falso rejeitado.
5. **O que falta para virar produto** (30 s): ver abaixo.

**Respostas preparadas:**
- *"E se a internet cair?"* O controle e o alarme são locais, as leituras ficam no buffer e são enviadas quando a rede volta.
- *"E se alguém publicar no seu tópico?"* Precisa de credencial do broker e, mesmo com ela, sem a assinatura HMAC o ESP32 rejeita o comando.
- *"E com 1.000 unidades?"* Um tópico por câmara (`<cliente>/<camaraNN>`), credencial e chave HMAC próprias para cada dispositivo, provisionamento de fábrica, atualização remota (OTA) e alertas agregados no painel com wildcard.

**O que falta para virar produto:** sonda de temperatura à prova d'água (DS18B20) no lugar do DHT22; nobreak ou bateria para alarmar a falta de energia; buffer na flash; certificado de cliente por dispositivo; OTA; caixa IP65. Custo estimado do hardware: [PREENCHER] (ESP32 + sensores + relé).

**Checklist de entrega (até 24/11):**
- [ ] Caderno preenchido (.docx ou PDF) em `docs/` e no Classroom
- [ ] Código do ESP32 e do Worker no GitHub, sem senhas nem chaves
- [ ] README curto: o que faz, como montar e como testar
- [ ] Print ou vídeo curto do sistema funcionando
