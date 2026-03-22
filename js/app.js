// ============================================================
// SENTINEL SURVEILLANCE DASHBOARD — app.js v5.0
// WebSocket Push Architecture
//   ✅ Commands pushed via WebSocket (<100ms, was 3s poll)
//   ✅ Snapshot button unlocks when new event arrives via Realtime
//   ✅ Server health pill (GET /health from sentinel-server.js)
//   ✅ Realtime subscriptions filtered to device only
//   ✅ Append-only DOM updates — no full re-render
//   ✅ Startup, motion, manual all update camera feed
// ============================================================

import { db } from "./supabaseClient.js";

// Read from js/config.js — update that file, not here
const { CAM_DEVICES, SENTINEL_SERVER_URL } = window.SENTINEL_CONFIG;
const SERVER_URL = SENTINEL_SERVER_URL;
// CAM_DEVICES is an array: [{ id, label }, ...]
// snapshotTarget tracks which camera the snapshot button targets.
let snapshotTarget = CAM_DEVICES[0]?.id || "ESP32-CAM-01";

// ============================================================
// STATE
// ============================================================
const state = {
  events: [],
  devices: [],
  logs: [],
  heartbeats: [],
  map: null,
  fullMap: null,
  markers: [],
  fullMarkers: [],
  currentView: "dashboard",
  unreadAlerts: 0,
  galleryFilter: "all",
  latestImageUrl: null,
  deviceStats: {}, // keyed by device_id → latest heartbeat
  totalEvents: 0, // true count from DB, not limited by local .limit(50)
  serverOnline: false,
  gpsState: {
    valid: false,
    satellites: 0,
    latitude: null,
    longitude: null,
    lastFixTime: null,
    lastDistance: null,
  },
};

// ============================================================
// DOM CACHE
// ============================================================
const _c = {};
const el = (id) => _c[id] || (_c[id] = document.getElementById(id));
const $ = (s, ctx) => (ctx || document).querySelector(s);
const $$ = (s, ctx) => [...(ctx || document).querySelectorAll(s)];

// ============================================================
// ============================================================
// TIME HELPERS
// ============================================================
// Supabase returns UTC strings often without "Z". To prevent JS
// from guessing local time, we force UTC parsing.
function parseUTC(iso) {
  if (!iso) return new Date();
  const s = String(iso);
  return new Date(s.endsWith("Z") || s.includes("+") ? s : s + "Z");
}

function timeAgo(iso) {
  const s = Math.floor((Date.now() - parseUTC(iso).getTime()) / 1000);
  if (s < 60) return `${Math.max(0, s)}s ago`;
  if (s < 3600) return `${Math.floor(s / 60)}m ago`;
  if (s < 86400) return `${Math.floor(s / 3600)}h ago`;
  return `${Math.floor(s / 86400)}d ago`;
}
function formatTime(iso) {
  return parseUTC(iso).toLocaleTimeString("en-US", { hour12: false });
}
function formatDateTime(iso) {
  return parseUTC(iso).toLocaleString("en-US", {
    year: "numeric",
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  });
}
function formatDateTimeShort(iso) {
  return parseUTC(iso).toLocaleString("en-US", {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    hour12: false,
  });
}

// ============================================================
// CLOCK
// ============================================================
function startClock() {
  const tick = () => {
    const e = el("clock");
    if (e)
      e.textContent = new Date().toLocaleTimeString("en-US", { hour12: false });
  };
  tick();
  setInterval(tick, 1000);
}

// ============================================================
// NAV
// ============================================================
function setupNav() {
  $$(".nav-item[data-view]").forEach((item) =>
    item.addEventListener("click", () => switchView(item.dataset.view)),
  );
  $$("[data-view]:not(.nav-item)").forEach((item) =>
    item.addEventListener("click", () => switchView(item.dataset.view)),
  );
}

function switchView(view) {
  state.currentView = view;
  $$(".nav-item").forEach((n) => n.classList.remove("active"));
  $(`.nav-item[data-view="${view}"]`)?.classList.add("active");
  $$(".view").forEach((v) => v.classList.remove("active"));
  el(`view-${view}`)?.classList.add("active");
  if (view === "gallery") renderGallery();
  if (view === "logs") renderLogsFull();
  if (view === "devices") renderDevicesFull();
  if (view === "map") {
    initFullMap();
    setTimeout(() => state.fullMap?.invalidateSize(), 80);
  }
}

// Debounced versions used by high-frequency Realtime channels.
// renderStats + renderDeviceCards fire on every heartbeat (30s) but
// also on every device-table change. Debouncing at 300ms collapses
// bursts without any visible lag.

// ============================================================
// UTILITIES
// ============================================================
function debounce(fn, delay) {
  let timer;
  return (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), delay);
  };
}

// We determine "online" based on a 3-minute heartbeat window
function computeLiveStatus(device) {
  if (!device.last_seen) return "offline";
  const age = Date.now() - parseUTC(device.last_seen).getTime();
  return age < (3 * 60 * 1000) ? "online" : "offline";
}

// Debounced versions used by high-frequency Realtime channels.
// renderStats + renderDeviceCards fire on every heartbeat (30s) but
// also on every device-table change. Debouncing at 300ms collapses
// bursts without any visible lag.
const debouncedRenderStats = debounce(renderStats, 300);
const debouncedRenderDeviceCards = debounce(renderDeviceCards, 300);
const debouncedUpdateTopbar = debounce(() => {
  updateTopbarPills();
  updateTopbarStatus();
}, 300);

// ============================================================
// LOAD INITIAL DATA
// ============================================================
async function loadInitialData() {
  try {
    // Events: omit removed fields (gps_accuracy, altitude, speed, hdop,
    //   ai_labels, hub_gsm_csq, hub_fw_version) per spec §3.
    // Devices: always fresh from DB, never cached.
    // Heartbeats: one per device_id (most recent) → deviceStats.
    const [eventsRes, devicesRes, logsRes, countRes] = await Promise.all([
      db
        .from("events")
        .select(
          "id,device_id,triggered_by,snapshot_type,latitude,longitude,snapshot_url,distance_cm,sms_sent,created_at,satellites,gps_valid,cam_rssi,cam_heap_bytes",
        )
        .order("created_at", { ascending: false })
        .limit(50),
      db
        .from("devices")
        .select(
          "device_id,name,device_type,status,last_seen,ip_address,firmware_version,created_at",
        )
        .order("last_seen", { ascending: false }),
      db
        .from("logs")
        .select("id,level,category,message,created_at,device_id")
        .order("created_at", { ascending: false })
        .limit(200),
      db.from("events").select("*", { count: "exact", head: true }),
    ]);

    state.totalEvents = countRes.count ?? eventsRes.data?.length ?? 0;
    state.events = eventsRes.data || [];
    state.devices = devicesRes.data || [];
    state.logs = logsRes.data || [];

    // Load latest heartbeat per device from DB (not cached)
    state.deviceStats = {};
    const deviceIds = state.devices.map((d) => d.device_id);
    if (deviceIds.length > 0) {
      // Fetch last 5 heartbeats per device to get one per device_id
      const { data: hbData } = await db
        .from("heartbeats")
        .select("*")
        .in("device_id", deviceIds)
        .order("created_at", { ascending: false })
        .limit(deviceIds.length * 5);
      (hbData || []).forEach((hb) => {
        // Only keep the newest per device_id (they're already DESC ordered)
        if (!state.deviceStats[hb.device_id])
          state.deviceStats[hb.device_id] = hb;
      });
    }

    const latestGPS = state.events.find(
      (e) => e.latitude && Math.abs(e.latitude) > 0.001,
    );
    if (latestGPS) updateGPSState(latestGPS);

    const latest = state.events.find((e) => e.snapshot_url);
    if (latest)
      updateCameraImage(latest.snapshot_url, latest.created_at, latest);

    renderAll();
  } catch (err) {
    console.error("[INIT]", err);
    showToast(
      "error",
      "Load Failed",
      err.message || "Cannot reach cloud",
      7000,
    );
  }
}

