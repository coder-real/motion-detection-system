"use strict";

require("dotenv").config();
const express = require("express");
const multer = require("multer");
const { WebSocketServer, WebSocket } = require("ws");
const { createClient } = require("@supabase/supabase-js");
const http = require("http");
const os = require("os");

// ── Config ────────────────────────────────────────────────────────
const PORT = parseInt(process.env.PORT || "3000", 10);
const HOST = process.env.HOST || "0.0.0.0";
const SUPABASE_URL = process.env.SUPABASE_URL;
const SUPABASE_KEY = process.env.SUPABASE_KEY;
const BUCKET = process.env.SUPABASE_BUCKET || "snapshots";
const DEVICE_ID = process.env.DEVICE_ID || "ESP32-CAM-01";
const TOKEN = process.env.DEVICE_TOKEN || "";
// Optional: force the IP shown in the startup banner.
// Windows with VPN picks the wrong adapter (e.g. 10.56.x.x VPN IP).
// Set PREFERRED_IP=192.168.x.x in .env to override auto-detection.
const PREFERRED_IP = process.env.PREFERRED_IP || null;

if (!SUPABASE_URL || !SUPABASE_KEY) {
  console.error("[FATAL] SUPABASE_URL and SUPABASE_KEY must be set in .env");
  process.exit(1);
}

// ── Supabase clients ──────────────────────────────────────────────
// Two clients: one for REST calls, one dedicated to Realtime.
// Using the same client for both can cause Realtime drops during
// heavy REST load.
const supa = createClient(SUPABASE_URL, SUPABASE_KEY);
const supaRT = createClient(SUPABASE_URL, SUPABASE_KEY, {
  realtime: { params: { eventsPerSecond: 10 } },
});

// ── Metrics ───────────────────────────────────────────────────────
const metrics = {
  started: Date.now(),
  uploads: { ok: 0, fail: 0 },
  events: { ok: 0, fail: 0 },
  commands: { pushed: 0, acked: 0, ackFailed: 0 },
  wsConnections: 0,
  wsReconnects: 0,
};

// ── WebSocket state ───────────────────────────────────────────────
// One live WebSocket per connected ESP32.
// esp32Socket = the active connection (null if ESP32 not connected).
// pendingAcks = Map<commandId, { timer, resolve }>
// commandDedup = Set<commandId> — IDs already pushed this session
let esp32Socket = null;
const pendingAcks = new Map();
const commandDedup = new Set();
const COMMAND_DEDUP_TTL = 90_000; // forget after 90s
const ACK_TIMEOUT_MS = 20_000; // ESP32 has 20s to ack

// ─────────────────────────────────────────────────────────────────
//  HTTP + EXPRESS
// ─────────────────────────────────────────────────────────────────
const app = express();
const server = http.createServer(app);

app.use(express.json({ limit: "1mb" }));

