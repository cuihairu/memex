# 服务端 CLI：serve 面旗标

> 本页记录 **`memex_server serve` 的完整旗标契约**：全部 40 个旗标、
> 其背后的严格解析规则、环境变量兜底，以及旗标到运行面的装配后果。
> 解析器是 `parse_serve_args`（`server/src/serve_args.cpp`，自 `cmd_serve`
> 抽出）；验收测试为 `serve_args` ctest（`server/tests/test_serve_args.cpp`，
> 直打解析器、不起进程）。应用内设置开关见
> [配置效果](/zh/guide/config-effects)。

## 参数形态

`serve` 子命令的旗标从**剥离子命令后的参数段**解析
（`serve_args.hpp:51-54`）：

- `argv[0]` 即**首个旗标**（如 `{"--port", "24370"}`）——不带程序名、
  不带子命令。
- 裸词（不以 `--` 开头）一律按**位置参数**拒绝、退出码 2——sub_argv
  口径下不该有裸词。
- `argc = 0` 合法，得到全默认值（等价裸 `serve`）。

## 严格解析（退出码 2 契约）

下列失败一律向 stderr 打印原因并以 **rc 2** 退出——绝不静默回落默认值
（`serve_args.cpp:1-4` 头注释）：

| 违例 | 示例 | 规则 |
|---|---|---|
| 未知旗标 | `--prot`（`--port` 的拼写错误） | `serve_args.cpp:326-330`——拼错的旗标不许让端口/安全开关悄悄走默认 |
| 位置参数 | `serve extra` | `serve_args.cpp:99-102` |
| 尾旗标缺值 | `serve --port` 是最后一个参数 | `serve_args.cpp:74-85`——旧口径「静默跳过」会让端口/安全开关悄悄漂到默认 |
| 布尔旗标带值 | `--s3-use-ssl=true` | `serve_args.cpp:198-201` 及其余布尔分支——布尔语义无值可带 |
| 非严格数字 | `--port abc`、`--port 99999999999` | `serve_args.cpp:32-49`——仅可选负号＋全数字；杂字符/空串/超 int 量级一律拒绝**而非回绕**（std::atoi 静默把 `"abc"` 变 0、把超大值截断，拼写错误会悄悄变成另一份「合法」配置） |
| 端口越界 | `--port 0`、`--port 70000` | 见下表逐旗标界 |
| `--ldap-group-role` 形态错 | `--ldap-group-role admins`（无 `=`） | `serve_args.cpp:266-270`——须 `组=角色` 且两侧非空 |

**内联形态**：全部取值旗标均支持 `--flag=value`（`serve_args.cpp:93-97`，
codex-761），与 `--flag value` 等价；布尔旗标两种形态都不带值。

## 旗标参考

### 端口与功能面

| 旗标 | 默认 | 界 | 0 的含义 |
|---|---|---|---|
| `--port` | 24360 | 1–65535 | —（必须 > 0） |
| `--webhook-port` | 24361 | 0–65535 | 关闭 webhook 接入；其余功能不受影响 |
| `--model-port` | 0（关） | 0–65535 | 模型网关关（默认） |
| `--files-port` | 0（关） | 0–65535 | 文件面关（默认） |
| `--uplink-port` | 0（关） | 0–65535 | 外网单向 uplink 面关（默认） |

默认值在 `serve_args.hpp:16-22`；安全默认是刻意口径——文件面、uplink
面、模型网关都**须显式给端口才开**。

### 群内智能助手（平台三期）

| 旗标 | 效果 |
|---|---|
| `--assistant <bot>` | 轮询四指令（`@助手/@纪要/@整理/@检索`）的 bot 名；空＝关 |
| `--assistant-token <token>` | 该 bot 的明文 token |

装配门（`main.cpp:276-295`）：助手另需 **webhook 接入口开启**、token
（旗标或 `MEMEX_ASSISTANT_TOKEN` env——建议 env，旗标值在 `ps` 可见）、
bot 已存在（先 `memex_server bot add` 并 join 群）。任一门未过都打印
原因且助手不启用（服务端照常起）。

