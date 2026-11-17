# Registro dos testes (Etapa 10)

Evidências brutas usadas para preencher a Etapa 10 do caderno.

## No ataque — Worker (05/11/2026)

Chamadas feitas com `curl` contra `https://frioseguro.deisab12.workers.dev`. O "lote forjado" é uma leitura falsa de 3,0 °C, como faria alguém tentando esconder que a câmara esquentou.

| Ataque | Resposta do Worker |
|---|---|
| Gravar sem token | `401 nao_autorizado` |
| Gravar com token errado | `401 nao_autorizado` |
| Token certo, lote sem assinatura | `401 sem_assinatura` |
| Token certo, lote assinado com chave adivinhada | `401 assinatura_invalida` |
| Ler histórico sem PIN | `401 pin_invalido` |
| Pedir assinatura de comando sem PIN | `401 pin_invalido` |
| 5 PINs errados seguidos | `401 pin_invalido` em todos |
| 6ª tentativa, **mesmo com o PIN certo** | `429 bloqueado` (10 min) |

Conferência no D1 logo depois: `SELECT COUNT(*) FROM leituras WHERE ts > unixepoch()-120 AND temp_c = 3.0` → **0**. Nenhuma leitura forjada entrou no banco.

## Rodada A — Wokwi, simulação contínua iniciada às 22:01 (05/11/2026)

Trechos do Serial (o log completo foi colado no chat do desenvolvimento).

| Passo | O que foi feito | Serial (resumo) | Resultado |
|---|---|---|---|
| Boot | Simulação reiniciada com o DHT22 em 14,6 °C | `[regra] T=14.6C acima de 5.0C...` aparece **antes** de `[wifi] conectado`; `[alerta] temp_alta ativo=1` 30 s depois | ✅ a regra roda na borda, antes de haver rede |
| Compressor (histerese) | 1,9 °C → 4,0 °C → 4,1 °C | `comp=off` a 1,9 e a 4,0 (só liga **acima** de 4,0); `comp=ON` a 4,1 | ✅ |
| Alarme no limite | 5,1 °C | `[regra] T=5.1C ... alarme em 30s`; `[alerta] temp_alta ativo=1` 30 s depois | ✅ |
| Histerese do alarme | 4,8 °C, depois 4,4 °C | a 4,8 continua `alarme=SIM`; a 4,4 `[alerta] temp_alta ativo=0` | ✅ só normaliza abaixo de 4,5 |
| Porta | ultrassônico em 11 cm, depois 5 cm | `[regra] porta aberta: alarme em 20s`; `[alerta] porta_aberta ativo=1`; ao fechar, `ativo=0` | ✅ |
| Comando com ack | 5,5 °C, alarme; **Silenciar** e **Cancelar silêncio** no painel | `[comando] executado: silenciar (...)`; `[comando] executado: cancelar_silencio (...)` | ✅ painel mostrou "executado pelo dispositivo" |
| Extremos | −19,2 / −10 °C e 80 / 60 °C (máximo do slider) | frio: `comp=off`, sem alarme; quente: `comp=ON` e `temp_alta ativo=1` após 30 s | ✅ sem travar nem leitura inválida |
| Histórico | durante toda a rodada | `[historico] publicado: 1 leituras, N eventos` a cada minuto; `buffer` volta a 0 | ✅ |

## No ataque — comandos MQTT falsos (05/11/2026, 22:14–22:17)

Publicados no tópico `sis1a/gabrieltelles/camara01/comando` pelo MQTTX Web, conectado como o usuário `painel` (ou seja, **com** a senha do broker). Respostas lidas no tópico `.../ack`.

| Comando publicado | Ack do ESP32 | Resultado |
|---|---|---|
| Mensagem vazia | `{"ok":false,"motivo":"json_invalido"}` | ✅ recusado |
| Sem assinatura (5 vezes) | `{"ok":false,"motivo":"sem_assinatura"}` | ✅ recusado |
| Assinatura errada | `{"id":"c-ataque2-e01e69","ok":false,"motivo":"assinatura_invalida"}` | ✅ recusado |
| Assinatura válida, vencido há 10 min | `{"id":"c-ataque3-659343","ok":false,"motivo":"expirado"}` | ✅ recusado |
| Válido (`cancelar_silencio`) | `{"id":"c-ataque4-fff8e7","ok":true}` — publicado 22:16:52.777, ack 22:16:53.271 (**~0,5 s**) | ✅ executado; comprova o RF4 (ack ≤ 3 s) |
| Replay: comando válido `c-replay-f88234` publicado de novo | 1ª vez (22:18:01): `{"ok":true}`; 2ª vez (22:20:03): `{"id":"c-replay-f88234","ok":false,"motivo":"repetido"}` | ✅ quem copia um comando legítimo não consegue reutilizá-lo |

Evidência: `docs/img/ataque_mqtt_sem_assinatura.png`.

## Na duração — 15 min contínuos (22:01 → 22:16)

Consulta no D1 depois da rodada:

```sql
SELECT COUNT(*) AS leituras, MIN(...) AS primeira, MAX(...) AS ultima, SUM(sensor_ok=0) AS invalidas
FROM leituras WHERE ts >= <22:01>;
```

| Leituras | Primeira | Última | Inválidas |
|---|---|---|---|
| **16** | 22:01:35 | 22:16:49 | 0 |

