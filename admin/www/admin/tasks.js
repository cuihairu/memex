// 管理后台页面模块（LAY-008 自 admin.js 逐字搬移）：taskpull, tasktypes；注册表在 helpers.js。
"use strict";

Object.assign(pages, {
  taskpull: {
    title: "任务源拉取",
    desc: "服务端定时拉取外部任务源（GitHub issues 起步）落成员个人任务清单并经既有通知长连接推送。数据外发默认关：总开关默认关闭且服务未配 --task-pull-secret 时整面不启用；源凭据只在服务端密封落库，后台只见「已配置」位，无明文无密文。",
    async render(el) {
      el.innerHTML = `
        <h2>任务源拉取</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>拉取总开关</h3>
          <p class="muted" id="tp-scope" style="font-size:12px"></p>
          <form id="f-tp" class="inline">
            <label style="flex-direction:row;align-items:center"><input type="checkbox" name="enabled" id="tp-enabled" style="min-width:0"> 开启定时拉取（数据外发）</label>
            <button type="submit">保存</button>
          </form>
        </div>
        <div class="card"><h3>成员已登记源（盘点）</h3>
          <table><thead><tr>
            <th>属主</th><th>源</th><th>仓库</th><th>状态</th><th>间隔秒</th><th>最近拉取</th><th>凭据</th>
          </tr></thead><tbody id="t-tp"></tbody></table>
        </div>`;

      const refresh = async () => {
        const st = await api("/api/admin/task-pull");
        el.querySelector("#tp-enabled").checked = !!st.enabled;
        el.querySelector("#tp-scope").textContent =
          (st.enabled ? "当前状态：开启。" : "当前状态：关闭（默认，零外呼）。") +
          " 开启后的拉取范围＝下列「启用」状态的源；无启用源时开启不产生任何拉取。";
        el.querySelector("#t-tp").innerHTML = st.sources.map((s) => `
          <tr><td class="mono">${esc(s.owner)}</td>
          <td>${esc(s.provider)}</td>
          <td class="mono">${esc(s.repo)}</td>
          <td>${s.enabled ? '<span class="pill">启用</span>' : '<span class="pill off">停用</span>'}</td>
          <td>${s.interval_s}</td>
          <td class="muted">${fmt_ms(s.last_pull_ms)}</td>
          <td>${s.cred_configured ? '<span class="pill">已配置</span>' : '<span class="muted">未配置（公开源裸拉）</span>'}</td></tr>`).join("") ||
          '<tr><td colspan="7" class="muted">暂无成员登记任务源</td></tr>';
      };

      el.querySelector("#f-tp").onsubmit = async (e) => {
        e.preventDefault();
        const on = el.querySelector("#tp-enabled").checked;
        if (on) {
          // 显式开启＝明示拉取范围（把将要被拉取的启用源列进确认框）
          const st = await api("/api/admin/task-pull");
          const scope = st.sources
            .filter((s) => s.enabled)
            .map((s) => `${s.owner}/${s.provider} ${s.repo}`);
          const msg = scope.length
            ? `开启定时拉取（数据外发）？\n将按各自间隔拉取以下已启用源：\n${scope.join("\n")}`
            : "开启定时拉取（数据外发）？\n当前没有任何启用状态的源，开启后也不会产生拉取。";
          if (!confirm(msg)) return;
        }
        try {
          await api("/api/admin/task-pull", { body: { enabled: on } });
          toast(on ? "已开启（审计已记拉取范围）" : "已关闭");
          await refresh();
        } catch (err) { toast(err.message, true); }
      };

      await refresh();
    },
  },

  tasktypes: {
    title: "任务类型",
    desc: "任务类型在服务端配置（初始四类：任务/需求/缺陷/其他，存量迁移为「任务」），桌面/手机端从服务端拉取同源展示。默认类型与有任务引用的类型拒删（存量任务不悬空）；增删均落审计。",
    async render(el) {
      el.innerHTML = `
        <h2>任务类型</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>类型清单</h3>
          <form id="f-tt" class="inline">
            <label>新类型名<input name="name" required></label>
            <button type="submit">添加</button>
          </form>
          <table style="margin-top:10px"><thead><tr>
            <th>类型</th><th>默认</th><th></th>
          </tr></thead><tbody id="t-tt"></tbody></table>
        </div>`;

      const refresh = async () => {
        const st = await api("/api/admin/task-types");
        el.querySelector("#t-tt").innerHTML = st.types.map((t) => `
          <tr><td>${esc(t.name)}</td>
          <td>${t.is_default ? '<span class="pill">默认</span>' : ""}</td>
          <td>${t.is_default ? "" : `<button data-del="${esc(t.name)}" type="button">删除</button>`}</td></tr>`).join("");
        el.querySelectorAll("button[data-del]").forEach((b) => {
          b.onclick = async () => {
            if (!confirm(`删除任务类型「${b.dataset.del}」？默认类型与有任务引用的类型会被服务端拒。`)) return;
            try {
              await api("/api/admin/task-types/delete", { body: { name: b.dataset.del } });
              toast("已删除");
              await refresh();
            } catch (err) { toast(err.message, true); }
          };
        });
      };

      el.querySelector("#f-tt").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          await api("/api/admin/task-types", { body: { name: f.name.trim() } });
          toast("已添加");
          e.target.reset();
          await refresh();
        } catch (err) { toast(err.message, true); }
      };

      await refresh();
    },
  },
});