### uplink 面防护（R23-5）

| 旗标 | 效果 |
|---|---|
| `--uplink-ext-denylist <csv>` | 危险扩展名黑名单**整表替换**（逗号分隔、小写、不带点；无后缀文件不命中）。空串＝清空（＝不限扩展名）。默认表 23 项：`exe dll bat cmd com scr msi msp cpl hta jar js jse vbs vbe wsf wsh ps1 sh bash apk deb rpm`（`files_server.hpp:93-97`） |
| `--uplink-ext-allowlist <csv>` | 非空即**启用白名单模式**：名单外后缀一律拒（无后缀也拒），黑名单失效（`serve_args.cpp:155-158`） |
| `--uplink-max-mb <n>` | uplink 面独立上传上限（MiB），≥ 0；0＝沿用全局上传上限（`serve_args.cpp:159-167`） |
| `--uplink-login-secret <s>` | 外网登录部署级第二口令：`/uplink/session` 请求体须带匹配 `secondary` 字段，不匹配 401（`files_server.hpp:102-104`） |

### 功能密钥

| 旗标 | 效果 |
|---|---|
| `--tool-cred-secret <s>` | 凭据面主密钥（服务端派生 GCM 密钥）。缺省＝凭据面未启用 |
| `--task-pull-secret <s>` | 任务源拉取面主密钥（PBKDF2 同水位派生）。缺省＝拉取面未启用＝**零外呼**；配置路由答 503 |

### S3（文件/uplink 存储）

| 旗标 | 默认 | 说明 |
|---|---|---|
| `--s3-endpoint` | 空 | 内网 http＋path-style 为默认（`main.cpp:66-72`） |
| `--s3-bucket` | 空 | 启动时幂等引导（先 head 查，10 次 × 2s 重试；失败打印原因且面照起、上传一律 503，`main.cpp:74-84`） |
| `--s3-access-key` | 空 | |
| `--s3-secret-key` | 空 | |
| `--s3-region` | `auto` | **唯一没有环境变量兜底**的旗标 |
| `--s3-use-ssl` | 关 | 布尔，不带值 |

文件面真正打开需 `--files-port > 0` **且** S3 四值配齐（`main.cpp:70`）；
给了端口但 S3 不齐会打印缺变量提示、起面后一律 503（`main.cpp:87-91`）。

### LDAP / AD（可选企业身份）

不配 `--ldap-host`＝LDAP 关——判登全本地，轻量部署行为与旧版逐字节
一致（`ldap.hpp:9-10`）。红线口径（`ldap.hpp:11-16`）：目录不可用＝拒绝
登录（fail-closed，绝不回落本地口令）；口令只在 bind 报文里走一次
（不落日志/库）；映射表外的组不授任何角色；本地账号常备（应急恢复
入口）。

