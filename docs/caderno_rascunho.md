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
| RF4 | Executar comandos remotos (silenciar o alarme, ajustar limites) e responder com ack em até 3 s, rejeitando comandos sem assinatura válida, expirados ou repetidos. | Enviar um comando pelo painel e medir o tempo até o ack; publicar um comando falso no tópico e ver a rejeição. | O gerente consegue agir de longe, mas um estranho não consegue desligar o alarme. |

| Código | Requisito não funcional (como e quanto) |
|---|---|
| RNF1 | **Sem internet:** controle do compressor e alarme local continuam funcionando. As leituras ficam em buffer (até 360 leituras = 6 h) e são enviadas em lote quando a rede volta. |
| RNF2 | **Segurança:** toda comunicação usa TLS com verificação do certificado do servidor; a gravação no histórico exige token; nenhum segredo fica no repositório ou no Serial. |
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

**Onde a regra roda e por quê:** no **ESP32 (borda)**. Ele precisa reagir mesmo se a internet cair (restrição do projeto) e não pode depender da rede para ligar o compressor. A nuvem só guarda o histórico, mostra o painel e ajusta os limites. Como os limites são ajustados por alguém de fora, eles também ficam guardados no KV, e o ESP32 os lê ao ligar.

**O que acontece se a internet cair? E quando ela voltar?**
- **Durante a queda:** compressor, alarme, buzzer e LED continuam funcionando normalmente. As leituras de 60 s vão para um buffer circular na RAM (até 360 leituras = 6 h), com o horário que o ESP32 obteve por NTP quando ainda estava online. No broker, a *last will* do MQTT marca o dispositivo como `offline`, e o painel mostra isso.
- **Quando volta:** o ESP32 reconecta (com espera crescente entre as tentativas), publica `online` no tópico de estado, envia o buffer em lotes ao Worker e publica os alertas que ocorreram durante a queda.
- **Limitação conhecida:** se a energia cair junto, o buffer na RAM se perde. Num produto real, ele iria para a flash.

---

## Etapa 4 — Comunicar: protocolo, tópicos e mensagem

**Protocolos escolhidos e por quê:**
- **MQTT com TLS (8883)** entre o ESP32 e o broker (HiveMQ Cloud): telemetria e alertas em tempo real e comandos chegando ao ESP32 sem ele precisar perguntar.
- **MQTT sobre WebSocket seguro (WSS, 8884)** no painel: o navegador recebe as mudanças sozinho (é o Extra de tempo real).
- **HTTPS para o Cloudflare Worker:** o ESP32 grava as leituras no D1 por uma rota com token, e o painel consulta o histórico e o resumo.

**Árvore de tópicos** (prefixo `sis1a/gabrieltelles/camara01/`):

| Tópico | Quem publica | Quem assina | QoS / retain |
|---|---|---|---|
| `.../telemetria` | ESP32 (a cada 10 s) | Painel | QoS 0 |
| `.../estado` | ESP32 (ao mudar e ao conectar; `offline` pela last will) | Painel | QoS 0, retain (last will: QoS 1, retain) |
| `.../comando` | Painel | ESP32 (assina com QoS 1) | QoS 1 |
| `.../ack` | ESP32 | Painel | QoS 0 |
| `.../alerta` | ESP32 (com cooldown) | Painel | QoS 0 |

