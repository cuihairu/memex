// memex 管理后台前端（波次 1）：零构建 vanilla JS，hash 路由单页。
// 所有 API 走同源 /api/*；改写类请求统一带 X-Memex-Admin 头（CSRF 轻闸）。
"use strict";

const $ = (sel) => document.querySelector(sel);

function toast(msg, is_error) {
  const t = $("#toast");
  t.textContent = msg;
  t.classList.toggle("error", !!is_error);
  t.classList.remove("hidden");
  clearTimeout(t._timer);
  t._timer = setTimeout(() => t.classList.add("hidden"), 3500);
}

async function api(path, opts = {}) {
  const init = { headers: { "X-Memex-Admin": "1" }, credentials: "same-origin", ...opts };
  if (opts.body !== undefined) {
    init.method = init.method || "POST";
    init.headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(opts.body);
  }
  const resp = await fetch(path, init);
  let j = {};
  try { j = await resp.json(); } catch (e) { /* 导出等非 JSON 应答 */ }
  if (!resp.ok) throw new Error(j.error || `HTTP ${resp.status}`);
  return j;
}

function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[c]));
}

function fmt_ms(ms) {
  if (!ms) return "—";
  const d = new Date(Number(ms));
  return d.toLocaleString("sv-SE"); // yyyy-MM-dd HH:mm:ss
}

function fmt_bytes(n) {
  n = Number(n) || 0;
  const u = ["B", "KB", "MB", "GB", "TB"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return (i ? n.toFixed(1) : n) + " " + u[i];
}

function role_pills(roles) {
  return (roles || []).map((r) =>
    `<span class="pill ${esc(r)}">${esc(r)}</span>`).join(" ") || '<span class="muted">member</span>';
}

// ============ 页面定义 ============
const pages = {};
