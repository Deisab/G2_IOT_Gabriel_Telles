// FrioSeguro — Cloudflare Worker
//
// Rota de gravação (chamada pela regra de Data Integration do EMQX, não pelo ESP32):
//   POST /api/leituras   {"p":"<lote JSON>","sig":"<HMAC>"} -> leituras e eventos no D1
//                        exige Authorization: Bearer <DEVICE_TOKEN> (configurado no EMQX)
//                        E a assinatura HMAC do ESP32 (quem só tem a senha do broker não grava)
// Rotas do painel (cabeçalho X-Pin: <PAINEL_PIN>, bloqueio após 5 erros):
//   GET  /api/historico  leituras das últimas N horas
//   GET  /api/resumo     mín/média/máx por hora e minutos fora da faixa (24 h)
//   GET  /api/eventos    últimos eventos
//   POST /api/assinar    assina um comando com HMAC (a chave nunca vai ao navegador)
// Cron diário: retenção (leituras 90 dias, eventos 1 ano).

const DISP_RE = /^[a-z0-9_-]{1,32}$/;
const TIPOS_EVENTO = new Set([
  "temp_alta", "porta_aberta", "falha_sensor",
  "silenciado_local", "silenciado_remoto", "silencio_cancelado", "config_alterada", "comando_rejeitado",
]);
const COMANDOS = new Set(["silenciar", "cancelar_silencio", "config"]);
const CONFIG_PADRAO = { liga_c: 4.0, desliga_c: 2.0, alarme_c: 5.0, normal_c: 4.5, alarme_s: 30, porta_s: 20 };
const CMD_VALIDADE_S = 30;
const MAX_FALHAS_PIN = 5;

// ------------------------------------------------------------------ utilidades
const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });

const agora = () => Math.floor(Date.now() / 1000);

function iguais(a, b) {
  const x = new TextEncoder().encode(a), y = new TextEncoder().encode(b);
  if (x.length !== y.length) return false;
  let d = 0;
  for (let i = 0; i < x.length; i++) d |= x[i] ^ y[i];
  return d === 0;
}

