// 管理后台页面模块（LAY-008 自 admin.js 逐字搬移）：policy, retention；注册表在 helpers.js。
"use strict";

Object.assign(pages, {
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
});