// ============================================================
// SERVER HEALTH CHECK
// Called at init and every 30s. Shows "Server" pill in topbar.
// ============================================================
async function checkServerHealth() {
  try {
    const res = await fetch(`${SERVER_URL}/health`, {
      signal: AbortSignal.timeout(3000),
    });
    if (res.ok) {
      const data = await res.json();
      state.serverOnline = true;
      const esp32 = data.esp32 === "connected";
      setPill(
        "pill-server",
        "online",
        esp32 ? "green" : "yellow",
        esp32 ? "Server+ESP32" : "Server only",
      );
    } else {
      throw new Error(`HTTP ${res.status}`);
    }
  } catch {
    state.serverOnline = false;
    setPill("pill-server", "offline", "red", "Server offline");
  }
}

// ============================================================
// RENDER ALL
// ============================================================
function renderAll() {
  renderStats();
  renderEventFeed();
  renderDeviceCards();
  renderLogFeed();
  renderMapMarkers();
  renderGPSBar();
  updateTopbarPills();
  updateTopbarStatus();
}

// ============================================================
// STATS
// ============================================================
function renderStats() {
  const today = new Date().toDateString();
  const todayMotion = state.events.filter(
    (e) =>
      new Date(e.created_at).toDateString() === today &&
      e.snapshot_type === "motion",
  ).length;
  const online = state.devices.filter((d) => computeLiveStatus(d) === "online").length;
  const recentDevices = state.devices.filter(d => {
    if (computeLiveStatus(d) === "online") return true;
    if (!d.last_seen) return false;
    return Date.now() - new Date(d.last_seen).getTime() < 86400000;
  }).length;
  const coords = state.gpsState.latitude
    ? `${state.gpsState.latitude.toFixed(4)}, ${state.gpsState.longitude.toFixed(4)}`
    : "—";

  setVal("stat-total", state.totalEvents);
  setVal("stat-today", todayMotion);
  setVal("stat-devices", `${online}/${recentDevices}`);
  setVal("stat-last-coords", coords);

  const badge = el("alert-badge");
  if (badge) {
    badge.textContent = state.unreadAlerts;
    badge.style.display = state.unreadAlerts > 0 ? "inline-flex" : "none";
  }
}
function setVal(id, v) {
  const e = el(id);
  if (e) e.textContent = v;
}

// ============================================================
// CAMERA IMAGE
// ============================================================
function updateCameraImage(url, timestamp, event) {
  if (!url) return;
  state.latestImageUrl = url;

  const imgEl = el("live-img");
  const tsEl = el("live-timestamp");
  const overlay = el("live-overlay");
  const loadEl = el("live-loading");

  if (imgEl) {
    // Try thumbnail first (server generates _thumb.jpg at 480px wide via sharp)
    // Falls back to full URL on 404. Set src immediately so old image shows
    // during load — no blank + spinner delay.
    const baseUrl = url.includes("_thumb") ? url : url.replace(/\.jpg($|\?)/, "_thumb.jpg$1");
    const thumbSrc = baseUrl + (baseUrl.includes("?") ? "&" : "?") + "t=" + Date.now();
    const fullSrc = url + "?t=" + Date.now();

    if (loadEl) loadEl.classList.add("visible");

    // Set the src immediately — old image still visible while new one loads
    imgEl.style.display = "block";
    overlay?.classList.add("hidden");

    const tryLoad = (src, fallback) => {
      const img = new Image();
      img.onload = () => {
        imgEl.src = src;
        loadEl?.classList.remove("visible");
        flashCapture();
      };
      img.onerror = () => {
        if (fallback) tryLoad(fallback, null);
        else loadEl?.classList.remove("visible");
      };
      img.src = src;
    };
    tryLoad(thumbSrc, fullSrc);
  }

  if (tsEl && timestamp) tsEl.textContent = formatDateTime(timestamp);

  const fullImg = el("live-img-full");
  const fullOver = el("live-overlay-full");
  if (fullImg && url) {
    const fullSrc = url + "?t=" + Date.now();
    fullImg.src = fullSrc;
    fullImg.style.display = "block";
    fullOver?.classList.add("hidden");
  }
  if (el("live-timestamp-full") && timestamp)
    el("live-timestamp-full").textContent = formatDateTime(timestamp);

  if (event) {
    setVal("live-type-full", event.snapshot_type || "—");
    const gpsText =
      event.latitude && Math.abs(event.latitude) > 0.001
        ? `${event.latitude.toFixed(5)}, ${event.longitude.toFixed(5)}`
        : "No GPS Fix";
    setVal("live-gps-full", gpsText);
    addToRecentCaptures(event);
  }
}

function flashCapture() {
  const wrap = $(".live-feed-wrap");
  if (!wrap) return;
  const f = document.createElement("div");
  f.className = "live-flash";
  wrap.appendChild(f);
  setTimeout(() => f.remove(), 350);
}

function addToRecentCaptures(event) {
  const list = el("live-recent");
  if (!list) return;
  const div = document.createElement("div");
  div.className = "recent-capture-item";
  if (event.snapshot_url) {
    const img = document.createElement("img");
    img.src = event.snapshot_url;
    img.className = "recent-thumb";
    img.loading = "lazy";
    div.appendChild(img);
  }
  const info = document.createElement("div");
  info.innerHTML = `
    <div class="recent-type ${event.snapshot_type || ""}">${(event.snapshot_type || "—").toUpperCase()}</div>
    <div class="recent-time">${formatDateTimeShort(event.created_at)}</div>
  `;
  div.appendChild(info);
  div.addEventListener("click", () => openLightbox(event));
  list.prepend(div);
  while (list.children.length > 12) list.removeChild(list.lastChild);
}

// ============================================================
// SNAPSHOT BUTTON
// Commands are now pushed via WebSocket (<100ms) — not polled.
// Dashboard inserts a row → Supabase Realtime → Node server →
// WebSocket push → ESP32 executes → ack → DB updated.
// Button stays locked until new event arrives via Realtime.
// ============================================================
let snapshotPending = false;

function setupSnapshotButton() {
  ["btn-snapshot", "btn-snapshot-2"].forEach((id) => {
    el(id)?.addEventListener("click", triggerSnapshot);
  });
  buildCameraSelector();
}

// Build a small <select> inside every element with class "cam-selector-wrap"
// so the user can pick which camera to snapshot.
function buildCameraSelector() {
  document.querySelectorAll(".cam-selector-wrap").forEach((wrap) => {
    // Hide if only one camera is configured
    if (CAM_DEVICES.length <= 1) {
      wrap.style.display = "none";
      return;
    }
    wrap.innerHTML = "";
    CAM_DEVICES.forEach(({ id, label }) => {
      const btn = document.createElement("button");
      btn.className = "cam-btn" + (id === snapshotTarget ? " active" : "");
      btn.dataset.camId = id;
      btn.title = `Switch snapshot target to ${label}`;
      // Camera icon SVG + label
      btn.innerHTML = `<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="3"/><path d="M3 9a2 2 0 012-2h.93a2 2 0 001.664-.89l.812-1.22A2 2 0 0110.07 4h3.86a2 2 0 011.664.89l.812 1.22A2 2 0 0018.07 7H19a2 2 0 012 2v9a2 2 0 01-2 2H5a2 2 0 01-2-2V9z"/></svg>${label}`;
      btn.addEventListener("click", () => {
        snapshotTarget = id;
        // Update all cam-selector-wrap instances
        document.querySelectorAll(".cam-btn").forEach((b) => {
          b.classList.toggle("active", b.dataset.camId === id);
        });
      });
      wrap.appendChild(btn);
    });
  });
}

