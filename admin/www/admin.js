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

function role_pills(roles) {
  return (roles || []).map((r) =>
    `<span class="pill ${esc(r)}">${esc(r)}</span>`).join(" ") || '<span class="muted">member</span>';
}

// ============ 页面定义 ============

const pages = {
  accounts: {
    title: "账号与组织",
    desc: "建号（基础角色 admin|member）、部门树、成员归属与资料、追加角色授权（org-admin/auditor）、汇报线。",
    async render(el) {
      el.innerHTML = `
        <h2>账号与组织</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>账号</h3>
          <form id="f-acc" class="inline">
            <label>账号<input name="account" required></label>
            <label>口令<input name="password" type="password" required></label>
            <label>显示名<input name="name"></label>
            <label>基础角色<select name="role"><option>member</option><option>admin</option></select></label>
            <button type="submit">建号</button>
          </form>
          <table id="t-acc"><thead><tr>
            <th>账号</th><th>显示名</th><th>角色</th><th>授权操作</th>
          </tr></thead><tbody></tbody></table>
        </div>
        <div class="card"><h3>部门</h3>
          <form id="f-dept" class="inline">
            <label>全路径（公司/研发部/组）<input name="path" size="30" required></label>
            <button type="submit">建部门</button>
          </form>
          <table id="t-dept"><thead><tr><th>ID</th><th>路径</th></tr></thead><tbody></tbody></table>
        </div>
        <div class="card"><h3>成员归属与资料</h3>
          <form id="f-mem" class="inline">
            <label>账号<input name="account" required></label>
            <label>操作<select name="op"><option value="add">加入</option><option value="remove">移出</option></select></label>
            <label>部门<select name="department_id"></select></label>
            <button type="submit">执行</button>
          </form>
          <form id="f-prof" class="inline" style="margin-top:8px">
            <label>账号<input name="account" required></label>
            <label>部门<select name="department_id"></select></label>
            <label>职务<input name="title"></label>
            <label>直属上级账号<input name="manager"></label>
            <button type="submit">建档/更新</button>
          </form>
        </div>
        <div class="card"><h3>角色授权（追加角色，可带时间窗）</h3>
          <form id="f-role" class="inline">
            <label>账号<input name="account" required></label>
            <label>角色<select name="role"><option>org-admin</option><option>auditor</option></select></label>
            <label>作用域（空=全局）<input name="scope"></label>
            <label>起 ms（0=即刻）<input name="valid_from_ms" value="0"></label>
            <label>止 ms（0=无限）<input name="valid_until_ms" value="0"></label>
            <button type="submit">授予</button>
          </form>
          <table id="t-role"><thead><tr>
            <th>ID</th><th>账号</th><th>角色</th><th>作用域</th><th>窗口</th><th>授权人</th><th></th>
          </tr></thead><tbody></tbody></table>
        </div>`;

      const depts = (await api("/api/admin/departments")).departments;
      for (const sel of el.querySelectorAll('select[name="department_id"]')) {
        sel.innerHTML = depts.map((d) =>
          `<option value="${d.id}">${esc(d.path)}</option>`).join("");
      }

      const accs = (await api("/api/admin/accounts")).accounts;
      el.querySelector("#t-acc tbody").innerHTML = accs.map((a) => `
        <tr><td class="mono">${esc(a.account)}</td><td>${esc(a.label)}</td>
        <td>${role_pills(a.roles)}</td>
        <td class="row-actions">
          <button data-grant="${esc(a.account)}" class="ghost">授权</button>
        </td></tr>`).join("");

      el.querySelector("#t-role tbody").innerHTML =
        (await api("/api/admin/roles")).assignments.map((r) => `
        <tr><td>${r.id}</td><td class="mono">${esc(r.account)}</td>
        <td><span class="pill ${esc(r.role)}">${esc(r.role)}</span></td>
        <td>${esc(r.scope) || '<span class="muted">全局</span>'}</td>
        <td class="muted">${fmt_ms(r.valid_from_ms)} ~ ${r.valid_until_ms ? fmt_ms(r.valid_until_ms) : "∞"}</td>
        <td>${esc(r.granted_by)}</td>
        <td class="row-actions"><button data-revoke="${r.id}" class="ghost">吊销</button></td></tr>`).join("");

      el.querySelector("#f-acc").onsubmit = async (e) => {
        e.preventDefault();
        const f = new FormData(e.target);
        try {
          await api("/api/admin/accounts", { body: Object.fromEntries(f) });
          toast("已建号"); this.render(el);
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-dept").onsubmit = async (e) => {
        e.preventDefault();
        const f = new FormData(e.target);
        try {
          await api("/api/admin/departments", { body: Object.fromEntries(f) });
          toast("已建部门"); this.render(el);
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-mem").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          await api("/api/admin/membership", { body: f });
          toast("已执行");
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-prof").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        f.department_id = Number(f.department_id);
        try {
          await api("/api/admin/profile", { body: f });
          toast("已建档");
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-role").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        f.valid_from_ms = Number(f.valid_from_ms);
        f.valid_until_ms = Number(f.valid_until_ms);
        try {
          await api("/api/admin/roles", { body: f });
          toast("已授予"); this.render(el);
        } catch (err) { toast(err.message, true); }
      };
      el.querySelectorAll("[data-grant]").forEach((b) => {
        b.onclick = () => {
          const form = el.querySelector("#f-role");
          form.querySelector('input[name="account"]').value = b.dataset.grant;
          form.scrollIntoView({ behavior: "smooth" });
        };
      });
      el.querySelectorAll("[data-revoke]").forEach((b) => {
        b.onclick = async () => {
          try {
            await api("/api/admin/roles/revoke", { body: { id: Number(b.dataset.revoke) } });
            toast("已吊销"); this.render(el);
          } catch (err) { toast(err.message, true); }
        };
      });
    },
  },

  policy: {
    title: "策略下发",
    desc: "免登录 / 跨态 / 新设备审批 / 跨部门文件 / 转发 五开关，按部门行配置（空路径=全局行）。",
    async render(el) {
      el.innerHTML = `
        <h2>策略下发</h2><p class="desc">${this.desc}</p>
        <div class="card">
          <form id="f-pol" class="inline">
            <label>部门路径（空=全局）<input name="department_path" size="26"></label>
            <label>免登录<select name="allow_anonymous"><option value="true">允</option><option value="false">禁</option></select></label>
            <label>跨态<select name="allow_cross_state"><option value="true">允</option><option value="false">禁</option></select></label>
            <label>新设备审批<select name="new_device_approval"><option value="false">免审批</option><option value="true">须审批</option></select></label>
            <label>跨部门文件<select name="allow_cross_dept_file"><option value="false">禁</option><option value="true">允</option></select></label>
            <label>转发<select name="allow_forward_file"><option value="true">允</option><option value="false">禁</option></select></label>
            <button type="submit">下发</button>
          </form>
          <table style="margin-top:12px"><thead><tr>
            <th>范围</th><th>免登录</th><th>跨态</th><th>新设备审批</th><th>跨部门文件</th><th>转发</th>
          </tr></thead><tbody></tbody></table>
        </div>`;
      const fill = (rows) => {
        el.querySelector("tbody").innerHTML = rows.map((p) => `
          <tr><td>${esc(p.department_path) || '<span class="pill">全局</span>'}</td>
          <td>${p.allow_anonymous ? "允" : '<span class="pill off">禁</span>'}</td>
          <td>${p.allow_cross_state ? "允" : '<span class="pill off">禁</span>'}</td>
          <td>${p.new_device_approval ? '<span class="pill off">须审批</span>' : "免"}</td>
          <td>${p.allow_cross_dept_file ? "允" : '<span class="pill off">禁</span>'}</td>
          <td>${p.allow_forward_file ? "允" : '<span class="pill off">禁</span>'}</td></tr>`).join("");
      };
      fill((await api("/api/admin/policy")).rows);
      el.querySelector("#f-pol").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        for (const k of ["allow_anonymous", "allow_cross_state", "new_device_approval",
                         "allow_cross_dept_file", "allow_forward_file"]) f[k] = f[k] === "true";
        try {
          await api("/api/admin/policy", { body: f });
          toast("策略已下发"); this.render(el);
        } catch (err) { toast(err.message, true); }
      };
    },
  },

  retention: {
    title: "留存策略",
    desc: "按部门配置留存期（0=不限）；清理（purge）走双人审批红线：审批人≠执行人、理由必填。",
    async render(el) {
      el.innerHTML = `
        <h2>留存策略</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>留存期配置</h3>
          <form id="f-ret" class="inline">
            <label>部门路径（空=全局）<input name="department_path" size="26"></label>
            <label>留存天数<select name="days">
              <option value="0">0（不限）</option><option value="30">30</option>
              <option value="180">180</option><option value="365">365</option>
              <option value="1095">1095</option></select></label>
            <button type="submit">保存</button>
          </form>
          <table style="margin-top:12px"><thead><tr>
            <th>范围</th><th>留存天数</th><th>更新人</th><th>更新时间</th>
          </tr></thead><tbody></tbody></table>
        </div>
        <div class="card"><h3>到期清理（Retention Purge）</h3>
          <form id="f-purge" class="inline">
            <label>清理 before（ms）<input name="before_ms" required></label>
            <label>理由<input name="reason" size="26" required></label>
            <label>审批人账号<input name="approved_by" required></label>
            <label>policy_days<input name="policy_days" value="0"></label>
            <button type="submit">执行清理</button>
          </form>
          <table style="margin-top:12px"><thead><tr>
            <th>ID</th><th>执行人</th><th>审批人</th><th>理由</th><th>条数</th><th>时间</th>
          </tr></thead><tbody></tbody></table>
        </div>`;
      const data = await api("/api/admin/retention");
      const [t1, t2] = el.querySelectorAll("tbody");
      t1.innerHTML = data.rows.map((p) => `
        <tr><td>${esc(p.department_path) || '<span class="pill">全局</span>'}</td>
        <td>${p.retention_days || "不限"}</td><td>${esc(p.updated_by)}</td>
        <td class="muted">${fmt_ms(p.updated_ms)}</td></tr>`).join("");
      t2.innerHTML = data.purges.map((g) => `
        <tr><td>${g.id}</td><td>${esc(g.purged_by)}</td><td>${esc(g.approved_by)}</td>
        <td>${esc(g.reason)}</td><td>${g.msg_count}</td>
        <td class="muted">${fmt_ms(g.purged_ms)}</td></tr>`).join("");
      el.querySelector("#f-ret").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        f.days = Number(f.days);
        try {
          await api("/api/admin/retention", { body: f });
          toast("留存策略已保存"); this.render(el);
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-purge").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        f.before_ms = Number(f.before_ms);
        f.policy_days = Number(f.policy_days);
        try {
          const r = await api("/api/admin/retention/purge", { body: f });
          toast(`清理已执行（台账 ${r.id}）`); this.render(el);
        } catch (err) { toast(err.message, true); }
      };
    },
  },

  messages: {
    title: "归档检索与导出",
    desc: "持 auditor 有效角色方可查（org-admin 不自动可读消息）；每次检索/导出落查阅日志。",
    async render(el) {
      el.innerHTML = `
        <h2>归档检索与导出</h2><p class="desc">${this.desc}</p>
        <div class="card">
          <form id="f-msg" class="inline">
            <label>账号<input name="account"></label>
            <label>关键词<input name="keyword"></label>
            <label>对端/群<input name="peer"></label>
            <label>since ms<input name="since_ms" value="0"></label>
            <label>until ms<input name="until_ms" value="0"></label>
            <label>条数<input name="limit" value="200"></label>
            <button type="submit">检索</button>
            <button type="button" id="btn-exp" class="ghost">导出 JSONL</button>
          </form>
          <table style="margin-top:12px"><thead><tr>
            <th>时间</th><th>发</th><th>收</th><th>撤回</th><th>内容</th>
          </tr></thead><tbody></tbody></table>
        </div>`;
      const form_body = () => {
        const f = Object.fromEntries(new FormData(el.querySelector("#f-msg")));
        f.since_ms = Number(f.since_ms || 0);
        f.until_ms = Number(f.until_ms || 0);
        f.limit = Number(f.limit || 200);
        return f;
      };
      el.querySelector("#f-msg").onsubmit = async (e) => {
        e.preventDefault();
        try {
          const r = await api("/api/admin/messages/search", { body: form_body() });
          el.querySelector("tbody").innerHTML = r.messages.map((m) => `
            <tr><td class="muted">${fmt_ms(m.ts_ms)}</td>
            <td class="mono">${esc(m.from)}</td><td class="mono">${esc(m.to)}</td>
            <td>${m.recalled ? '<span class="pill off">已撤回</span>' : ""}</td>
            <td>${esc(m.text)}</td></tr>`).join("") ||
            '<tr><td colspan="5" class="muted">无命中</td></tr>';
          toast(`命中 ${r.count} 条（已落查阅日志）`);
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#btn-exp").onclick = async () => {
        try {
          const resp = await fetch("/api/admin/messages/export", {
            method: "POST",
            headers: { "X-Memex-Admin": "1", "Content-Type": "application/json" },
            credentials: "same-origin",
            body: JSON.stringify(form_body()),
          });
          if (!resp.ok) {
            const j = await resp.json().catch(() => ({}));
            throw new Error(j.error || `HTTP ${resp.status}`);
          }
          const blob = await resp.blob();
          const a = document.createElement("a");
          a.href = URL.createObjectURL(blob);
          a.download = "memex-export.jsonl";
          a.click();
          toast("已导出（已落查阅日志）");
        } catch (err) { toast(err.message, true); }
      };
    },
  },

  auditreads: {
    title: "查阅日志",
    desc: "谁、何时、用什么条件、命中几条——检索与导出全部留痕，被拒尝试（audit.denied）同表可对账。",
    async render(el) {
      el.innerHTML = `
        <h2>查阅日志</h2><p class="desc">${this.desc}</p>
        <div class="card"><table><thead><tr>
          <th>时间</th><th>操作者</th><th>动作</th><th>条件摘要</th><th>命中</th>
        </tr></thead><tbody>
        ${(await api("/api/admin/audit-reads")).reads.map((r) => `
          <tr><td class="muted">${fmt_ms(r.ts_ms)}</td>
          <td class="mono">${esc(r.op_account)}</td>
          <td>${r.action === "audit.denied" ? `<span class="pill off">${esc(r.action)}</span>` : esc(r.action)}</td>
          <td class="mono">${esc(r.filters)}</td><td>${r.result_count}</td></tr>`).join("")}
        </tbody></table></div>`;
    },
  },

  devices: {
    title: "设备与登录台账",
    desc: "设备首登自动建档；停用即拒登录；解绑清责任人；登录成功失败都记。",
    async render(el) {
      el.innerHTML = `
        <h2>设备与登录台账</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>设备台账</h3>
          <table><thead><tr>
            <th>指纹</th><th>类型</th><th>名称</th><th>责任人</th><th>状态</th><th>最近活跃</th><th>操作</th>
          </tr></thead><tbody>
          ${(await api("/api/admin/devices")).devices.map((d) => `
            <tr><td class="mono">${esc(d.fingerprint.slice(0, 12))}…</td>
            <td>${esc(d.kind)}</td><td>${esc(d.name)}</td>
            <td>${esc(d.owner_account) || '<span class="muted">未登记</span>'}</td>
            <td>${d.enabled ? '<span class="pill">启用</span>' : '<span class="pill off">停用</span>'}</td>
            <td class="muted">${fmt_ms(d.last_seen_ms)}</td>
            <td class="row-actions">
              <button data-dev="${esc(d.fingerprint)}" data-on="${d.enabled ? "0" : "1"}" class="ghost">${d.enabled ? "停用" : "启用"}</button>
            </td></tr>`).join("")}
          </tbody></table>
          <form id="f-owner" class="inline" style="margin-top:10px">
            <label>指纹（完整）<input name="fingerprint" size="40" required></label>
            <label>责任人账号<input name="owner"></label>
            <button type="submit">登记责任人</button>
          </form>
        </div>
        <div class="card"><h3>登录台账</h3>
          <table><thead><tr>
            <th>时间</th><th>账号</th><th>结果</th><th>类型</th><th>设备名</th><th>指纹</th><th>来源</th>
          </tr></thead><tbody>
          ${(await api("/api/admin/logins")).logins.map((r) => `
            <tr><td class="muted">${fmt_ms(r.ts_ms)}</td>
            <td class="mono">${esc(r.account)}</td>
            <td>${r.result === "ok" ? '<span class="pill">ok</span>' : `<span class="pill off">${esc(r.result)}</span>`}</td>
            <td>${esc(r.kind)}</td><td>${esc(r.name)}</td>
            <td class="mono">${esc(r.fingerprint)}</td><td>${esc(r.source_ip)}</td></tr>`).join("")}
          </tbody></table></div>`;
      el.querySelectorAll("[data-dev]").forEach((b) => {
        b.onclick = async () => {
          try {
            await api("/api/admin/devices/enabled", {
              body: { fingerprint: b.dataset.dev, enabled: b.dataset.on === "1" },
            });
            toast("已更新"); this.render(el);
          } catch (err) { toast(err.message, true); }
        };
      });
      el.querySelector("#f-owner").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          await api("/api/admin/devices/owner", { body: f });
          toast("已登记"); this.render(el);
        } catch (err) { toast(err.message, true); }
      };
    },
  },

  health: {
    title: "健康状态",
    desc: "admin 进程、本地库、文件面（配置 MEMEX_FILES_URL 才探）。",
    async render(el) {
      const h = await api("/api/admin/health");
      el.innerHTML = `
        <h2>健康状态</h2><p class="desc">${this.desc}</p>
        <div class="card"><table><tbody>
          <tr><th>admin 进程</th><td><span class="pill">ok</span></td></tr>
          <tr><th>本地库</th><td>${h.db ? '<span class="pill">open</span>' : '<span class="pill off">closed</span>'}</td></tr>
          <tr><th>文件面</th><td>${esc(h.files)}</td></tr>
          <tr><th>运行时长</th><td>${Math.round(h.uptime_ms / 1000)}s</td></tr>
        </tbody></table></div>`;
    },
  },
};

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
