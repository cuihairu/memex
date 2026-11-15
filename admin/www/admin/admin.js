// 管理后台前端入口（LAY-008 自原 admin.js 保留）：路由与登录态；
// 公共面（api/esc/格式化）在 helpers.js，页面注册在各页模块。
"use strict";


// ============ 路由与登录态 ============

async function refresh_whoami() {
  try {
    const j = await api("/api/whoami");
    $("#whoami").textContent = `${j.account}（${(j.roles || []).join(", ") || "member"}）`;
    return j;
  } catch (e) { return null; }
}

function show_login() {
  $("#view-login").classList.remove("hidden");
  $("#view-main").classList.add("hidden");
}

async function show_main() {
  const me = await refresh_whoami();
  if (!me) return show_login();
  $("#view-login").classList.add("hidden");
  $("#view-main").classList.remove("hidden");
  const page = location.hash.replace("#", "") || "accounts";
  const def = pages[page];
  document.querySelectorAll(".nav a").forEach((a) =>
    a.classList.toggle("active", a.dataset.page === page));
  const content = $("#content");
  if (def) {
    try { await def.render(content); }
    catch (err) {
      if (String(err.message).includes("401")) return show_login();
      content.innerHTML = `<h2>${esc(def.title)}</h2>
        <div class="card error">${esc(err.message)}</div>`;
    }
  } else {
    content.innerHTML = "<h2>页面不存在</h2>";
  }
}

$("#login-form").onsubmit = async (e) => {
  e.preventDefault();
  const err = $("#login-error");
  err.classList.add("hidden");
  try {
    await api("/api/login", {
      body: {
        account: $("#login-account").value,
        password: $("#login-password").value,
      },
    });
    location.hash = "#accounts";
    await show_main();
  } catch (ex) {
    err.textContent = ex.message;
    err.classList.remove("hidden");
  }
};

$("#btn-logout").onclick = async () => {
  try { await api("/api/logout", { body: {} }); } catch (e) { /* 会话已过期即可 */ }
  show_login();
};

window.addEventListener("hashchange", show_main);
show_main();