async function triggerSnapshot() {
  if (snapshotPending) {
    showToast("info", "Please Wait", "Snapshot in progress...", 2000);
    return;
  }

  snapshotPending = true;
  const btns = ["btn-snapshot", "btn-snapshot-2", "btn-snapshot-full"]
    .map((id) => el(id))
    .filter(Boolean);

  btns.forEach((b) => {
    b.disabled = true;
    b.classList.add("btn-loading");
    b._origText = b.innerHTML;
    b.innerHTML = `<div class="btn-spinner" style="width:12px;height:12px;border-width:2px"></div> Capturing...`;
  });

  el("live-loading")?.classList.add("visible");

  try {
    // Delete any stale pending/processing commands
    const { error: delError } = await db
      .from("commands")
      .delete()
      .eq("device_id", snapshotTarget)
      .in("status", ["pending", "processing"]);
    if (delError) console.warn("[CMD] Delete stale failed:", delError.message);

    const targetLabel =
      CAM_DEVICES.find((d) => d.id === snapshotTarget)?.label || snapshotTarget;

    const commandTime = new Date().toISOString();

    const { error } = await db.from("commands").insert({
      device_id: snapshotTarget,
      command: "capture",
      status: "pending",
    });
    if (error) throw error;

    showToast(
      "info",
      "Snapshot Sent",
      `${targetLabel} — waiting for image...`,
      10000,
    );

    // Ping server directly to wake it up (in case Render is sleeping)
    fetch(`${SERVER_URL}/health`).catch(() => {});

    // Poll Supabase every 3s for a new event from this device
    // (fallback for when SSE drops or misses the event)
    let pollCount = 0;
    const maxPolls = 30; // 30 × 3s = 90s max

    const pollInterval = setInterval(async () => {
      if (!snapshotPending) {
        clearInterval(pollInterval);
        return;
      }

      pollCount++;
      if (pollCount > maxPolls) {
        clearInterval(pollInterval);
        snapshotPending = false;
        unlockSnapshotButtons();
        el("live-loading")?.classList.remove("visible");
        showToast(
          "warn",
          "No Response",
          "Camera did not respond in 90s — check device is online",
          6000,
        );
        return;
      }

      try {
        const { data } = await db
          .from("events")
          .select("id,device_id,snapshot_type,snapshot_url,created_at,latitude,longitude,triggered_by,distance_cm,sms_sent,satellites,gps_valid,cam_rssi,cam_heap_bytes")
          .eq("device_id", snapshotTarget)
          .gte("created_at", commandTime)
          .order("created_at", { ascending: false })
          .limit(1);

        if (data?.length > 0) {
          clearInterval(pollInterval);
          const event = data[0];

          // Avoid duplicate if SSE already handled it
          const alreadyInList = state.events.find((e) => e.id === event.id);
          if (!alreadyInList) {
            state.events.unshift(event);
            state.totalEvents++;
            prependEventToFeed(event);
            addSingleMarker(event);
            renderStats();
            updateTopbarPills();
            if (state.currentView === "gallery") renderGallery();
          }

          if (event.snapshot_url) {
            updateCameraImage(event.snapshot_url, event.created_at, event);
          }

          snapshotPending = false;
          unlockSnapshotButtons();
          el("live-loading")?.classList.remove("visible");
          showToast("success", "Snapshot Ready", "Image captured successfully", 3000);
        }
      } catch (pollErr) {
        console.warn("[POLL] Error polling for snapshot:", pollErr.message);
      }
    }, 3000);

  } catch (err) {
    snapshotPending = false;
    unlockSnapshotButtons();
    el("live-loading")?.classList.remove("visible");
    showToast(
      "error",
      "Command Failed",
      err.message || "Could not reach database",
      5000,
    );
  }
}

function unlockSnapshotButtons() {
  ["btn-snapshot", "btn-snapshot-2", "btn-snapshot-full"]
    .map((id) => el(id))
    .filter(Boolean)
    .forEach((b) => {
      b.disabled = false;
      b.classList.remove("btn-loading");
      if (b._origText) {
        b.innerHTML = b._origText;
        delete b._origText;
      }
    });
}

// ============================================================
// GPS STATE + STATUS BAR
// ============================================================
function updateGPSState(event) {
  if (!event) return;
  const valid =
    event.latitude && event.longitude && Math.abs(event.latitude) > 0.001;
  if (valid) {
    state.gpsState.valid = true;
    state.gpsState.latitude = event.latitude;
    state.gpsState.longitude = event.longitude;
    state.gpsState.lastFixTime = event.created_at;
  }
  if (event.satellites !== undefined) state.gpsState.satellites = event.satellites;
  if (event.distance_cm) state.gpsState.lastDistance = event.distance_cm;
}

function renderGPSBar() {
  const sats = state.gpsState.satellites || 0;
  const valid = state.gpsState.valid;
  const hasLastCoords = state.gpsState.latitude != null;

  let label, sub, cls;
  if (!valid) {
    if (sats === 0) {
      label = "No GPS Signal";
      sub = hasLastCoords ? "Showing last known position" : "No satellite data";
      cls = "fix-none";
    } else {
      label = "Searching...";
      sub = `${sats} satellite${sats !== 1 ? "s" : ""} visible`;
      cls = "fix-search";
    }
  } else if (sats >= 7) {
    label = "3D Fix — Strong";
    sub = `${sats} satellites · High accuracy`;
    cls = "fix-strong";
  } else if (sats >= 4) {
    label = "3D Fix — Good";
    sub = `${sats} satellites · Standard accuracy`;
    cls = "fix-good";
  } else if (sats >= 1) {
    label = "2D Fix — Weak";
    sub = `${sats} satellite${sats !== 1 ? "s" : ""} · Low accuracy`;
    cls = "fix-weak";
  } else {
    label = "3D Fix";
    sub = "Valid location";
    cls = "fix-good";
  }

  const iconWrap = el("gps-icon-wrap");
  if (iconWrap) iconWrap.className = `gps-icon-wrap ${cls}`;

  const fixLabel = el("gps-fix-label");
  if (fixLabel) {
    fixLabel.textContent = label;
    fixLabel.className = `gps-fix-label ${cls}`;
  }
  setVal("gps-fix-sub", sub);
  // Only show active satellite count when we have a fix; show 0 when none
  setVal("gps-sats", valid ? sats : 0);

  // Satellite bars go dark when no fix
  const activeBars = valid ? Math.min(sats, 8) : 0;
  $$(".gps-sat-bar", el("gps-sats-display")).forEach((bar, i) =>
    bar.classList.toggle("active", i < activeBars),
  );

  // Always show last known coordinates — go grey labelled when no active fix
  const coordEl = el("gps-coords");
  if (coordEl) {
    if (hasLastCoords) {
      coordEl.textContent = `${state.gpsState.latitude.toFixed(6)}, ${state.gpsState.longitude.toFixed(6)}`;
      coordEl.style.color = valid ? "" : "var(--white-25)";
    } else {
      coordEl.textContent = "—";
      coordEl.style.color = "";
    }
  }
  setVal(
    "gps-last-fix",
    state.gpsState.lastFixTime
      ? formatDateTime(state.gpsState.lastFixTime)
      : "—",
  );
  setVal(
    "gps-last-distance",
    state.gpsState.lastDistance ? `${state.gpsState.lastDistance} cm` : "—",
  );

  const dotColor =
    {
      "fix-strong": "green",
      "fix-good": "green",
      "fix-weak": "orange",
      "fix-search": "yellow",
      "fix-none": "grey",
    }[cls] || "grey";
  const pillState =
    cls === "fix-none" ? "offline" : cls === "fix-search" ? "warn" : "online";
  setPill(
    "pill-gps",
    pillState,
    dotColor,
    valid && sats > 0 ? `${sats} Sats` : hasLastCoords ? "Last Fix" : "No Fix",
  );
}

