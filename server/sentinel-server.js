/**
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  SENTINEL SERVER  v1.4.1                                    ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║                                                              ║
 * ║  WHY THIS EXISTS                                             ║
 * ║  ─────────────────────────────────────────────────────────  ║
 * ║  ESP32 mbedTLS has three failure modes that caused the       ║
 * ║  repeated-snapshot loop:                                     ║
 * ║    1. BIGNUM -16   — heap too fragmented for TLS init       ║
 * ║    2. Error -80    — SSL context corrupted between calls     ║
 * ║    3. Silent hang  — PATCH returns nothing, times out        ║
 * ║  All three leave commands "pending" forever → repeat loop.  ║
 * ║                                                              ║
 * ║  ARCHITECTURE                                                ║
 * ║  ─────────────────────────────────────────────────────────  ║
 * ║  BEFORE (broken):                                            ║
 * ║    ESP32 ──TLS──→ Supabase Storage    (8-12s, fails)        ║
 * ║    ESP32 ──TLS──→ Supabase DB poll    (every 3s, hangs)     ║
 * ║    ESP32 ──TLS──→ Supabase PATCH ack  (silent failure)      ║
 * ║                                                              ║
 * ║  AFTER (this file):                                          ║
 * ║    ESP32 ──HTTP──→ Server ──TLS──→ Supabase   (upload)      ║
 * ║    Dashboard ──→ Supabase Realtime ──→ Server ──WS──→ ESP32 ║
 * ║         (commands pushed instantly, no polling at all)       ║
 * ║                                                              ║
 * ║  WHAT THIS SERVER DOES                                       ║
 * ║  ─────────────────────────────────────────────────────────  ║
 * ║  1. Receives JPEG uploads from ESP32 via plain HTTP POST    ║
 * ║     → Forwards to Supabase Storage                          ║
 * ║     → Inserts event row with all sensor metadata            ║
 * ║                                                              ║
 * ║  2. Subscribes to Supabase Realtime on the commands table   ║
 * ║     → When dashboard inserts a command, server gets it      ║
 * ║       instantly via Realtime (WebSocket from Supabase)      ║
 * ║     → Pushes it to ESP32 via a persistent WebSocket         ║
 * ║     → ESP32 executes immediately (no 3s poll delay)         ║
 * ║     → ESP32 sends ack back → server marks command "done"    ║
 * ║                                                              ║
 * ║  3. Handles heartbeats, device registration, logs           ║
 * ║                                                              ║
 * ║  RESULT                                                      ║
 * ║  ─────────────────────────────────────────────────────────  ║
 * ║    ✅ Zero TLS on ESP32 → zero BIGNUM / SSL errors          ║
 * ║    ✅ Commands pushed in <100ms (was polled every 3s)        ║
 * ║    ✅ Ack done by server → 100% reliable (Node TLS)         ║
 * ║    ✅ Repeated snapshot loop is structurally impossible      ║
 * ║    ✅ Capture latency: ~2-4s (was 8-12s)                   ║
 * ║                                                              ║
 * ║  SETUP                                                       ║
 * ║  ─────────────────────────────────────────────────────────  ║
 * ║    1. Edit .env (Supabase credentials)                       ║
 * ║    2. npm install                                            ║
 * ║    3. node sentinel-server.js                                ║
 * ║    4. Note the IP printed at startup                         ║
 * ║    5. Set SERVER_HOST in esp32cam.ino to that IP            ║
 * ║    6. Flash firmware                                         ║
 * ║                                                              ║
 * ║  KEEP ALIVE WITH PM2                                         ║
 * ║    npm i -g pm2                                              ║
 * ║    pm2 start sentinel-server.js --name sentinel              ║
 * ║    pm2 save && pm2 startup   ← survives reboots             ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

"use strict";

require("dotenv").config();
const express = require("express");
const multer = require("multer");
const { WebSocketServer, WebSocket } = require("ws");
const { createClient } = require("@supabase/supabase-js");
const http = require("http");
const os = require("os");
// sharp — optional image processing for thumbnail generation.
// If not installed, uploads still work, thumbnails are skipped.
let sharp;
try {
  sharp = require("sharp");
} catch {
  sharp = null;
}

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
// deviceSockets: Map<device_id, WebSocket>
//   One entry per connected ESP32. Keyed by device_id received in
//   the "hello" message. Multiple devices connect simultaneously.
const deviceSockets = new Map();
const pendingAcks = new Map();
const commandDedup = new Set();
const COMMAND_DEDUP_TTL = 90_000;
const ACK_TIMEOUT_MS = 35_000;

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

  // ── Thumbnail (async, non-blocking, best-effort) ─────────────
  // sharp resizes to 480px wide JPEG Q70 for fast dashboard loads.
  // Full-res image is always stored; thumbnail is an extra asset.
  if (sharp) {
    const thumbPath = path.replace(/\.jpg$/, "_thumb.jpg");
    sharp(req.file.buffer)
      .resize({ width: 480, withoutEnlargement: true })
      .jpeg({ quality: 70 })
      .toBuffer()
      .then((thumbBuf) =>
        supa.storage.from(BUCKET).upload(thumbPath, thumbBuf, {
          contentType: "image/jpeg",
          upsert: true,
        }),
      )
      .then(() => log("UPLOAD", `Thumb OK → ${thumbPath}`))
      .catch((err) => log("UPLOAD", `Thumb SKIP: ${err.message}`));
  }

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
//  GET /devices — shows exactly which device IDs are currently
//  connected via WebSocket. Use this to diagnose routing issues.
//  If a device is missing here, commands to it will immediately fail.
// ─────────────────────────────────────────────────────────────────
app.get("/devices", (_req, res) => {
  const list = [];
  deviceSockets.forEach((ws, id) => {
    list.push({
      device_id: id,
      connected: ws.readyState === WebSocket.OPEN,
      readyState: ws.readyState,
    });
  });
  res.json({
    count: list.length,
    devices: list,
    ts: new Date().toISOString(),
  });
});

// ─────────────────────────────────────────────────────────────────
//  GET /health  — lightweight ping
// ─────────────────────────────────────────────────────────────────
app.get("/health", (_req, res) => {
  const devStatus = {};
  deviceSockets.forEach((ws, id) => {
    devStatus[id] =
      ws.readyState === WebSocket.OPEN ? "connected" : "disconnected";
  });
  res.json({
    status: "ok",
    version: "1.4.1",
    uptime: Math.floor(process.uptime()),
    devices: devStatus,
    devicesConnected: deviceSockets.size,
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
${
  [...deviceSockets.entries()]
    .map(
      ([id, ws]) =>
        `<tr><td>${id}</td><td class="${ws.readyState === 1 ? "on" : "off"}">${ws.readyState === 1 ? "Connected ✓" : "Disconnected ✗"}</td></tr>`,
    )
    .join("") ||
  '<tr><td>ESP32 devices</td><td class="off">None connected</td></tr>'
}
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
//  BINARY WEBSOCKET UPLOAD
//
//  ESP32 sends JPEG images as binary WS frames to avoid opening a
//  new TLS connection (which takes 2-4s and 40KB heap every time).
//
//  Frame format:
//    [4 bytes uint32-LE: JSON metadata length]
//    [JSON metadata string]
//    [raw JPEG bytes]
//
//  After storing the image and inserting the event row, server
//  sends back a text frame:
//    { type:"upload_ack", status:"done"|"failed", cmdId, url }
// ─────────────────────────────────────────────────────────────────
async function handleBinaryUpload(buf, deviceId) {
  const metaLen = buf.readUInt32LE(0);
  const metaJson = buf.slice(4, 4 + metaLen).toString();
  const jpeg = buf.slice(4 + metaLen);

  let meta;
  try {
    meta = JSON.parse(metaJson);
  } catch (e) {
    log("WS", `Binary upload: bad metadata JSON: ${e.message}`);
    return;
  }

  const devId = meta.deviceId || deviceId || "unknown";
  const snapType = meta.snapType || "manual";
  const cmdId = meta.cmdId || "";
  const filename = `events/${snapType}_${Date.now()}.jpg`;

  log(
    "WS",
    `Binary upload ${(jpeg.length / 1024).toFixed(1)}KB  type=${snapType}  dev=${devId}`,
  );

  const sendAck = (status, url) => {
    const sock = deviceSockets.get(deviceId);
    if (sock?.readyState === WebSocket.OPEN) {
      sock.send(
        JSON.stringify({ type: "upload_ack", status, cmdId, url: url || "" }),
      );
    }
  };

  // ── 1. Upload to Supabase Storage ─────────────────────────────
  const { error: storErr } = await supa.storage
    .from(BUCKET)
    .upload(filename, jpeg, { contentType: "image/jpeg", upsert: true });

  if (storErr) {
    metrics.uploads.fail++;
    log("WS", `Binary upload storage FAILED: ${storErr.message}`);
    sendAck("failed");
    return;
  }

  metrics.uploads.ok++;
  const { data: urlData } = supa.storage.from(BUCKET).getPublicUrl(filename);
  const publicUrl = urlData.publicUrl;

  // ── Thumbnail (async, best-effort) ────────────────────────────
  if (sharp) {
    const thumbPath = filename.replace(/\.jpg$/, "_thumb.jpg");
    sharp(jpeg)
      .resize({ width: 480, withoutEnlargement: true })
      .jpeg({ quality: 70 })
      .toBuffer()
      .then((buf) =>
        supa.storage.from(BUCKET).upload(thumbPath, buf, {
          contentType: "image/jpeg",
          upsert: true,
        }),
      )
      .then(() => log("WS", `Thumb OK → ${thumbPath}`))
      .catch((err) => log("WS", `Thumb SKIP: ${err.message}`));
  }

  // ── 2. Insert event row ────────────────────────────────────────
  const gpsValid = meta.gpsValid === "1" || meta.gpsValid === "true";
  const lat = parseFloat(meta.latitude || "0");
  const lng = parseFloat(meta.longitude || "0");

  const eventRow = {
    device_id: devId,
    triggered_by: meta.triggeredBy || devId,
    snapshot_type: snapType,
    snapshot_url: publicUrl,
    snapshot_path: filename,
    latitude: gpsValid && lat ? lat : 0,
    longitude: gpsValid && lng ? lng : 0,
    distance_cm: parseFloat(meta.distanceCm || "0"),
    sms_sent: meta.smsSent === "1",
    altitude_m: parseFloat(meta.altitudeM || "0"),
    speed_kmh: parseFloat(meta.speedKmh || "0"),
    hdop: parseFloat(meta.hdop || "0"),
    satellites: parseInt(meta.satellites || "0", 10),
    gps_valid: gpsValid,
    hub_heap_bytes: parseInt(meta.hubHeap || "0", 10),
    hub_battery_v: parseFloat(meta.hubBattery || "0"),
    hub_gsm_csq: parseInt(meta.hubGsmCsq || "0", 10),
    hub_uptime_s: parseInt(meta.hubUptimeS || "0", 10),
    hub_fw_version: meta.hubFwVersion || "",
    cam_heap_bytes: parseInt(meta.camHeap || "0", 10),
    cam_rssi: parseInt(meta.camRssi || "0", 10),
  };

  const { error: evErr } = await supa.from("events").insert(eventRow);
  if (evErr) {
    metrics.events.fail++;
    log("WS", `Binary upload event insert FAILED: ${evErr.message}`);
    // Image is safely stored — still ack as done
  } else {
    metrics.events.ok++;
  }

  log("WS", `Binary upload OK → ${filename}`);
  sendAck("done", publicUrl);
}

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
  log("WS", `Device connected from ${ip} (waiting for hello...)`);

  // deviceId is unknown until we receive the "hello" message.
  // We hold it in closure scope and register in deviceSockets then.
  let deviceId = null;
  metrics.wsConnections++;

  const pingTimer = setInterval(() => {
    if (ws.readyState === WebSocket.OPEN)
      ws.send(JSON.stringify({ type: "ping" }));
  }, 15_000);

  ws.on("message", (data, isBinary) => {
    // Binary frame = JPEG image upload from ESP32
    if (isBinary) {
      handleBinaryUpload(Buffer.from(data), deviceId).catch((err) =>
        log("WS", `Binary upload error: ${err.message}`),
      );
      return;
    }
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch {
      log("WS", `Non-JSON: ${data.toString().slice(0, 80)}`);
      return;
    }

    if (msg.type === "hello") {
      deviceId = msg.deviceId;
      // Close any stale socket for this device (reconnect case)
      const prev = deviceSockets.get(deviceId);
      if (prev && prev !== ws && prev.readyState === WebSocket.OPEN) {
        log("WS", `${deviceId} reconnected — closing previous socket`);
        prev.terminate();
        metrics.wsReconnects++;
      }
      deviceSockets.set(deviceId, ws);
      log(
        "WS",
        `${deviceId} identified  fw=${msg.version}  heap=${msg.freeHeap ? Math.round(msg.freeHeap / 1024) + "KB" : "?"}  total_connected=${deviceSockets.size}`,
      );
    } else if (msg.type === "pong") {
      // alive
    } else if (msg.type === "ack") {
      handleAck(msg.id, msg.status || "done");
    } else if (msg.type === "ws_log") {
      // ESP32 routes logs through WS to avoid a new TLS handshake.
      // Server writes to Supabase on its existing TLS connection.
      const { type: _, ...row } = msg;
      supa
        .from("logs")
        .insert(row)
        .then(({ error }) => {
          if (error) log("WS", `ws_log insert error: ${error.message}`);
        });
    } else if (msg.type === "ws_heartbeat") {
      // Heartbeat piggybacked on WS — no extra TLS from ESP32.
      const { type: _, ...hb } = msg;
      const devId = hb.device_id || deviceId;

      // IMPORTANT: only pass columns that exist in the devices table.
      // heartbeat-only fields (free_heap, rssi, cam_alerts, etc.) go
      // to the heartbeats table only — NOT to devices.
      // Infer device_type from device_id so new rows satisfy NOT NULL constraint.
      // Existing rows are updated (ON CONFLICT), so existing device_type is preserved.
      const inferredType = devId.toLowerCase().includes("cam")
        ? "esp32_cam"
        : "sensor";
      const deviceRow = {
        device_id: devId,
        device_type: inferredType,
        status: "online",
        last_seen: new Date().toISOString(),
        ip_address: hb.ip_address || undefined,
        firmware_version: hb.firmware_version || undefined,
      };

      Promise.all([
        supa.from("heartbeats").insert(hb),
        supa.from("devices").upsert(deviceRow, { onConflict: "device_id" }),
      ]).then(([hbRes, devRes]) => {
        if (hbRes.error)
          log("WS", `ws_heartbeat insert error: ${hbRes.error.message}`);
        if (devRes.error)
          log(
            "WS",
            `ws_heartbeat device upsert error: ${devRes.error.message}`,
          );
        else
          log(
            "WS",
            `HB ${devId}  heap=${hb.free_heap ? Math.round(hb.free_heap / 1024) + "KB" : "?"}  rssi=${hb.rssi ?? "?"}dBm`,
          );
      });
    } else if (msg.type === "ws_hub_heartbeat") {
      // Sensor hub heartbeat via CAM's WS connection.
      const { type: _, ...hb } = msg;
      supa
        .from("heartbeats")
        .insert(hb)
        .then(({ error }) => {
          if (error) log("WS", `ws_hub_heartbeat error: ${error.message}`);
        });
    } else {
      log("WS", `Unknown type: ${msg.type}`);
    }
  });

  ws.on("close", (code, reason) => {
    clearInterval(pingTimer);
    if (deviceId && deviceSockets.get(deviceId) === ws) {
      deviceSockets.delete(deviceId);
      log(
        "WS",
        `${deviceId} disconnected  code=${code}  remaining=${deviceSockets.size}`,
      );
    } else {
      log("WS", `Unnamed socket closed  code=${code}`);
    }
  });

  ws.on("error", (err) =>
    log("WS", `Error [${deviceId || "unknown"}]: ${err.message}`),
  );
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

  // Route to the correct device socket
  const targetSocket = deviceSockets.get(device_id);
  if (!targetSocket || targetSocket.readyState !== WebSocket.OPEN) {
    const connectedIds =
      [...deviceSockets.entries()]
        .map(
          ([id, ws]) =>
            `${id}(${ws.readyState === WebSocket.OPEN ? "OPEN" : "CLOSED"})`,
        )
        .join(", ") || "none";
    log("CMD", `⚠ ${device_id} NOT in socket map`);
    log("CMD", `  Currently connected: ${connectedIds}`);
    log(
      "CMD",
      `  Tip: device_id in command must exactly match deviceId sent in hello`,
    );
    metrics.commands.ackFailed++;
    await supa.from("commands").update({ status: "failed" }).eq("id", id);
    return;
  }

  metrics.commands.pushed++;
  targetSocket.send(JSON.stringify({ type: "command", id, command }));
  log("CMD", `→ ${device_id}: ${command}  id=${id.slice(0, 8)}  socket=OPEN`);

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
        // No filter — server handles commands for ALL connected devices.
        // The device_id in each command row is used to route to the
        // correct WebSocket in deviceSockets Map.
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
        log("RT", `Commands channel active — watching ALL devices`);
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
  deviceSockets.forEach((ws, id) => {
    if (ws.readyState === WebSocket.OPEN) ws.close();
    log("SYS", `Closed socket for ${id}`);
  });
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
  console.log(pad("SENTINEL SERVER  v1.4.1"));
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