async function hmacHex(chave, msg) {
  const k = await crypto.subtle.importKey("raw", new TextEncoder().encode(chave),
    { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const sig = await crypto.subtle.sign("HMAC", k, new TextEncoder().encode(msg));
  return [...new Uint8Array(sig)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

const bit = (v) => (v === 1 || v === true ? 1 : v === 0 || v === false ? 0 : null);
const numOuNull = (v, min, max) =>
  v === null || v === undefined ? null : typeof v === "number" && v >= min && v <= max ? v : undefined;

function lerDisp(url) {
  const d = url.searchParams.get("disp") || "camara01";
  return DISP_RE.test(d) ? d : null;
}

// ------------------------------------------------------------------ autenticação
function tokenOk(req, env) {
  const h = req.headers.get("authorization") || "";
  return !!env.DEVICE_TOKEN && h.startsWith("Bearer ") && iguais(h.slice(7), env.DEVICE_TOKEN);
}

// PIN do painel com bloqueio por IP: 5 erros -> 10 min bloqueado
async function pinOk(req, env) {
  const ip = req.headers.get("cf-connecting-ip") || "local";
  const chave = `falhas_pin:${ip}`;
  const falhas = parseInt((await env.KV.get(chave)) || "0", 10);
  if (falhas >= MAX_FALHAS_PIN) return json({ erro: "bloqueado", detalhe: "muitas tentativas; aguarde 10 min" }, 429);
  const pin = req.headers.get("x-pin") || "";
  // sem PIN nenhum não é tentativa de adivinhar: recusa sem contar (só PIN errado conta)
  if (!pin) return json({ erro: "pin_invalido" }, 401);
  if (env.PAINEL_PIN && iguais(pin, env.PAINEL_PIN)) return null;
  await env.KV.put(chave, String(falhas + 1), { expirationTtl: 600 });
  return json({ erro: "pin_invalido" }, 401);
}

// ------------------------------------------------------------------ configuração
function validarConfig(c) {
  return c.desliga_c >= -5 && c.desliga_c < c.liga_c && c.liga_c <= 8 &&
    c.normal_c < c.alarme_c && c.alarme_c <= 10 &&
    Number.isInteger(c.alarme_s) && c.alarme_s >= 10 && c.alarme_s <= 3600 &&
    Number.isInteger(c.porta_s) && c.porta_s >= 10 && c.porta_s <= 1800;
}

async function lerConfig(env, disp) {
  const c = await env.KV.get(`config:${disp}`, "json");
  return c || null;
}

// ------------------------------------------------------------------ rotas do dispositivo
async function postLeituras(req, env) {
  if (!tokenOk(req, env)) return json({ erro: "nao_autorizado" }, 401);

  let ext, corpo;
  try { ext = await req.json(); } catch { return json({ erro: "json_invalido" }, 400); }
  if (typeof ext?.p !== "string" || typeof ext?.sig !== "string") return json({ erro: "sem_assinatura" }, 401);
  if (!iguais(await hmacHex(env.HMAC_KEY, ext.p), ext.sig)) return json({ erro: "assinatura_invalida" }, 401);
  try { corpo = JSON.parse(ext.p); } catch { return json({ erro: "json_invalido" }, 400); }
  const disp = corpo?.disp;
  if (typeof disp !== "string" || !DISP_RE.test(disp)) return json({ erro: "disp_invalido" }, 400);
  const leituras = Array.isArray(corpo.leituras) ? corpo.leituras : [];
  const eventos = Array.isArray(corpo.eventos) ? corpo.eventos : [];
  if (leituras.length > 100 || eventos.length > 50) return json({ erro: "lote_grande_demais" }, 413);

  // aceita leituras de até 7 dias atrás (buffer offline) e no máximo 5 min no futuro
  const t = agora(), min = t - 7 * 86400, max = t + 300;
  const tsOk = (ts) => Number.isInteger(ts) && ts >= min && ts <= max;
  const stmts = [];
  let ignoradas = 0;

  const insL = env.DB.prepare(
    `INSERT OR IGNORE INTO leituras (disp, ts, temp_c, umid_pct, porta_aberta, compressor, alarme, sensor_ok)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?)`);
  for (const l of leituras) {
    const temp = numOuNull(l?.temp_c, -40, 80), umid = numOuNull(l?.umid_pct, 0, 100);
    const flags = [bit(l?.porta_aberta), bit(l?.compressor), bit(l?.alarme), bit(l?.sensor_ok)];
    if (!tsOk(l?.ts) || temp === undefined || umid === undefined || flags.includes(null)) { ignoradas++; continue; }
    stmts.push(insL.bind(disp, l.ts, temp, umid, ...flags));
  }

  const insE = env.DB.prepare(
    `INSERT OR IGNORE INTO eventos (disp, ts, tipo, ativo, temp_c, origem) VALUES (?, ?, ?, ?, ?, 'esp32')`);
  for (const e of eventos) {
    const temp = numOuNull(e?.temp_c, -40, 80), ativo = bit(e?.ativo);
    if (!tsOk(e?.ts) || !TIPOS_EVENTO.has(e?.tipo) || temp === undefined || ativo === null) { ignoradas++; continue; }
    stmts.push(insE.bind(disp, e.ts, e.tipo, ativo, temp));
  }

  const res = stmts.length ? await env.DB.batch(stmts) : [];
  const gravadas = res.reduce((n, r) => n + (r.meta?.changes || 0), 0);
  // duplicadas = já estavam no banco (reenvio depois de uma resposta perdida)
  return json({ ok: true, gravadas, duplicadas: stmts.length - gravadas, ignoradas });
}

// ------------------------------------------------------------------ rotas do painel
async function getHistorico(env, url) {
  const disp = lerDisp(url);
  if (!disp) return json({ erro: "disp_invalido" }, 400);
  const horas = Math.min(Math.max(parseInt(url.searchParams.get("horas") || "24", 10) || 24, 1), 168);
  const { results } = await env.DB.prepare(
    `SELECT ts, temp_c, umid_pct, porta_aberta, compressor, alarme, sensor_ok
     FROM leituras WHERE disp = ? AND ts >= ? ORDER BY ts LIMIT 5000`)
    .bind(disp, agora() - horas * 3600).all();
  return json({ disp, horas, leituras: results });
}

// Pergunta: "nas últimas 24 h, qual foi a mínima, média e máxima de cada hora,
// e por quantos minutos a câmara ficou acima do limite?" (1 leitura = 1 minuto)
async function getResumo(env, url) {
  const disp = lerDisp(url);
  if (!disp) return json({ erro: "disp_invalido" }, 400);
  const cfg = (await lerConfig(env, disp)) || CONFIG_PADRAO;
  const { results } = await env.DB.prepare(
    `SELECT strftime('%Y-%m-%d %H:00', ts, 'unixepoch', '-3 hours') AS hora,
            MIN(temp_c)           AS min_c,
            ROUND(AVG(temp_c), 1) AS media_c,
            MAX(temp_c)           AS max_c,
            SUM(CASE WHEN temp_c > ? THEN 1 ELSE 0 END) AS minutos_fora,
            COUNT(*)              AS leituras
     FROM leituras
     WHERE disp = ? AND sensor_ok = 1 AND ts >= ?
     GROUP BY hora ORDER BY hora`)
    .bind(cfg.alarme_c, disp, agora() - 86400).all();
  return json({ disp, limite_c: cfg.alarme_c, horas: results });
}

async function getEventos(env, url) {
  const disp = lerDisp(url);
  if (!disp) return json({ erro: "disp_invalido" }, 400);
  const limite = Math.min(Math.max(parseInt(url.searchParams.get("limite") || "20", 10) || 20, 1), 100);
  const { results } = await env.DB.prepare(
    `SELECT ts, tipo, ativo, temp_c, origem FROM eventos WHERE disp = ? ORDER BY ts DESC, id DESC LIMIT ?`)
    .bind(disp, limite).all();
  return json({ disp, eventos: results });
}

async function postAssinar(req, env) {
  let corpo;
  try { corpo = await req.json(); } catch { return json({ erro: "json_invalido" }, 400); }
  const disp = corpo?.disp ?? "camara01";
  const cmd = corpo?.cmd;
  if (typeof disp !== "string" || !DISP_RE.test(disp)) return json({ erro: "disp_invalido" }, 400);
  if (!COMANDOS.has(cmd)) return json({ erro: "comando_desconhecido" }, 400);

  const t = agora();
  const rnd = [...crypto.getRandomValues(new Uint8Array(4))].map((b) => b.toString(16).padStart(2, "0")).join("");
  const comando = { id: `c-${t}-${rnd}`, cmd, exp: t + CMD_VALIDADE_S };

  if (cmd === "config") {
    const args = corpo?.args;
    if (!args || typeof args !== "object") return json({ erro: "config_invalida" }, 400);
    const atual = (await lerConfig(env, disp)) || CONFIG_PADRAO;
    const nova = { ...atual };
    for (const k of Object.keys(CONFIG_PADRAO)) if (typeof args[k] === "number") nova[k] = args[k];
    if (!validarConfig(nova)) return json({ erro: "config_invalida" }, 400);
    comando.args = nova;
    await env.KV.put(`config:${disp}`, JSON.stringify(nova));   // registro dos últimos limites enviados
  }

  const p = JSON.stringify(comando);
  const sig = await hmacHex(env.HMAC_KEY, p);
  await env.DB.prepare(
    `INSERT OR IGNORE INTO eventos (disp, ts, tipo, ativo, temp_c, origem) VALUES (?, ?, ?, 1, NULL, 'painel')`)
    .bind(disp, t, `comando_${cmd}`).run();
  return json({ id: comando.id, exp: comando.exp, p, sig });
}

// ------------------------------------------------------------------ roteamento
export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const rota = `${req.method} ${url.pathname}`;
    try {
      if (rota === "POST /api/leituras") return await postLeituras(req, env);
      if (url.pathname.startsWith("/api/")) {
        const negado = await pinOk(req, env);
        if (negado) return negado;
        switch (rota) {
          case "GET /api/historico": return await getHistorico(env, url);
          case "GET /api/resumo":    return await getResumo(env, url);
          case "GET /api/eventos":   return await getEventos(env, url);
          case "POST /api/assinar":  return await postAssinar(req, env);
          case "GET /api/login":     return json({ ok: true });
        }
        return json({ erro: "rota_inexistente" }, 404);
      }
      return env.ASSETS ? env.ASSETS.fetch(req) : new Response("Not found", { status: 404 });
    } catch (e) {
      console.error(e);
      return json({ erro: "interno" }, 500);
    }
  },

  async scheduled(_evt, env) {
    const t = agora();
    await env.DB.batch([
      env.DB.prepare("DELETE FROM leituras WHERE ts < ?").bind(t - 90 * 86400),
      env.DB.prepare("DELETE FROM eventos WHERE ts < ?").bind(t - 365 * 86400),
    ]);
  },
};