// ============================================================
// TOPBAR PILLS
// ============================================================
function updateTopbarPills() {
  // ── Camera pill: use the SELECTED camera (snapshotTarget) ─────
  // All values come from DB-fresh state.devices + state.deviceStats.
  const camDev = state.devices.find((d) => d.device_id === snapshotTarget);
  const latestHB = state.deviceStats[snapshotTarget];
  const camOnline = computeLiveStatus(camDev || {});

  const camLabel = camOnline
    ? "Online"
    : camDev?.last_seen
      ? timeAgo(camDev.last_seen)
      : "Offline";
  setPill(
    "pill-camera",
    camOnline ? "online" : "offline",
    camOnline ? "green" : "grey",
    camLabel,
  );

  // ── WiFi pill: RSSI from latest heartbeat for selected camera ─
  const rssi = latestHB?.rssi ?? latestHB?.cam_rssi ?? null;
  const rssiColor =
    rssi === null
      ? camOnline
        ? "green"
        : "grey"
      : rssi > -65
        ? "green"
        : rssi > -80
          ? "yellow"
          : "red";
  setPill(
    "pill-wifi",
    camOnline ? "online" : "offline",
    camOnline ? rssiColor : "grey",
    rssi !== null ? `${rssi} dBm` : camOnline ? "Connected" : "—",
  );

  // ── ESP-NOW / Sensor pill ──────────────────────────────────────
  const recentMotion = state.events.find((e) => e.snapshot_type === "motion");
  const espActive =
    recentMotion && Date.now() - new Date(recentMotion.created_at) < 600_000;
  const hubAlive = Object.values(state.deviceStats).some(
    (hb) =>
      hb.hub_uptime_s > 0 &&
      hb.created_at &&
      Date.now() - new Date(hb.created_at).getTime() < 180_000,
  );
  const espOnline = espActive || hubAlive;
  setPill(
    "pill-espnow",
    espOnline ? "online" : "offline",
    espOnline ? "green" : "grey",
    espOnline ? "Active" : "Standby",
  );

  // ── GSM pill: hub heartbeat data only (never inferred) ────────
  // CSQ ranges: 99=no signal, 0=off, 1-9=poor, 10-14=weak, 15-19=fair, 20-30=good, >30=excellent
  const hubHB = Object.values(state.deviceStats).find(
    (hb) => hb.hub_gsm_csq !== undefined,
  );
  const gsmCsq = hubHB?.hub_gsm_csq;

  let gsmState = "offline";
  let gsmColor = "grey";
  let gsmLabel = "—";

  if (gsmCsq != null) {
    if (gsmCsq === 99) {
      // 99 means "not known or not detectable" per 3GPP — definitively no signal
      gsmState = "offline";
      gsmColor = "grey";
      gsmLabel = "No Signal";
    } else if (gsmCsq === 0) {
      gsmState = "offline";
      gsmColor = "grey";
      gsmLabel = "Off (CSQ 0)";
    } else if (gsmCsq <= 9) {
      // -113 to -97 dBm — very poor, may drop calls
      gsmState = "warn";
      gsmColor = "red";
      gsmLabel = `CSQ ${gsmCsq} · Poor`;
    } else if (gsmCsq <= 14) {
      // -95 to -85 dBm — marginal
      gsmState = "warn";
      gsmColor = "orange";
      gsmLabel = `CSQ ${gsmCsq} · Weak`;
    } else if (gsmCsq <= 19) {
      // -83 to -73 dBm — acceptable
      gsmState = "online";
      gsmColor = "yellow";
      gsmLabel = `CSQ ${gsmCsq} · Fair`;
    } else if (gsmCsq <= 30) {
      // -71 to -51 dBm — good
      gsmState = "online";
      gsmColor = "green";
      gsmLabel = `CSQ ${gsmCsq} · Good`;
    } else {
      // -49 dBm and above — excellent
      gsmState = "online";
      gsmColor = "green";
      gsmLabel = `CSQ ${gsmCsq} · Excellent`;
    }
  } else if (state.events.some((e) => e.sms_sent)) {
    gsmState = "online";
    gsmColor = "green";
    gsmLabel = "Ready";
  }

  setPill("pill-gsm", gsmState, gsmColor, gsmLabel);
}

function setPill(id, state, dotColor, text) {
  const pill = el(id);
  const dot = el(`${id}-dot`);
  const textEl = el(`${id}-text`);
  if (!pill) return;
  pill.className = `status-pill ${state}`;
  if (dot) dot.className = `pill-dot ${dotColor}`;
  if (textEl) textEl.textContent = text;
}

function updateTopbarStatus() {
  // Use computeLiveStatus (time-based) — same as renderStats() so both counts agree
  const online = state.devices.filter((d) => computeLiveStatus(d) === "online").length;
  const total = state.devices.length;
  const statusEl = el("topbar-device-status");
  if (statusEl) {
    statusEl.textContent = `${online}/${total} Devices`;
  }
  const dot = el("topbar-device-dot");
  if (dot)
    dot.className =
      "status-dot" + (online === 0 ? " off" : online < total ? " warn" : "");
}

// ============================================================
// EVENT FEED — fragment-based, append-only
// ============================================================
function renderEventFeed() {
  const feed = el("event-feed");
  if (!feed) return;
  if (state.events.length === 0) {
    feed.innerHTML = `<div class="empty-state"><div class="empty-icon">📭</div><p class="empty-text">No events yet</p><p class="empty-sub">Waiting for motion or manual capture</p></div>`;
    return;
  }
  const frag = document.createDocumentFragment();
  state.events
    .slice(0, 25)
    .forEach((e) => frag.appendChild(createEventItem(e)));
  feed.innerHTML = "";
  feed.appendChild(frag);
}

function prependEventToFeed(event) {
  const feed = el("event-feed");
  if (!feed) return;
  const empty = feed.querySelector(".empty-state");
  if (empty) feed.innerHTML = "";
  feed.insertBefore(createEventItem(event), feed.firstChild);
  while (feed.children.length > 25) feed.removeChild(feed.lastChild);
}

function createEventItem(event) {
  const div = document.createElement("div");
  div.className = "event-item";
  const sensor = event.triggered_by || "unknown";
  const snapType = event.snapshot_type || "motion";
  const coords =
    event.latitude && Math.abs(event.latitude) > 0.001
      ? `${event.latitude.toFixed(5)}, ${event.longitude.toFixed(5)}`
      : "No GPS Fix";
  div.innerHTML = `
    ${
      event.snapshot_url
        ? `<img class="event-thumb" src="${event.snapshot_url}" alt="snap" loading="lazy">`
        : `<div class="event-no-thumb"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="8.5" cy="8.5" r="1.5"/><path d="M21 15l-5-5L5 21"/></svg></div>`
    }
    <div class="event-info">
      <div class="event-sensor ${sensor}">${sensor.toUpperCase()} <span class="type-pill ${snapType}">${snapType}</span></div>
      <div class="event-coords">📍 ${coords}</div>
      ${event.distance_cm ? `<div class="event-coords" style="color:var(--yellow)">📡 ${event.distance_cm} cm</div>` : ""}
    </div>
    <div class="event-time">${timeAgo(event.created_at)}</div>`;
  div.addEventListener("click", () => openLightbox(event));
  return div;
}

