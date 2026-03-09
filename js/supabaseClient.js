/**
 * SENTINEL — Supabase Client
 *
 * Reads credentials from window.SENTINEL_CONFIG which is set
 * by js/config.js (loaded before this module in index.html).
 *
 * Uses the CDN-loaded supabase-js (window.supabase) instead of
 * a bundler import — keeps this project zero-build-step.
 */

const { SUPABASE_URL, SUPABASE_ANON_KEY } = window.SENTINEL_CONFIG;

if (!SUPABASE_URL || SUPABASE_URL.includes("YOUR_")) {
  console.error("[SENTINEL] SUPABASE_URL not configured — edit js/config.js");
}

if (!SUPABASE_ANON_KEY || SUPABASE_ANON_KEY.includes("YOUR_")) {
  console.error(
    "[SENTINEL] SUPABASE_ANON_KEY not configured — edit js/config.js",
  );
}

export const db = window.supabase.createClient(
  SUPABASE_URL,
  SUPABASE_ANON_KEY,
  {
    auth: { persistSession: false },
    realtime: { params: { eventsPerSecond: 10 } },
  },
);
