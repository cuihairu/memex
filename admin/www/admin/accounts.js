// 管理后台页面模块（LAY-008 自 admin.js 逐字搬移）：accounts；注册表在 helpers.js。
"use strict";

Object.assign(pages, {
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
});
