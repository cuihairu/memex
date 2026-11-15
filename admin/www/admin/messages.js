// 管理后台页面模块（LAY-008 自 admin.js 逐字搬移）：messages, auditreads, devices；注册表在 helpers.js。
"use strict";

Object.assign(pages, {
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
});