A biblioteca PubSubClient do ESP32 só publica com QoS 0. Isso é compensado assim: o estado fica retido no broker; os alertas também vão para o D1 pelo HTTPS e só saem do buffer depois da confirmação do Worker; e o painel espera o ack e, se ele não vier, mostra que não houve confirmação.

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
Porta **8883**. O ESP32 usa `WiFiClientSecure` com `setCACert()` e um pacote com os certificados raiz das autoridades que assinam o broker (ISRG Root X1 e X2, do Let's Encrypt) e o Worker (GTS Root R1 e R4, do Google Trust Services). As cadeias foram conferidas com `openssl s_client` em 03/11/2026. Assim ele confere se está falando com o broker verdadeiro. **Não** é usado `setInsecure()`. O HTTPS para o Worker usa o mesmo método, com o certificado raiz do domínio `workers.dev`.

---

## Etapa 5 — Guardar: o histórico

**KV, D1 ou os dois? Por quê?** Os dois.
- **D1:** cada leitura é um registro, e é preciso responder perguntas sobre o passado (máxima por hora, tempo fora da faixa).
- **KV:** guarda o último estado e a configuração dos limites, que são lidos pela chave exata.

**Estrutura:**

```sql
CREATE TABLE leituras (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  disp TEXT NOT NULL,
  ts INTEGER NOT NULL,          -- unix epoch (UTC)
  temp_c REAL,
  umid_pct REAL,
  porta_aberta INTEGER,         -- 0/1
  compressor INTEGER,           -- 0/1
  alarme INTEGER,               -- 0/1
  sensor_ok INTEGER             -- 0/1
);
CREATE INDEX idx_leituras_disp_ts ON leituras(disp, ts);

CREATE TABLE eventos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  disp TEXT NOT NULL,
  ts INTEGER NOT NULL,
  tipo TEXT NOT NULL,           -- temp_alta, porta_aberta, falha_sensor, comando, normalizado
  detalhe TEXT
);
```

Chaves no KV:
- `config:camara01` → `{"liga_c":4.0,"desliga_c":2.0,"alarme_c":5.0,"normal_c":4.5,"alarme_min":5,"porta_min":2}`
- `estado:camara01` → última telemetria recebida

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
- `config`: ajusta os limites (também salvos no KV).

Ambos são publicados em `.../comando`. Antes de publicar, o painel pede ao Worker (rota `POST /api/assinar`, que exige o PIN do gerente) a assinatura HMAC do comando. A chave HMAC fica só no Worker e no ESP32, nunca no navegador.

**Como o painel sabe que o comando foi executado? Quanto tempo espera?**
Cada comando tem um `id` único. O painel mostra **"enviado — aguardando confirmação"** e só troca para **"executado"** quando chega no tópico `.../ack` uma mensagem com o mesmo `id` e `ok: true`. Se o ack não chegar em **10 s**, mostra "dispositivo não respondeu". Se o estado retido for `offline`, o botão já avisa antes do envio que o dispositivo está offline.

**Alerta: quando dispara, por onde chega e qual o cooldown:**
- **Quando dispara:** em `temp_alta`, `porta_aberta` e `falha_sensor`, nas condições da Etapa 3.
- **Por onde chega:** o ESP32 publica em `.../alerta`. O painel mostra um banner vermelho e uma notificação do navegador, e o Worker grava o evento no D1.
- **Cooldown:** **5 min** por tipo de alerta (60 s no modo demonstração). O mesmo tipo não é republicado antes disso. Se o alarme continua ativo, sai um lembrete a cada cooldown, e a mensagem de que a situação normalizou é sempre enviada.

**Polling ou WebSocket? Por quê?**
**WebSocket** (MQTT sobre WSS) para o estado ao vivo: o painel muda sozinho em menos de 2 s, sem recarregar e sem ficar perguntando ao servidor a cada segundo. O **histórico e o resumo** são consultados por HTTPS quando a página abre e a cada 5 min, porque mudam devagar.

---

## Etapa 7 — Proteger: segurança

| Buraco | Existe no nosso sistema? | Como fechamos (camada) |
|---|---|---|
| 1. MQTT sem TLS (porta 1883) | Existiria se usássemos um broker público. | **Rede:** só a porta 8883 (TLS) no ESP32 e WSS 8884 no painel, com usuário e senha no HiveMQ Cloud. |
| 2. `setInsecure()` no HTTPS | Existiria no HTTPS para o Worker e no MQTT. | **Dispositivo:** `setCACert()` com o certificado raiz; a conexão falha se o certificado não conferir. |
| 3. Rota de gravação aceita qualquer um | Sim, a rota `POST /api/leituras`. | **Nuvem:** o Worker exige o cabeçalho `Authorization: Bearer <token>`, guardado como *secret* no Cloudflare, valida o JSON (faixas e tipos) e recusa o resto com 401/400. |
| 4. Tópico de comando aberto | Sim, `.../comando`. | **Dispositivo + nuvem:** o broker exige credencial, e todo comando leva uma assinatura HMAC-SHA256, um `exp` (validade de 30 s) e um `id` único. O ESP32 rejeita (e responde ack com `ok:false`) comandos sem assinatura válida, expirados ou com `id` repetido. Mesmo quem tiver a senha do broker não consegue forjar um comando. |
| 5. Segredo no GitHub ou no Serial | Risco: senha do Wi-Fi, do MQTT, token e chave HMAC. | **Código:** segredos ficam em `firmware/secrets.h` e `worker/.dev.vars`, ambos no `.gitignore`. O repositório traz só `secrets.example.h`, o Worker usa `wrangler secret`, e o Serial nunca imprime credenciais nem tokens. |

---

## Etapa 8 — Extra

**Extra escolhido e por que combina com o problema:** **Tempo real**, com o painel atualizado por WebSocket (MQTT sobre WSS). Numa câmara fria, a equipe precisa ver o alarme na hora, sem recarregar a página. O extra de energia (deep sleep) não combina, porque a câmara fica ligada na tomada e o controle do compressor exige o ESP32 sempre acordado.

**Evidência:** [PREENCHER] vídeo curto mostrando o slider de temperatura do Wokwi sendo mexido e o painel mudando sozinho, com o tempo medido entre a mudança e a atualização.

---

## Etapa 9 — Arquitetura final

| Camada | O que tem nela no nosso sistema |
|---|---|
| Dispositivo | ESP32 (Wokwi) com DHT22, HC-SR04, relé, buzzer, LED RGB e botão. Faz a decisão local com histerese, mantém o estado seguro, guarda o buffer offline, os limites na NVS e verifica o HMAC dos comandos. |
| Rede | Wi-Fi (Wokwi-GUEST); MQTT com TLS na 8883 até o HiveMQ Cloud; HTTPS até o Cloudflare Worker; WSS na 8884 entre o navegador e o broker. |
| Nuvem | HiveMQ Cloud (broker); Cloudflare Worker com as rotas `/api/leituras` (gravação com token), `/api/historico`, `/api/resumo`, `/api/config` e `/api/assinar`; D1 (`leituras`, `eventos`); KV (`config`, `estado`); cron diário de retenção. |
| Interface | Painel web servido pelo Worker com estado ao vivo, gráfico de 24 h, tabela de resumo, alertas, botão de silenciar e formulário de limites com confirmação por ack. |

**Desenho ou print do sistema:** [PREENCHER] print do diagrama no Wokwi e diagrama de arquitetura.

```
[DHT22]  [HC-SR04]  [Botão]
    \        |        /
     ---- ESP32 (decide) ---- [Relé/compressor] [Buzzer] [LED RGB]
              |  MQTT/TLS 8883            | HTTPS + token
              v                           v
        HiveMQ Cloud  <--- WSS 8884 ---  Painel (navegador)
                                          |  HTTPS
                                          v
                              Cloudflare Worker --> D1 (histórico)
                                                --> KV (config/estado)
```

---

## Etapa 10 — Testes antes de apresentar

| Teste | Como fizemos | Resultado |
|---|---|---|
| Isolado: cada peça sozinha | Sketch de teste: ler o DHT22 e o HC-SR04 no Serial, ligar o relé, o buzzer e cada cor do LED; testar o Worker com `curl`; ver as mensagens do broker no cliente web do HiveMQ. | [PREENCHER] |
| No limite: forçar o sensor | Slider do DHT22 em 4,1 / 1,9 / 5,1 °C e −10 / 60 °C; ultrassônico em 9 e 11 cm. | [PREENCHER] |
| Na falha: tirar o sensor ou o Wi-Fi | Desconectar o fio de dados do DHT22 no diagrama (estado seguro); trocar o SSID para um inexistente e ver se o controle continua, o buffer enche e o envio acontece quando volta. | [PREENCHER] |
| No reinício: tirar e ligar o USB | Reiniciar a simulação e conferir se os limites voltam da NVS, se o estado seguro vale até a primeira leitura e se o painel mostra offline → online. | [PREENCHER] |
| No ataque: comando falso | Publicar um `silenciar` sem assinatura, com assinatura errada, expirado e repetido (replay) pelo cliente web do HiveMQ; chamar `/api/leituras` sem token. | [PREENCHER] |
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