// CORS — required for cloud deployment where dashboard origin
// differs from the server origin (e.g. Vercel dashboard → Railway server)
app.use((_req, res, next) => {
  res.header("Access-Control-Allow-Origin", "*");
  res.header("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  res.header("Access-Control-Allow-Headers", "Content-Type,X-Device-Token");
  if (_req.method === "OPTIONS") return res.sendStatus(204);
  next();
});

const upload = multer({
  storage: multer.memoryStorage(),
  limits: { fileSize: 3 * 1024 * 1024 }, // 3 MB
});

function authGuard(req, res, next) {
  if (!TOKEN) return next();
  if (req.headers["x-device-token"] !== TOKEN) {
    return res.status(401).json({ error: "Unauthorized" });
  }
  next();
}

// ─────────────────────────────────────────────────────────────────
//  POST /upload
//
//  ESP32 sends a multipart POST with:
//    image       (file)   — JPEG bytes
//    snapType    (text)   — "motion" | "manual" | "startup"
//    deviceId    (text)
//    triggeredBy (text)
//    latitude    (text)
//    longitude   (text)
//    distanceCm  (text)
//    smsSent     (text)   "0"|"1"
//    altitudeM / speedKmh / hdop / satellites / gpsValid
//    hubHeap / hubBattery / hubGsmCsq / hubUptimeS / hubFwVersion
//    camHeap / camRssi
//
//  Server:
//    1. Uploads JPEG to Supabase Storage (Node TLS — reliable)
//    2. Inserts event row into events table
//    3. Returns { url, path }
// ─────────────────────────────────────────────────────────────────
app.post("/upload", authGuard, upload.single("image"), async (req, res) => {
  if (!req.file) return res.status(400).json({ error: "No image" });

  const b = req.body;
  const snapType = b.snapType || "unknown";
  const filename = `${snapType}_${Date.now()}.jpg`;
  const path = `events/${filename}`;

  log(
    "UPLOAD",
    `${(req.file.size / 1024).toFixed(1)}KB  type=${snapType}  dev=${b.deviceId || "?"}`,
  );

  // ── 1. Upload to Supabase Storage ────────────────────────────
  const { error: storErr } = await supa.storage
    .from(BUCKET)
    .upload(path, req.file.buffer, {
      contentType: "image/jpeg",
      upsert: true,
    });

  if (storErr) {
    metrics.uploads.fail++;
    log("UPLOAD", `Storage FAILED: ${storErr.message}`);
    return res.status(502).json({ error: storErr.message });
  }

  metrics.uploads.ok++;
  const { data: urlData } = supa.storage.from(BUCKET).getPublicUrl(path);
  const publicUrl = urlData.publicUrl;
  log("UPLOAD", `Storage OK → ${path}`);

  // ── 2. Insert event row ───────────────────────────────────────
  const gpsValid = b.gpsValid === "1" || b.gpsValid === "true";
  const lat = parseFloat(b.latitude || "0");
  const lng = parseFloat(b.longitude || "0");

  const eventRow = {
    device_id: b.deviceId || DEVICE_ID,
    triggered_by: b.triggeredBy || b.deviceId || DEVICE_ID,
    snapshot_type: snapType,
    snapshot_url: publicUrl,
    snapshot_path: path,
    latitude: gpsValid && lat ? lat : 0,
    longitude: gpsValid && lng ? lng : 0,
    distance_cm: parseFloat(b.distanceCm || "0"),
    sms_sent: b.smsSent === "1",
    // Extended columns — require sql_full_fix migration
    altitude_m: parseFloat(b.altitudeM || "0"),
    speed_kmh: parseFloat(b.speedKmh || "0"),
    hdop: parseFloat(b.hdop || "0"),
    satellites: parseInt(b.satellites || "0", 10),
    gps_valid: gpsValid,
    hub_heap_bytes: parseInt(b.hubHeap || "0", 10),
    hub_battery_v: parseFloat(b.hubBattery || "0"),
    hub_gsm_csq: parseInt(b.hubGsmCsq || "0", 10),
    hub_uptime_s: parseInt(b.hubUptimeS || "0", 10),
    hub_fw_version: b.hubFwVersion || "",
    cam_heap_bytes: parseInt(b.camHeap || "0", 10),
    cam_rssi: parseInt(b.camRssi || "0", 10),
  };

  const { error: evErr } = await supa.from("events").insert(eventRow);
  if (evErr) {
    metrics.events.fail++;
    log("UPLOAD", `Event insert FAILED: ${evErr.message}`);
    // Don't fail the response — image is safely stored
  } else {
    metrics.events.ok++;
    log("UPLOAD", `Event OK  type=${snapType}`);
  }

  res.json({ url: publicUrl, path, event: evErr ? "fail" : "ok" });
});

// ─────────────────────────────────────────────────────────────────
//  POST /heartbeat  — upserts device + inserts heartbeat row
// ─────────────────────────────────────────────────────────────────
app.post("/heartbeat", authGuard, async (req, res) => {
  const body = req.body;
  if (!body.device_id)
    return res.status(400).json({ error: "Missing device_id" });

  const [hbRes, devRes] = await Promise.all([
    supa.from("heartbeats").insert(body),
    supa
      .from("devices")
      .update({
        status: "online",
        last_seen: new Date().toISOString(),
        ip_address: body.ip_address || undefined,
        firmware_version: body.firmware_version || undefined,
      })
      .eq("device_id", body.device_id),
  ]);

  const ok = !hbRes.error && !devRes.error;
  if (!ok) {
    log("HB", `FAIL hb=${hbRes.error?.message} dev=${devRes.error?.message}`);
  } else {
    log(
      "HB",
      `${body.device_id}  heap=${body.free_heap ? Math.round(body.free_heap / 1024) + "KB" : "?"}  rssi=${body.rssi || "?"}dBm`,
    );
  }
  res.json({ ok });
});

// ─────────────────────────────────────────────────────────────────
//  POST /device  — upsert device record
// ─────────────────────────────────────────────────────────────────
app.post("/device", authGuard, async (req, res) => {
  const { error } = await supa
    .from("devices")
    .upsert(req.body, { onConflict: "device_id" });
  if (error) log("DEVICE", `Upsert failed: ${error.message}`);
  else log("DEVICE", `Upsert OK: ${req.body.device_id}`);
  res.json({ ok: !error });
});

// ─────────────────────────────────────────────────────────────────
//  POST /log
// ─────────────────────────────────────────────────────────────────
app.post("/log", authGuard, async (req, res) => {
  const { error } = await supa.from("logs").insert(req.body);
  res.json({ ok: !error });
});

// ─────────────────────────────────────────────────────────────────
//  GET /ping — keeps Render free tier awake
//  Hit this every 14 minutes via cron-job.org (free).
//  The ESP32's 30s heartbeats also keep the server alive during
//  normal operation, so this is only needed when the device is off.
// ─────────────────────────────────────────────────────────────────
app.get("/ping", (_req, res) => res.json({ ok: true, ts: Date.now() }));

// ─────────────────────────────────────────────────────────────────
//  GET /health  — lightweight ping
// ─────────────────────────────────────────────────────────────────
app.get("/health", (_req, res) => {
  res.json({
    status: "ok",
    version: "1.0.0",
    uptime: Math.floor(process.uptime()),
    esp32:
      esp32Socket?.readyState === WebSocket.OPEN ? "connected" : "disconnected",
    wsClients: metrics.wsConnections,
    uploads: metrics.uploads,
    events: metrics.events,
    commands: metrics.commands,
    memMB: (process.memoryUsage().heapUsed / 1024 / 1024).toFixed(1),
    ts: new Date().toISOString(),
  });
});

// ─────────────────────────────────────────────────────────────────
//  GET /status  — human readable status page
// ─────────────────────────────────────────────────────────────────
app.get("/status", (_req, res) => {
  const upSec = Math.floor(process.uptime());
  const h = `<!DOCTYPE html>
<html><head><title>SENTINEL Server</title>
<style>body{font-family:monospace;background:#0a0a0a;color:#ddd;padding:24px}
h2{color:#4af}table{border-spacing:0 8px}td:first-child{color:#888;padding-right:20px}
.on{color:#4f4}  .off{color:#f44}</style></head><body>
<h2>SENTINEL SERVER v1.0.0</h2>
<table>
<tr><td>ESP32 WebSocket</td><td class="${esp32Socket?.readyState === 1 ? "on" : "off"}">${esp32Socket?.readyState === 1 ? "Connected" : "Disconnected"}</td></tr>
<tr><td>Supabase Realtime</td><td class="on">Subscribed</td></tr>
<tr><td>Server uptime</td><td>${Math.floor(upSec / 3600)}h ${Math.floor((upSec % 3600) / 60)}m ${upSec % 60}s</td></tr>
<tr><td>Uploads</td><td>${metrics.uploads.ok} OK / ${metrics.uploads.fail} fail</td></tr>
<tr><td>Events</td><td>${metrics.events.ok} OK / ${metrics.events.fail} fail</td></tr>
<tr><td>Commands pushed</td><td>${metrics.commands.pushed}</td></tr>
<tr><td>Commands acked</td><td>${metrics.commands.acked}</td></tr>
<tr><td>Memory</td><td>${(process.memoryUsage().heapUsed / 1024 / 1024).toFixed(1)} MB</td></tr>
</table>
<br><a href="/health" style="color:#4af">JSON status</a>
</body></html>`;
  res.send(h);
});

// ─────────────────────────────────────────────────────────────────
//  WEBSOCKET SERVER
//  ESP32 connects once at boot and keeps the connection alive.
//  The server pushes commands as JSON frames; ESP32 sends acks back.
//
//  Protocol (both directions are JSON):
//    Server → ESP32:
//      { type:"command", id:"<uuid>", command:"capture" }
//      { type:"ping" }
//
//    ESP32 → Server:
//      { type:"ack", id:"<uuid>", status:"done"|"failed" }
//      { type:"pong" }
//      { type:"hello", deviceId:"ESP32-CAM-01", version:"10.0.0" }
// ─────────────────────────────────────────────────────────────────
const wss = new WebSocketServer({ server, path: "/ws" });

wss.on("connection", (ws, req) => {
  const ip = req.socket.remoteAddress;
  log("WS", `ESP32 connected from ${ip}`);

  // If there was already a socket (reconnect), close the old one cleanly
  if (esp32Socket && esp32Socket.readyState === WebSocket.OPEN) {
    log("WS", "Closing previous connection (ESP32 reconnected)");
    esp32Socket.terminate();
    metrics.wsReconnects++;
  }
  esp32Socket = ws;
  metrics.wsConnections++;

  // Heartbeat ping every 15s — detects dead connections early
  let pingTimer = setInterval(() => {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: "ping" }));
    }
  }, 15_000);

  ws.on("message", (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch {
      log("WS", `Non-JSON message: ${data.toString().slice(0, 80)}`);
      return;
    }

    if (msg.type === "hello") {
      log(
        "WS",
        `ESP32 identified — id=${msg.deviceId}  fw=${msg.version}  heap=${msg.freeHeap ? Math.round(msg.freeHeap / 1024) + "KB" : "?"}`,
      );
    } else if (msg.type === "pong") {
      // alive — no action needed
    } else if (msg.type === "ack") {
      // ESP32 has finished executing a command
      handleAck(msg.id, msg.status || "done");
    } else {
      log("WS", `Unknown message type: ${msg.type}`);
    }
  });

  ws.on("close", (code, reason) => {
    log(
      "WS",
      `ESP32 disconnected code=${code} reason=${reason?.toString() || "—"}`,
    );
    clearInterval(pingTimer);
    if (esp32Socket === ws) esp32Socket = null;
  });

  ws.on("error", (err) => {
    log("WS", `Error: ${err.message}`);
  });
});

