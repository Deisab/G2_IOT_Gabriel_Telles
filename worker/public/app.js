// FrioSeguro — painel
// Estado ao vivo por MQTT sobre WebSocket seguro (WSS 8084); histórico e assinatura pelo Worker.

const MQTT_URL = "wss://zeebdd7a.ala.dedicated.aws.emqxcloud.com:8084/mqtt";
const MQTT_USER = "painel";
const DISP = "camara01";
const BASE = `sis1a/gabrieltelles/${DISP}`;
const T = {
  telemetria: `${BASE}/telemetria`, estado: `${BASE}/estado`,
  comando: `${BASE}/comando`, ack: `${BASE}/ack`, alerta: `${BASE}/alerta`,
};
const ACK_TIMEOUT_MS = 10000;
const ATUALIZA_HIST_MS = 60 * 1000;     // mesmo ritmo do lote de histórico do ESP32
const ATRASO_APOS_EVENTO_MS = 65 * 1000; // tempo para o lote com o evento chegar ao D1

const $ = (id) => document.getElementById(id);
const fmtHora = (ts) => new Date(ts * 1000).toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit", second: "2-digit", timeZone: "America/Sao_Paulo" });
const fmtTemp = (v) => (v === null || v === undefined ? "—" : `${Number(v).toFixed(1)} °C`);

const NOMES = {
  temp_alta: "Temperatura alta", porta_aberta: "Porta aberta", falha_sensor: "Falha no sensor",
  silenciado_local: "Silenciado no local", silenciado_remoto: "Silenciado pelo painel",
  silencio_cancelado: "Silêncio cancelado (sirene reativada)",
  comando_cancelar_silencio: "Painel: pedido de cancelar o silêncio",
  config_alterada: "Limites alterados", comando_rejeitado: "Comando rejeitado",
  comando_silenciar: "Painel: pedido de silenciar", comando_config: "Painel: pedido de novos limites",
};

let pin = "";
let cliente = null;
let dispOnline = false;
let estadoAtual = null;
let grafico = null;
const pendentes = new Map();   // id -> { cmd, enviadoEm, timer }

// ------------------------------------------------------------------ armazenamento da sessão
function guardar(k, v) { try { sessionStorage.setItem(k, v); } catch {} }
function ler(k) { try { return sessionStorage.getItem(k); } catch { return null; } }

// ------------------------------------------------------------------ API do Worker
async function api(caminho, opcoes = {}) {
  const r = await fetch(caminho, { ...opcoes, headers: { "content-type": "application/json", "x-pin": pin, ...(opcoes.headers || {}) } });
  const corpo = await r.json().catch(() => ({}));
  if (!r.ok) throw Object.assign(new Error(corpo.erro || `HTTP ${r.status}`), { status: r.status, corpo });
  return corpo;
}

// ------------------------------------------------------------------ login
$("form-login").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  $("login-erro").textContent = "";
  pin = $("in-pin").value;
  try {
    await api("/api/login");
  } catch (e) {
    $("login-erro").textContent = e.status === 429 ? "Muitas tentativas. Aguarde 10 minutos." : "PIN incorreto.";
    return;
  }
  guardar("pin", pin);
  guardar("mqtt", $("in-mqtt").value);
  if ("Notification" in window && Notification.permission === "default") Notification.requestPermission();
  iniciar($("in-mqtt").value);
});

function iniciar(senhaMqtt) {
  $("login").hidden = true;
  $("app").hidden = false;
  conectarMqtt(senhaMqtt);
  carregarHistorico();
  setInterval(carregarHistorico, ATUALIZA_HIST_MS);
}

// ------------------------------------------------------------------ MQTT
function selo(id, texto, classe) { const s = $(id); s.textContent = texto; s.className = `selo ${classe || ""}`; }

