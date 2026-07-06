/*
 * HydroNode — Render Backend
 * ─────────────────────────────────────────────
 * ESP32 posts sensor data here every 5s
 * Dashboard fetches data from here
 * Dashboard posts commands → ESP32 polls and executes
 */

const express = require('express');
const cors    = require('cors');
const path    = require('path');

const app  = express();
const PORT = process.env.PORT || 3000;

// ── Simple API key auth ───────────────────────────────────────
// Set this as an environment variable on Render: API_KEY=yourSecretKey
// ESP32 and dashboard must send this in the X-API-Key header
const API_KEY = process.env.API_KEY || 'hydronode-secret';

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ── In-memory state ──────────────────────────────────────────
let state = {
  tds:       0,
  waterLow:  false,
  pump:      false,
  mode:      'AUTO',
  status:    'Waiting for ESP32...',
  onSec:     15,
  offSec:    60,
  lastSeen:  null
};

// Pending command queue (ESP32 polls this)
let pendingCommand = null;

// ── Middleware: verify API key ────────────────────────────────
function authCheck(req, res, next) {
  const key = req.headers['x-api-key'];
  if (key !== API_KEY) {
    return res.status(401).json({ error: 'Unauthorized' });
  }
  next();
}

// ── ESP32 → POST /api/update ─────────────────────────────────
// ESP32 sends sensor data every 5s
app.post('/api/update', authCheck, (req, res) => {
  const { tds, waterLow, pump, mode, status } = req.body;
  if (tds      !== undefined) state.tds      = tds;
  if (waterLow !== undefined) state.waterLow = waterLow;
  if (pump     !== undefined) state.pump     = pump;
  if (mode     !== undefined) state.mode     = mode;
  if (status   !== undefined) state.status   = status;
  state.lastSeen = new Date().toISOString();
  res.json({ ok: true });
});

// ── ESP32 → GET /api/command ──────────────────────────────────
// ESP32 polls this every 3s for pending commands
app.get('/api/command', authCheck, (req, res) => {
  if (pendingCommand) {
    const cmd = pendingCommand;
    pendingCommand = null;   // clear after sending
    return res.json({ command: cmd });
  }
  res.json({ command: null });
});

// ── Dashboard → GET /api/data ─────────────────────────────────
// Dashboard polls this every 2s
app.get('/api/data', (req, res) => {
  const online = state.lastSeen
    ? (Date.now() - new Date(state.lastSeen).getTime()) < 15000
    : false;
  res.json({ ...state, online });
});

// ── Dashboard → POST /api/mode ────────────────────────────────
// Set pump mode: AUTO / ON / OFF
app.post('/api/mode', (req, res) => {
  const { mode } = req.body;
  if (!['AUTO', 'ON', 'OFF'].includes(mode))
    return res.status(400).json({ error: 'Invalid mode' });
  pendingCommand = { type: 'MODE', value: mode };
  state.mode = mode;
  res.json({ ok: true });
});

// ── Dashboard → POST /api/timer ───────────────────────────────
// Update timer settings
app.post('/api/timer', (req, res) => {
  const { onSec, offSec } = req.body;
  if (!onSec || !offSec || onSec < 1 || offSec < 1)
    return res.status(400).json({ error: 'Invalid timer values' });
  pendingCommand = { type: 'TIMER', onSec, offSec };
  state.onSec  = onSec;
  state.offSec = offSec;
  res.json({ ok: true });
});

// ── Serve dashboard for all other routes ─────────────────────
app.get('*', (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

app.listen(PORT, () => {
  console.log(`HydroNode backend running on port ${PORT}`);
});