// ─────────────────────────────────────────────────────────────────
//  PUSH COMMAND TO ESP32
//  Called when Supabase Realtime delivers a new command row.
//  1. Dedup check — skip if this ID was pushed recently
//  2. Pre-mark "processing" in DB (reliable Node TLS)
//  3. Push to ESP32 via WebSocket
//  4. Wait up to ACK_TIMEOUT_MS for ack, then mark "done"/"failed"
// ─────────────────────────────────────────────────────────────────
async function pushCommandToESP32(cmd) {
  const { id, command, device_id } = cmd;

  // Dedup guard — prevents duplicate push if Realtime fires twice
  if (commandDedup.has(id)) {
    log("CMD", `Dedup skip id=${id.slice(0, 8)}`);
    return;
  }
  commandDedup.add(id);
  setTimeout(() => commandDedup.delete(id), COMMAND_DEDUP_TTL);

  log("CMD", `New command: ${command}  id=${id.slice(0, 8)}  dev=${device_id}`);

  // Pre-mark "processing" in DB (this is the key ack fix —
  // done by Node not by ESP32, so it never fails silently)
  const { error: preErr } = await supa
    .from("commands")
    .update({ status: "processing" })
    .eq("id", id)
    .eq("status", "pending");

  if (preErr) {
    log("CMD", `Pre-mark failed: ${preErr.message} — pushing anyway`);
  }

  // Push to ESP32 if connected
  if (!esp32Socket || esp32Socket.readyState !== WebSocket.OPEN) {
    log(
      "CMD",
      `ESP32 not connected — command id=${id.slice(0, 8)} marked failed`,
    );
    metrics.commands.ackFailed++;
    await supa.from("commands").update({ status: "failed" }).eq("id", id);
    return;
  }

  metrics.commands.pushed++;
  esp32Socket.send(JSON.stringify({ type: "command", id, command }));
  log("CMD", `Pushed to ESP32: ${command}  id=${id.slice(0, 8)}`);

  // Wait for ack with timeout
  await new Promise((resolve) => {
    const timer = setTimeout(async () => {
      pendingAcks.delete(id);
      log("CMD", `Ack timeout id=${id.slice(0, 8)} — marking failed`);
      metrics.commands.ackFailed++;
      await supa.from("commands").update({ status: "failed" }).eq("id", id);
      resolve();
    }, ACK_TIMEOUT_MS);

    pendingAcks.set(id, { timer, resolve });
  });
}

