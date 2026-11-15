// 管理后台页面模块（LAY-008 自 admin.js 逐字搬移）：flow；注册表在 helpers.js。
"use strict";

Object.assign(pages, {
  flow: {
    title: "业务流程页",
    desc: "审批（台账＋决定）＋日报团队聚合＋办公室位置图（工位建改/绑定）＋远程协助（部门放行策略＋会话台账＋动作审计）＋个人任务与备忘录台账。org-admin=组织级管理面，直连库同权、不过成员面关系闸（report:read 只对直属上级、approval:decide 只对直属上级或无上级时 org-admin 兜底）。成员红线动作只读：撤回只属申请人、批拒只属受控方（consent）、完成/提醒回执只属清单主人、日报由员工自助提交，后台不代写不代发；assist 会话生命周期（request/respond/start/end）是当事方短票流程，只有台账与策略。个人备忘录为员工私人文本——本面按组织级合规审阅呈现，与平台-6 消息内容红线不同口径。",
    async render(el) {
      el.innerHTML = `
        <h2>业务流程页</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>审批（列表与决定）</h3>
          <form id="f-ap-q" class="inline">
            <label>申请人（空=全部待决）<input name="applicant" size="12"></label>
            <button type="submit">查询</button>
          </form>
          <table><thead><tr>
            <th>ID</th><th>申请人</th><th>类型</th><th>起止</th><th>事由</th>
            <th>状态</th><th>决定人</th><th>备注</th><th>时刻</th>
          </tr></thead><tbody id="t-approvals"></tbody></table>
          <form id="f-ap-dec" class="inline" style="margin-top:10px">
            <label>申请 ID<input name="id" required></label>
            <label>备注<input name="note" size="20"></label>
            <button type="submit">批准</button>
            <button type="button" id="ap-reject" class="ghost">驳回</button>
          </form>
          <p class="muted" style="font-size:12px">成员面口径：撤回只属申请人（本面不开）；自审不成立（申请人本人不可决定）；已决不可重复决定。无「已决全量」接口——store 只有待决全量＋逐申请人，故已决按申请人查。</p>
        </div>
        <div class="card"><h3>日报团队聚合</h3>
          <form id="f-rp-q" class="inline">
            <label>直属上级（空=会话账号）<input name="manager" size="12"></label>
            <button type="submit">查询团队</button>
          </form>
          <table><thead><tr><th>直属下属</th><th>日报数</th><th>最新日报</th><th>最新时刻</th></tr></thead><tbody id="t-team"></tbody></table>
          <form id="f-rp-one" class="inline" style="margin-top:10px">
            <label>作者<input name="author" size="12" required></label>
            <button type="submit">查询日报</button>
          </form>
          <table><thead><tr><th>ID</th><th>作者</th><th>日期</th><th>内容</th><th>更新时刻</th></tr></thead><tbody id="t-reports"></tbody></table>
        </div>
        <div class="card"><h3>办公室位置图</h3>
          <form id="f-of-q" class="inline">
            <label>楼层<input name="floor" size="8" required></label>
            <button type="submit">查询</button>
          </form>
          <div id="of-stage" style="position:relative;height:140px;border:1px dashed #ccc;margin-top:8px;background:#fafafa"></div>
          <table><thead><tr><th>ID</th><th>楼层</th><th>工位号</th><th>x</th><th>y</th><th>占用者</th><th></th></tr></thead><tbody id="t-seats"></tbody></table>
          <form id="f-of-seat" class="inline" style="margin-top:10px">
            <label>楼层<input name="floor" required></label>
            <label>工位号<input name="label" required></label>
            <label>x(0~1)<input name="x" value="0.5" size="6"></label>
            <label>y(0~1)<input name="y" value="0.5" size="6"></label>
            <button type="submit">建/改工位</button>
          </form>
          <form id="f-of-bind" class="inline" style="margin-top:8px">
            <label>工位 ID<input name="id" required></label>
            <label>账号（空=解绑）<input name="account" size="12"></label>
            <button type="submit">绑定/解绑</button>
          </form>
          <form id="f-of-del" class="inline" style="margin-top:8px">
            <label>工位 ID<input name="id" required></label>
            <button type="submit">删除工位</button>
          </form>
          <p class="muted" style="font-size:12px">抽象平面（网格归一化坐标 0~1，不画真实底图）；越界坐标夹回 0~1；一人一工位——绑定已占他座须先解绑（不静默顶替）。</p>
        </div>
        <div class="card"><h3>远程协助（策略＋会话台账＋审计）</h3>
          <h4>部门放行策略</h4>
          <table><thead><tr><th>作用域</th><th>放行</th><th>配置人</th><th>配置时刻</th></tr></thead><tbody id="t-apolicy"></tbody></table>
          <form id="f-ap-set" class="inline" style="margin-top:10px">
            <label>作用域（空=全局）<input name="department_path" size="16"></label>
            <label>状态<select name="allow"><option value="1">放行</option><option value="0">禁止</option></select></label>
            <button type="submit">设置策略</button>
          </form>
          <p class="muted" style="font-size:12px">白名单口径：未配置部门沿链上溯至全局行，再默认禁止（从严）。部门行须挂已存在部门。策略变更落 assist_audits（session_id 为空）。</p>
          <h4>会话台账（只读）</h4>
          <table><thead><tr><th>ID</th><th>发起人</th><th>受控方</th><th>申请权限</th><th>实批权限</th><th>状态</th><th>发起时刻</th><th>结束原因</th></tr></thead><tbody id="t-asessions"></tbody></table>
          <h4>动作审计</h4>
          <table><thead><tr><th>ID</th><th>会话</th><th>操作人</th><th>动作</th><th>详情</th><th>时刻</th></tr></thead><tbody id="t-aaudit"></tbody></table>
        </div>
        <div class="card"><h3>个人任务与备忘录台账</h3>
          <form id="f-tk-q" class="inline">
            <label>账号<input name="account" required></label>
            <button type="submit">查询</button>
          </form>
          <h4>清单（owner=<span id="tk-acc"></span>）</h4>
          <table><thead><tr><th>ID</th><th>标题</th><th>创建人</th><th>到期</th><th>已提醒</th><th>完成</th><th>外部引用</th></tr></thead><tbody id="t-tasks"></tbody></table>
          <h4>我派出的</h4>
          <table><thead><tr><th>ID</th><th>标题</th><th>接收人</th><th>到期</th><th>完成</th></tr></thead><tbody id="t-tasks2"></tbody></table>
          <form id="f-mt-q" class="inline" style="margin-top:10px">
            <label>账号<input name="account" required></label>
            <label>limit<input name="limit" value="50" size="5"></label>
            <label>offset<input name="offset" value="0" size="5"></label>
            <button type="submit">查询备忘录</button>
          </form>
          <table><thead><tr><th>ID</th><th>属主</th><th>内容</th><th>创建</th><th>更新</th></tr></thead><tbody id="t-memos"></tbody></table>
          <p class="muted" style="font-size:12px">个人备忘录为员工私人文本，本面只读台账（组织级合规审阅）；完成/提醒回执只属清单主人，任务与备忘录写入口一律不在后台。</p>
        </div>`;

      const load_approvals = async () => {
        const f = Object.fromEntries(new FormData(el.querySelector("#f-ap-q")));
        const qs = f.applicant
          ? `?applicant=${encodeURIComponent(f.applicant)}` : "";
        const j = await api("/api/admin/approvals" + qs);
        el.querySelector("#t-approvals").innerHTML = (j.approvals || []).map((r) => `
          <tr><td>${r.id}</td><td class="mono">${esc(r.applicant)}</td><td>${esc(r.type)}</td>
          <td>${esc(r["from"]) || "—"} ~ ${esc(r.to) || "—"}</td>
          <td style="font-size:12px">${esc(r.reason)}</td>
          <td>${r.status === "pending" ? '<span class="pill">待决</span>' : `<span class="pill off">${esc(r.status)}</span>`}</td>
          <td class="mono">${esc(r.decider) || "—"}</td>
          <td style="font-size:12px">${esc(r.decision_note) || "—"}</td>
          <td class="muted">${fmt_ms(r.decided_ms || r.created_ms)}</td></tr>`).join("") ||
          `<tr><td colspan="9" class="muted">${j.scope === "pending" ? "无待决" : "无申请"}</td></tr>`;
      };
      el.querySelector("#f-ap-q").onsubmit = (e) => { e.preventDefault(); load_approvals().catch((err) => toast(err.message, true)); };
      const decide = async (approved) => {
        const f = Object.fromEntries(new FormData(el.querySelector("#f-ap-dec")));
        if (!approved && !confirm("驳回该申请？")) return;
        try {
          await api("/api/admin/approvals/decide", {
            body: { id: Number(f.id), approved, note: f.note || "" },
          });
          toast(approved ? "已批准" : "已驳回"); await load_approvals();
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-ap-dec").onsubmit = (e) => { e.preventDefault(); decide(true); };
      el.querySelector("#ap-reject").onclick = () => decide(false);

      const load_team = async () => {
        const f = Object.fromEntries(new FormData(el.querySelector("#f-rp-q")));
        const qs = f.manager ? `?manager=${encodeURIComponent(f.manager)}` : "";
        const j = await api("/api/admin/reports/team" + qs);
        el.querySelector("#t-team").innerHTML = (j.team || []).map((g) => {
          const rs = g.reports || [];
          const last = rs[0];
          return `<tr><td class="mono">${esc(g.author)}</td><td>${rs.length}</td>
            <td>${last ? esc(last.report_date) : '<span class="muted">无</span>'}</td>
            <td class="muted">${fmt_ms(last ? last.updated_ms : 0)}</td></tr>`;
        }).join("") || '<tr><td colspan="4" class="muted">无直属下属（或无汇报线）</td></tr>';
      };
      el.querySelector("#f-rp-q").onsubmit = (e) => { e.preventDefault(); load_team().catch((err) => toast(err.message, true)); };
      el.querySelector("#f-rp-one").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          const j = await api(`/api/admin/reports?author=${encodeURIComponent(f.author)}`);
          el.querySelector("#t-reports").innerHTML = (j.reports || []).map((r) => `
            <tr><td>${r.id}</td><td class="mono">${esc(r.author)}</td><td>${esc(r.report_date)}</td>
            <td style="font-size:12px;white-space:pre-wrap">${esc(r.content)}</td>
            <td class="muted">${fmt_ms(r.updated_ms)}</td></tr>`).join("") ||
            '<tr><td colspan="5" class="muted">无日报</td></tr>';
        } catch (err) { toast(err.message, true); }
      };

      const load_office = async () => {
        const f = Object.fromEntries(new FormData(el.querySelector("#f-of-q")));
        const j = await api(`/api/admin/office-map?floor=${encodeURIComponent(f.floor)}`);
        const seats = j.seats || [];
        el.querySelector("#of-stage").innerHTML = seats.map((s) => `
          <div title="${esc(s.label)}${s.account ? " · " + esc(s.account) : " · 空位"}"
            style="position:absolute;left:${Math.round(s.x * 100)}%;top:${Math.round(s.y * 100)}%;
            transform:translate(-50%,-50%);font-size:11px;padding:2px 6px;border-radius:3px;
            ${s.account ? 'background:#4a9;color:#fff' : 'background:#eee;color:#555'}">
            ${esc(s.label)}${s.account ? "·" + esc(s.account) : ""}
          </div>`).join("") || '<span class="muted" style="padding:8px">空层</span>';
        el.querySelector("#t-seats").innerHTML = seats.map((s) => `
          <tr><td>${s.id}</td><td>${esc(s.floor)}</td><td>${esc(s.label)}</td>
          <td>${s.x.toFixed(2)}</td><td>${s.y.toFixed(2)}</td>
          <td class="mono">${esc(s.account) || '<span class="muted">空位</span>'}</td>
          <td><button data-unbind="${s.id}" class="ghost">解绑</button></td></tr>`).join("") ||
          '<tr><td colspan="7" class="muted">无工位</td></tr>';
        el.querySelectorAll("[data-unbind]").forEach((b) => {
          b.onclick = async () => {
            try {
              await api("/api/admin/office-map/bind", { body: { id: Number(b.dataset.unbind), account: "" } });
              toast("已解绑"); await load_office();
            } catch (err) { toast(err.message, true); }
          };
        });
      };
      el.querySelector("#f-of-q").onsubmit = (e) => { e.preventDefault(); load_office().catch((err) => toast(err.message, true)); };
      el.querySelector("#f-of-seat").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          await api("/api/admin/office-map/seat", {
            body: { floor: f.floor, label: f.label, x: Number(f.x), y: Number(f.y) },
          });
          toast("工位已保存"); el.querySelector("#f-of-q input[name=floor]").value = f.floor;
          await load_office();
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-of-bind").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          await api("/api/admin/office-map/bind", {
            body: { id: Number(f.id), account: f.account || "" },
          });
          toast(f.account ? "已绑定" : "已解绑"); await load_office();
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-of-del").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        if (!confirm(`删除工位 #${f.id}？`)) return;
        try {
          await api("/api/admin/office-map/remove", { body: { id: Number(f.id) } });
          toast("工位已删除"); await load_office();
        } catch (err) { toast(err.message, true); }
      };

      const load_assist = async () => {
        const p = (await api("/api/admin/assist/policy")).policy || [];
        el.querySelector("#t-apolicy").innerHTML = p.map((r) => `
          <tr><td>${esc(r.scope)}</td>
          <td>${r.allow ? '<span class="pill">放行</span>' : '<span class="pill off">禁止</span>'}</td>
          <td class="mono">${esc(r.updated_by)}</td>
          <td class="muted">${fmt_ms(r.updated_ms)}</td></tr>`).join("") ||
          '<tr><td colspan="4" class="muted">未配置任何策略（默认禁止，从严）</td></tr>';
        const [s, a] = await Promise.all([
          api("/api/admin/assist/sessions"),
          api("/api/admin/assist/audit"),
        ]);
        el.querySelector("#t-asessions").innerHTML = (s.sessions || []).map((r) => `
          <tr><td class="mono" style="font-size:11px">${esc(r.id)}</td>
          <td class="mono">${esc(r.requester)}</td><td class="mono">${esc(r.target)}</td>
          <td>${esc(r.requested_perms) || "—"}</td><td>${esc(r.granted_perms) || "—"}</td>
          <td><span class="pill ${r.status === "active" ? "" : "off"}">${esc(r.status)}</span></td>
          <td class="muted">${fmt_ms(r.requested_ms)}</td>
          <td style="font-size:12px">${esc(r.end_reason) || "—"}</td></tr>`).join("") ||
          '<tr><td colspan="8" class="muted">无会话</td></tr>';
        el.querySelector("#t-aaudit").innerHTML = (a.audits || []).map((r) => `
          <tr><td>${r.id}</td><td class="mono" style="font-size:11px">${r.session_id ? esc(r.session_id) : '<span class="muted">（策略面）</span>'}</td>
          <td class="mono">${esc(r.actor)}</td><td>${esc(r.action)}</td>
          <td style="font-size:12px">${esc(r.detail)}</td><td class="muted">${fmt_ms(r.ts_ms)}</td></tr>`).join("") ||
          '<tr><td colspan="6" class="muted">无审计行</td></tr>';
      };
      el.querySelector("#f-ap-set").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        const allow = f.allow === "1";
        if (allow && !confirm("放行该作用域的远程协助？（白名单口径，默认禁止）")) return;
        try {
          await api("/api/admin/assist/policy", {
            body: { allow, department_path: f.department_path || "" },
          });
          toast("策略已更新"); await load_assist();
        } catch (err) { toast(err.message, true); }
      };

      const load_tasks = async () => {
        const f = Object.fromEntries(new FormData(el.querySelector("#f-tk-q")));
        const j = await api(`/api/admin/tasks?account=${encodeURIComponent(f.account)}`);
        el.querySelector("#tk-acc").textContent = j.account;
        el.querySelector("#t-tasks").innerHTML = (j.tasks || []).map((r) => `
          <tr><td>${r.id}</td><td>${esc(r.title)}</td><td class="mono">${esc(r.creator)}</td>
          <td class="muted">${fmt_ms(r.due_ms)}</td>
          <td class="muted">${r.reminded_ms ? "是" : "否"}</td>
          <td>${r.done ? '<span class="pill off">done</span>' : '<span class="pill">todo</span>'}</td>
          <td class="mono" style="font-size:11px">${r.provider ? `${esc(r.provider)}#${esc(r.ext_key)}` : "—"}</td></tr>`).join("") ||
          '<tr><td colspan="7" class="muted">无任务</td></tr>';
        el.querySelector("#t-tasks2").innerHTML = (j.assigned_by_me || []).map((r) => `
          <tr><td>${r.id}</td><td>${esc(r.title)}</td><td class="mono">${esc(r.owner)}</td>
          <td class="muted">${fmt_ms(r.due_ms)}</td>
          <td>${r.done ? '<span class="pill off">done</span>' : '<span class="pill">todo</span>'}</td></tr>`).join("") ||
          '<tr><td colspan="5" class="muted">无派出</td></tr>';
      };
      el.querySelector("#f-tk-q").onsubmit = (e) => { e.preventDefault(); load_tasks().catch((err) => toast(err.message, true)); };
      el.querySelector("#f-mt-q").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          const j = await api(`/api/admin/memos?account=${encodeURIComponent(f.account)}` +
            `&limit=${encodeURIComponent(f.limit || "50")}&offset=${encodeURIComponent(f.offset || "0")}`);
          el.querySelector("#t-memos").innerHTML = (j.memos || []).map((r) => `
            <tr><td>${r.id}</td><td class="mono">${esc(r.owner)}</td>
            <td style="font-size:12px;white-space:pre-wrap">${esc(r.content)}</td>
            <td class="muted">${fmt_ms(r.created_ms)}</td>
            <td class="muted">${fmt_ms(r.updated_ms)}</td></tr>`).join("") ||
            '<tr><td colspan="5" class="muted">无备忘录</td></tr>';
        } catch (err) { toast(err.message, true); }
      };

      await Promise.all([load_approvals(), load_assist()]);
    },
  },
});