// ============================================================
// DEVICE CARDS
// ============================================================
function renderDeviceCards() {
  const container = el("device-cards");
  if (!container) return;
  if (state.devices.length === 0) {
    container.innerHTML = `<div class="empty-state"><div class="empty-icon">📡</div><p class="empty-text">No devices registered</p></div>`;
    return;
  }
  const frag = document.createDocumentFragment();
  state.devices.forEach((d) => frag.appendChild(createDeviceCard(d)));
  container.innerHTML = "";
  container.appendChild(frag);
}

function createDeviceCard(device) {
  const div = document.createElement("div");
  div.className = "device-card";
  div.dataset.deviceId = device.device_id;

  const hb = state.deviceStats[device.device_id];
  const status = device.status || "offline";
  const lastSeen = device.last_seen ? timeAgo(device.last_seen) : "never";
  const isCam = device.device_type?.includes("cam");

  // Live metrics from latest heartbeat
  const rssi = hb?.rssi ? `${hb.rssi} dBm` : null;
  const heap = hb?.free_heap ? `${Math.round(hb.free_heap / 1024)} KB` : null;
  const uptime = hb?.uptime_seconds ? formatUptime(hb.uptime_seconds) : null;
  const fw = hb?.firmware_version || device.firmware_version || null;
  const ip = hb?.ip_address || device.ip_address || "—";

  // Signal strength colour
  const rssiColor = !rssi
    ? "grey"
    : hb.rssi > -65
      ? "var(--green)"
      : hb.rssi > -80
        ? "var(--yellow)"
        : "var(--red-bright)";

  const metaBits = [ip, uptime ? `↑ ${uptime}` : null, fw ? `fw ${fw}` : null]
    .filter(Boolean)
    .join(" · ");

  div.innerHTML = `
    <div class="device-icon">
      ${
        isCam
          ? `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75"><path d="M15 10l4.553-2.069A1 1 0 0121 8.87v6.26a1 1 0 01-1.447.9L15 14M4 8h11a2 2 0 012 2v4a2 2 0 01-2 2H4a2 2 0 01-2-2v-4a2 2 0 012-2z"/></svg>`
          : `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75"><rect x="2" y="4" width="20" height="16" rx="2"/><path d="M8 20h8M12 16v4"/></svg>`
      }
    </div>
    <div class="device-info">
      <div class="device-name">${device.device_id}</div>
      <div class="device-meta">${metaBits} · ${lastSeen}</div>
      ${
        rssi || heap
          ? `<div class="device-live-stats">
        ${rssi ? `<span style="color:${rssiColor};font-size:10px">⬡ ${rssi}</span>` : ""}
        ${heap ? `<span style="color:var(--white-dim);font-size:10px">⬡ ${heap}</span>` : ""}
      </div>`
          : ""
      }
    </div>
    <div class="device-status-pill ${status}">${status}</div>`;
  return div;
}

function formatUptime(seconds) {
  if (!seconds) return null;
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return h > 0 ? `${h}h ${m}m` : `${m}m`;
}

// ============================================================
// LOG FEEDS
// ============================================================
function renderLogFeed() {
  const feed = el("log-feed");
  if (!feed) return;
  const frag = document.createDocumentFragment();
  state.logs.slice(0, 25).forEach((l) => frag.appendChild(createLogItem(l)));
  feed.innerHTML = "";
  feed.appendChild(frag);
}

function renderLogsFull() {
  const feed = el("log-full");
  if (!feed) return;
  if (state.logs.length === 0) {
    feed.innerHTML = `<div class="empty-state"><p class="empty-text">No logs yet</p></div>`;
    return;
  }
  const frag = document.createDocumentFragment();
  state.logs.forEach((l) => frag.appendChild(createLogItem(l)));
  feed.innerHTML = "";
  feed.appendChild(frag);
}

function prependLogToFeed(log) {
  ["log-feed", "log-full"].forEach((id) => {
    const feed = el(id);
    if (!feed) return;
    feed.insertBefore(createLogItem(log), feed.firstChild);
    while (feed.children.length > 50) feed.removeChild(feed.lastChild);
  });
}

function createLogItem(log) {
  const div = document.createElement("div");
  div.className = "log-item";
  div.innerHTML = `
    <span class="log-level ${log.level || "info"}">${log.level || "info"}</span>
    <span class="log-text"><span class="log-category">[${log.category || "sys"}]</span> ${log.message}</span>
    <span class="log-time">${formatDateTime(log.created_at)}</span>`;
  return div;
}

// ============================================================
// GALLERY
// ============================================================
function setupGalleryFilters() {
  $$(".filter-chip").forEach((chip) => {
    chip.addEventListener("click", () => {
      $$(".filter-chip").forEach((c) => c.classList.remove("active"));
      chip.classList.add("active");
      state.galleryFilter = chip.dataset.filter;
      renderGallery();
    });
  });
}

function renderGallery() {
  const grid = el("gallery-grid");
  if (!grid) return;
  let filtered = state.events.filter((e) => e.snapshot_url);
  if (state.galleryFilter !== "all")
    filtered = filtered.filter((e) => e.snapshot_type === state.galleryFilter);
  if (filtered.length === 0) {
    grid.innerHTML = `<div class="empty-state" style="grid-column:1/-1;padding:80px 24px"><p class="empty-text">No snapshots for this filter</p></div>`;
    return;
  }
  const frag = document.createDocumentFragment();
  filtered.forEach((event) => {
    const card = document.createElement("div");
    card.className = "gallery-card";
    const snapType = event.snapshot_type || "motion";
    card.innerHTML = `
      <img class="gallery-img" src="${event.snapshot_url}" alt="snapshot" loading="lazy">
      <div class="gallery-meta">
        <div class="gallery-sensor">${(event.triggered_by || "—").toUpperCase()} · ${snapType.toUpperCase()}</div>
        <div class="gallery-time">${formatDateTime(event.created_at)}</div>
        <div class="gallery-coords">${event.latitude?.toFixed(5) || "—"}, ${event.longitude?.toFixed(5) || "—"}</div>
      </div>`;
    card.addEventListener("click", () => openLightbox(event));
    frag.appendChild(card);
  });
  grid.innerHTML = "";
  grid.appendChild(frag);
}

// ============================================================
// DEVICES FULL VIEW
// ============================================================
// ── helper: coloured status pill string ──────────────────────
function statusBadge(status) {
  return `<span class="device-status-pill ${status}">${status}</span>`;
}

// ── helper: signal colour from RSSI value ────────────────────
function rssiColor(rssi) {
  if (rssi == null) return "var(--white-45)";
  if (rssi > -65) return "var(--green)";
  if (rssi > -80) return "var(--yellow)";
  return "var(--red-bright)";
}

// ── helper: render one data field row ────────────────────────
function devField(label, value, color) {
  const c = color ? `color:${color}` : "";
  return `
    <div class="dv-field">
      <div class="dv-label">${label}</div>
      <div class="dv-value" style="${c}">${value ?? "—"}</div>
    </div>`;
}

// ── helper: section header inside a device card ───────────────
function devSection(title, fields) {
  return `
    <div class="dv-section">
      <div class="dv-section-title">${title}</div>
      <div class="dv-section-grid">${fields.join("")}</div>
    </div>`;
}

function renderDevicesFull() {
  const container = el("devices-full");
  if (!container) return;
  if (state.devices.length === 0) {
    container.innerHTML = `<div class="empty-state" style="padding:80px"><p class="empty-text">No devices registered</p></div>`;
    return;
  }
  container.innerHTML = "";

  state.devices.forEach((device) => {
    // ── All data sourced from DB state only ─────────────────────
    const hb = state.deviceStats[device.device_id] || {};
    const status = computeLiveStatus(hb || device);
    const isCam = device.device_type?.includes("cam");

    // Device identity
    const devName = device.device_id;
    const lastSeen = device.last_seen
      ? formatDateTime(device.last_seen)
      : "Never";
    const lastAgo = device.last_seen ? timeAgo(device.last_seen) : "—";
    const fw = hb.firmware_version || device.firmware_version || "—";
    const ip = hb.ip_address || device.ip_address || "—";
    const uptime = hb.uptime_seconds ? formatUptime(hb.uptime_seconds) : "—";

    // Connectivity
    const rssi = hb.rssi ?? hb.cam_rssi ?? null;
    const heap = hb.free_heap ? `${Math.round(hb.free_heap / 1024)} KB` : "—";
    const wsState =
      hb.ws_connected != null
        ? hb.ws_connected
          ? "Connected"
          : "Disconnected"
        : "—";

    // Hub data (sensor hub fields from heartbeat)
    const hubBatV = hb.hub_battery_v ? `${hb.hub_battery_v.toFixed(2)} V` : "—";
    const hubHeap = hb.hub_free_heap
      ? `${Math.round(hb.hub_free_heap / 1024)} KB`
      : "—";
    const gpsValid =
      hb.hub_gps_valid != null ? (hb.hub_gps_valid ? "Fix" : "No Fix") : "—";
    const gpsSats = hb.hub_gps_sats ?? "—";
    const gsmReady =
      hb.hub_gsm_ready != null ? (hb.hub_gsm_ready ? "Ready" : "Off") : "—";

    // Latest event for this specific device
    const latestEv = state.events.find((e) => e.device_id === device.device_id);
    const evCount = state.events.filter(
      (e) => e.device_id === device.device_id,
    ).length;

    // Camera-specific stats
    const camAlerts = hb.cam_alerts ?? "—";
    const camUploads =
      hb.cam_upload_ok != null
        ? `${hb.cam_upload_ok} OK / ${hb.cam_upload_fail ?? 0} fail`
        : "—";

    // ── Build card ──────────────────────────────────────────────
    const card = document.createElement("div");
    card.className = "dv-card";
    card.dataset.deviceId = device.device_id;

    card.innerHTML = `
      <!-- Card header: device identity + status ──────────────── -->
      <div class="dv-header">
        <div class="dv-icon ${isCam ? "cam" : "hub"}">
          ${
            isCam
              ? `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75"><path d="M15 10l4.553-2.069A1 1 0 0121 8.87v6.26a1 1 0 01-1.447.9L15 14M4 8h11a2 2 0 012 2v4a2 2 0 01-2 2H4a2 2 0 01-2-2v-4a2 2 0 012-2z"/></svg>`
              : `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75"><rect x="2" y="4" width="20" height="16" rx="2"/><path d="M8 20h8M12 16v4"/></svg>`
          }
        </div>
        <div class="dv-header-info">
          <div class="dv-name">${devName}</div>
          <div class="dv-id">${device.device_id} · ${device.device_type || "unknown"}</div>
        </div>
        <div class="dv-header-right">
          ${statusBadge(status)}
          <div class="dv-last-seen">${lastAgo}</div>
        </div>
      </div>

      <!-- Content wrapper (flex row on desktop) ─────────────── -->
      <div class="dv-content">
        <!-- Sections grid ─────────────────────────────────────── -->
        <div class="dv-sections">

          ${devSection("Identity", [
            devField("Device Name", devName),
            devField("Device ID", device.device_id),
            devField("Type", device.device_type || "—"),
            devField("Firmware", fw),
            devField("IP Address", ip),
            devField("Last Seen", lastSeen),
          ])}

          ${devSection("Connectivity", [
            devField(
              "Status",
              status.toUpperCase(),
              status === "online" ? "var(--green)" : "var(--white-45)",
            ),
            devField(
              "WiFi RSSI",
              rssi !== null ? `${rssi} dBm` : "—",
              rssiColor(rssi),
            ),
            devField(
              "WebSocket",
              wsState,
              wsState === "Connected" ? "var(--green)" : "var(--white-45)",
            ),
            devField("Free Heap", heap),
            devField("Uptime", uptime),
          ])}

          ${
            isCam
              ? devSection("Camera", [
                  devField(
                    "Camera State",
                    hb.cam_alerts != null ? "Active" : "—",
                  ),
                  devField("Alerts", camAlerts),
                  devField("Uploads", camUploads),
                  devField("Events (loaded)", evCount),
                  devField(
                    "Last Event",
                    latestEv ? timeAgo(latestEv.created_at) : "—",
                  ),
                  devField("Last Type", latestEv?.snapshot_type || "—"),
                ])
              : ""
          }

          ${devSection("Sensor Hub", [
            devField(
              "Battery",
              hubBatV,
              hubBatV !== "—" && parseFloat(hubBatV) < 3.5
                ? "var(--yellow)"
                : undefined,
            ),
            devField("Hub Heap", hubHeap),
            devField(
              "GPS",
              gpsValid,
              gpsValid === "Fix" ? "var(--green)" : "var(--white-45)",
            ),
            devField("Satellites", gpsSats),
            devField(
              "GSM",
              gsmReady,
              gsmReady === "Ready" ? "var(--green)" : "var(--white-45)",
            ),
          ])}

        </div>

        ${
          latestEv?.snapshot_url
            ? `
        <div class="dv-latest-snap">
          <div class="dv-snap-label">Latest Capture · ${formatDateTime(latestEv.created_at)}</div>
          <img class="dv-snap-img" src="${latestEv.snapshot_url}" alt="latest capture" loading="lazy">
        </div>`
            : ""
        }
      </div>`;

    container.appendChild(card);
  });
}

// ============================================================
// MAP
// ============================================================
function initMap() {
  const mapEl = document.getElementById("map");
  if (!mapEl || typeof L === "undefined") return;
  state.map = L.map("map", { center: [0, 0], zoom: 2 });
  L.tileLayer("https://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}", {
    maxZoom: 19,
  }).addTo(state.map);
  renderMapMarkers();
}

function initFullMap() {
  if (state.fullMap) return;
  const mapEl = document.getElementById("map-full");
  if (!mapEl || typeof L === "undefined") return;
  state.fullMap = L.map("map-full", { center: [0, 0], zoom: 2 });
  L.tileLayer("https://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}", {
    maxZoom: 19,
  }).addTo(state.fullMap);
  renderFullMapMarkers();
}

const colorMap = {
  motion: "#ff2020",
  periodic: "#888",
  manual: "#4a90d9",
  startup: "#555",
};

function renderMapMarkers() {
  if (!state.map) return;
  state.markers.forEach((m) => state.map.removeLayer(m));
  state.markers = [];
  const gpsEvents = state.events.filter(
    (e) => e.latitude && e.longitude && Math.abs(e.latitude) > 0.001,
  );
  if (!gpsEvents.length) return;
  gpsEvents.forEach((event) => addMarkerToMap(event, state.map, state.markers));
  const bounds = gpsEvents.map((e) => [e.latitude, e.longitude]);
  if (bounds.length)
    state.map.fitBounds(bounds, { padding: [20, 20], maxZoom: 15 });
}

function renderFullMapMarkers() {
  if (!state.fullMap) return;
  state.fullMarkers.forEach((m) => state.fullMap.removeLayer(m));
  state.fullMarkers = [];
  const gpsEvents = state.events.filter(
    (e) => e.latitude && e.longitude && Math.abs(e.latitude) > 0.001,
  );
  gpsEvents.forEach((event) =>
    addMarkerToMap(event, state.fullMap, state.fullMarkers),
  );
  if (gpsEvents.length) {
    const bounds = gpsEvents.map((e) => [e.latitude, e.longitude]);
    state.fullMap.fitBounds(bounds, { padding: [20, 20], maxZoom: 15 });
  }
}

function addMarkerToMap(event, map, markersArr) {
  const color = colorMap[event.snapshot_type] || "#cc0000";
  const icon = L.divIcon({
    className: "",
    html: `<div style="width:10px;height:10px;border-radius:50%;background:${color};border:2px solid #fff;box-shadow:0 0 8px ${color}"></div>`,
    iconSize: [10, 10],
    iconAnchor: [5, 5],
  });
  const marker = L.marker([event.latitude, event.longitude], { icon }).addTo(
    map,
  ).bindPopup(`<div style="font-family:monospace;font-size:11px">
      <strong>${(event.triggered_by || "—").toUpperCase()} [${event.snapshot_type}]</strong><br>
      ${formatDateTime(event.created_at)}<br>
      ${event.latitude.toFixed(5)}, ${event.longitude.toFixed(5)}
      ${event.snapshot_url ? `<br><img src="${event.snapshot_url}" style="width:120px;margin-top:6px;border-radius:2px">` : ""}
    </div>`);
  markersArr.push(marker);
}

function addSingleMarker(event) {
  if (!event.latitude || !event.longitude || Math.abs(event.latitude) < 0.001)
    return;
  if (state.map) addMarkerToMap(event, state.map, state.markers);
  if (state.fullMap) addMarkerToMap(event, state.fullMap, state.fullMarkers);
}

// ============================================================
// LIGHTBOX
// ============================================================
function openLightbox(event) {
  const lb = el("lightbox");
  if (!lb) return;
  el("lb-img").src = event.snapshot_url || "";
  el("lb-img").style.display = event.snapshot_url ? "block" : "none";
  el("lb-sensor").textContent = event.triggered_by?.toUpperCase() || "—";
  el("lb-type").textContent = event.snapshot_type || "—";
  el("lb-datetime").textContent = formatDateTime(event.created_at);
  el("lb-lat").textContent = event.latitude?.toFixed(6) || "—";
  el("lb-lng").textContent = event.longitude?.toFixed(6) || "—";
  el("lb-dist").textContent = event.distance_cm
    ? `${event.distance_cm} cm`
    : "—";
  el("lb-sms").textContent = event.sms_sent ? "✓ Sent" : "—";
  el("lb-device").textContent = event.device_id || "—";
  const mapsUrl = event.latitude
    ? `https://maps.google.com/?q=${event.latitude},${event.longitude}`
    : null;
  const btn = el("lb-maps-btn");
  if (btn) {
    btn.style.display = mapsUrl ? "inline-flex" : "none";
    btn.href = mapsUrl || "#";
  }
  lb.classList.add("open");
}