async function handleAck(id, status) {
  const entry = pendingAcks.get(id);
  if (!entry) {
    log("ACK", `Received ack for unknown/expired id=${id?.slice(0, 8)}`);
    // Still update DB in case this is a late ack
    if (id) await supa.from("commands").update({ status }).eq("id", id);
    return;
  }

  clearTimeout(entry.timer);
  pendingAcks.delete(id);

  const { error } = await supa.from("commands").update({ status }).eq("id", id);

  if (error) {
    metrics.commands.ackFailed++;
    log("ACK", `DB update FAILED id=${id.slice(0, 8)}: ${error.message}`);
  } else {
    metrics.commands.acked++;
    log("ACK", `${status.toUpperCase()}  id=${id.slice(0, 8)}`);
  }
  entry.resolve();
}

// ─────────────────────────────────────────────────────────────────
//  SUPABASE REALTIME — subscribe to new commands
//  When the dashboard inserts a command, Supabase pushes it here
//  in <100ms. No polling needed anywhere.
// ─────────────────────────────────────────────────────────────────
function subscribeToCommands() {
  supaRT
    .channel("commands-insert")
    .on(
      "postgres_changes",
      {
        event: "INSERT",
        schema: "public",
        table: "commands",
        filter: `device_id=eq.${DEVICE_ID}`,
      },
      (payload) => {
        const cmd = payload.new;
        if (cmd.status !== "pending") return; // only act on fresh inserts
        pushCommandToESP32(cmd).catch((err) =>
          log("CMD", `pushCommandToESP32 exception: ${err.message}`),
        );
      },
    )
    .subscribe((status, err) => {
      if (status === "SUBSCRIBED") {
        log("RT", `Commands channel active — watching device_id=${DEVICE_ID}`);
      } else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT") {
        log(
          "RT",
          `Channel error: ${status} ${err?.message || ""} — will retry`,
        );
      }
    });
}

