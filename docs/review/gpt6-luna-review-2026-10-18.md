[0m
> build · gpt-6-luna
[0m→ [0mSkill "code-review" 
> build · gpt-6-luna
[0m✱ [0mGlob "**/*" [90min . · 250 matches[0m
[0m$ [0mgit status --short && git log -1 --oneline
d2bd7cf docs: add deep source audit queue
[0m✱ [0mGrep "TODO|FIXME|XXX|HACK|NotImplemented|pass$|stub" [90min . · 41 matches[0m
> build · gpt-6-luna
[0m→ [0mRead docs/review/audit-todo-2026-10-18.md 
[0m→ [0mRead README.zh.md [90m[limit=220][0m
[0m→ [0mRead todo.md [90m[limit=250][0m
[0m→ [0mRead docs/plan-review-report-v10.0.md 
> build · gpt-6-luna
[0m✱ [0mGrep "send_login_frame|password_|QTcpSocket|start_tls|ssl::|SSL" [90min client/engine/collab/collab_engine.cpp · 8 matches[0m
[0m✱ [0mGrep "append\(sm|ACK|queue_offline|archive_and_resolve" [90min client/engine/collab/collab_engine.cpp · 16 matches[0m
[0m✱ [0mGrep "queue_offline|archive_and_resolve|send_ack|ACK" [90min server/src/session.cpp · 16 matches[0m
[0m✱ [0mGlob ".github/workflows/*" [90min . · 0 matches[0m
[0m✱ [0mGrep "remove\(in.final_path\)|rename|sanitize_rel_path|final_path" [90min client/engine/direct/file_transfer.cpp · 12 matches[0m
> build · gpt-6-luna
[0m→ [0mRead server/src/session.cpp [90m[offset=265, limit=48][0m
[0m→ [0mRead client/engine/collab/collab_engine.cpp [90m[offset=520, limit=35][0m
[0m→ [0mRead client/engine/direct/file_transfer.cpp [90m[offset=330, limit=75][0m
[0m→ [0mRead client/engine/direct/file_transfer.cpp [90m[offset=510, limit=42][0m
[0m→ [0mRead client/engine/collab/collab_engine.cpp [90m[offset=890, limit=35][0m
[0m→ [0mRead common/src/frame.cpp [90m[offset=1, limit=90][0m
> build · gpt-6-luna
[0m→ [0mRead client/core/local_store.hpp [90m[offset=1, limit=170][0m
[0m→ [0mRead client/engine/direct/file_transfer.cpp [90m[offset=55, limit=44][0m
[0m✱ [0mGrep "bool LocalStore::append|LocalStore::append|append\(const StoredMessage|queue_offline\(" [90min client/core/local_store.cpp · 1 match[0m
[0m✱ [0mGrep "bool Store::queue_offline|queue_offline\(" [90min server/src/store.cpp · 1 match[0m
[0m✱ [0mGrep "android|Android|paths-ignore|ctest|harmony|ArkTS|未验证|待真机|WebRTC|TLS|明文" [90min README.zh.md · 8 matches[0m
[0m✱ [0mGlob ".github/workflows/*" [90min . · 8 matches[0m
> build · gpt-6-luna
[0m→ [0mRead client/core/local_store.cpp [90m[offset=145, limit=85][0m
[0m→ [0mRead server/src/store.cpp [90m[offset=1620, limit=58][0m
[0m→ [0mRead .github/workflows/ci.yml [90m[limit=220][0m
[0m→ [0mRead docs/audit-doc-consistency.md 
[0m→ [0mRead .github/workflows/harmony.yml [90m[limit=180][0m
[0m→ [0mRead .github/workflows/ios.yml [90m[limit=140][0m
> build · gpt-6-luna
[0m→ [0mRead server/src/session.cpp [90m[offset=35, limit=35][0m
[0m→ [0mRead server/src/server.cpp [90m[offset=35, limit=100][0m
[0m→ [0mRead client/engine/collab/collab_engine.cpp [90m[offset=35, limit=60][0m
[0m✱ [0mGrep "frame_decoder|FrameDecoder|buffer_" [90min server/src/session.cpp · 0 matches[0m
[0m✱ [0mGrep "WebRTC|HTTP relay|JPEG|base64|remote control|鼠标|键盘|input injection|录屏|录制|R12|高可用" [90min docs/design/remote-assistance.md · 2 matches[0m
[0m✱ [0mGrep "R12|高可用|WebRTC|录屏|Android.*CI|已落地|待真机" [90min todo.md · 10 matches[0m
> build · gpt-6-luna
[0m✱ [0mGrep "OIDC|LDAP|目录认证|身份源|单点登录" [90min todo.md · 7 matches[0m
[0m✱ [0mGlob "server/src/*oidc*" [90min . · 0 matches[0m
[0m✱ [0mGrep "class Ldap|LDAP|ldap" [90min server/src/ldap.cpp · 39 matches[0m
[0m→ [0mRead docs/design/remote-assistance.md [90m[offset=1, limit=85][0m
[0m→ [0mRead apps/harmony/README.md [90m[limit=150][0m
[0m✱ [0mGrep "bool.*(recall|delete)|DELETE.*archive|R12|ha|failover|replica" [90min server/src · 50 matches[0m
> build · gpt-6-luna
[0m✱ [0mGrep "QCursor::setPos|key.*inject|QKeyEvent|frame_b64|JPEG" [90min client/app/assist_dialog.cpp · 4 matches[0m
> build · gpt-6-luna
## 1）最薄弱的 3 处（按风险排序）

1. **协作态及文件面缺少端到端 TLS，口令和消息可能明文传输（高）**  
   `client/engine/collab/collab_engine.cpp:51-52, 534-547` 使用 `QTcpSocket`，登录口令直接写入 LOGIN 帧；`server/src/server.cpp:35-38` 以普通 TCP acceptor 接入。README 还列出直连 HTTP 的管理与文件入口（`README.zh.md:194-213`）。依赖隧道加密不能覆盖局域网直连等部署路径，也不能替代证书校验与失败关闭。

2. **持久化失败仍可能返回 ACK，存在“显示已受理、实际丢消息”窗口（高）**  
   接收端 `collab_engine.cpp:904-913` 调用 `store_->append()` 后不检查返回值，仍发送 ACK；而 `local_store.cpp:170-176` 明确会在 SQL 写入失败时返回 `false`。服务端也在 `session.cpp:287-303` 忽略 `queue_offline()` 的结果并向发送方 ACK；该函数可能因 SQLite 失败返回 `false`（`store.cpp:1638-1650`）。磁盘满、DB 错误或队列写入失败时，接收方可能收不到消息，发送方却已被告知受理。

3. **直连文件接收会在新文件完整验证前删除旧文件，且路径未防符号链接逃逸（中高）**  
   `file_transfer.cpp:387` 在接收开始时删除哈希不符的已有目标；完成时又在改名之前删除目标（`file_transfer.cpp:532-534`）。传输中断或改名失败可能造成旧文件丢失。路径清洗只拒绝 `..`、反斜杠和冒号（`file_transfer.cpp:62-71`），之后直接拼接下载目录（`344-357`），没有证明最终真实路径仍在根目录内，也未阻止父目录 symlink 指向外部。

## 2）最值得补的 3 项新能力

1. **服务端高可用：主备、故障切换与恢复验证**  
   目前单进程、单库部署；R12 的验收口径是服务端故障时直连仍可通信、具备备份恢复预案（`todo.md:34`），不等于服务端本身高可用。之所以现在没有，是原架构明确采用单中心部署，当前重点在备份恢复，而非复制、选主与自动故障切换。应先补产品级可用性目标和 RTO/RPO，再设计实现。

2. **OIDC 单点登录与身份源统一**  
   OIDC 目前是设计稿，不是已交付能力；LDAP/AD 已有实现，但身份源优先级及并存方式仍列为待决策（`todo.md:72-73, 121-123`）。现在缺少它，主要因为目录认证和 OIDC 的先后次序、账号绑定及属性映射尚未定案。先确定身份主源和冲突策略，再补 OIDC 可避免形成两套不一致的账号生命周期。

3. **会议音视频能力（优先集成，不建议本仓自研）**  
   当前协议没有音视频帧；现有远程协助是 JPEG 抓屏轮询（`client/app/assist_dialog.cpp:3, 410`），WebRTC 仍标为延期（`docs/design/remote-assistance.md:9-10`）。它现在没有，是因为规划把会议列为第四期、独立立项或集成项（`docs/plan-review-report-v10.0.md:86`），而不是本仓核心通信闭环。若要补，应先选定内网媒体服务及身份、权限、录制/归档边界。

## 3）文档与实现可能对不上的地方

- **WebRTC 的架构承诺领先于当前实现**：V10 把 WebRTC 写成媒体传输基础（`docs/plan-review-report-v10.0.md:59-61`），但远程协助设计明确说当前通过服务端 HTTP 中继，WebRTC 延期（`docs/design/remote-assistance.md:9-10`）。应把 V10 标成目标架构，避免被读成已落地。
- **R12“服务端高可用”容易被理解过宽**：`todo.md:34` 的实际验收是“直连在服务端故障时仍可用＋备份恢复预案”，并未承诺服务端故障切换。需求名称与验收口径不一致，建议改名或扩展验收标准。
- **“远程协助已实现”不等于完整远程控制已实现**：实现包含授权、帧中继和鼠标移动；设计注明 WebRTC、键盘/鼠标深度注入等能力仍延期（`docs/design/remote-assistance.md:9-10, 37`；`assist_dialog.cpp:210`）。验收和宣传应明确当前是最小闭环。
- **128 项完成与新审计队列不是同一个完成口径**：`docs/review/audit-todo-2026-10-18.md:5-8` 声明它是增量审计清单；其中 P0/P1 项仍未勾选（如 `:38-56, 58-86, 104-162`）。这不否定原产品 TODO 的完成，但说明“功能任务完成”不能等同“安全与可靠性审计完成”。

---

## 执行队列（2026-10-18 追加）

> 本节把上文发现转成可执行项。**排队执行，不与在跑的两单并发**：
> - run_c6a6a2ca  UI 原型对齐（改界面文件）
> - run_107182f9  389 行深审清单（P0 安全类）
>
> 本节项涉及的文件若与上述两单冲突，**停下回报，不许强行合并**。

- [ ] 核对「WebRTC 架构承诺领先于实现」：docs/plan-review-report-v10.0.md:59 vs client/app/assist_dialog.cpp:3,410
      核实后二选一：把文档改成与实际一致（JPEG 抓屏轮询），或标注 WebRTC 为未交付规划
- [ ] 核对「OIDC 是设计稿」：确认当前交付边界，README/文档不得把未交付能力写成已有
- [ ] 文档口径统一：凡写「已支持」的能力逐条与代码比对，对不上的改文档或补实现