function setupLightbox() {
  const lb = el("lightbox");
  if (!lb) return;
  el("lb-close")?.addEventListener("click", () => lb.classList.remove("open"));
  lb.addEventListener("click", (e) => {
    if (e.target === lb) lb.classList.remove("open");
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") lb.classList.remove("open");
  });
}

// ============================================================
// REALTIME — EventSource (SSE) from Vercel Node Server
// ============================================================
function setupRealtime() {
  const streamUrl = `${SERVER_URL}/stream`;
  const es = new EventSource(streamUrl);

  es.onopen = () => {
    console.log("[SSE] Connected to Vercel stream");
    updateRealtimeIndicator(true);
  };

  es.onerror = (err) => {
    console.warn("[SSE] Connection lost, retrying...");
    updateRealtimeIndicator(false);
  };

  // ── New Events ────────────────────────────────────────────────
  es.addEventListener("events", (e) => {
    const payload = JSON.parse(e.data);
    const event = payload.new;
    state.events.unshift(event);
    state.totalEvents++;

    if (event.snapshot_url) {
      updateCameraImage(event.snapshot_url, event.created_at, event);
    }

    if (
      event.snapshot_type === "manual" &&
      snapshotPending &&
      event.device_id === snapshotTarget
    ) {
      snapshotPending = false;
      unlockSnapshotButtons();
      el("live-loading")?.classList.remove("visible");
      showToast("success", "Snapshot Ready", "Image captured instantly", 3000);
    }

    if (event.snapshot_type === "motion") {
      state.unreadAlerts++;
      showMotionToast(event);
    }

    if (event.latitude && Math.abs(event.latitude) > 0.001) {
      updateGPSState(event);
      renderGPSBar();
    }

    prependEventToFeed(event);
    addSingleMarker(event);
    renderStats();
    updateTopbarPills();
    if (state.currentView === "gallery") renderGallery();
  });

  // ── Device Updates ────────────────────────────────────────────
  es.addEventListener("devices", (e) => {
    const payload = JSON.parse(e.data);
    const device = payload.new;
    const idx = state.devices.findIndex((d) => d.device_id === device.device_id);
    if (idx >= 0) {
      state.devices[idx] = { ...state.devices[idx], ...device };
    } else {
      state.devices.unshift(device);
    }
    debouncedRenderDeviceCards();
    debouncedRenderStats();
    debouncedUpdateTopbar();
    if (state.currentView === "devices") renderDevicesFull();
  });

  // ── New Logs ──────────────────────────────────────────────────
  es.addEventListener("logs", (e) => {
    const payload = JSON.parse(e.data);
    state.logs.unshift(payload.new);
    prependLogToFeed(payload.new);
  });

  // ── Heartbeats ────────────────────────────────────────────────
  es.addEventListener("heartbeats", (e) => {
    const payload = JSON.parse(e.data);
    const hb = payload.new;
    state.deviceStats[hb.device_id] = hb;

    const devIdx = state.devices.findIndex((d) => d.device_id === hb.device_id);
    const freshDevice = {
      device_id: hb.device_id,
      name: hb.device_id,
      device_type: hb.device_id?.toLowerCase().includes("cam") ? "esp32_cam" : "sensor",
      status: "online",
      last_seen: hb.created_at || new Date().toISOString(),
      ip_address: hb.ip_address || "—",
      firmware_version: hb.firmware_version || "—",
    };
    if (devIdx >= 0) {
      state.devices[devIdx] = { ...state.devices[devIdx], ...freshDevice };
    } else {
      state.devices.unshift(freshDevice);
    }

    if (hb.hub_gps_sats !== undefined) state.gpsState.satellites = hb.hub_gps_sats;
    if (hb.hub_gps_valid !== undefined) {
      state.gpsState.valid = !!hb.hub_gps_valid;
      if (!hb.hub_gps_valid) {
        state.gpsState.satellites = hb.hub_gps_sats ?? state.gpsState.satellites;
      }
    }
    renderGPSBar();

    debouncedRenderDeviceCards();
    debouncedRenderStats();
    debouncedUpdateTopbar();
    if (state.currentView === "devices") renderDevicesFull();
  });

  // Age out stale devices every 60s
  setInterval(() => {
    let changed = false;
    state.devices.forEach((d) => {
      if (computeLiveStatus(d) === "offline" && d.status !== "offline") {
        d.status = "offline";
        changed = true;
      }
    });
    if (changed) {
      renderDeviceCards();
      renderStats();
      updateTopbarPills();
      updateTopbarStatus();
      if (state.currentView === "devices") renderDevicesFull();
    }
  }, 60_000);
}