Uma leitura por minuto, sem buraco, durante 15 min (16 pontos). Todos os **28 eventos** da rodada chegaram ao banco, em ordem: alarmes de temperatura e porta (início e fim), `comando_silenciar` / `silenciado_remoto`, `comando_cancelar_silencio` / `silencio_cancelado` e **11 `comando_rejeitado`** dos ataques MQTT. Durante a rodada inteira o Serial mostrou `wifi=1 mqtt=1`, sem reconexões nem travamentos.

## Na falha — internet cai (17/11/2026, 19:39 → 19:46)

Para simular a queda, o ESP32 foi bloqueado no broker: EMQX → Access Control → **Blacklist**, Client ID `frioseguro-camara01`. Para o dispositivo, o efeito é o mesmo de perder a internet (não alcança mais a nuvem).

| O que aconteceu | Evidência | Resultado |
|---|---|---|
| ESP32 perde o broker | `[mqtt] conectando com TLS... falhou (estado 5)` (5 = não autorizado), `mqtt=0` | ✅ |
| Reconexão com espera crescente | novas tentativas em 2 s, 4 s, 8 s, 16 s, 32 s, 60 s, 60 s... | ✅ não fica martelando o broker |
| Controle local continua sem rede | DHT22 em 9,5 → 6,0 °C: `[regra] T=9.5C acima de 5.0C...` e depois `alarme=SIM`, LED vermelho e sirene; de volta a 3,0 °C, `alarme=nao` | ✅ **decide na borda** |
| Leituras guardadas | `buffer=` sobe 1, 2, 3, 4, 5 durante a queda | ✅ |
| Volta da rede (bloqueio removido) | `[mqtt] ... ok`, `[alerta] temp_alta ativo=1`, `[alerta] temp_alta ativo=0`, `[historico] publicado: 6 leituras, 2 eventos`, `buffer=0` | ✅ |
| Dados no D1 | 6 leituras no intervalo da queda (uma delas a 6,0 °C com `alarme=1`) e os eventos `temp_alta` início/fim | ✅ nada perdido |

**Observação:** no D1 as leituras da queda aparecem com horários de 19:41 a 19:46, alguns minutos depois do real. Não houve perda (a contagem bate com o lote publicado). O ESP32 calcula o horário de cada leitura guardada como "hora atual − tempo decorrido desde a medição", e no Wokwi o tempo da simulação anda mais devagar que o relógio real. Numa placa real os dois relógios andam juntos. Para um produto, o horário seria gravado no momento da medição (com o relógio já sincronizado) em vez de calculado na hora do envio.

Evidências: `docs/img/falha_rede_blacklist_emqx.png`, `docs/img/falha_rede_serial_mqtt0.png`, `docs/img/falha_rede_alarme_local.png`.

## No reinício (17/11/2026, ~19:50)

Antes de reiniciar, o limite "liga compressor acima de" foi trocado de 4,0 para 4,5 °C pelo painel.

| O que foi verificado | Resultado |
|---|---|
| Comando `config` pelo painel | ✅ "Novos limites executado pelo dispositivo (confirmado em **501 ms**)" |
| Compressor no boot | ✅ `comp=ON` desde a primeira linha (estado seguro até a primeira leitura) |
| Reconexão e painel | ✅ Wi-Fi, MQTT e histórico voltam sozinhos; o painel volta a receber leituras sem recarregar |
| Limite 4,5 °C depois do reinício | ❌ o boot mostrou `limites: liga>4.0` |

**Análise:** o firmware grava os limites na NVS (memória flash) com `Preferences`, que numa placa real sobrevive a reinícios. O **Wokwi apaga a flash a cada reinício da simulação**, então no simulador os limites voltam ao padrão. Limitação do simulador, não da lógica.

**Defeito encontrado e corrigido:** depois do reinício o painel continuava mostrando 4,5 no formulário, embora o ESP32 estivesse usando 4,0. O painel deixava de atualizar o formulário enquanto o foco estava nele (para não apagar o que o usuário digita), e o botão "Enviar limites" ficava com o foco depois do clique. Agora o formulário só deixa de atualizar quando um **campo de texto** está selecionado; assim ele sempre mostra o limite que o dispositivo informa no tópico `estado`.

Evidência: `docs/img/reinicio_painel_serial.png`.

## Na falha — sensor com defeito (17/11/2026, ~19:57)

O fio de dados do DHT22 (`dht1:SDA` → `esp:15`) foi removido do `diagram.json` e a simulação reiniciada.

| O que foi verificado | Evidência | Resultado |
|---|---|---|
| Leitura inválida detectada | Serial: `T=nanC ... sensor=FALHA` | ✅ |
| Estado seguro | `comp=ON` (compressor forçado ligado: na dúvida, manter frio) | ✅ |
| Alarme local | `alarme=SIM`, LED vermelho piscando e sirene | ✅ |
| Alerta remoto | `[alerta] falha_sensor ativo=1`; painel com banner **"Alarme: Falha no sensor"**, temperatura **FALHA** e evento "Falha no sensor" às 19:57 | ✅ |
| Histórico | leituras gravadas com `temp_c = NULL` e `sensor_ok = 0` (ficam fora do resumo por hora) | ✅ conferido no D1: leitura de 20:01:11 com `temp_c = NULL`, `sensor_ok = 0`, `compressor = 1`, `alarme = 1`, e evento `falha_sensor` (início) às 20:00:15. Com a falha mantida, o alerta é repetido a cada cooldown (60 s no modo demonstração) |

Evidência: `docs/img/falha_sensor_estado_seguro.png`.