// ─────────────────────────────────────────────────────────────────
//  FIND LOCAL IP  — shown at startup so user knows what to set
// ─────────────────────────────────────────────────────────────────
/**
 * getLocalIP — Returns the best local IP for the ESP32 to reach.
 *
 * Windows with VPN/ethernet has multiple adapters and the naive
 * "return first non-internal IPv4" returns the VPN tunnel IP
 * (e.g. 10.56.x.x) which the ESP32 on home WiFi can't reach.
 *
 * Priority order:
 *   1. Interface named Wi-Fi / WLAN / wireless (home router)
 *   2. Interface with a 192.168.x.x address (most home routers)
 *   3. First non-internal IPv4 (fallback)
 *
 * getAllLocalIPs() is also exported so the startup banner can
 * print EVERY available address — so you can spot the right one.
 */
function getAllLocalIPs() {
  const ifaces = os.networkInterfaces();
  const result = [];
  for (const [name, addrs] of Object.entries(ifaces)) {
    for (const iface of addrs) {
      if (iface.family === "IPv4" && !iface.internal) {
        result.push({ name, address: iface.address });
      }
    }
  }
  return result;
}

function getLocalIP() {
  if (PREFERRED_IP) return PREFERRED_IP;
  const all = getAllLocalIPs();
  if (!all.length) return "127.0.0.1";

  // 1. Prefer interface named Wi-Fi / WLAN / wireless
  const wifi = all.find((i) => /wi.?fi|wlan|wireless/i.test(i.name));
  if (wifi) return wifi.address;

  // 2. Prefer 192.168.x.x (common home router range)
  const home = all.find((i) => i.address.startsWith("192.168."));
  if (home) return home.address;

  // 3. Prefer 10.0.x.x or 10.1.x.x over 10.56+ / 10.168+ (VPN ranges)
  const localTen = all.find((i) => /^10\.(0|1)\./.test(i.address));
  if (localTen) return localTen.address;

  // 4. Fallback — first available
  return all[0].address;
}