function updateRealtimeIndicator(connected) {
  const e = el("realtime-indicator");
  if (!e) return;
  e.className = `conn-indicator ${connected ? "" : "disconnected"}`;
  const dot = e.querySelector(".pill-dot") || e.querySelector(".status-dot");
  const span = e.querySelector("span:last-child");
  if (dot)
    dot.className = `${dot.className.replace(/ (green|red|grey)/g, "")} ${connected ? "green" : "red"}`;
  if (span) span.textContent = connected ? "Cloud" : "Offline";
}

// ============================================================
// TOASTS
// ============================================================
function showMotionToast(event) {
  const container = el("alert-banner");
  if (!container) return;
  const toast = document.createElement("div");
  toast.className = "alert-toast";
  const coords =
    event.latitude && Math.abs(event.latitude) > 0.001
      ? `${event.latitude.toFixed(4)}, ${event.longitude.toFixed(4)}`
      : "No GPS";
  toast.innerHTML = `
    <div class="toast-title">⚠ Motion Detected</div>
    <div class="toast-body">Sensor: ${event.triggered_by?.toUpperCase() || "—"}<br>${coords}</div>`;
  container.appendChild(toast);
  setTimeout(() => toast.remove(), 6000);
}

function showToast(type, title, body, duration = 4000) {
  const container = el("alert-banner");
  if (!container) return;
  const toast = document.createElement("div");
  toast.className = "alert-toast";
  const colors = {
    error: "var(--red-bright)",
    success: "var(--green)",
    info: "var(--blue)",
    warn: "var(--yellow)",
  };
  const color = colors[type] || "var(--white-dim)";
  toast.style.borderColor = color;
  toast.style.borderLeftColor = color;
  toast.innerHTML = `<div class="toast-title" style="color:${color}">${title}</div><div class="toast-body">${body}</div>`;
  container.appendChild(toast);
  setTimeout(() => toast.remove(), duration);
}

