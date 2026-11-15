// 管理后台页面模块（LAY-008 自 admin.js 逐字搬移）：groups；注册表在 helpers.js。
"use strict";

Object.assign(pages, {
  groups: {
    title: "群组管理面",
    desc: "群台账与能力开关（CLI group 同径）＋群 CI（流水线/触发/run 台账）＋群服务器与远程会话（登记 token 仅一次、会话台账）＋密码箱（信息/acl/rekey/审计，密文不回显）＋备忘录（修订史/回滚=一次编辑留痕）＋投票（匿名票不回带身份）/群任务/群工具凭证（掩码元数据）。org-admin=组织级管理面，不过成员面能力闸与白名单。",
    async render(el) {
      el.innerHTML = `
        <h2>群组管理面</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>群台账与能力</h3>
          <table><thead><tr>
            <th>群号</th><th>名称</th><th>群主</th><th>主题</th><th>目的</th><th>成员</th><th>管理员</th><th>备忘录开放编辑</th><th>能力（生效）</th>
          </tr></thead><tbody id="t-groups"></tbody></table>
          <form id="f-cap" class="inline" style="margin-top:10px">
            <label>群号<input name="gid" required></label>
            <label>能力<select name="capability">
              <option value="notice">notice</option><option value="memo">memo</option>
              <option value="vault">vault</option><option value="tools">tools</option>
              <option value="server_tools">server_tools</option><option value="files">files</option>
              <option value="uplink">uplink</option>
            </select></label>
            <label>状态<select name="enabled"><option value="1">启用</option><option value="0">停用</option></select></label>
            <button type="submit">设置</button>
          </form>
          <p class="muted" style="font-size:12px">未配置=现行允许；配置禁即拒（成员面口径）。变更落查阅台账（action=group.capability）。</p>
        </div>
        <div class="card"><h3>群 CI</h3>
          <form id="f-ci-q" class="inline">
            <label>群号<input name="gid" required></label>
            <button type="submit">查询</button>
          </form>
          <h4>流水线</h4>
          <table><thead><tr><th>名称</th><th>说明</th><th>最近状态</th><th>最近时刻</th><th>更新人</th></tr></thead><tbody id="t-pipes"></tbody></table>
          <h4>最近运行</h4>
          <table><thead><tr><th>ID</th><th>流水线</th><th>触发人</th><th>状态</th><th>时刻</th><th>结果</th></tr></thead><tbody id="t-runs"></tbody></table>
          <form id="f-ci-trig" class="inline" style="margin-top:10px">
            <label>群号<input name="gid" required></label>
            <label>流水线<input name="pipeline" required></label>
            <label>params JSON（可选）<input name="params" placeholder='{"fail":true}'></label>
            <button type="submit">触发</button>
          </form>
          <p class="muted" style="font-size:12px">stub 执行器：params.fail=true 即失败；admin 触发只落台账，结果卡片回群走 files 面。</p>
        </div>
        <div class="card"><h3>群服务器与远程会话</h3>
          <form id="f-srv-q" class="inline">
            <label>群号<input name="gid" required></label>
            <button type="submit">查询</button>
          </form>
          <h4>服务器</h4>
          <table><thead><tr><th>ID</th><th>名称</th><th>主机</th><th>在线</th><th>CPU%</th><th>内存</th><th>磁盘</th><th>负载</th><th>最近心跳</th><th>凭据</th></tr></thead><tbody id="t-servers"></tbody></table>
          <form id="f-enroll" class="inline" style="margin-top:10px">
            <label>群号<input name="gid" required></label>
            <label>名称<input name="name" required></label>
            <label>主机<input name="host" required></label>
            <button type="submit">登记</button>
          </form>
          <div id="srv-token" class="token-once hidden"></div>
          <h4>远程会话台账</h4>
          <table><thead><tr><th>ID</th><th>服务器</th><th>主机</th><th>协议</th><th>操作人</th><th>开始</th><th>已兑现</th><th>进行中</th><th>时长</th></tr></thead><tbody id="t-sessions"></tbody></table>
        </div>
        <div class="card"><h3>群密码箱</h3>
          <form id="f-vault-q" class="inline">
            <label>群号<input name="gid" required></label>
            <button type="submit">查询</button>
          </form>
          <div id="vault-info"></div>
          <h4>条目（名称/账号明文，密文不回显）</h4>
          <table><thead><tr><th>ID</th><th>名称</th><th>账号</th><th>建条人</th><th>更新时刻</th></tr></thead><tbody id="t-ventries"></tbody></table>
          <form id="f-vault-acl" class="inline" style="margin-top:10px">
            <label>群号<input name="gid" required></label>
            <label>授权名单（逗号分隔，空=恢复全成员）<input name="accounts"></label>
            <button type="submit">设置名单</button>
          </form>
          <form id="f-vault-rekey" class="inline" style="margin-top:10px">
            <label>群号<input name="gid" required></label>
            <label>kdf_salt<input name="kdf_salt" required></label>
            <label>kdf_iters(≥10000)<input name="kdf_iters" value="100000"></label>
            <label>wrapped_dek<input name="wrapped_dek" required></label>
            <button type="submit">重置包裹块</button>
          </form>
          <p class="muted" style="font-size:12px">无损重包裹是客户端仪式（同 DEK 新口令重包裹）；后台只落仪式产物，误投=成员暂失解锁（条目仍密文在库，可再 rekey 纠正）。</p>
          <h4>审计（reveal/copy 留痕）</h4>
          <table><thead><tr><th>ID</th><th>条目</th><th>操作人</th><th>动作</th><th>时刻</th></tr></thead><tbody id="t-vaudit"></tbody></table>
        </div>
        <div class="card"><h3>群备忘录</h3>
          <form id="f-memo-q" class="inline">
            <label>群号<input name="gid" required></label>
            <button type="submit">查询</button>
          </form>
          <table><thead><tr><th>ID</th><th>标题</th><th>作者</th><th>更新时刻</th><th></th></tr></thead><tbody id="t-memos"></tbody></table>
          <div id="memo-history"></div>
          <form id="f-rollback" class="inline" style="margin-top:10px">
            <label>备忘录 ID<input name="id" required></label>
            <label>修订 ID<input name="revision_id" required></label>
            <button type="submit">回滚</button>
          </form>
          <p class="muted" style="font-size:12px">回滚即一次编辑（落新笔，修订链不断）。</p>
        </div>
        <div class="card"><h3>投票与群任务</h3>
          <form id="f-poll-q" class="inline">
            <label>群号<input name="gid" required></label>
            <button type="submit">查询</button>
          </form>
          <h4>投票</h4>
          <table><thead><tr><th>ID</th><th>主题</th><th>选项/计数</th><th>状态</th><th>匿名</th><th>多选</th><th>投票</th><th></th></tr></thead><tbody id="t-polls"></tbody></table>
          <h4>群任务</h4>
          <table><thead><tr><th>ID</th><th>标题</th><th>认领人</th><th>状态</th><th>截止</th><th>创建人</th></tr></thead><tbody id="t-tasks"></tbody></table>
        </div>
        <div class="card"><h3>群工具凭证与动作审计</h3>
          <form id="f-tool-q" class="inline">
            <label>群号<input name="gid" required></label>
            <button type="submit">查询</button>
          </form>
          <h4>工具凭证（掩码元数据，密文永不出门）</h4>
          <table><thead><tr><th>工具</th><th>更新人</th><th>更新时刻</th></tr></thead><tbody id="t-creds"></tbody></table>
          <h4>工具动作审计</h4>
          <table><thead><tr><th>ID</th><th>工具</th><th>动作</th><th>操作人</th><th>时刻</th><th>结果</th></tr></thead><tbody id="t-taudit"></tbody></table>
        </div>`;

      const load_groups = async () => {
        const g = (await api("/api/admin/groups")).groups;
        el.querySelector("#t-groups").innerHTML = g.map((r) => {
          const caps = (r.capabilities || []).map((c) => {
            const on = c.effective;
            return `<span class="pill ${on ? "" : "off"}">${esc(c.capability)}</span>`;
          }).join(" ");
          const admins = (r.admins || []).join(", ") || '<span class="muted">—</span>';
          return `<tr>
            <td class="mono">${r.gid}</td>
            <td>${esc(r.name)}</td>
            <td class="mono">${esc(r.owner)}</td>
            <td>${r.topic ? esc(r.topic) : '<span class="muted">—</span>'}</td>
            <td>${r.purpose ? esc(r.purpose) : '<span class="muted">—</span>'}</td>
            <td>${(r.members || []).length}</td>
            <td>${admins}</td>
            <td>${r.memo_open_edit ? '<span class="pill">开放</span>' : '<span class="muted">关闭</span>'}</td>
            <td>${caps}</td>
          </tr>`;
        }).join("") || '<tr><td colspan="9" class="muted">无群</td></tr>';
        // 点击行=填能力表单群号
        el.querySelectorAll("#t-groups tr td:first-child").forEach((td) => {
          td.style.cursor = "pointer";
          td.onclick = () => {
            el.querySelector('#f-cap input[name="gid"]').value = td.textContent;
          };
        });
      };
      el.querySelector("#f-cap").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          await api("/api/admin/groups/capability", {
            body: { gid: Number(f.gid), capability: f.capability, enabled: f.enabled === "1" },
          });
          toast("能力已配置"); await load_groups();
        } catch (err) { toast(err.message, true); }
      };

      const gid_val = (formId) => el.querySelector(`#${formId} input[name="gid"]`).value;

      const load_ci = async () => {
        const gid = gid_val("f-ci-q");
        const j = (await api(`/api/admin/group-ci?gid=${encodeURIComponent(gid)}`));
        el.querySelector("#t-pipes").innerHTML = (j.pipelines || []).map((p) => `
          <tr><td>${esc(p.name)}</td><td>${esc(p.description)}</td>
          <td>${p.last_status ? `<span class="pill ${p.last_status === "success" ? "" : "off"}">${esc(p.last_status)}</span>` : '<span class="muted">—</span>'}</td>
          <td class="muted">${fmt_ms(p.last_ts)}</td>
          <td class="mono">${esc(p.updated_by)}</td></tr>`).join("") ||
          '<tr><td colspan="5" class="muted">无流水线</td></tr>';
        el.querySelector("#t-runs").innerHTML = (j.runs || []).map((r) => `
          <tr><td>${r.id}</td><td>${esc(r.pipeline)}</td><td class="mono">${esc(r.actor)}</td>
          <td><span class="pill ${r.status === "success" ? "" : "off"}">${esc(r.status)}</span></td>
          <td class="muted">${fmt_ms(r.ts_ms)}</td>
          <td class="mono" style="font-size:11px">${esc(r.result_json)}</td></tr>`).join("") ||
          '<tr><td colspan="6" class="muted">无运行</td></tr>';
      };
      el.querySelector("#f-ci-q").onsubmit = (e) => { e.preventDefault(); load_ci().catch((err) => toast(err.message, true)); };
      el.querySelector("#f-ci-trig").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        let params = {};
        if (f.params) { try { params = JSON.parse(f.params); } catch { toast("params 不是合法 JSON", true); return; } }
        try {
          await api("/api/admin/group-ci/trigger", {
            body: { gid: Number(f.gid), pipeline: f.pipeline, params },
          });
          toast("已触发（stub 执行器）"); await load_ci();
        } catch (err) { toast(err.message, true); }
      };

      const load_servers = async () => {
        const gid = gid_val("f-srv-q");
        const [s, sess] = await Promise.all([
          api(`/api/admin/group-servers?gid=${encodeURIComponent(gid)}`),
          api(`/api/admin/group-servers/sessions?gid=${encodeURIComponent(gid)}`),
        ]);
        el.querySelector("#t-servers").innerHTML = (s.servers || []).map((r) => `
          <tr><td>${r.id}</td><td>${esc(r.name)}</td><td class="mono">${esc(r.host)}</td>
          <td>${r.online ? '<span class="pill">在线</span>' : '<span class="pill off">离线</span>'}</td>
          <td>${r.cpu_percent >= 0 ? r.cpu_percent.toFixed(1) : "—"}</td>
          <td>${fmt_bytes(r.mem_used_mb * 1e6)}/${fmt_bytes(r.mem_total_mb * 1e6)}</td>
          <td>${fmt_bytes(r.disk_used_mb * 1e6)}/${fmt_bytes(r.disk_total_mb * 1e6)}</td>
          <td>${r.load1.toFixed(2)}</td>
          <td class="muted">${fmt_ms(r.last_seen_ms)}</td>
          <td>${r.cred ? `<span class="pill">已配</span>${esc(r.cred_updated_by)}` : '<span class="muted">—</span>'}</td>
          </tr>`).join("") || '<tr><td colspan="10" class="muted">无服务器</td></tr>';
        el.querySelector("#t-sessions").innerHTML = (sess.sessions || []).map((s2) => `
          <tr><td>${s2.id}</td><td>${esc(s2.server_name)}</td><td class="mono">${esc(s2.host)}</td>
          <td>${esc(s2.protocol)}</td><td class="mono">${esc(s2.actor)}</td>
          <td class="muted">${fmt_ms(s2.opened_ms)}</td>
          <td>${s2.redeemed ? "是" : "否"}</td>
          <td>${s2.open ? '<span class="pill">进行</span>' : "已结束"}</td>
          <td>${s2.duration_ms ? Math.round(s2.duration_ms / 1000) + "s" : "—"}</td></tr>`).join("") ||
          '<tr><td colspan="9" class="muted">无会话</td></tr>';
      };
      el.querySelector("#f-srv-q").onsubmit = (e) => { e.preventDefault(); load_servers().catch((err) => toast(err.message, true)); };
      const show_srv_token = (tok) => {
        const box = el.querySelector("#srv-token");
        box.innerHTML = `登记令牌（仅此一次）：<code>${esc(tok)}</code>`;
        box.classList.remove("hidden");
      };
      el.querySelector("#f-enroll").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          const j = await api("/api/admin/group-servers/enroll", {
            body: { gid: Number(f.gid), name: f.name, host: f.host },
          });
          show_srv_token(j.token); await load_servers();
        } catch (err) { toast(err.message, true); }
      };

      const load_vault = async () => {
        const gid = gid_val("f-vault-q");
        const [v, aud] = await Promise.all([
          api(`/api/admin/group-vault?gid=${encodeURIComponent(gid)}`),
          api(`/api/admin/group-vault/audit?gid=${encodeURIComponent(gid)}`),
        ]);
        const info = el.querySelector("#vault-info");
        if (!v.exists) {
          info.innerHTML = '<p class="muted">密码箱未建立</p>';
        } else {
          info.innerHTML = `<p>已建立 · kdf_iters=${v.kdf_iters} · 授权名单=[${esc((v.acl || []).join(", ")) || "全成员"}] · 更新 ${fmt_ms(v.updated_ms)}</p>`;
        }
        el.querySelector("#t-ventries").innerHTML = (v.entries || []).map((e2) => `
          <tr><td>${e2.id}</td><td>${esc(e2.name)}</td><td class="mono">${esc(e2.account_name)}</td>
          <td class="mono">${esc(e2.created_by)}</td><td class="muted">${fmt_ms(e2.updated_ms)}</td></tr>`).join("") ||
          '<tr><td colspan="5" class="muted">无条目</td></tr>';
        el.querySelector("#t-vaudit").innerHTML = (aud.rows || []).map((a) => `
          <tr><td>${a.id}</td><td>${a.entry_id}</td><td class="mono">${esc(a.actor)}</td>
          <td>${esc(a.action)}</td><td class="muted">${fmt_ms(a.ts_ms)}</td></tr>`).join("") ||
          '<tr><td colspan="5" class="muted">无审计行</td></tr>';
      };
      el.querySelector("#f-vault-q").onsubmit = (e) => { e.preventDefault(); load_vault().catch((err) => toast(err.message, true)); };
      el.querySelector("#f-vault-acl").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        const accounts = (f.accounts || "").split(",").map((s) => s.trim()).filter(Boolean);
        try {
          await api("/api/admin/group-vault/acl", { body: { gid: Number(f.gid), accounts } });
          toast("授权名单已设置"); await load_vault();
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-vault-rekey").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        if (!confirm(`重置群 ${f.gid} 密码箱包裹块？（误投=成员暂失解锁）`)) return;
        try {
          await api("/api/admin/group-vault/rekey", {
            body: { gid: Number(f.gid), kdf_salt: f.kdf_salt, kdf_iters: Number(f.kdf_iters), wrapped_dek: f.wrapped_dek },
          });
          toast("包裹块已重置"); await load_vault();
        } catch (err) { toast(err.message, true); }
      };

      const load_memos = async () => {
        const gid = gid_val("f-memo-q");
        const m = (await api(`/api/admin/group-memos?gid=${encodeURIComponent(gid)}`)).memos;
        el.querySelector("#t-memos").innerHTML = (m || []).map((r) => `
          <tr><td>${r.id}</td><td>${esc(r.title)}</td><td class="mono">${esc(r.author)}</td>
          <td class="muted">${fmt_ms(r.updated_ms)}</td>
          <td><button data-hist="${r.id}" class="ghost">修订史</button></td></tr>`).join("") ||
          '<tr><td colspan="5" class="muted">无备忘录</td></tr>';
        el.querySelectorAll("[data-hist]").forEach((b) => {
          b.onclick = async () => {
            try {
              const h = await api(`/api/admin/group-memo/history?id=${b.dataset.hist}`);
              el.querySelector("#memo-history").innerHTML =
                `<h4>备忘录 #${esc(h.id)} 修订史</h4><table><thead><tr>` +
                `<th>修订 ID</th><th>标题</th><th>编辑人</th><th>时刻</th><th>内容</th></tr></thead><tbody>` +
                h.revisions.map((r) => `<tr><td>${r.id}</td><td>${esc(r.title)}</td>` +
                  `<td class="mono">${esc(r.editor)}</td><td class="muted">${fmt_ms(r.ts_ms)}</td>` +
                  `<td class="mono" style="font-size:11px">${esc(r.content).slice(0, 80)}</td></tr>`).join("") +
                `</tbody></table>`;
            } catch (err) { toast(err.message, true); }
          };
        });
      };
      el.querySelector("#f-memo-q").onsubmit = (e) => { e.preventDefault(); load_memos().catch((err) => toast(err.message, true)); };
      el.querySelector("#f-rollback").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          await api("/api/admin/group-memo/rollback", {
            body: { id: Number(f.id), revision_id: Number(f.revision_id) },
          });
          toast("已回滚（一次编辑留痕）"); await load_memos();
        } catch (err) { toast(err.message, true); }
      };

      const load_polls = async () => {
        const gid = gid_val("f-poll-q");
        const [p, t] = await Promise.all([
          api(`/api/admin/group-polls?gid=${encodeURIComponent(gid)}`),
          api(`/api/admin/group-tasks?gid=${encodeURIComponent(gid)}`),
        ]);
        el.querySelector("#t-polls").innerHTML = (p.polls || []).map((r) => {
          const oc = (r.options || []).map((o, i) => `${esc(o)}:${r.counts[i] ?? 0}`).join(" / ");
          const votes = r.anonymous ? '<span class="muted">匿名（不回带）</span>'
            : (r.votes || []).map((v) => `${esc(v.account)}#${v.choice}`).join(", ");
          return `<tr><td>${r.id}</td><td>${esc(r.topic)}</td>
          <td style="font-size:12px">${oc}</td>
          <td>${r.status === "open" ? '<span class="pill">进行</span>' : '<span class="pill off">已关</span>'}</td>
          <td>${r.anonymous ? "是" : "否"}</td><td>${r.multi ? "是" : "否"}</td>
          <td style="font-size:12px">${votes}</td>
          <td>${r.status === "open" ? `<button data-close="${r.id}" class="ghost">关票</button>` : ""}</td></tr>`;
        }).join("") || '<tr><td colspan="8" class="muted">无投票</td></tr>';
        el.querySelectorAll("[data-close]").forEach((b) => {
          b.onclick = async () => {
            try {
              await api("/api/admin/group-polls/close", { body: { poll_id: Number(b.dataset.close) } });
              toast("已关票"); await load_polls();
            } catch (err) { toast(err.message, true); }
          };
        });
        el.querySelector("#t-tasks").innerHTML = (t.tasks || []).map((r) => `
          <tr><td>${r.id}</td><td>${esc(r.title)}</td><td class="mono">${esc(r.assignee) || "—"}</td>
          <td>${r.status === "done" ? '<span class="pill off">done</span>' : '<span class="pill">todo</span>'}</td>
          <td class="muted">${fmt_ms(r.due_ms)}</td><td class="mono">${esc(r.created_by)}</td></tr>`).join("") ||
          '<tr><td colspan="6" class="muted">无任务</td></tr>';
      };
      el.querySelector("#f-poll-q").onsubmit = (e) => { e.preventDefault(); load_polls().catch((err) => toast(err.message, true)); };

      const load_tools = async () => {
        const gid = gid_val("f-tool-q");
        const [c, a] = await Promise.all([
          api(`/api/admin/group-tools/credentials?gid=${encodeURIComponent(gid)}`),
          api(`/api/admin/group-tools/audit?gid=${encodeURIComponent(gid)}`),
        ]);
        el.querySelector("#t-creds").innerHTML = (c.credentials || []).map((r) => `
          <tr><td>${esc(r.tool)}</td><td class="mono">${esc(r.updated_by)}</td>
          <td class="muted">${fmt_ms(r.updated_ms)}</td></tr>`).join("") ||
          '<tr><td colspan="3" class="muted">无凭据</td></tr>';
        el.querySelector("#t-taudit").innerHTML = (a.rows || []).map((r) => `
          <tr><td>${r.id}</td><td>${esc(r.tool)}</td><td>${esc(r.action)}</td>
          <td class="mono">${esc(r.actor)}</td><td class="muted">${fmt_ms(r.ts_ms)}</td>
          <td class="mono" style="font-size:11px">${esc(r.result_json)}</td></tr>`).join("") ||
          '<tr><td colspan="6" class="muted">无审计行</td></tr>';
      };
      el.querySelector("#f-tool-q").onsubmit = (e) => { e.preventDefault(); load_tools().catch((err) => toast(err.message, true)); };

      await load_groups();
    },
  },
});
