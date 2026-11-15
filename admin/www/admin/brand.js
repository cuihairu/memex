// 管理后台页面模块（LAY-008 自 admin.js 逐字搬移）：brand, health；注册表在 helpers.js。
"use strict";

Object.assign(pages, {
  brand: {
    title: "品牌与外观",
    desc: "品牌设置（公司名/主色/标语＋logo/splash PNG 上传清除）＋表情包元数据清点＋头像查阅。org-admin=组织级管理面，不过成员面能力闸：表情包为个人素材仅本人读写，本面只清点元数据（名称/大小/时间，组织级合规清点，同个人备忘录先例），不代取字节、不代删；头像上传/删除仅本人，但读面在文件面本就组织内互见，本面代读不新增可见性。素材校验与文件面同口径：PNG magic＋≤512KiB＋≤2048×2048。",
    async render(el) {
      el.innerHTML = `
        <h2>品牌与外观</h2><p class="desc">${this.desc}</p>
        <div class="card"><h3>品牌设置</h3>
          <form id="f-br-q" class="inline">
            <button type="submit">载入当前</button>
            <span id="br-ver" class="muted"></span>
          </form>
          <form id="f-br-set" class="inline" style="margin-top:8px">
            <label>公司名<input name="company_name" size="20"></label>
            <label>主色(#rrggbb)<input name="accent" size="10" placeholder="#1a73e8"></label>
            <label>标语<input name="slogan" size="24"></label>
            <button type="submit">保存文本</button>
          </form>
          <p class="muted" style="font-size:12px">空串=清空该字段；主色须 #rrggbb 形态（空串=清除主色）；保存无变更字段亦会使版本+1（每次写入各 bump 一次）。</p>
          <h4>Logo（PNG ≤512KiB ≤2048×2048）</h4>
          <div id="br-logo" style="margin-bottom:8px"></div>
          <form id="f-br-logo" class="inline">
            <input type="file" name="logo" accept="image/png">
            <button type="submit">上传 logo</button>
            <button type="button" id="br-logo-del" class="ghost">清除 logo</button>
          </form>
          <h4>Splash（PNG ≤512KiB ≤2048×2048）</h4>
          <div id="br-splash" style="margin-bottom:8px"></div>
          <form id="f-br-splash" class="inline">
            <input type="file" name="splash" accept="image/png">
            <button type="submit">上传 splash</button>
            <button type="button" id="br-splash-del" class="ghost">清除 splash</button>
          </form>
        </div>
        <div class="card"><h3>表情包元数据清点（只读）</h3>
          <form id="f-em-q" class="inline">
            <label>账号<input name="account" required></label>
            <button type="submit">清点</button>
          </form>
          <table><thead><tr><th>ID</th><th>名称</th><th>大小</th><th>上传时刻</th></tr></thead><tbody id="t-emoji"></tbody></table>
          <p class="muted" style="font-size:12px">表情包为个人素材、仅本人读写：本面只清点元数据（组织级合规清点），不代取字节、不代删——字节与删除仍是属主能力闸。</p>
        </div>
        <div class="card"><h3>头像查阅（只读）</h3>
          <form id="f-av-q" class="inline">
            <label>账号<input name="account" required></label>
            <label>档位<select name="size"><option>32</option><option>64</option><option>128</option><option selected>256</option></select></label>
            <button type="submit">载入</button>
          </form>
          <div id="av-out" style="margin-top:8px"></div>
          <p class="muted" style="font-size:12px">头像上传/删除仅本人（后台不代传不代删）；读面在文件面本就组织内互见（任意成员 ?account=），本面代读不新增可见性。</p>
        </div>`;

      // 原始字节上传（api() 恒发 application/json，素材走 image/png）
      const api_raw = async (path, blob) => {
        const resp = await fetch(path, {
          method: "POST",
          headers: { "X-Memex-Admin": "1", "Content-Type": "image/png" },
          credentials: "same-origin",
          body: blob,
        });
        let j = {};
        try { j = await resp.json(); } catch (e) { /* 非 JSON 应答 */ }
        if (!resp.ok) throw new Error(j.error || `HTTP ${resp.status}`);
        return j;
      };

      const load_brand = async () => {
        const j = await api("/api/admin/branding");
        el.querySelector("#br-ver").textContent =
          `版本 ${j.version} · 更新 ${fmt_ms(j.updated_ms)}`;
        const f = el.querySelector("#f-br-set");
        f.elements.company_name.value = j.company_name || "";
        f.elements.accent.value = j.accent || "";
        f.elements.slogan.value = j.slogan || "";
        el.querySelector("#br-logo").innerHTML = j.has_logo
          ? `<img src="/api/admin/branding/logo" alt="logo" style="max-height:64px;border:1px solid #ddd;padding:2px"> <span class="muted">${fmt_bytes(j.logo_size)}</span>`
          : '<span class="muted">未配置</span>';
        el.querySelector("#br-splash").innerHTML = j.has_splash
          ? `<img src="/api/admin/branding/splash" alt="splash" style="max-height:64px;border:1px solid #ddd;padding:2px"> <span class="muted">${fmt_bytes(j.splash_size)}</span>`
          : '<span class="muted">未配置</span>';
      };

      el.querySelector("#f-br-q").onsubmit = (e) => {
        e.preventDefault(); load_brand().catch((err) => toast(err.message, true));
      };
      el.querySelector("#f-br-set").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          await api("/api/admin/branding", { body: f });
          toast("品牌文本已保存"); await load_brand();
        } catch (err) { toast(err.message, true); }
      };
      const upload_asset = async (form_sel, field, path, name) => {
        const inp = el.querySelector(form_sel).elements[field];
        if (!inp.files.length) { toast("请先选择文件", true); return; }
        if (inp.files[0].size > 512 * 1024) { toast("素材超大小上限（512KiB）", true); return; }
        try {
          await api_raw(path, inp.files[0]);
          toast(`${name} 已上传`); await load_brand();
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#f-br-logo").onsubmit = (e) => {
        e.preventDefault(); upload_asset("#f-br-logo", "logo", "/api/admin/branding/logo", "logo");
      };
      el.querySelector("#f-br-splash").onsubmit = (e) => {
        e.preventDefault(); upload_asset("#f-br-splash", "splash", "/api/admin/branding/splash", "splash");
      };
      const clear_asset = async (key, name) => {
        if (!confirm(`清除 ${name}？`)) return;
        try {
          await api("/api/admin/branding", { body: { [key]: true } });
          toast(`${name} 已清除`); await load_brand();
        } catch (err) { toast(err.message, true); }
      };
      el.querySelector("#br-logo-del").onclick = () => clear_asset("clear_logo", "logo");
      el.querySelector("#br-splash-del").onclick = () => clear_asset("clear_splash", "splash");

      el.querySelector("#f-em-q").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        try {
          const j = await api(`/api/admin/emoji?account=${encodeURIComponent(f.account)}`);
          el.querySelector("#t-emoji").innerHTML = (j.emoji || []).map((r) => `
            <tr><td>${r.id}</td><td class="mono">${esc(r.name)}</td>
            <td class="muted">${fmt_bytes(r.size)}</td>
            <td class="muted">${fmt_ms(r.ts_ms)}</td></tr>`).join("") ||
            '<tr><td colspan="4" class="muted">无素材</td></tr>';
        } catch (err) { toast(err.message, true); }
      };

      el.querySelector("#f-av-q").onsubmit = async (e) => {
        e.preventDefault();
        const f = Object.fromEntries(new FormData(e.target));
        const qs = `account=${encodeURIComponent(f.account)}&size=${encodeURIComponent(f.size)}`;
        try {
          const resp = await fetch(`/api/admin/avatar?${qs}`, {
            headers: { "X-Memex-Admin": "1" },
            credentials: "same-origin",
          });
          if (!resp.ok) {
            let msg = `HTTP ${resp.status}`;
            try { msg = (await resp.json()).error || msg; } catch (err) { /* 非 JSON */ }
            el.querySelector("#av-out").innerHTML = `<span class="muted">${esc(msg)}</span>`;
            return;
          }
          const px = Number(f.size) || 128;
          el.querySelector("#av-out").innerHTML =
            `<img src="${resp.url}" alt="avatar" style="height:${px}px;border:1px solid #ddd;padding:2px"> ` +
            `<span class="muted">${esc(f.account)} · ${px}px · ${esc(resp.headers.get("content-type") || "")}</span>`;
        } catch (err) { toast(err.message, true); }
      };

      await load_brand();
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
});