// ─────────────────────────────────────────────────────────────────
//  LOGGER
// ─────────────────────────────────────────────────────────────────
function log(mod, msg) {
  const ts = new Date().toTimeString().slice(0, 8);
  console.log(`[${ts}] ${mod.padEnd(8)} ${msg}`);
}

// ─────────────────────────────────────────────────────────────────
//  GRACEFUL SHUTDOWN
// ─────────────────────────────────────────────────────────────────
function shutdown(signal) {
  log("SYS", `${signal} received — shutting down`);
  if (esp32Socket?.readyState === WebSocket.OPEN) esp32Socket.close();
  server.close(() => process.exit(0));
}
process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT", () => shutdown("SIGINT"));

// ─────────────────────────────────────────────────────────────────
//  START
// ─────────────────────────────────────────────────────────────────
server.listen(PORT, HOST, () => {
  const bestIP = PREFERRED_IP || getLocalIP();
  const allIPs = getAllLocalIPs();
  const W = 62;
  const pad = (s) => `║  ${s.padEnd(W - 4)}║`;
  const sep = `╠${"═".repeat(W - 2)}╣`;
  const top = `╔${"═".repeat(W - 2)}╗`;
  const bot = `╚${"═".repeat(W - 2)}╝`;

  console.log("");
  console.log(top);
  console.log(pad("SENTINEL SERVER  v1.2.0"));
  console.log(sep);
  console.log(
    pad(`Supabase:  ${SUPABASE_URL.replace("https://", "").slice(0, 46)}`),
  );
  console.log(pad(`Auth:      ${TOKEN ? "Token enabled" : "Open (no token)"}`));
  console.log(pad(`ACK timeout: ${ACK_TIMEOUT_MS / 1000}s`));
  console.log(sep);
  // Show ALL detected network interfaces so the user can pick the right one
  console.log(pad("⚠  ALL detected network adapters:"));
  for (const { name, address } of allIPs) {
    const marker = address === bestIP ? " ← AUTO-SELECTED" : "";
    console.log(pad(`   ${address.padEnd(16)} (${name})${marker}`));
  }
  console.log(pad(""));
  console.log(pad("If AUTO-SELECTED is wrong, set PREFERRED_IP in .env"));
  console.log(pad("or hardcode SERVER_HOST in esp32cam firmware."));
  console.log(sep);
  console.log(pad("  ⚠  Set in esp32cam_v10.ino:"));
  console.log(pad(`     const char* SERVER_HOST = "${bestIP}";`));
  console.log(pad(`     const int   SERVER_PORT = ${PORT};`));
  console.log(pad(`     #define USE_CLOUD 0`));
  console.log(sep);
  console.log(pad(`HTTP:      http://${bestIP}:${PORT}`));
  console.log(pad(`WebSocket: ws://${bestIP}:${PORT}/ws`));
  console.log(pad(`Health:    http://${bestIP}:${PORT}/health`));
  console.log(bot);
  console.log("");

  subscribeToCommands();
  log("RT", "Supabase Realtime started");
  log("SYS", "Ready — waiting for ESP32");
});