function conectarMqtt(senha) {
  cliente = mqtt.connect(MQTT_URL, {
    username: MQTT_USER, password: senha,
    clientId: `painel-${Math.random().toString(16).slice(2, 10)}`,
    clean: true, reconnectPeriod: 3000, connectTimeout: 10000,
  });
  cliente.on("connect", () => {
    selo("selo-mqtt", "MQTT: conectado (TLS)", "ok");
    cliente.subscribe([T.telemetria, T.estado, T.ack, T.alerta], { qos: 1 });
  });
  cliente.on("reconnect", () => selo("selo-mqtt", "MQTT: reconectando…", "mal"));
  cliente.on("close", () => selo("selo-mqtt", "MQTT: desconectado", "mal"));
  cliente.on("error", (e) => {
    selo("selo-mqtt", e?.code === 5 || e?.code === 4 ? "MQTT: senha recusada" : "MQTT: erro", "mal");
    if (e?.code === 5 || e?.code === 4) cliente.end(true);
  });
  cliente.on("message", (topico, msg) => {
    let d; try { d = JSON.parse(msg.toString()); } catch { return; }
    if (topico === T.telemetria) aoTelemetria(d);
    else if (topico === T.estado) aoEstado(d);
    else if (topico === T.ack) aoAck(d);
    else if (topico === T.alerta) aoAlerta(d);
  });
}

function valor(id, texto, classe) { const el = $(id); el.textContent = texto; el.className = `valor ${classe || ""}`; }

function aoTelemetria(d) {
  const lim = estadoAtual?.config?.alarme_c ?? 5;
  const atencao = estadoAtual?.config?.normal_c ?? 4.5;
  valor("k-temp", d.sensor_ok ? fmtTemp(d.temp_c) : "FALHA",
        !d.sensor_ok || d.temp_c > lim ? "perigo" : d.temp_c > atencao ? "atencao" : "ok");
  valor("k-umid", d.umid_pct === null ? "—" : `${Math.round(d.umid_pct)} %`);
  valor("k-porta", d.porta_aberta ? "Aberta" : "Fechada", d.porta_aberta ? "atencao" : "ok");
  valor("k-comp", d.compressor ? "Ligado" : "Desligado");
  $("ultima").textContent = d.ts ? `Última leitura: ${fmtHora(d.ts)}` : "Última leitura: agora (sem hora no dispositivo)";
  if (grafico && d.sensor_ok && d.temp_c !== null && d.ts) {
    grafico.data.datasets[0].data.push({ x: d.ts * 1000, y: d.temp_c });
    if (grafico.data.datasets[0].data.length > 6000) grafico.data.datasets[0].data.shift();
    grafico.update("none");
  }
}

function aoEstado(d) {
  dispOnline = d.online === true;
  selo("selo-disp", dispOnline ? "Dispositivo: online" : "Dispositivo: OFFLINE", dispOnline ? "ok" : "mal");
  $("bt-silenciar").disabled = !dispOnline;
  $("bt-reativar").disabled = !dispOnline || !d.silenciado;   // só faz sentido se estiver silenciado
  if (!dispOnline) { mostrarBanner("Dispositivo offline: o controle local continua, mas comandos não serão executados até ele voltar."); return; }
  estadoAtual = d;
  const txt = d.alarme ? (d.silenciado ? "Ativo (silenciado)" : "ATIVO") : "Normal";
  valor("k-alarme", txt, d.alarme ? "perigo" : "ok");
  if (d.alarme) {
    const quais = Object.entries(d.alarmes || {}).filter(([, v]) => v).map(([k]) => NOMES[k] || k).join(", ");
    mostrarBanner(`Alarme: ${quais}${d.silenciado ? " — sirene silenciada" : ""}`);
  } else esconderBanner();
  if (d.config) preencherConfig(d.config);
  if (grafico && d.config) { grafico.data.datasets[1].data = linhaLimite(d.config.alarme_c); grafico.update("none"); }
}

// eventos como "silenciado" e "comando rejeitado" só chegam pelo D1: busca de novo
// quando o lote que os contém já deve ter sido gravado
let timerAtualizacao = null;
function agendarAtualizacao() {
  clearTimeout(timerAtualizacao);
  timerAtualizacao = setTimeout(carregarHistorico, ATRASO_APOS_EVENTO_MS);
}

const alertasAoVivo = [];   // chegaram pelo MQTT e talvez ainda não estejam no D1

function aoAlerta(d) {
  agendarAtualizacao();
  const e = { ts: d.ts || Math.floor(Date.now() / 1000), tipo: d.tipo, ativo: d.ativo ? 1 : 0, temp_c: d.temp_c, origem: "ao vivo" };
  alertasAoVivo.push(e);
  if (alertasAoVivo.length > 20) alertasAoVivo.shift();
  adicionarEvento(e, true);
  if (d.ativo && "Notification" in window && Notification.permission === "granted")
    new Notification("FrioSeguro", { body: `${NOMES[d.tipo] || d.tipo}: ${d.msg || ""}` });
}