| 旗标 | 默认 | 说明 |
|---|---|---|
| `--ldap-host` | 空（关） | |
| `--ldap-port` | 389 | 1–65535 |
| `--ldap-timeout-ms` | 4000 | ≥ 0 |
| `--ldap-user-base` | 空 | 如 `ou=people,dc=corp` |
| `--ldap-user-attr` | `uid` | AD 常用 `sAMAccountName` |
| `--ldap-memberof-attr` | `memberOf` | OpenLDAP 需 memberof overlay |
| `--ldap-bind-dn-template` | 空 | 模板直绑，`{u}` 替换用户名（OpenLDAP 常用） |
| `--ldap-bind-dn` | 空 | 服务账号 DN，先查后绑（AD 常用） |
| `--ldap-bind-password` | 空 | 建议 env `MEMEX_LDAP_BIND_PASSWORD`——旗标值在 `ps` 可见（`serve_args.cpp:239-246`） |
| `--ldap-group-role <组=角色>` | 空 | 组→角色显式映射，**可重复**；表外组一概忽略，映射表不含 admin 就永不授管理员（`serve_args.cpp:261-271`） |
| `--ldap-disabled-attr` / `--ldap-disabled-rule` / `--ldap-disabled-value` | 空 | 可选禁用检测三元组；AD 默认：`userAccountControl` / `1.2.840.113556.1.4.803` / `2`（`serve_args.cpp:250-260`） |
| `--ldap-kick-disabled <n>` | 1 | 目录禁用 × 存量会话：0＝仅拒新登录，非 0＝踢下线 |
| `--ldap-recheck-seconds` | 60 | 存量会话复检间隔，≥ 0；0＝不复检 |
| `--ldap-ssl` | 关 | 布尔——ldaps:// 整连接 TLS（636 口径），证书主机名校验必开 |
| `--ldap-starttls` | 关 | 布尔——明连 389 后升级 TLS；**与 `--ldap-ssl` 互斥**（`ldap.cpp:540`） |
| `--ldap-allow-plain` | 关 | 布尔——明文 bind 显式放行（默认拒＝密码不明文红线；仅限测试/受控内网，启动日志明示） |
| `--ldap-tls-insecure` | 关 | 布尔——自签证书跳过校验（启动明示警告） |
| `--ldap-auto-bind` | 关 | 布尔——已有本地账号无绑定：目录认证成功即自动绑定（默认关＝只认 CLI 显式绑定） |
| `--ldap-auto-provision` | 关 | 布尔——账号不存在：目录认证成功即建影子账号（本地口令随机不可知＝仅 LDAP 登录） |

## 环境变量兜底（旗标恒赢）

旗标缺省时回落环境变量；**命令行旗标恒赢**——即使排障脚本改写了 env
也不影响服务端（`serve_args.cpp:332-334`）。env 让容器部署的密钥不进
`ps`/日志：

| 变量 | 填充 |
|---|---|
| `MEMEX_S3_ENDPOINT` / `MEMEX_S3_BUCKET` / `MEMEX_S3_ACCESS_KEY` / `MEMEX_S3_SECRET_KEY` | S3 四值（`serve_args.cpp:339-342`） |
| `MEMEX_LDAP_HOST` / `MEMEX_LDAP_USER_BASE` / `MEMEX_LDAP_BIND_DN_TEMPLATE` / `MEMEX_LDAP_BIND_DN` / `MEMEX_LDAP_BIND_PASSWORD` / `MEMEX_LDAP_MEMBEROF_ATTR` | 六个 LDAP 字符串（`serve_args.cpp:346-355`） |
| `MEMEX_LDAP_GROUP_ROLE` | 逗号分隔 `组=角色` 列表，追加到旗标映射之后（`serve_args.cpp:356-370`） |
| `MEMEX_ASSISTANT_TOKEN` | `--assistant-token` 缺省时的助手 token（`main.cpp:280-284`） |

一处不对称值得知道：**`MEMEX_LDAP_GROUP_ROLE` 里的坏形态条目**（无 `=`
或两侧空）会被**静默跳过**（`serve_args.cpp:363-366`），同样的坏形态放
在旗标上则是 rc 2。旗标路径才是严格路径——映射重要时优先用旗标。

## 验证

`serve_args` ctest（`server/tests/test_serve_args.cpp`）直打
`parse_serve_args`，覆盖：默认值、全部端口界（0／负／越界／溢出／杂字符
→rc 2）、uplink 整表替换与 `max-mb` 换算、S3/LDAP env 兜底与旗标恒赢、
旗标与 env 两条路的组→角色映射、全部严格解析腿（未知旗标／位置参数／
缺值／布尔带值／内联形态）。在 server 构建树里 `ctest -R serve_args`。

> 核验日期：2026-11-15（旗标清单自 `serve_args.cpp` 以
> `grep -oP 'arg == "--\K[a-z0-9-]+'` 重提＝40；端口与各界已对照上文
> 引用行复核）。
