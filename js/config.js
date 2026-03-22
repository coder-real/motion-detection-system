window.SENTINEL_CONFIG = {
  // ── Supabase ──────────────────────────────────────────────
  // Dashboard → Project Settings → API → Project URL
  SUPABASE_URL: "https://uzqehjjiygcdxhdpthxd.supabase.co",

  // Dashboard → Project Settings → API → anon / public key
  SUPABASE_ANON_KEY:
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InV6cWVoamppeWdjZHhoZHB0aHhkIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzE4MTczNDUsImV4cCI6MjA4NzM5MzM0NX0.vrzu_kon3fj7KYKf7-LY8fhLXrLTC4BeqKfXv4GjOuk",

  // ── Render backend server ─────────────────────────────────
  // Your Render service URL (no trailing slash).
  // After deploying to Render, replace this with your actual URL.
  // Format: "https://your-service-name.onrender.com"
  SENTINEL_SERVER_URL: "http://127.0.0.1:3000",

  // ── Device ID ─────────────────────────────────────────────
  CAM_DEVICES: [
    { id: "ESP32-CAM-01", label: "Camera 1" },
    { id: "ESP32-CAM-02", label: "Camera 2" }
  ],
};