// ------------------------------------------------------------------ comandos com confirmação (ack)
const ROTULO_CMD = { silenciar: "Silenciar", cancelar_silencio: "Cancelar silêncio", config: "Novos limites" };
const rotulo = (cmd) => ROTULO_CMD[cmd] || cmd;

function statusCmd(texto, classe) { const s = $("cmd-status"); s.textContent = texto; s.className = `cmd-status ${classe || ""}`; }

async function enviarComando(cmd, args) {
  if (!cliente?.connected) { statusCmd("Sem conexão com o broker. Comando não enviado.", "mal"); return; }
  if (!dispOnline) { statusCmd("Dispositivo offline: comando não enviado.", "mal"); return; }
  let assinado;
  try {
    assinado = await api("/api/assinar", { method: "POST", body: JSON.stringify({ disp: DISP, cmd, args }) });
  } catch (e) {
    statusCmd(e.message === "config_invalida" ? "Limites inválidos (ex.: desliga precisa ser menor que liga)." : `Falha ao assinar: ${e.message}`, "mal");
    return;
  }
  const { id, p, sig } = assinado;
  const timer = setTimeout(() => {
    pendentes.delete(id);
    statusCmd(`"${rotulo(cmd)}" não confirmado em ${ACK_TIMEOUT_MS / 1000} s: o dispositivo não respondeu.`, "mal");
  }, ACK_TIMEOUT_MS);
  pendentes.set(id, { cmd, enviadoEm: performance.now(), timer });
  cliente.publish(T.comando, JSON.stringify({ p, sig }), { qos: 1 });
  statusCmd(`"${rotulo(cmd)}" enviado — aguardando confirmação do dispositivo…`, "espera");
}

function aoAck(d) {
  const pend = pendentes.get(d.id);
  if (!pend) return;   // ack de outro painel ou de comando já expirado
  clearTimeout(pend.timer);
  pendentes.delete(d.id);
  agendarAtualizacao();
  const ms = Math.round(performance.now() - pend.enviadoEm);
  if (d.ok) statusCmd(`"${rotulo(pend.cmd)}" executado pelo dispositivo (confirmado em ${ms} ms).`, "ok");
  else statusCmd(`"${rotulo(pend.cmd)}" rejeitado pelo dispositivo: ${d.motivo}.`, "mal");
}

$("bt-silenciar").addEventListener("click", () => enviarComando("silenciar"));
$("bt-reativar").addEventListener("click", () => enviarComando("cancelar_silencio"));

$("form-config").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target), args = {};
  for (const [k, v] of f.entries()) if (v !== "") args[k] = k.endsWith("_s") ? parseInt(v, 10) : parseFloat(v);
  enviarComando("config", args);
});

function preencherConfig(c) {
  const form = $("form-config");
  if (form.contains(document.activeElement)) return;   // não sobrescreve o que o usuário está digitando
  for (const k of Object.keys(c)) if (form.elements[k]) form.elements[k].value = c[k];
}

// ------------------------------------------------------------------ banner e eventos
function mostrarBanner(t) { const b = $("banner"); b.textContent = t; b.hidden = false; }
function esconderBanner() { $("banner").hidden = true; }

