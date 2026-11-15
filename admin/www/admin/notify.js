// 管理后台页面模块（LAY-008 自 admin.js 逐字搬移）：notifications；注册表在 helpers.js。
"use strict";

Object.assign(pages, {
  notifications: {
    title: "通知与集成",
    desc: "webhook 接入与 bot 机器人（token 仅创建一次显示，台账只存摘要）；模型网关端点（api_key 只写不回显）；三级通知偏好只读盘点。",
    async render(el) {
      el.innerHTML = `
        <h2>通知与集成</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>三级通知偏好（只读）</h3>
          <p class="muted" id="ov-note" style="font-size:12px"></p>
          <div class="stat-row" id="ov-global"></div>
          <h4>群级通知接入（target=group:群号 的 webhook 与 bot 白名单）</h4>
          <table><thead><tr>
            <th>ID</th><th>目标</th><th>备注</th><th>token 前缀</th><th>创建</th><th>状态</th>
          </tr></thead><tbody id="t-ovgroup"></tbody></table>
          <table style="margin-top:8px"><thead><tr>
            <th>bot</th><th>状态</th><th>群白名单</th><th>部门白名单</th>
          </tr></thead><tbody id="t-ovbot"></tbody></table>
          <h4>个人级通知接入（target=账号 的 webhook）</h4>
          <table><thead><tr>
            <th>ID</th><th>目标</th><th>备注</th><th>token 前缀</th><th>创建</th><th>状态</th>
          </tr></thead><tbody id="t-ovperson"></tbody></table>
        </div>
        <div class="card"><h3>Webhook 接入</h3>
          <form id="f-hook" class="inline">
            <label>目标（账号 或 group:群号）<input name="target" required></label>
            <label>备注<input name="name"></label>
            <button type="submit">创建</button>
          </form>
          <div id="hook-token" class="token-once hidden"></div>
          <table style="margin-top:10px"><thead><tr>
            <th>ID</th><th>目标</th><th>备注</th><th>token 摘要前缀</th><th>创建时间</th><th>状态</th><th></th>
          </tr></thead><tbody id="t-hook"></tbody></table>
        </div>
        <div class="card"><h3>Bot 机器人</h3>
          <form id="f-bot" class="inline">
            <label>名称（不含 bot: 前缀与冒号）<input name="name" required></label>
            <button type="submit">创建</button>
          </form>
          <div id="bot-token" class="token-once hidden"></div>
          <table style="margin-top:10px"><thead><tr>
            <th>名称</th><th>全名</th><th>创建人</th><th>token 摘要前缀</th><th>创建时间</th><th>白名单（空=不设限）</th><th>状态</th><th></th>
          </tr></thead><tbody id="t-bot"></tbody></table>
        </div>
        <div class="card"><h3>模型网关端点</h3>
          <form id="f-ep" class="inline">
            <label>路由名（不含冒号）<input name="name" required></label>
            <label>base_url（http:// 内网）<input name="base_url" required></label>
            <label>api_key（留空=不鉴权；只写不回显）<input name="api_key" type="password"></label>
            <label>上游模型名（空=透传）<input name="model"></label>
            <label style="flex-direction:row;align-items:center"><input type="checkbox" name="is_local" style="min-width:0"> 本地端点（可承接归档数据）</label>
            <button type="submit">登记</button>
          </form>
          <table style="margin-top:10px"><thead><tr>
            <th>序</th><th>名称</th><th>base_url</th><th>模型</th><th>api_key</th><th>本地</th><th>状态</th><th>登记人</th><th></th>
          </tr></thead><tbody id="t-ep"></tbody></table>
          <h4>调用审计（最近 20 条，元数据不含正文）</h4>
          <table><thead><tr>
            <th>时间</th><th>调用者</th><th>端点</th><th>模型</th><th>归档</th><th>状态</th><th>耗时 ms</th>
          </tr></thead><tbody id="t-calls"></tbody></table>
        </div>`;

      // token 一次性显示（创建应答唯一一次；表格刷新不清掉本框）
      const show_token = (sel, token) => {
        const box = el.querySelector(sel);
        box.classList.remove("hidden");
        box.innerHTML =
          `<strong>token（仅此一次显示，请立即保存）：</strong><code class="mono">${esc(token)}</code>`;
      };

      const hook_row = (w) => `
        <tr><td>${w.id}</td>
        <td class="mono">${esc(w.target)}${w.group_name ? `（${esc(w.group_name)}）` : ""}</td>
        <td>${esc(w.name) || '<span class="muted">—</span>'}</td>
        <td class="mono">${esc(w.token_prefix)}…</td>
        <td class="muted">${fmt_ms(w.created_ms)}</td>
        <td>${w.revoked ? '<span class="pill off">已吊销</span>' : '<span class="pill">有效</span>'}</td></tr>`;

      // 全量刷新（盘点＋三张表＋调用审计）。创建后走这里而非整页重渲：
      // 整页重渲会清掉 token 一次性显示框。
      const refresh = async () => {
        const [ov, hooks, bots, eps, calls] = await Promise.all([
          api("/api/admin/notify-overview"),
          api("/api/admin/webhooks"),
          api("/api/admin/bots"),
          api("/api/admin/model/endpoints"),
          api("/api/admin/model/calls?limit=20"),
        ]);
        $("#ov-note").textContent = ov.note;
        $("#ov-global").innerHTML = [
          `webhook ${ov.global.webhooks_total}（有效 ${ov.global.webhooks_active}）`,
          `bot ${ov.global.bots_total}（启用 ${ov.global.bots_enabled}）`,
          `模型端点 ${ov.global.endpoints_total}（启用 ${ov.global.endpoints_enabled}／本地 ${ov.global.endpoints_local}）`,
        ].map((s) => `<span class="pill">${esc(s)}</span>`).join("");
        el.querySelector("#t-ovgroup").innerHTML =
          ov.group_scope.map(hook_row).join("") ||
          '<tr><td colspan="6" class="muted">无群级 webhook</td></tr>';
        el.querySelector("#t-ovbot").innerHTML = ov.bots.map((b) => `
          <tr><td class="mono">bot:${esc(b.name)}</td>
          <td>${b.disabled ? '<span class="pill off">禁用</span>' : '<span class="pill">启用</span>'}</td>
          <td>${b.groups_allowed.length ? esc(b.groups_allowed.join(",")) : '<span class="muted">不设限</span>'}</td>
          <td>${b.depts_allowed.length ? esc(b.depts_allowed.join(",")) : '<span class="muted">不设限</span>'}</td></tr>`).join("") ||
          '<tr><td colspan="4" class="muted">无 bot</td></tr>';
        el.querySelector("#t-ovperson").innerHTML =
          ov.personal_scope.map(hook_row).join("") ||
          '<tr><td colspan="6" class="muted">无个人级 webhook</td></tr>';

        el.querySelector("#t-hook").innerHTML =
          hooks.webhooks.map((w) => `
          <tr><td>${w.id}</td><td class="mono">${esc(w.target)}</td>
          <td>${esc(w.name) || '<span class="muted">—</span>'}</td>
          <td class="mono">${esc(w.token_prefix)}…</td>
          <td class="muted">${fmt_ms(w.created_ms)}</td>
          <td>${w.revoked ? '<span class="pill off">已吊销</span>' : '<span class="pill">有效</span>'}</td>
          <td class="row-actions">${w.revoked ? "" : `<button data-revoke-hook="${w.id}" class="ghost">吊销</button>`}</td></tr>`).join("") ||
          '<tr><td colspan="7" class="muted">暂无 webhook</td></tr>';

        el.querySelector("#t-bot").innerHTML =
          bots.bots.map((b) => `
          <tr><td class="mono">${esc(b.name)}</td><td class="mono">${esc(b.full_name)}</td>
          <td>${esc(b.created_by)}</td>
          <td class="mono">${esc(b.token_prefix)}…</td>
          <td class="muted">${fmt_ms(b.created_ms)}</td>
          <td>${b.groups_allowed.length || b.depts_allowed.length
            ? `群{${esc(b.groups_allowed.join(","))}} 部门{${esc(b.depts_allowed.join(","))}}`
            : '<span class="muted">不设限</span>'}</td>
          <td>${b.disabled ? '<span class="pill off">禁用</span>' : '<span class="pill">启用</span>'}</td>
          <td class="row-actions">
            <button data-bot-toggle="${esc(b.name)}" data-to="${b.disabled ? "1" : "0"}" class="ghost">${b.disabled ? "启用" : "禁用"}</button>
            <button data-bot-remove="${esc(b.name)}" class="ghost">删除</button>
          </td></tr>`).join("") ||
          '<tr><td colspan="8" class="muted">暂无 bot（白名单维护走 CLI bot allow/deny）</td></tr>';

        el.querySelector("#t-ep").innerHTML =
          eps.endpoints.map((p, i) => `
          <tr><td>${i + 1}</td><td class="mono">${esc(p.name)}</td>
          <td class="mono">${esc(p.base_url)}</td>
          <td>${esc(p.model) || '<span class="muted">透传</span>'}</td>
          <td>${p.api_key_set ? '<span class="pill">已配置</span>' : '<span class="muted">未配置</span>'}</td>
          <td>${p.is_local ? '<span class="pill auditor">本地</span>' : "外部"}</td>
          <td>${p.enabled ? '<span class="pill">启用</span>' : '<span class="pill off">停用</span>'}</td>
          <td>${esc(p.created_by)}</td>
          <td class="row-actions">
            <button data-ep-toggle="${esc(p.name)}" data-to="${p.enabled ? "0" : "1"}" class="ghost">${p.enabled ? "停用" : "启用"}</button>
            <button data-ep-remove="${esc(p.name)}" class="ghost">移除</button>
          </td></tr>`).join("") ||
          '<tr><td colspan="9" class="muted">暂无端点（注册序即路由优先序）</td></tr>';

        el.querySelector("#t-calls").innerHTML = calls.calls.map((c) => `
          <tr><td class="muted">${fmt_ms(c.created_ms)}</td>
          <td class="mono">${esc(c.caller)}</td><td class="mono">${esc(c.endpoint)}</td>
          <td>${esc(c.model)}</td>
          <td>${c.archive_scope ? '<span class="pill auditor">归档</span>' : '<span class="muted">—</span>'}</td>
          <td>${c.status === 200 ? '<span class="pill">200</span>' : `<span class="pill off">${c.status}</span>`}</td>
          <td>${c.latency_ms}</td></tr>`).join("") ||
          '<tr><td colspan="7" class="muted">暂无调用</td></tr>';

        // 行内操作（每次刷新后重绑）
        el.querySelectorAll("[data-revoke-hook]").forEach((b) => {
          b.onclick = async () => {
            try {
              await api("/api/admin/webhooks/revoke", { body: { id: Number(b.dataset.revokeHook) } });
              toast("已吊销"); this.render(el);
            } catch (err) { toast(err.message, true); }
          };
        });
        el.querySelectorAll("[data-bot-toggle]").forEach((b) => {
          b.onclick = async () => {
            try {
              await api("/api/admin/bots/disabled", {
                body: { name: b.dataset.botToggle, disabled: b.dataset.to === "0" },
              });
              toast("已更新"); this.render(el);
            } catch (err) { toast(err.message, true); }
          };
        });
        el.querySelectorAll("[data-bot-remove]").forEach((b) => {
          b.onclick = async () => {
            if (!confirm(`删除 bot ${b.dataset.botRemove}？（连带退出全部群与白名单）`)) return;
            try {
              await api("/api/admin/bots/remove", { body: { name: b.dataset.botRemove } });
              toast("已删除"); this.render(el);
            } catch (err) { toast(err.message, true); }
          };
        });
        el.querySelectorAll("[data-ep-toggle]").forEach((b) => {
          b.onclick = async () => {
            try {
              await api("/api/admin/model/endpoints/enabled", {
                body: { name: b.dataset.epToggle, enabled: b.dataset.to === "1" },
              });
              toast("已更新"); this.render(el);
            } catch (err) { toast(err.message, true); }
          };
        });
        el.querySelectorAll("[data-ep-remove]").forEach((b) => {
          b.onclick = async () => {
            if (!confirm(`移除端点 ${b.dataset.epRemove}？`)) return;
            try {
              await api("/api/admin/model/endpoints/remove", { body: { name: b.dataset.epRemove } });
              toast("已移除"); this.render(el);
            } catch (err) { toast(err.message, true); }
          };
        });
      };

      // —— 表单（创建＝token 一次性显示＋表格刷新；其余动作整页重渲） ——
      el.querySelector("#f-hook").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          const r = await api("/api/admin/webhooks", { body: f });
          show_token("#hook-token", r.token);
          toast(`已创建 webhook（id ${r.id}）`);
          await refresh();
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-bot").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          const r = await api("/api/admin/bots", { body: f });
          show_token("#bot-token", r.token);
          toast(`已创建 bot（bot:${esc(r.name)}）`);
          await refresh();
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-ep").onsubmit = async (e) => {
        e.preventDefault();
        const fd = new FormData(e.target);
        const f = Object.fromEntries(fd);
        f.is_local = fd.get("is_local") === "on";
        try {
          await api("/api/admin/model/endpoints", { body: f });
          toast("已登记端点");
          await refresh();
        } catch (err) { toast(err.message, true); }
      };

      await refresh();
    },
  },
});
