// 管理后台页面模块（LAY-008 自 admin.js 逐字搬移）：files；注册表在 helpers.js。
"use strict";

Object.assign(pages, {
  files: {
    title: "文件与配额",
    desc: "文件台账（置顶/删除，删除＝台账行＋双面配额退费，对象字节留待过期清理对账）；两级配额（群+个人，仅 org-admin 写）；存储状态只读探活（up/down/compose 编排在宿主，后台不代启停）。",
    async render(el) {
      el.innerHTML = `
        <h2>文件与配额</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>文件台账</h3>
          <form id="f-page" class="inline">
            <label>条数<input name="limit" value="200"></label>
            <label>偏移<input name="offset" value="0"></label>
            <button type="submit">查询</button>
          </form>
          <table style="margin-top:10px"><thead><tr>
            <th>ID</th><th>名称</th><th>属主</th><th>归属</th><th>大小</th><th>类目</th><th>状态</th><th>置顶</th><th>上传时间</th><th></th>
          </tr></thead><tbody id="t-files"></tbody></table>
        </div>
        <div class="card"><h3>两级配额（群＋个人）</h3>
          <form id="f-quota" class="inline">
            <label>作用域<select name="scope"><option value="group">群</option><option value="user">个人</option></select></label>
            <label>目标（群号 或 账号）<input name="target" required></label>
            <label>上限字节（0=不限）<input name="limit_bytes" value="0"></label>
            <button type="submit">设置</button>
          </form>
          <h4>群级配额</h4>
          <table><thead><tr>
            <th>群号</th><th>群名</th><th>已用</th><th>上限</th><th></th>
          </tr></thead><tbody id="t-qgroup"></tbody></table>
          <h4>个人级配额</h4>
          <table><thead><tr>
            <th>账号</th><th>显示名</th><th>已用</th><th>上限</th><th></th>
          </tr></thead><tbody id="t-quser"></tbody></table>
        </div>
        <div class="card"><h3>存储状态（只读）</h3>
          <p class="muted" id="st-note" style="font-size:12px"></p>
          <table><tbody id="t-storage"></tbody></table>
        </div>`;

      const load_files = async () => {
        const f = Object.fromEntries(new FormData(el.querySelector("#f-page")));
        const qs = `limit=${encodeURIComponent(f.limit || 200)}&offset=${encodeURIComponent(f.offset || 0)}`;
        const rows = (await api(`/api/admin/files?${qs}`)).files;
        el.querySelector("#t-files").innerHTML = rows.map((x) => `
          <tr><td>${x.id}</td>
          <td>${esc(x.file_name)}</td>
          <td class="mono">${esc(x.owner)}</td>
          <td>${x.belong_gid ? `群 ${esc(x.belong_gid)}` : `个人 ${esc(x.belong_uid || x.owner)}`}</td>
          <td>${fmt_bytes(x.file_size)}</td>
          <td>${x.kind === 1 ? "收件箱" : "常规"}</td>
          <td>${x.status === 0 ? '<span class="pill">正常</span>' : `<span class="pill off">${x.status === 1 ? "隔离" : "过期"}</span>`}</td>
          <td>${x.pin ? '<span class="pill">置顶</span>' : ""}</td>
          <td class="muted">${fmt_ms(x.upload_ts)}</td>
          <td class="row-actions">
            <button data-pin="${x.id}" data-to="${x.pin ? "0" : "1"}" class="ghost">${x.pin ? "取消置顶" : "置顶"}</button>
            <button data-del="${x.id}" class="ghost">删除</button>
          </td></tr>`).join("") ||
          '<tr><td colspan="10" class="muted">无文件行</td></tr>';
        el.querySelectorAll("[data-pin]").forEach((b) => {
          b.onclick = async () => {
            try {
              await api("/api/admin/files/pin", {
                body: { id: Number(b.dataset.pin), pin: b.dataset.to === "1" },
              });
              toast("置顶已更新"); await load_files();
            } catch (err) { toast(err.message, true); }
          };
        });
        el.querySelectorAll("[data-del]").forEach((b) => {
          b.onclick = async () => {
            if (!confirm(`删除文件 #${b.dataset.del}？（台账行删除＋配额退费；对象字节留待过期清理对账）`)) return;
            try {
              await api("/api/admin/files/delete", { body: { id: Number(b.dataset.del) } });
              toast("已删除（stdout 留痕）"); this.render(el);
            } catch (err) { toast(err.message, true); }
          };
        });
      };
      el.querySelector("#f-page").onsubmit = async (e) => {
        e.preventDefault();
        try { await load_files(); } catch (err) { toast(err.message, true); }
      };

      const quota_row = (q) => `
        <tr><td class="mono">${esc(q.target)}</td><td>${esc(q.name)}</td>
        <td>${fmt_bytes(q.used_bytes)}</td>
        <td>${q.limit_bytes ? fmt_bytes(q.limit_bytes) : '<span class="muted">不限</span>'}</td>
        <td class="row-actions"><button data-qset="${esc(q.target)}" data-scope="${esc(q.scope)}" class="ghost">设置</button></td></tr>`;
      const [qg, st] = await Promise.all([
        api("/api/admin/quotas"),
        api("/api/admin/storage/status"),
      ]);
      el.querySelector("#t-qgroup").innerHTML =
        qg.groups.map(quota_row).join("") ||
        '<tr><td colspan="5" class="muted">无群</td></tr>';
      el.querySelector("#t-quser").innerHTML =
        qg.users.map(quota_row).join("") ||
        '<tr><td colspan="5" class="muted">无账号</td></tr>';
      el.querySelector("#f-quota").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        f.limit_bytes = Number(f.limit_bytes || 0);
        try {
          await api("/api/admin/quotas", { body: f });
          toast("配额已设置"); this.render(el);
        } catch (err) { toast(err.message, true); }
      };
      el.querySelectorAll("[data-qset]").forEach((b) => {
        b.onclick = () => {
          const form = el.querySelector("#f-quota");
          form.querySelector('select[name="scope"]').value = b.dataset.scope;
          form.querySelector('input[name="target"]').value = b.dataset.qset;
          form.scrollIntoView({ behavior: "smooth" });
        };
      });

      $("#st-note").textContent = st.note;
      const st_row = (k, v) => {
        let cell;
        if (String(v).startsWith("未配置")) cell = `<span class="muted">${esc(v)}</span>`;
        else cell = v === "up" ? '<span class="pill">up</span>' : '<span class="pill off">down</span>';
        return `<tr><th>${k}</th><td>${cell}</td></tr>`;
      };
      el.querySelector("#t-storage").innerHTML =
        st_row("文件面（/files/health）", st.files) +
        st_row("对象存储（/minio/health/live）", st.storage);

      await load_files();
    },
  },
});