function adicionarEvento(e, noTopo) {
  const li = document.createElement("li");
  const q = document.createElement("span"); q.className = "quando";
  q.textContent = new Date(e.ts * 1000).toLocaleString("pt-BR", { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit", timeZone: "America/Sao_Paulo" });
  const nome = NOMES[e.tipo] || e.tipo;
  const fase = e.tipo.startsWith("comando_") || e.tipo.startsWith("silenci") || e.tipo === "config_alterada" ? "" : e.ativo ? " — começou" : " — normalizou";
  li.append(q, document.createTextNode(`${nome}${fase}${e.temp_c !== null && e.temp_c !== undefined ? ` (${fmtTemp(e.temp_c)})` : ""}`));
  const lista = $("eventos");
  noTopo ? lista.prepend(li) : lista.append(li);
  while (lista.children.length > 50) lista.lastChild.remove();
}

// ------------------------------------------------------------------ histórico, gráfico e resumo
function linhaLimite(lim) {
  const agora = Date.now();
  return [{ x: agora - 24 * 3600 * 1000, y: lim }, { x: agora + 3600 * 1000, y: lim }];
}

async function carregarHistorico() {
  try {
    const [hist, resumo, ev] = await Promise.all([
      api(`/api/historico?disp=${DISP}&horas=24`),
      api(`/api/resumo?disp=${DISP}`),
      api(`/api/eventos?disp=${DISP}&limite=30`),
    ]);
    desenharGrafico(hist.leituras.filter((l) => l.sensor_ok && l.temp_c !== null).map((l) => ({ x: l.ts * 1000, y: l.temp_c })), resumo.limite_c);
    preencherResumo(resumo);
    $("eventos").replaceChildren();
    ev.eventos.forEach((e) => adicionarEvento(e, false));
    // alertas ao vivo mais novos que o último evento gravado continuam no topo
    const ultimoGravado = ev.eventos.length ? ev.eventos[0].ts : 0;
    alertasAoVivo.filter((e) => e.ts > ultimoGravado).forEach((e) => adicionarEvento(e, true));
  } catch (e) {
    if (e.status === 401 || e.status === 429) { $("ultima").textContent = "Sessão inválida: recarregue e entre de novo."; }
  }
}

function desenharGrafico(pontos, limite) {
  const css = getComputedStyle(document.documentElement);
  const cor = css.getPropertyValue("--linha").trim(), perigo = css.getPropertyValue("--perigo").trim(), sutil = css.getPropertyValue("--sutil").trim();
  if (grafico) {
    grafico.data.datasets[0].data = pontos;
    grafico.data.datasets[1].data = linhaLimite(limite);
    grafico.options.scales.x.min = Date.now() - 24 * 3600 * 1000;   // janela de 24 h anda junto
    grafico.update("none");
    return;
  }
  grafico = new Chart($("grafico"), {
    type: "line",
    data: { datasets: [
      { label: "Temperatura (°C)", data: pontos, borderColor: cor, backgroundColor: cor, pointRadius: 0, borderWidth: 2, tension: 0.2 },
      { label: "Limite de alarme", data: linhaLimite(limite), borderColor: perigo, borderDash: [6, 4], pointRadius: 0, borderWidth: 1.5 },
    ] },
    options: {
      animation: false, maintainAspectRatio: false, parsing: false,
      interaction: { mode: "nearest", intersect: false },
      scales: {
        x: { type: "linear", min: Date.now() - 24 * 3600 * 1000, ticks: { color: sutil, maxTicksLimit: 8,
             callback: (v) => new Date(v).toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit", timeZone: "America/Sao_Paulo" }) } },
        y: { ticks: { color: sutil, callback: (v) => `${v} °C` }, suggestedMin: 0, suggestedMax: 8 },
      },
      plugins: {
        legend: { labels: { color: sutil } },
        tooltip: { callbacks: { title: (it) => new Date(it[0].parsed.x).toLocaleString("pt-BR", { timeZone: "America/Sao_Paulo" }),
                                label: (it) => `${it.dataset.label}: ${it.parsed.y.toFixed(1)} °C` } },
      },
    },
  });
}

function preencherResumo(r) {
  const tb = $("resumo");
  tb.replaceChildren();
  if (!r.horas.length) {
    const tr = document.createElement("tr"); const td = document.createElement("td");
    td.colSpan = 5; td.textContent = "Ainda não há leituras nas últimas 24 h."; tr.append(td); tb.append(tr); return;
  }
  for (const h of r.horas) {
    const tr = document.createElement("tr");
    const cel = (t, cls) => { const td = document.createElement("td"); td.textContent = t; if (cls) td.className = cls; tr.append(td); };
    cel(h.hora.slice(11) + " · " + h.hora.slice(8, 10) + "/" + h.hora.slice(5, 7));
    cel(fmtTemp(h.min_c)); cel(fmtTemp(h.media_c)); cel(fmtTemp(h.max_c), h.max_c > r.limite_c ? "fora" : "");
    cel(String(h.minutos_fora), h.minutos_fora > 0 ? "fora" : "");
    tb.append(tr);
  }
}

// ------------------------------------------------------------------ sessão já aberta nesta aba
const pinSalvo = ler("pin"), mqttSalvo = ler("mqtt");
if (pinSalvo && mqttSalvo) {
  pin = pinSalvo;
  api("/api/login").then(() => iniciar(mqttSalvo)).catch(() => {});
}