// ============================================================
// LOG EXPORT
// ============================================================
function setupLogExport() {
  el("btn-export-logs")?.addEventListener("click", () => {
    const lines = state.logs
      .map(
        (l) =>
          `${formatDateTime(l.created_at)}\t[${(l.level || "").toUpperCase()}]\t[${l.category || "sys"}]\t${l.message}`,
      )
      .join("\n");
    const blob = new Blob([lines], { type: "text/plain" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = `sentinel_logs_${new Date().toISOString().split("T")[0]}.txt`;
    a.click();
    URL.revokeObjectURL(a.href);
  });
}

// ============================================================
// CSV EXPORT (SNAPSHOTS)
// ============================================================
function setupCsvExport() {
  const downloadCsv = () => {
    // We only export events that have snapshot_url OR triggered_by
    const rows = [
      ["Timestamp", "Device ID", "Trigger", "Type", "Lat", "Lng", "Distance[cm]", "Satellites", "RSSI[dBm]", "Image URL"]
    ];
    state.events.forEach(e => {
      rows.push([
        new Date(e.created_at).toISOString(),
        e.device_id || "",
        (e.triggered_by || "unknown").toUpperCase(),
        e.snapshot_type || "motion",
        e.latitude || "",
        e.longitude || "",
        e.distance_cm || "",
        e.satellites || "",
        e.cam_rssi || "",
        e.snapshot_url || ""
      ]);
    });
    
    // Convert to CSV
    const csvContent = rows.map(r => r.map(cell => `"${String(cell).replace(/"/g, '""')}"`).join(",")).join("\n");
    const blob = new Blob([csvContent], { type: "text/csv;charset=utf-8;" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = `sentinel_snapshots_${new Date().toISOString().split("T")[0]}.csv`;
    a.click();
    URL.revokeObjectURL(a.href);
  };
  
  el("btn-export-csv")?.addEventListener("click", downloadCsv);
  el("btn-export-csv-gallery")?.addEventListener("click", downloadCsv);
}
// ============================================================
// THEME TOGGLE AND MOBILE NAV
// ============================================================
function setupThemeToggle() {
  const btn = el("theme-toggle");
  if (!btn) return;

  if (localStorage.getItem("sentinel-theme") === "light") {
    document.body.classList.add("light");
  }

  btn.addEventListener("click", () => {
    const isLight = document.body.classList.toggle("light");
    localStorage.setItem("sentinel-theme", isLight ? "light" : "dark");
  });
}

function setupMobileMenu() {
  const toggleBtn = el("mobile-menu-btn");
  const sidebar = document.querySelector(".sidebar");
  if (!toggleBtn || !sidebar) return;

  toggleBtn.addEventListener("click", () => {
    sidebar.classList.toggle("mobile-open");
  });

  // Close sidebar if a nav item is clicked on mobile
  document.querySelectorAll(".nav-item").forEach((item) => {
    item.addEventListener("click", () => {
      if (window.innerWidth <= 768) {
        sidebar.classList.remove("mobile-open");
      }
    });
  });
}

// ============================================================
// BOOT
// ============================================================
async function init() {
  setupThemeToggle();
  setupMobileMenu();
  startClock();
  setupNav();
  setupLightbox();
  setupGalleryFilters();
  setupLogExport();
  setupCsvExport();
  await loadInitialData();
  setupRealtime();
  initMap();
  setupSnapshotButton();

  // Server health check — once at boot and every 30s
  checkServerHealth();
  setInterval(checkServerHealth, 30_000);

  // Periodic device re-fetch — belt-and-suspenders in case
  // Supabase Realtime drops or RLS blocks change events.
  setInterval(async () => {
    try {
      const { data } = await db
        .from("devices")
        .select("*")
        .order("last_seen", { ascending: false });
      if (data) {
        state.devices = data;
        renderDeviceCards();
        renderStats();
        updateTopbarPills();
        updateTopbarStatus();
        if (state.currentView === "devices") renderDevicesFull();
      }
    } catch (e) {
      /* silent — UI stays on last known state */
    }
  }, 120_000);

  // Periodic event poll — catches new events when SSE drops
  // (motion, manual captures, etc.) so no manual refresh needed.
  setInterval(async () => {
    try {
      const newestKnown = state.events[0]?.created_at;
      if (!newestKnown) return;

      const { data } = await db
        .from("events")
        .select("id,device_id,triggered_by,snapshot_type,latitude,longitude,snapshot_url,distance_cm,sms_sent,created_at,satellites,gps_valid,cam_rssi,cam_heap_bytes")
        .gt("created_at", newestKnown)
        .order("created_at", { ascending: false })
        .limit(10);

      if (!data?.length) return;

      let addedAny = false;
      data.forEach((event) => {
        const already = state.events.find((e) => e.id === event.id);
        if (already) return;

        state.events.unshift(event);
        state.totalEvents++;
        prependEventToFeed(event);
        addSingleMarker(event);
        addedAny = true;

        if (event.snapshot_url) {
          updateCameraImage(event.snapshot_url, event.created_at, event);
        }
        if (event.latitude && Math.abs(event.latitude) > 0.001) {
          updateGPSState(event);
          renderGPSBar();
        }
        if (event.snapshot_type === "motion") {
          state.unreadAlerts++;
          showMotionToast(event);
        }
        // Unlock snapshot button if a manual/motion capture came in
        if (snapshotPending && event.device_id === snapshotTarget) {
          snapshotPending = false;
          unlockSnapshotButtons();
          el("live-loading")?.classList.remove("visible");
          showToast("success", "Snapshot Ready", "Image captured successfully", 3000);
        }
      });

      if (addedAny) {
        renderStats();
        updateTopbarPills();
        if (state.currentView === "gallery") renderGallery();
      }
    } catch (e) {
      /* silent */
    }
  }, 30_000);
}

document.addEventListener("DOMContentLoaded", () => {
  init().then(() => {
    window._appState = state;
  });
});
