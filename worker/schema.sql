-- FrioSeguro: histórico no D1
-- Aplicar: npx wrangler d1 execute frioseguro --remote --file schema.sql

CREATE TABLE IF NOT EXISTS leituras (
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
CREATE INDEX IF NOT EXISTS idx_leituras_disp_ts ON leituras (disp, ts);

CREATE TABLE IF NOT EXISTS eventos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  disp TEXT NOT NULL,
  ts INTEGER NOT NULL,
  tipo TEXT NOT NULL,             -- temp_alta, porta_aberta, falha_sensor, silenciado_*, config_alterada, comando_*
  ativo INTEGER NOT NULL,         -- 1 = começou, 0 = normalizou
  temp_c REAL,
  origem TEXT NOT NULL DEFAULT 'esp32',  -- esp32 | painel
  UNIQUE (disp, ts, tipo, ativo, origem)
);
CREATE INDEX IF NOT EXISTS idx_eventos_disp_ts ON eventos (disp, ts);
